#!/usr/bin/env python3
from __future__ import annotations

import argparse
import cmd as cmdlib
import codecs
import os
import queue
import random
import shlex
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional, Tuple


# --------------------------------------------------------------------------- #
# Protocol constants
#
# Matches:
#
#   #define PROTOCOL_SIGNATURE 0xfaaa
#
#   #pragma pack(push,1)
#   struct PacketHeader
#   {
#       uint16_t signature;
#       union
#       {
#           MasterCommand masterCommandID;
#           ClientCommand clientCommandID;
#       };
#       uint32_t followingLength;
#   };
#   #pragma pack(pop)
#
# Wire format:
#
#   offset  size
#   0       2       uint16_t signature, little endian
#   2       1       command
#   3       4       uint32_t followingLength, little endian
#   7       N       payload
#
# This is therefore a 7-byte header.
# --------------------------------------------------------------------------- #

DEFAULT_HOST = "192.168.10.1"
DEFAULT_PORT = 90
DEFAULT_MAX_CLIENTS = 4

PROTOCOL_SIGNATURE = 0xFAAA

# uint16_t signature + uint8_t command + uint32_t followingLength
HEADER = struct.Struct("<HBI")
HEADER_SIZE = HEADER.size  # 7

# Safety limit used by this diagnostic tool when decoding.
#
# This is NOT a protocol limit. The actual C++ protocol uses uint32_t, so
# lengths up to 0xffffffff can be represented on the wire.
SANE_LEN = 4096


MASTER_CMDS = {
    "INVALID": 0,
    "IDENT": 1,
    "STATUS": 2,
    "CUSTOM": 3,
    "START": 4,
    "STOP": 5,
    "PAUSE": 6,
    "RESET": 7,
}

CLIENT_CMDS = {
    "INVALID": 0,
    "IDENT": 1,
    "STATUS": 2,
    "CUSTOM": 3,
}

STATES = {
    "RUNNING": 0,
    "PAUSED": 1,
    "WAITING": 2,
}

MASTER_NAMES = {v: k for k, v in MASTER_CMDS.items()}
CLIENT_NAMES = {v: k for k, v in CLIENT_CMDS.items()}

CMD_IDENT = CLIENT_CMDS["IDENT"]

# Prefix of the text the master sends when all seats are occupied.
FULL_MSG = b"Too many client"


# --------------------------------------------------------------------------- #
# Output helpers
# --------------------------------------------------------------------------- #

class Style:
    on = sys.stdout.isatty() and "NO_COLOR" not in os.environ

    @classmethod
    def c(cls, code: str, s: str) -> str:
        return f"\x1b[{code}m{s}\x1b[0m" if cls.on else s


dim = lambda s: Style.c("2", s)
red = lambda s: Style.c("31", s)
green = lambda s: Style.c("32", s)
yellow = lambda s: Style.c("33", s)
cyan = lambda s: Style.c("36", s)
bold = lambda s: Style.c("1", s)

_print_lock = threading.Lock()
_T0 = time.monotonic()


def log(msg: str = "") -> None:
    with _print_lock:
        print(
            f"{dim(f'{time.monotonic() - _T0:8.3f}')} {msg}",
            flush=True,
        )


def hexdump(data: bytes, indent: str = "", width: int = 16) -> str:
    lines = []

    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        hx = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(
            chr(b) if 32 <= b < 127 else "."
            for b in chunk
        )

        lines.append(
            f"{indent}{off:04x}  "
            f"{hx:<{width * 3 - 1}}  "
            f"|{asc}|"
        )

    return "\n".join(lines)


def short_hex(data: bytes, limit: int = 24) -> str:
    s = data[:limit].hex(" ")

    if len(data) > limit:
        s += f" …(+{len(data) - limit})"

    return s


# --------------------------------------------------------------------------- #
# Packet building / stream decoding
# --------------------------------------------------------------------------- #

def pack(
    cmd: int,
    payload: bytes = b"",
    length: Optional[int] = None,
) -> bytes:
    """Build a protocol packet.

    Wire format:

        AA FA
        command
        length[0]
        length[1]
        length[2]
        length[3]
        payload...

    for PROTOCOL_SIGNATURE = 0xFAAA.
    """

    n = len(payload) if length is None else length

    return (
        HEADER.pack(
            PROTOCOL_SIGNATURE,
            cmd & 0xFF,
            n & 0xFFFFFFFF,
        )
        + payload
    )


@dataclass
class Frame:
    cmd: int
    length: int
    payload: bytes


class Decoder:
    """Decode the superV TCP protocol from a byte stream.

    The protocol is:

        uint16_t signature       LE
        uint8_t  command
        uint32_t followingLength LE
        uint8_t  payload[]

    Header size = 7 bytes.

    Data that does not begin with PROTOCOL_SIGNATURE is treated as raw.
    This allows us to recognize responses such as:

        ERROR : Too many client connected.
    """

    def __init__(self, names: Dict[int, str]):
        self.names = names
        self.buf = bytearray()

    def feed(self, data: bytes) -> List[Tuple[str, object]]:
        self.buf += data

        out: List[Tuple[str, object]] = []

        while self.buf:
            # We need two bytes to inspect the uint16 signature.
            if len(self.buf) < 2:
                break

            signature = struct.unpack_from("<H", self.buf, 0)[0]

            # Not protocol data.
            if signature != PROTOCOL_SIGNATURE:
                nl = self.buf.find(b"\n")

                if nl >= 0:
                    n = nl + 1
                else:
                    n = len(self.buf)

                out.append(
                    ("raw", bytes(self.buf[:n]))
                )

                del self.buf[:n]
                continue

            # Signature is correct but the rest of the header hasn't arrived.
            if len(self.buf) < HEADER_SIZE:
                break

            signature, cmd, length = HEADER.unpack_from(
                self.buf,
                0,
            )

            # Signature check, mostly defensive because we already checked it.
            if signature != PROTOCOL_SIGNATURE:
                out.append(
                    ("raw", bytes(self.buf[:2]))
                )
                del self.buf[:2]
                continue

            # Unknown command.
            #
            # INVALID (0) is intentionally allowed because it is a legitimate
            # enum value in protocol.hpp.
            if cmd not in self.names:
                out.append(
                    ("raw", bytes(self.buf[:HEADER_SIZE]))
                )
                del self.buf[:HEADER_SIZE]
                continue

            # Diagnostic decoder safety limit.
            #
            # The real protocol supports uint32_t lengths. We simply don't
            # attempt to buffer absurd amounts of data here.
            if length > SANE_LEN:
                out.append(("raw", bytes(self.buf)))
                self.buf.clear()
                break

            frame_size = HEADER_SIZE + length

            # Valid header, but payload hasn't arrived yet.
            if len(self.buf) < frame_size:
                break

            payload = bytes(
                self.buf[HEADER_SIZE:frame_size]
            )

            del self.buf[:frame_size]

            out.append(
                (
                    "frame",
                    Frame(
                        cmd=cmd,
                        length=length,
                        payload=payload,
                    ),
                )
            )

        return out

    def flush(self) -> List[Tuple[str, object]]:
        if not self.buf:
            return []

        rest = bytes(self.buf)
        self.buf.clear()

        return [("partial", rest)]


def describe_frame(
    f: Frame,
    names: Dict[int, str],
) -> str:
    s = (
        f"{names.get(f.cmd, '?')}"
        f"({f.cmd}) "
        f"len={f.length}"
    )

    if f.payload:
        txt = f.payload.rstrip(b"\0")

        if txt and all(32 <= b < 127 for b in txt):
            try:
                s += f' "{txt.decode()}"'
            except UnicodeDecodeError:
                s += " " + short_hex(f.payload, 16)
        else:
            s += " " + short_hex(f.payload, 16)

    return s


def describe_bytes(
    data: bytes,
    names: Dict[int, str],
) -> str:
    """Best-effort description of bytes about to be sent."""

    if len(data) < HEADER_SIZE:
        if len(data) >= 2:
            signature = struct.unpack_from("<H", data)[0]

            return (
                f"incomplete header "
                f"sig=0x{signature:04x}, "
                f"{len(data)}/{HEADER_SIZE} bytes"
            )

        return f"incomplete header ({len(data)}/{HEADER_SIZE} bytes)"

    signature, cmd, length = HEADER.unpack_from(data)

    if signature != PROTOCOL_SIGNATURE:
        return (
            f"bad-signature=0x{signature:04x} "
            f"expected=0x{PROTOCOL_SIGNATURE:04x}"
        )

    payload = data[
        HEADER_SIZE:HEADER_SIZE + length
    ]

    s = describe_frame(
        Frame(
            cmd=cmd,
            length=length,
            payload=payload,
        ),
        names,
    )

    actual_payload = len(data) - HEADER_SIZE

    if actual_payload != length:
        s += yellow(
            f"  [header says {length}, "
            f"actual payload {actual_payload}]"
        )

    return s


# --------------------------------------------------------------------------- #
# Connection wrapper: background reader + event queue
# --------------------------------------------------------------------------- #

class Conn:
    def __init__(
        self,
        host: str,
        port: int,
        timeout: float = 5.0,
        name: str = "",
        verbose: bool = True,
        full_dump: bool = False,
    ):
        self.host = host
        self.port = port
        self.timeout = timeout

        self.name = name
        self.verbose = verbose
        self.full_dump = full_dump

        self.sock: Optional[socket.socket] = None

        # Incoming data from the master uses MasterCommand.
        self.dec = Decoder(MASTER_NAMES)

        self.q: "queue.Queue[Tuple[str, object]]" = queue.Queue()

        self.alive = False
        self.error: Optional[BaseException] = None

        self._eof_seen = False
        self._closing = False

        self.chunk = 0
        self.chunk_delay = 0.0

        self.connect_ms = 0.0

    # -- plumbing ----------------------------------------------------------

    def _p(self, msg: str) -> None:
        if self.verbose:
            prefix = (
                f"{dim('[' + self.name + ']')} "
                if self.name
                else ""
            )

            log(prefix + msg)

    def connect(self) -> "Conn":
        t0 = time.monotonic()

        self.sock = socket.create_connection(
            (self.host, self.port),
            timeout=self.timeout,
        )

        self.sock.setsockopt(
            socket.IPPROTO_TCP,
            socket.TCP_NODELAY,
            1,
        )

        self.sock.settimeout(None)

        self.connect_ms = (
            time.monotonic() - t0
        ) * 1000

        self.alive = True

        self._p(
            f"{bold('●')} connected to "
            f"{self.host}:{self.port} "
            f"({self.connect_ms:.0f} ms)"
        )

        threading.Thread(
            target=self._read_loop,
            daemon=True,
        ).start()

        return self

    def _push(self, kind: str, val: object) -> None:
        if self.verbose:
            if kind == "frame":
                self._p(
                    f"  {green('↳')} "
                    f"{describe_frame(val, MASTER_NAMES)}"
                )

            elif kind == "raw":
                self._p(
                    f"  {yellow('↳ text/raw')} "
                    f"{bytes(val)!r}"
                )

            elif kind == "partial":
                self._p(
                    f"  {yellow('↳ incomplete frame at EOF')} "
                    f"{short_hex(bytes(val))}"
                )

        self.q.put((kind, val))

    def _read_loop(self) -> None:
        assert self.sock

        try:
            while True:
                data = self.sock.recv(4096)

                if not data:
                    break

                if self.verbose:
                    self._p(
                        f"{green('←')} RX "
                        f"{len(data):>3}B  "
                        f"{short_hex(data)}"
                    )

                    if self.full_dump:
                        self._p(
                            "\n"
                            + hexdump(
                                data,
                                "           ",
                            )
                        )

                for kind, val in self.dec.feed(data):
                    self._push(kind, val)

        except OSError as e:
            if not self._closing:
                self.error = e

        finally:
            for kind, val in self.dec.flush():
                self._push(kind, val)

            self.alive = False

            if not self._closing:
                why = (
                    f" ({self.error})"
                    if self.error
                    else ""
                )

                self._p(
                    f"{red('✕')} connection closed "
                    f"by peer{why}"
                )

            self.q.put(
                ("eof", self.error)
            )

    # -- events ------------------------------------------------------------

    def next_event(
        self,
        timeout: float,
    ) -> Optional[Tuple[str, object]]:
        if self._eof_seen and self.q.empty():
            return ("eof", self.error)

        try:
            ev = self.q.get(
                timeout=max(timeout, 0)
            )
        except queue.Empty:
            return None

        if ev[0] == "eof":
            self._eof_seen = True

        return ev

    def wait_frame(
        self,
        cmd: Optional[int],
        timeout: float,
    ) -> Optional[Frame]:
        deadline = time.monotonic() + timeout

        while True:
            ev = self.next_event(
                deadline - time.monotonic()
            )

            if ev is None or ev[0] == "eof":
                return None

            if (
                ev[0] == "frame"
                and (
                    cmd is None
                    or ev[1].cmd == cmd
                )
            ):
                return ev[1]  # type: ignore[return-value]

            if time.monotonic() >= deadline:
                return None

    def collect(
        self,
        duration: float,
    ) -> List[Tuple[str, object]]:
        """Collect events for duration seconds."""

        out: List[Tuple[str, object]] = []

        deadline = time.monotonic() + duration

        while True:
            ev = self.next_event(
                deadline - time.monotonic()
            )

            if ev is None:
                return out

            out.append(ev)

            if ev[0] == "eof":
                return out

    # -- sending / closing -------------------------------------------------

    def send(
        self,
        data: bytes,
        desc: Optional[str] = None,
        chunks: Optional[int] = None,
        delay: Optional[float] = None,
    ) -> None:
        assert self.sock

        chunks = (
            self.chunk
            if chunks is None
            else chunks
        )

        delay = (
            self.chunk_delay
            if delay is None
            else delay
        )

        if chunks > 0:
            pieces = [
                data[i:i + chunks]
                for i in range(
                    0,
                    len(data),
                    chunks,
                )
            ]
        else:
            pieces = [data]

        info = (
            desc
            if desc is not None
            else describe_bytes(
                data,
                CLIENT_NAMES,
            )
        )

        for i, piece in enumerate(pieces):
            self.sock.sendall(piece)

            if self.verbose:
                part = (
                    f" (chunk {i + 1}/{len(pieces)})"
                    if len(pieces) > 1
                    else ""
                )

                self._p(
                    f"{cyan('→')} TX "
                    f"{len(piece):>3}B  "
                    f"{short_hex(piece)}"
                    f"{part}"
                )

                if i == 0 and info:
                    self._p(
                        f"  {cyan('↳')} {info}"
                    )

                if self.full_dump:
                    self._p(
                        "\n"
                        + hexdump(
                            piece,
                            "           ",
                        )
                    )

            if (
                delay
                and i < len(pieces) - 1
            ):
                time.sleep(delay)

    def half_close(self) -> None:
        if self.sock:
            self._p(
                f"{yellow('⇥')} "
                f"half-close "
                f"(FIN, still reading)"
            )

            self.sock.shutdown(
                socket.SHUT_WR
            )

    def abort(self) -> None:
        """Close with RST instead of FIN."""

        if self.sock:
            self._closing = True

            self._p(
                f"{yellow('✕')} abort (RST)"
            )

            self.sock.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_LINGER,
                struct.pack(
                    "ii",
                    1,
                    0,
                ),
            )

            try:
                self.sock.shutdown(
                    socket.SHUT_RD
                )
            except OSError:
                pass

            self.sock.close()
            self.alive = False

            time.sleep(0.05)

    def close(self) -> None:
        if self.sock and not self._closing:
            self._closing = True

            self._p(
                f"{dim('○ closing')}"
            )

            try:
                self.sock.shutdown(
                    socket.SHUT_RDWR
                )
            except OSError:
                pass

            self.sock.close()
            self.alive = False


# --------------------------------------------------------------------------- #
# Argument helpers
# --------------------------------------------------------------------------- #

def parse_int(s: str) -> int:
    return int(s, 0)


def parse_cmd(token: str) -> int:
    t = token.strip().upper()

    if t in CLIENT_CMDS:
        return CLIENT_CMDS[t]

    if t in MASTER_CMDS:
        return MASTER_CMDS[t]

    return int(token, 0) & 0xFF


def parse_hex(s: str) -> bytes:
    cleaned = (
        s.replace("0x", " ")
         .replace("0X", " ")
         .replace(",", " ")
         .replace(":", " ")
    )

    return bytes.fromhex(
        "".join(cleaned.split())
    )


def add_common(
    p: argparse.ArgumentParser,
) -> None:
    g = p.add_argument_group(
        "connection"
    )

    g.add_argument(
        "-H",
        "--host",
        default=DEFAULT_HOST,
        help=(
            f"master address "
            f"(default {DEFAULT_HOST})"
        ),
    )

    g.add_argument(
        "-p",
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=(
            f"master port "
            f"(default {DEFAULT_PORT})"
        ),
    )

    g.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help=(
            "connect/greeting timeout in s "
            "(default 5)"
        ),
    )

    g.add_argument(
        "-x",
        "--hexdump",
        action="store_true",
        help="full hexdump of every TX/RX chunk",
    )

    g.add_argument(
        "--no-color",
        action="store_true",
    )


def add_payload_args(
    p: argparse.ArgumentParser,
) -> None:
    g = p.add_argument_group(
        "payload"
    )

    g.add_argument(
        "-t",
        "--text",
        help="payload as text",
    )

    g.add_argument(
        "--nul",
        action="store_true",
        help=(
            "append an explicit NUL after --text "
            "(Protocol::sendCommand(const char*) "
            "does not normally transmit this NUL)"
        ),
    )

    g.add_argument(
        "--hex",
        help=(
            "payload as hex, "
            "e.g. 'de ad be ef'"
        ),
    )

    g.add_argument(
        "--file",
        help="payload from a file",
    )

    g.add_argument(
        "--len",
        dest="length",
        type=parse_int,
        help=(
            "override the header length field "
            "(lie about it)"
        ),
    )


def payload_from_args(
    ns: argparse.Namespace,
) -> bytes:
    if ns.hex:
        return parse_hex(ns.hex)

    if ns.file:
        with open(ns.file, "rb") as fh:
            return fh.read()

    if ns.text is not None:
        return (
            ns.text.encode()
            + (b"\0" if ns.nul else b"")
        )

    return b""


def new_conn(
    ns: argparse.Namespace,
    name: str = "",
    verbose: bool = True,
) -> Conn:
    return Conn(
        ns.host,
        ns.port,
        ns.timeout,
        name=name,
        verbose=verbose,
        full_dump=getattr(
            ns,
            "hexdump",
            False,
        ),
    )


# --------------------------------------------------------------------------- #
# Health probe
# --------------------------------------------------------------------------- #

def probe(
    host: str,
    port: int,
    timeout: float = 5.0,
) -> Tuple[str, str]:
    """Return (status, detail).

    status:
        ok
        refused
        unreachable
        no-greeting
        seat-leak
        bad-greeting
    """

    c = Conn(
        host,
        port,
        timeout,
        verbose=False,
    )

    try:
        c.connect()

    except ConnectionRefusedError:
        return (
            "refused",
            "connection refused",
        )

    except OSError as e:
        return (
            "unreachable",
            str(e),
        )

    try:
        ev = c.next_event(timeout)

        if ev is None:
            return (
                "no-greeting",
                f"no IDENT within {timeout}s "
                "(loop stuck / board hung?)",
            )

        kind, val = ev

        if (
            kind == "frame"
            and val.cmd == CMD_IDENT
        ):
            return (
                "ok",
                f"IDENT greeting in "
                f"{c.connect_ms:.0f} ms",
            )

        if (
            kind == "raw"
            and FULL_MSG in bytes(val)
        ):
            return (
                "seat-leak",
                "master says 'Too many client' "
                "- a seat was never freed",
            )

        return (
            "bad-greeting",
            repr(val),
        )

    finally:
        c.close()


# --------------------------------------------------------------------------- #
# Sub-command: pack
# --------------------------------------------------------------------------- #

def cmd_pack(
    ns: argparse.Namespace,
) -> int:
    data = pack(
        parse_cmd(ns.cmd),
        payload_from_args(ns),
        ns.length,
    )

    if ns.output:
        with open(ns.output, "wb") as fh:
            fh.write(data)

        print(
            f"wrote {len(data)} bytes "
            f"to {ns.output}"
        )

    print(hexdump(data))
    print(
        describe_bytes(
            data,
            CLIENT_NAMES,
        )
    )

    return 0


# --------------------------------------------------------------------------- #
# Sub-command: decode
# --------------------------------------------------------------------------- #

def cmd_decode(
    ns: argparse.Namespace,
) -> int:
    if ns.file == "-":
        raw = sys.stdin.buffer.read()
    else:
        with open(ns.file, "rb") as fh:
            raw = fh.read()

    names = (
        MASTER_NAMES
        if ns.direction == "master"
        else CLIENT_NAMES
    )

    dec = Decoder(names)

    print(hexdump(raw))
    print()

    for kind, val in (
        dec.feed(raw) + dec.flush()
    ):
        if kind == "frame":
            print(
                "frame   "
                + describe_frame(
                    val,
                    names,
                )
            )

        elif kind == "raw":
            print(
                f"raw     {bytes(val)!r}"
            )

        else:
            print(
                f"partial "
                f"{short_hex(bytes(val))}  "
                "(header/payload incomplete)"
            )

    return 0


# --------------------------------------------------------------------------- #
# Sub-command: send
# --------------------------------------------------------------------------- #

def cmd_send(
    ns: argparse.Namespace,
) -> int:
    c = new_conn(ns)

    try:
        c.connect()

    except OSError as e:
        print(
            red(f"connect failed: {e}")
        )
        return 2

    if not ns.no_greeting:
        if (
            c.wait_frame(
                CMD_IDENT,
                ns.timeout,
            )
            is None
        ):
            log(
                red(
                    "no IDENT greeting received "
                    "(continuing anyway)"
                )
            )

    if ns.raw:
        data = parse_hex(ns.raw)

    else:
        data = pack(
            parse_cmd(ns.cmd),
            payload_from_args(ns),
            ns.length,
        )

    for i in range(ns.repeat):
        c.send(
            data,
            chunks=ns.chunk,
            delay=ns.chunk_delay / 1000,
        )

        if (
            ns.interval
            and i < ns.repeat - 1
        ):
            time.sleep(ns.interval)

    if ns.half_close:
        c.half_close()

    log(
        dim(
            f"holding connection for "
            f"{ns.hold}s ..."
        )
    )

    events = c.collect(ns.hold)

    if ns.rst:
        c.abort()
    else:
        c.close()

    return (
        0
        if not any(
            k == "eof" and not ns.half_close
            for k, _ in events
        )
        else 1
    )


# --------------------------------------------------------------------------- #
# Sub-command: shell
# --------------------------------------------------------------------------- #

class Shell(cmdlib.Cmd):
    intro = (
        f"{bold('superV shell')} - "
        "type 'help' for commands, 'quit' to leave. "
        "Connection stays open; RX is printed live."
    )

    prompt = "superv> "

    def __init__(
        self,
        ns: argparse.Namespace,
    ):
        super().__init__()

        self.ns = ns
        self.conn: Optional[Conn] = None
        self.failed = False

    # -- helpers ------------------------------------------------------------

    def _need(self) -> Optional[Conn]:
        if (
            self.conn is None
            or not self.conn.alive
        ):
            print(
                red(
                    "not connected - "
                    "use 'reconnect'"
                )
            )

            return None

        return self.conn

    def _tx(
        self,
        data: bytes,
        desc: Optional[str] = None,
    ) -> None:
        c = self._need()

        if c:
            try:
                c.send(
                    data,
                    desc,
                )

            except OSError as e:
                print(
                    red(
                        f"send failed: {e}"
                    )
                )

    def connect(self) -> bool:
        try:
            self.conn = (
                new_conn(self.ns).connect()
            )

            return True

        except OSError as e:
            print(
                red(
                    f"connect failed: {e}"
                )
            )

            self.failed = True
            return False

    def onecmd(self, line: str) -> bool:
        try:
            return super().onecmd(line)

        except (
            ValueError,
            OSError,
            IndexError,
        ) as e:
            print(
                red(f"error: {e}")
            )

            return False

    def emptyline(self) -> bool:
        return False

    # -- commands -----------------------------------------------------------

    def do_ident(self, arg: str) -> None:
        """ident NAME - send IDENT with NAME.

        Unlike the old version, no implicit NUL is added. This matches
        Protocol::sendCommand(..., const char*) where strlen() determines
        the payload length.
        """

        self._tx(
            pack(
                CLIENT_CMDS["IDENT"],
                arg.encode(),
            )
        )

    def do_status(self, arg: str) -> None:
        """status STATE - send STATUS with a 1-byte state.

        STATE:
            RUNNING
            PAUSED
            WAITING
            or numeric value
        """

        a = (
            arg.strip().upper()
            or "RUNNING"
        )

        state = (
            STATES[a]
            if a in STATES
            else int(a, 0)
        )

        self._tx(
            pack(
                CLIENT_CMDS["STATUS"],
                bytes([state & 0xFF]),
            )
        )

    def do_custom(self, arg: str) -> None:
        """custom TEXT - send CUSTOM with TEXT."""

        self._tx(
            pack(
                CLIENT_CMDS["CUSTOM"],
                arg.encode(),
            )
        )

    def do_send(self, arg: str) -> None:
        """send CMD [text...|hex:DEADBEEF] [len=N]

        Examples:

            send ident hello
            send status 00
            send 0xff hex:00 01 02
            send ident AAAA len=1000
        """

        parts = shlex.split(arg)

        if not parts:
            raise ValueError(
                "send requires a command"
            )

        cmd_id = parse_cmd(parts[0])

        length = None
        rest = []

        for p in parts[1:]:
            if p.startswith("len="):
                length = int(
                    p[4:],
                    0,
                )
            else:
                rest.append(p)

        body = " ".join(rest)

        if body.startswith("hex:"):
            payload = parse_hex(
                body[4:]
            )
        else:
            payload = body.encode()

        self._tx(
            pack(
                cmd_id,
                payload,
                length,
            )
        )

    def do_raw(self, arg: str) -> None:
        """raw HEX - send raw bytes without any header."""

        self._tx(
            parse_hex(arg),
            "",
        )

    def do_rawtext(self, arg: str) -> None:
        r"""rawtext TEXT - send raw text.

        Escapes are interpreted, e.g.:

            rawtext GET / HTTP/1.1\r\n\r\n
        """

        self._tx(
            codecs.decode(
                arg,
                "unicode_escape",
            ).encode("latin-1"),
            "",
        )

    def do_chunk(self, arg: str) -> None:
        """chunk N [DELAY_MS] - split following sends.

        chunk 1 50
            sends byte-by-byte with 50 ms between bytes.

        chunk 0
            disables chunking.
        """

        c = self._need()

        if c:
            p = arg.split()

            c.chunk = (
                int(p[0])
                if p
                else 0
            )

            c.chunk_delay = (
                float(p[1]) / 1000
                if len(p) > 1
                else 0.0
            )

            print(
                f"chunk={c.chunk} "
                f"delay="
                f"{c.chunk_delay * 1000:.0f}ms"
            )

    def do_wait(self, arg: str) -> None:
        """wait SECONDS - sleep."""

        time.sleep(
            float(arg or "1")
        )

    def do_expect(self, arg: str) -> None:
        """expect NAME [SECS] - wait for a master frame or EOF."""

        p = arg.split()

        c = self._need()

        if not c or not p:
            self.failed = True
            return

        secs = (
            float(p[1])
            if len(p) > 1
            else 5.0
        )

        if p[0].lower() == "eof":
            ok = any(
                k == "eof"
                for k, _ in c.collect(secs)
            )
        else:
            ok = (
                c.wait_frame(
                    parse_cmd(p[0]),
                    secs,
                )
                is not None
            )

        print(
            green("expect OK")
            if ok
            else red("expect FAILED")
        )

        self.failed |= not ok

    def do_halfclose(self, arg: str) -> None:
        """halfclose - send FIN but keep reading."""

        c = self._need()

        if c:
            c.half_close()

    def do_rst(self, arg: str) -> None:
        """rst - abort the connection with TCP RST."""

        if self.conn:
            self.conn.abort()

    def do_close(self, arg: str) -> None:
        """close - graceful close."""

        if self.conn:
            self.conn.close()

    def do_reconnect(self, arg: str) -> None:
        """reconnect - close and connect again."""

        if self.conn:
            self.conn.close()
            time.sleep(0.3)

        self.connect()

    def do_info(self, arg: str) -> None:
        """info - show connection state."""

        c = self.conn

        print(
            f"{'connected' if c and c.alive else 'disconnected'}  "
            f"{self.ns.host}:{self.ns.port}"
        )

    def do_hexdump(self, arg: str) -> None:
        """hexdump on|off - toggle full hexdumps."""

        if self.conn:
            self.conn.full_dump = (
                arg.strip().lower()
                in ("on", "1", "true", "")
            )

            print(
                "hexdump "
                + (
                    "on"
                    if self.conn.full_dump
                    else "off"
                )
            )

    def do_source(self, arg: str) -> None:
        """source FILE - run commands from a file."""

        with open(arg.strip()) as fh:
            for line in fh:
                line = (
                    line
                    .split("#", 1)[0]
                    .strip()
                )

                if line:
                    print(
                        dim(f"> {line}")
                    )

                    if self.onecmd(line):
                        return

    def do_quit(self, arg: str) -> bool:
        """quit - leave."""

        if self.conn:
            self.conn.close()

        return True

    do_exit = do_quit
    do_EOF = do_quit


def cmd_shell(
    ns: argparse.Namespace,
) -> int:
    sh = Shell(ns)

    if not sh.connect():
        return 2

    batch = ns.exec or ns.script

    if ns.exec:
        for part in ns.exec.split(";"):
            if part.strip():
                print(
                    dim(
                        f"> {part.strip()}"
                    )
                )

                if sh.onecmd(
                    part.strip()
                ):
                    break

    if ns.script:
        sh.onecmd(
            f"source {ns.script}"
        )

    if batch and not ns.interactive:
        sh.onecmd("quit")

    else:
        sh.intro = Shell.intro

        try:
            sh.cmdloop()

        except KeyboardInterrupt:
            sh.onecmd("quit")

    return 1 if sh.failed else 0


# --------------------------------------------------------------------------- #
# Sub-command: client
# --------------------------------------------------------------------------- #

def cmd_client(
    ns: argparse.Namespace,
) -> int:
    state = STATES["WAITING"]

    try:
        while True:
            c = new_conn(
                ns,
                name=ns.ident,
            )

            try:
                c.connect()

            except OSError as e:
                log(
                    red(
                        f"connect failed: {e}"
                    )
                )

                if not ns.reconnect:
                    return 2

                time.sleep(2)
                continue

            next_status = (
                time.monotonic()
                + (
                    ns.status_every
                    if ns.status_every
                    else 1e9
                )
            )

            while c.alive:
                ev = c.next_event(0.25)

                if ev is not None:
                    kind, val = ev

                    if kind == "eof":
                        break

                    if kind == "frame":
                        f: Frame = val  # type: ignore[assignment]

                        name = MASTER_NAMES.get(
                            f.cmd,
                            "",
                        )

                        if name == "IDENT":
                            # Match Protocol::sendCommand(..., const char*)
                            # semantics: no terminating NUL.
                            c.send(
                                pack(
                                    CLIENT_CMDS["IDENT"],
                                    ns.ident.encode(),
                                )
                            )

                        elif name == "START":
                            state = STATES["RUNNING"]

                        elif name == "PAUSE":
                            state = STATES["PAUSED"]

                        elif name in (
                            "STOP",
                            "RESET",
                        ):
                            state = STATES["WAITING"]

                if (
                    ns.status_every
                    and time.monotonic()
                    >= next_status
                ):
                    c.send(
                        pack(
                            CLIENT_CMDS["STATUS"],
                            bytes([state]),
                        )
                    )

                    next_status = (
                        time.monotonic()
                        + ns.status_every
                    )

            c.close()

            if not ns.reconnect:
                return 0

            log(
                dim(
                    "reconnecting in 2s ..."
                )
            )

            time.sleep(2)

    except KeyboardInterrupt:
        return 0


# --------------------------------------------------------------------------- #
# Sub-command: stress
# --------------------------------------------------------------------------- #

def classify_greeting(
    c: Conn,
    timeout: float,
) -> str:
    ev = c.next_event(timeout)

    if ev is None:
        return "silent"

    kind, val = ev

    if (
        kind == "frame"
        and val.cmd == CMD_IDENT
    ):
        return "accepted"

    if (
        kind == "raw"
        and FULL_MSG in bytes(val)
    ):
        return "rejected"

    if kind == "eof":
        return "closed"

    return f"unexpected:{val!r}"


def cmd_stress(
    ns: argparse.Namespace,
) -> int:
    conns: List[
        Tuple[int, Conn, str]
    ] = []

    for i in range(ns.clients):
        c = new_conn(
            ns,
            name=f"c{i}",
            verbose=False,
        )

        try:
            c.connect()

        except OSError as e:
            log(
                red(
                    f"c{i}: connect failed: {e}"
                )
            )

            conns.append(
                (i, c, "connect-failed")
            )

            continue

        res = classify_greeting(
            c,
            ns.timeout,
        )

        colour = (
            green
            if res == "accepted"
            else yellow
            if res == "rejected"
            else red
        )

        log(
            f"c{i:<2} "
            f"{colour(res):<20} "
            f"({c.connect_ms:.0f} ms)"
        )

        conns.append(
            (i, c, res)
        )

        time.sleep(ns.interval)

    accepted = [
        x for x in conns
        if x[2] == "accepted"
    ]

    rejected = [
        x for x in conns
        if x[2] == "rejected"
    ]

    for i, c, res in accepted:
        try:
            c.send(
                pack(
                    CLIENT_CMDS["IDENT"],
                    f"stress-{i}".encode(),
                )
            )
        except OSError:
            pass

    log(
        dim(
            f"holding {len(accepted)} "
            f"connections for {ns.hold}s ..."
        )
    )

    time.sleep(ns.hold)

    dead = [
        i
        for i, c, res in accepted
        if not c.alive
    ]

    for _, c, _ in conns:
        c.close()

    ok = True

    expected_acc = min(
        ns.clients,
        ns.max_clients,
    )

    print()

    print(
        f"accepted={len(accepted)} "
        f"(expected {expected_acc})  "
        f"rejected={len(rejected)} "
        f"(expected "
        f"{max(0, ns.clients - ns.max_clients)})  "
        f"dropped-while-holding={len(dead)}"
    )

    if (
        len(accepted) != expected_acc
        or dead
    ):
        ok = False

    log(
        dim(
            f"waiting {ns.settle}s "
            "for the board to free the seats ..."
        )
    )

    time.sleep(ns.settle)

    status, detail = probe(
        ns.host,
        ns.port,
        ns.timeout,
    )

    print(
        "post-stress probe: "
        + (
            green(status)
            if status == "ok"
            else red(status)
        )
        + f" - {detail}"
    )

    ok &= status == "ok"

    print(
        green("STRESS OK")
        if ok
        else red("STRESS FAILED")
    )

    return 0 if ok else 1


# --------------------------------------------------------------------------- #
# Sub-command: fuzz
# --------------------------------------------------------------------------- #

def gen_packet(
    rng: random.Random,
) -> Tuple[bytes, str, int]:
    """Return (bytes, description, tx_chunk_size)."""

    rb = lambda n: bytes(
        rng.randrange(256)
        for _ in range(n)
    )

    kind = rng.choice(
        [
            "valid",
            "bad_signature",
            "badcmd",
            "biglen",
            "short",
            "garbage",
            "zerolen",
            "long",
            "bytewise",
        ]
    )

    if kind == "valid":
        p = (
            bytes(
                rng.randrange(97, 123)
                for _ in range(
                    rng.randrange(0, 70)
                )
            )
        )

        return (
            pack(1, p),
            f"valid IDENT, {len(p)}B name",
            0,
        )

    if kind == "bad_signature":
        bad_sig = rng.choice(
            [
                0x0000,
                0x0001,
                0x0055,
                0xAA55,
                0x55AA,
                0xFFFF,
                rng.randrange(0x10000),
            ]
        )

        p = rb(
            rng.randrange(0, 8)
        )

        data = (
            struct.pack(
                "<H",
                bad_sig,
            )
            + bytes([CLIENT_CMDS["IDENT"]])
            + struct.pack(
                "<I",
                len(p),
            )
            + p
        )

        return (
            data,
            f"bad signature=0x{bad_sig:04x}",
            0,
        )

    if kind == "badcmd":
        c = rng.choice(
            [
                8,
                9,
                0x7F,
                0x80,
                0xFF,
                rng.randrange(256),
            ]
        )

        p = rb(
            rng.randrange(0, 8)
        )

        return (
            pack(c, p),
            (
                f"bad cmd=0x{c:02x} "
                f"payload={len(p)}B"
            ),
            0,
        )

    if kind == "biglen":
        n = rng.choice(
            [
                0xFFFFFFFF,
                0x80000000,
                0x10000,
                1000,
                65,
                64,
            ]
        )

        p = rb(
            rng.randrange(0, 5)
        )

        return (
            pack(
                1,
                p,
                n,
            ),
            (
                f"IDENT len={n:#x} "
                f"actual={len(p)}B"
            ),
            0,
        )

    if kind == "short":
        n = rng.randrange(2, 100)

        p = rb(
            rng.randrange(0, n)
        )

        return (
            pack(
                1,
                p,
                n,
            ),
            (
                f"IDENT len={n} "
                f"but only {len(p)}B sent"
            ),
            0,
        )

    if kind == "garbage":
        n = rng.randrange(1, 120)

        return (
            rb(n),
            f"{n}B random garbage",
            0,
        )

    if kind == "zerolen":
        return (
            pack(1),
            "IDENT len=0",
            0,
        )

    if kind == "long":
        n = rng.randrange(200, 1200)

        return (
            pack(
                1,
                rb(n),
            ),
            f"IDENT len={n}",
            0,
        )

    return (
        pack(
            1,
            b"bytewise",
        ),
        "valid IDENT sent byte-by-byte",
        1,
    )


def cmd_fuzz(
    ns: argparse.Namespace,
) -> int:
    seed = (
        ns.seed
        if ns.seed is not None
        else random.randrange(1 << 32)
    )

    rng = random.Random(seed)

    log(
        f"fuzzing {ns.host}:{ns.port}  "
        f"seed={seed}  "
        f"iterations={ns.iterations}"
    )

    failures = 0

    try:
        for i in range(ns.iterations):
            data, desc, chunk = gen_packet(
                rng
            )

            closing = rng.choice(
                [
                    "fin",
                    "rst",
                    "half",
                ]
            )

            c = Conn(
                ns.host,
                ns.port,
                ns.timeout,
                verbose=False,
            )

            try:
                c.connect()

                greeted = (
                    c.wait_frame(
                        CMD_IDENT,
                        ns.timeout,
                    )
                    is not None
                )

                c.send(
                    data,
                    chunks=chunk,
                    delay=0.03,
                )

                c.collect(ns.hold)

                if closing == "rst":
                    c.abort()

                elif closing == "half":
                    c.half_close()
                    c.collect(0.2)
                    c.close()

                else:
                    c.close()

            except OSError as e:
                greeted = False

                log(
                    f"#{i:03d} "
                    f"{desc}: socket error {e}"
                )

            time.sleep(ns.settle)

            status, detail = probe(
                ns.host,
                ns.port,
                ns.timeout,
            )

            mark = (
                green("ok  ")
                if status == "ok"
                else red(status)
            )

            log(
                f"#{i:03d} "
                f"{desc:<45} "
                f"close={closing:<4} "
                f"greeted={greeted!s:<5} "
                f"-> {mark}"
            )

            if status != "ok":
                failures += 1

                fname = (
                    f"fuzz-fail-{seed}-{i}.bin"
                )

                with open(fname, "wb") as fh:
                    fh.write(data)

                log(
                    red(
                        f"    board unhealthy: "
                        f"{detail}"
                    )
                )

                log(
                    red(
                        f"    offending packet "
                        f"saved to {fname}:"
                    )
                )

                print(
                    hexdump(
                        data[:256],
                        "        ",
                    )
                )

                if not ns.keep_going:
                    break

                log(
                    dim(
                        "    waiting for recovery ..."
                    )
                )

                time.sleep(5)

    except KeyboardInterrupt:
        pass

    print(
        green("FUZZ: no failures")
        if not failures
        else red(
            f"FUZZ: {failures} "
            f"failure(s), seed={seed}"
        )
    )

    return 1 if failures else 0


# --------------------------------------------------------------------------- #
# Sub-command: selftest
# --------------------------------------------------------------------------- #

@dataclass
class Ctx:
    host: str
    port: int
    max_clients: int
    timeout: float
    settle: float
    latency_limit: float
    hold: float
    verbose: bool
    hexdump: bool


class TestFail(Exception):
    pass


TESTS: List[
    Tuple[str, str, Callable[[Ctx], str]]
] = []


def test(
    name: str,
    desc: str,
):
    def deco(fn):
        TESTS.append(
            (
                name,
                desc,
                fn,
            )
        )

        return fn

    return deco


def open_conn(
    ctx: Ctx,
    name: str = "",
) -> Conn:
    c = Conn(
        ctx.host,
        ctx.port,
        ctx.timeout,
        name=name,
        verbose=ctx.verbose,
        full_dump=ctx.hexdump,
    )

    try:
        c.connect()

    except OSError as e:
        raise TestFail(
            f"connect failed: {e}"
        )

    if (
        c.wait_frame(
            CMD_IDENT,
            ctx.timeout,
        )
        is None
    ):
        c.close()

        raise TestFail(
            "no IDENT greeting from master"
        )

    return c


def assert_open(
    c: Conn,
    secs: float,
    what: str = "connection",
) -> List[Tuple[str, object]]:
    evs = c.collect(secs)

    if any(
        k == "eof"
        for k, _ in evs
    ):
        raise TestFail(
            f"{what} was closed by the master"
        )

    return evs


@test(
    "greeting",
    "master sends an IDENT request "
    "(7-byte header, len=0) right after connect",
)
def t_greeting(ctx: Ctx) -> str:
    c = open_conn(ctx)

    c.close()

    return "IDENT len=0 received"


@test(
    "ident",
    "valid IDENT 'hello' is accepted "
    "and the connection stays open",
)
def t_ident(ctx: Ctx) -> str:
    c = open_conn(ctx)

    c.send(
        pack(
            1,
            b"hello",
        )
    )

    assert_open(
        c,
        1.5,
    )

    c.close()

    return "still open after 1.5s"


@test(
    "idle",
    "an idle client is not kicked",
)
def t_idle(ctx: Ctx) -> str:
    c = open_conn(ctx)

    assert_open(
        c,
        ctx.hold,
    )

    c.close()

    return (
        f"open after "
        f"{ctx.hold:.0f}s of silence"
    )


@test(
    "split-bytewise",
    "packet sent 1 byte per write, "
    "50 ms apart",
)
def t_split_bytewise(ctx: Ctx) -> str:
    c = open_conn(ctx)

    c.send(
        pack(
            1,
            b"split",
        ),
        chunks=1,
        delay=0.05,
    )

    assert_open(
        c,
        1.5,
    )

    c.close()

    return "ok"


@test(
    "split-payload",
    "header, 300 ms pause, then payload "
    "(payload arrives late)",
)
def t_split_payload(ctx: Ctx) -> str:
    c = open_conn(ctx)

    pkt = pack(
        1,
        b"late-payload",
    )

    c.send(
        pkt[:HEADER_SIZE]
    )

    time.sleep(0.3)

    c.send(
        pkt[HEADER_SIZE:],
        desc="(payload part)",
    )

    assert_open(
        c,
        1.5,
    )

    c.close()

    return "ok"


@test(
    "half-close",
    "send IDENT then FIN immediately "
    "(what 'ncat < payload.bin' does)",
)
def t_half_close(ctx: Ctx) -> str:
    c = open_conn(ctx)

    c.send(
        pack(
            1,
            b"hello",
        )
    )

    c.half_close()

    c.collect(1.5)

    c.close()

    return (
        "board must still be healthy "
        "afterwards (checked by probe)"
    )


@test(
    "max-clients",
    "N clients accepted, client N+1 gets "
    "'Too many client' text, seats free again",
)
def t_max_clients(ctx: Ctx) -> str:
    conns = [
        open_conn(
            ctx,
            f"c{i}",
        )
        for i in range(
            ctx.max_clients
        )
    ]

    extra = Conn(
        ctx.host,
        ctx.port,
        ctx.timeout,
        name="extra",
        verbose=ctx.verbose,
    )

    try:
        extra.connect()

        res = classify_greeting(
            extra,
            ctx.timeout,
        )

        if res != "rejected":
            raise TestFail(
                f"extra client was '{res}', "
                "expected 'rejected'"
            )

    finally:
        extra.close()

        for c in conns:
            c.close()

    return (
        f"{ctx.max_clients} accepted, "
        f"#{ctx.max_clients + 1} rejected"
    )


@test(
    "seat-reuse",
    "connect/disconnect 2*N times in a row: "
    "seats are really freed",
)
def t_seat_reuse(ctx: Ctx) -> str:
    for i in range(
        ctx.max_clients * 2
    ):
        c = open_conn(
            ctx,
            f"r{i}",
        )

        c.send(
            pack(
                1,
                f"r{i}".encode(),
            )
        )

        time.sleep(0.2)

        c.close()

        time.sleep(
            ctx.settle
        )

    return (
        f"{ctx.max_clients * 2} "
        "sequential sessions ok"
    )


@test(
    "oversize-ident",
    "IDENT with a 200-byte name "
    "(master buffer is 64 bytes)",
)
def t_oversize_ident(ctx: Ctx) -> str:
    c = open_conn(ctx)

    c.send(
        pack(
            1,
            b"A" * 200,
        )
    )

    assert_open(
        c,
        2.0,
    )

    c.close()

    return "survived"


@test(
    "exact-buffer-ident",
    "IDENT with exactly 64 bytes "
    "(no room for a terminator)",
)
def t_exact_buffer(ctx: Ctx) -> str:
    c = open_conn(ctx)

    c.send(
        pack(
            1,
            b"B" * 64,
        )
    )

    assert_open(
        c,
        2.0,
    )

    c.close()

    return "survived"


@test(
    "invalid-cmds",
    "unknown command ids 8, 0x7f, 0xff "
    "with empty payload",
)
def t_invalid_cmds(ctx: Ctx) -> str:
    c = open_conn(ctx)

    for cid in (
        8,
        0x7F,
        0xFF,
    ):
        c.send(
            pack(cid)
        )

        time.sleep(0.2)

    assert_open(
        c,
        1.0,
    )

    c.close()

    return "survived"


@test(
    "invalid-signature",
    "packets with invalid uint16 protocol signatures",
)
def t_invalid_signature(ctx: Ctx) -> str:
    c = open_conn(ctx)

    for signature in (
        0x0000,
        0x0001,
        0x55AA,
        0xAA55,
        0xFFFF,
    ):
        packet = (
            struct.pack(
                "<H",
                signature,
            )
            + bytes([
                CLIENT_CMDS["IDENT"]
            ])
            + struct.pack(
                "<I",
                0,
            )
        )

        c.send(packet)

        time.sleep(0.2)

    assert_open(
        c,
        1.0,
    )

    c.close()

    return "survived"


@test(
    "garbage",
    "64 random bytes",
)
def t_garbage(ctx: Ctx) -> str:
    rng = random.Random(1234)

    c = open_conn(ctx)

    c.send(
        bytes(
            rng.randrange(256)
            for _ in range(64)
        )
    )

    assert_open(
        c,
        1.5,
    )

    c.close()

    return "survived"


@test(
    "huge-length",
    "IDENT whose length field is "
    "0xFFFFFFFF with no payload",
)
def t_huge_length(ctx: Ctx) -> str:
    c = open_conn(ctx)

    c.send(
        pack(
            1,
            b"",
            length=0xFFFFFFFF,
        )
    )

    assert_open(
        c,
        2.5,
    )

    c.close()

    return "survived"


@test(
    "no-blocking",
    "a client that lies about its length "
    "must not delay other clients",
)
def t_no_blocking(ctx: Ctx) -> str:
    a = open_conn(
        ctx,
        "liar",
    )

    a.send(
        pack(
            1,
            b"abc",
            length=1000,
        )
    )

    t0 = time.monotonic()

    b = Conn(
        ctx.host,
        ctx.port,
        ctx.timeout,
        name="victim",
        verbose=ctx.verbose,
    )

    try:
        b.connect()

        if (
            b.wait_frame(
                CMD_IDENT,
                ctx.timeout + 5,
            )
            is None
        ):
            raise TestFail(
                "second client never got "
                "its IDENT"
            )

        dt = (
            time.monotonic()
            - t0
        )

    finally:
        b.close()
        a.close()

    if dt > ctx.latency_limit:
        raise TestFail(
            f"second client waited "
            f"{dt:.2f}s for IDENT "
            f"(limit {ctx.latency_limit}s) - "
            "the master may be blocking while "
            "waiting for the missing payload"
        )

    return (
        f"second client greeted "
        f"after {dt:.2f}s"
    )


@test(
    "rapid-connect",
    "20 connect/close cycles, "
    "100 ms apart, no data",
)
def t_rapid(ctx: Ctx) -> str:
    fails = 0

    for _ in range(20):
        c = Conn(
            ctx.host,
            ctx.port,
            2.0,
            verbose=False,
        )

        try:
            c.connect()
            c.close()

        except OSError:
            fails += 1

        time.sleep(0.1)

    return (
        f"{20 - fails}/20 "
        "connects succeeded"
    )


def cmd_selftest(
    ns: argparse.Namespace,
) -> int:
    if ns.list:
        for name, desc, _ in TESTS:
            print(
                f"{name:<20} {desc}"
            )

        return 0

    ctx = Ctx(
        ns.host,
        ns.port,
        ns.max_clients,
        ns.timeout,
        ns.settle,
        ns.latency_limit,
        ns.idle_hold,
        ns.verbose,
        ns.hexdump,
    )

    wanted = (
        set(ns.only.split(","))
        if ns.only
        else None
    )

    selected = [
        t
        for t in TESTS
        if not wanted
        or t[0] in wanted
    ]

    skipped = (
        set(ns.skip.split(","))
        if ns.skip
        else set()
    )

    selected = [
        t
        for t in selected
        if t[0] not in skipped
    ]

    status, detail = probe(
        ns.host,
        ns.port,
        ns.timeout,
    )

    if status != "ok":
        print(
            red(
                "master not reachable/healthy "
                f"before starting: "
                f"{status} - {detail}"
            )
        )

        return 2

    print(
        f"{bold('superV selftest')} "
        f"against {ns.host}:{ns.port}  "
        f"(max_clients={ns.max_clients})\n"
    )

    results: List[
        Tuple[str, bool, str, float]
    ] = []

    for name, desc, fn in selected:
        t0 = time.monotonic()

        ok = True
        detail = ""

        try:
            detail = fn(ctx)

        except TestFail as e:
            ok = False
            detail = str(e)

        except OSError as e:
            ok = False
            detail = (
                f"socket error: {e}"
            )

        time.sleep(
            ns.settle
        )

        # Health probe.
        deadline = (
            time.monotonic()
            + ns.recover
        )

        pstat, pdet = probe(
            ns.host,
            ns.port,
            ns.timeout,
        )

        while (
            pstat != "ok"
            and time.monotonic() < deadline
        ):
            time.sleep(1.0)

            pstat, pdet = probe(
                ns.host,
                ns.port,
                ns.timeout,
            )

        if pstat != "ok":
            ok = False

            detail += (
                f"  | board unhealthy "
                f"afterwards: "
                f"{pstat} - {pdet}"
            )

        dt = (
            time.monotonic()
            - t0
        )

        results.append(
            (
                name,
                ok,
                detail,
                dt,
            )
        )

        print(
            f"{green('PASS') if ok else red('FAIL')}  "
            f"{name:<20} "
            f"{dt:5.1f}s  "
            f"{detail}"
        )

        if (
            not ok
            and pstat != "ok"
            and not ns.keep_going
        ):
            print(
                red(
                    "\nboard did not recover - "
                    "aborting "
                    "(use --keep-going "
                    "to continue)"
                )
            )

            break

        if (
            not ok
            and not ns.keep_going
            and ns.fail_fast
        ):
            break

    failed = [
        r
        for r in results
        if not r[1]
    ]

    print(
        f"\n"
        f"{len(results) - len(failed)}"
        f"/{len(results)} passed"
    )

    return 1 if failed else 0


# --------------------------------------------------------------------------- #
# Sub-command: mock
# --------------------------------------------------------------------------- #

def cmd_mock(
    ns: argparse.Namespace,
) -> int:
    srv = socket.socket()

    srv.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_REUSEADDR,
        1,
    )

    srv.bind(
        (
            ns.bind,
            ns.port,
        )
    )

    srv.listen(8)

    lock = threading.Lock()

    seats = {
        "n": 0
    }

    log(
        f"mock master on "
        f"{ns.bind}:{ns.port}  "
        f"max_clients={ns.max_clients}  "
        f"(blocking="
        f"{'yes' if ns.block_ms else 'no'})"
    )

    stall = {
        "until": 0.0
    }

    def serve(
        conn: socket.socket,
        addr,
        idx: int,
    ) -> None:
        dec = Decoder(
            CLIENT_NAMES
        )

        try:
            wait = (
                stall["until"]
                - time.monotonic()
            )

            if wait > 0:
                # Emulate a single-threaded master that is blocked in its
                # receive loop.
                time.sleep(wait)

            # Master sends IDENT request:
            #
            # AA FA 01 00 00 00 00
            conn.sendall(
                pack(CMD_IDENT)
            )

            while True:
                data = conn.recv(4096)

                if not data:
                    break

                log(
                    f"mock: client{idx} "
                    f"sent {len(data)}B  "
                    f"{short_hex(data)}"
                )

                for kind, val in dec.feed(data):
                    if kind == "frame":
                        log(
                            f"mock: client{idx} "
                            f"-> "
                            f"{describe_frame(val, CLIENT_NAMES)}"
                        )

                    else:
                        log(
                            f"mock: client{idx} "
                            f"unparsable "
                            f"{bytes(val)!r}"
                        )

                if (
                    ns.block_ms
                    and dec.buf
                ):
                    # Emulate readBytes() waiting for a payload that never
                    # completes.
                    stall["until"] = (
                        time.monotonic()
                        + ns.block_ms / 1000
                    )

        except OSError:
            pass

        finally:
            conn.close()

            with lock:
                seats["n"] -= 1

                log(
                    f"mock: client{idx} "
                    f"gone "
                    f"({seats['n']}/"
                    f"{ns.max_clients})"
                )

    idx = 0

    try:
        while True:
            conn, addr = srv.accept()

            with lock:
                if (
                    seats["n"]
                    >= ns.max_clients
                ):
                    conn.sendall(
                        b"ERROR : Too many client connected.\r\n"
                    )

                    conn.close()

                    log(
                        "mock: rejected "
                        "a client (full)"
                    )

                    continue

                seats["n"] += 1
                idx += 1

                log(
                    f"mock: client{idx} "
                    f"connected "
                    f"({seats['n']}/"
                    f"{ns.max_clients})"
                )

            threading.Thread(
                target=serve,
                args=(
                    conn,
                    addr,
                    idx,
                ),
                daemon=True,
            ).start()

    except KeyboardInterrupt:
        return 0

    finally:
        srv.close()


# --------------------------------------------------------------------------- #
# CLI definition
# --------------------------------------------------------------------------- #

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="superv.py",
        description=__doc__,
        formatter_class=(
            argparse.RawDescriptionHelpFormatter
        ),
    )

    sub = ap.add_subparsers(
        dest="sub",
        required=True,
        metavar="COMMAND",
    )

    common = argparse.ArgumentParser(
        add_help=False
    )

    add_common(common)

    # --------------------------------------------------------------------- #
    # shell
    # --------------------------------------------------------------------- #

    p = sub.add_parser(
        "shell",
        parents=[common],
        help="interactive session / mini scripting",
    )

    p.add_argument(
        "-e",
        "--exec",
        help=(
            "run ';'-separated shell commands "
            "then exit, e.g. "
            "-e 'expect ident; ident bob; wait 2'"
        ),
    )

    p.add_argument(
        "--script",
        help="run commands from a file then exit",
    )

    p.add_argument(
        "-i",
        "--interactive",
        action="store_true",
        help=(
            "stay in the shell after "
            "-e/--script"
        ),
    )

    p.set_defaults(
        fn=cmd_shell
    )

    # --------------------------------------------------------------------- #
    # send
    # --------------------------------------------------------------------- #

    p = sub.add_parser(
        "send",
        parents=[common],
        help=(
            "one-shot: send one packet "
            "and show the answer"
        ),
    )

    p.add_argument(
        "cmd",
        nargs="?",
        default="ident",
        help=(
            "command name or number "
            "(default ident)"
        ),
    )

    add_payload_args(p)

    p.add_argument(
        "--raw",
        help=(
            "send these raw bytes "
            "instead of a framed packet"
        ),
    )

    p.add_argument(
        "--hold",
        type=float,
        default=2.0,
        help=(
            "keep the connection open this "
            "long after sending "
            "(default 2)"
        ),
    )

    p.add_argument(
        "--chunk",
        type=int,
        default=0,
        help=(
            "split the packet into "
            "N-byte writes"
        ),
    )

    p.add_argument(
        "--chunk-delay",
        type=float,
        default=50,
        help=(
            "ms between chunks "
            "(default 50)"
        ),
    )

    p.add_argument(
        "--repeat",
        type=int,
        default=1,
    )

    p.add_argument(
        "--interval",
        type=float,
        default=0.0,
        help=(
            "seconds between repeats"
        ),
    )

    p.add_argument(
        "--half-close",
        action="store_true",
        help=(
            "send FIN right after the data"
        ),
    )

    p.add_argument(
        "--rst",
        action="store_true",
        help=(
            "end with RST instead of FIN"
        ),
    )

    p.add_argument(
        "--no-greeting",
        action="store_true",
        help=(
            "don't wait for the IDENT "
            "request first"
        ),
    )

    p.set_defaults(
        fn=cmd_send
    )

    # --------------------------------------------------------------------- #
    # client
    # --------------------------------------------------------------------- #

    p = sub.add_parser(
        "client",
        parents=[common],
        help=(
            "simulate a well-behaved client"
        ),
    )

    p.add_argument(
        "--ident",
        default="py-client",
        help="name to answer IDENT with",
    )

    p.add_argument(
        "--status-every",
        type=float,
        default=0,
        help=(
            "send STATUS every N seconds"
        ),
    )

    p.add_argument(
        "--reconnect",
        action="store_true",
        help=(
            "reconnect forever when dropped"
        ),
    )

    p.set_defaults(
        fn=cmd_client
    )

    # --------------------------------------------------------------------- #
    # stress
    # --------------------------------------------------------------------- #

    p = sub.add_parser(
        "stress",
        parents=[common],
        help="open many clients at once",
    )

    p.add_argument(
        "-n",
        "--clients",
        type=int,
        default=DEFAULT_MAX_CLIENTS + 2,
    )

    p.add_argument(
        "--max-clients",
        type=int,
        default=DEFAULT_MAX_CLIENTS,
    )

    p.add_argument(
        "--interval",
        type=float,
        default=0.2,
        help=(
            "delay between connects "
            "(default 0.2)"
        ),
    )

    p.add_argument(
        "--hold",
        type=float,
        default=3.0,
    )

    p.add_argument(
        "--settle",
        type=float,
        default=2.0,
    )

    p.set_defaults(
        fn=cmd_stress
    )

    # --------------------------------------------------------------------- #
    # fuzz
    # --------------------------------------------------------------------- #

    p = sub.add_parser(
        "fuzz",
        parents=[common],
        help=(
            "random malformed packets "
            "+ health probe"
        ),
    )

    p.add_argument(
        "-n",
        "--iterations",
        type=int,
        default=50,
    )

    p.add_argument(
        "--seed",
        type=int,
        help="reproduce a previous run",
    )

    p.add_argument(
        "--hold",
        type=float,
        default=0.4,
        help=(
            "seconds to wait after sending "
            "(default 0.4)"
        ),
    )

    p.add_argument(
        "--settle",
        type=float,
        default=0.8,
        help=(
            "wait before probing "
            "(default 0.8)"
        ),
    )

    p.add_argument(
        "--keep-going",
        action="store_true",
    )

    p.set_defaults(
        fn=cmd_fuzz
    )

    # --------------------------------------------------------------------- #
    # selftest
    # --------------------------------------------------------------------- #

    p = sub.add_parser(
        "selftest",
        parents=[common],
        help="scripted regression suite",
    )

    p.add_argument(
        "--list",
        action="store_true",
        help="list tests and exit",
    )

    p.add_argument(
        "--only",
        help="comma-separated test names",
    )

    p.add_argument(
        "--skip",
        help="comma-separated test names",
    )

    p.add_argument(
        "--max-clients",
        type=int,
        default=DEFAULT_MAX_CLIENTS,
    )

    p.add_argument(
        "--settle",
        type=float,
        default=1.0,
        help=(
            "pause after disconnects "
            "(default 1.0)"
        ),
    )

    p.add_argument(
        "--recover",
        type=float,
        default=10.0,
        help=(
            "max seconds to wait for "
            "the board to recover"
        ),
    )

    p.add_argument(
        "--latency-limit",
        type=float,
        default=2.5,
        help=(
            "max IDENT latency in "
            "'no-blocking' "
            "(default 2.5)"
        ),
    )

    p.add_argument(
        "--idle-hold",
        type=float,
        default=5.0,
    )

    p.add_argument(
        "--keep-going",
        action="store_true",
    )

    p.add_argument(
        "--fail-fast",
        action="store_true",
    )

    p.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help=(
            "show every TX/RX line"
        ),
    )

    p.set_defaults(
        fn=cmd_selftest
    )

    # --------------------------------------------------------------------- #
    # pack
    # --------------------------------------------------------------------- #

    p = sub.add_parser(
        "pack",
        help=(
            "build a packet "
            "(and optionally write it to a file)"
        ),
    )

    p.add_argument(
        "cmd",
        help="command name or number",
    )

    add_payload_args(p)

    p.add_argument(
        "-o",
        "--output",
        help="write the packet to this file",
    )

    p.set_defaults(
        fn=cmd_pack
    )

    # --------------------------------------------------------------------- #
    # decode
    # --------------------------------------------------------------------- #

    p = sub.add_parser(
        "decode",
        help=(
            "decode a binary capture "
            "(use - for stdin)"
        ),
    )

    p.add_argument(
        "file"
    )

    p.add_argument(
        "--as",
        dest="direction",
        choices=[
            "client",
            "master",
        ],
        default="master",
        help=(
            "who sent it: 'master' "
            "(e.g. result.bin) or "
            "'client' (e.g. payload.bin)"
        ),
    )

    p.set_defaults(
        fn=cmd_decode
    )

    # --------------------------------------------------------------------- #
    # mock
    # --------------------------------------------------------------------- #

    p = sub.add_parser(
        "mock",
        help=(
            "fake master for offline "
            "testing of this tool"
        ),
    )

    p.add_argument(
        "--bind",
        default="127.0.0.1",
    )

    p.add_argument(
        "-p",
        "--port",
        type=int,
        default=9090,
    )

    p.add_argument(
        "--max-clients",
        type=int,
        default=DEFAULT_MAX_CLIENTS,
    )

    p.add_argument(
        "--block-ms",
        type=int,
        default=0,
        help=(
            "emulate the readBytes() stall "
            "on short payloads"
        ),
    )

    p.set_defaults(
        fn=cmd_mock
    )

    return ap


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

def main(
    argv: Optional[List[str]] = None,
) -> int:
    ns = build_parser().parse_args(argv)

    if getattr(
        ns,
        "no_color",
        False,
    ):
        Style.on = False

    try:
        return ns.fn(ns)

    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())