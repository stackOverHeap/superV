#!/usr/bin/env python3
"""superV interactive shell.

Wire format (little endian):

    offset  size
    0       2     uint16_t signature (0xfaaa)
    2       1     Command
    3       4     uint32_t followingLength
    7       N     payload

The master's ALIVE heartbeat is answered automatically by a background
reader thread, so the connection is never dropped for a heartbeat timeout
while you work in the shell.
"""
from __future__ import annotations

import argparse
import cmd as cmdlib
import codecs
import os
import queue
import shlex
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

DEFAULT_HOST = "192.168.10.1"
DEFAULT_PORT = 90

PROTOCOL_SIGNATURE = 0xFAAA
HEADER = struct.Struct("<HBI")  # signature, command, followingLength
HEADER_SIZE = HEADER.size       # 7
SANE_LEN = 4096                 # tool-side safety limit, not a protocol limit

# Same enum in both directions (Protocol::Command)
CMDS: Dict[str, int] = {
    "INVALID": 0,
    "IDENT": 1,
    "STATUS": 2,
    "ALIVE": 3,
    "CUSTOM": 4,
    "START": 5,
    "STOP": 6,
    "PAUSE": 7,
    "RESET": 8,
}
CMD_NAMES = {v: k for k, v in CMDS.items()}

STATES = {"RUNNING": 0, "PAUSED": 1, "WAITING": 2}


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
        print(f"{dim(f'{time.monotonic() - _T0:8.3f}')} {msg}", flush=True)


def short_hex(data: bytes, limit: int = 24) -> str:
    s = data[:limit].hex(" ")
    if len(data) > limit:
        s += f" …(+{len(data) - limit})"
    return s


def hexdump(data: bytes, indent: str = "", width: int = 16) -> str:
    lines = []
    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        hx = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{indent}{off:04x}  {hx:<{width * 3 - 1}}  |{asc}|")
    return "\n".join(lines)


# --------------------------------------------------------------------------- #
# Packets
# --------------------------------------------------------------------------- #

def pack(cmd: int, payload: bytes = b"", length: Optional[int] = None) -> bytes:
    n = len(payload) if length is None else length
    return HEADER.pack(PROTOCOL_SIGNATURE, cmd & 0xFF, n & 0xFFFFFFFF) + payload


@dataclass
class Frame:
    cmd: int
    length: int
    payload: bytes


def describe_frame(f: Frame) -> str:
    s = f"{CMD_NAMES.get(f.cmd, '?')}({f.cmd}) len={f.length}"
    if f.payload:
        txt = f.payload.rstrip(b"\0")
        if txt and all(32 <= b < 127 for b in txt):
            s += f' "{txt.decode()}"'
        else:
            s += " " + short_hex(f.payload, 16)
    return s


def describe_bytes(data: bytes) -> str:
    """Best-effort description of bytes about to be sent."""
    if len(data) < HEADER_SIZE:
        return f"incomplete header ({len(data)}/{HEADER_SIZE} bytes)"

    sig, cmd, length = HEADER.unpack_from(data)
    if sig != PROTOCOL_SIGNATURE:
        return f"bad-signature=0x{sig:04x} expected=0x{PROTOCOL_SIGNATURE:04x}"

    s = describe_frame(Frame(cmd, length, data[HEADER_SIZE:HEADER_SIZE + length]))
    actual = len(data) - HEADER_SIZE
    if actual != length:
        s += yellow(f"  [header says {length}, actual payload {actual}]")
    return s


class Decoder:
    """Stream decoder. Data that doesn't start with the signature is 'raw'
    (e.g. a plain-text error line from the master)."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes) -> List[Tuple[str, object]]:
        self.buf += data
        out: List[Tuple[str, object]] = []

        while len(self.buf) >= 2:
            if struct.unpack_from("<H", self.buf)[0] != PROTOCOL_SIGNATURE:
                nl = self.buf.find(b"\n")
                n = nl + 1 if nl >= 0 else len(self.buf)
                out.append(("raw", bytes(self.buf[:n])))
                del self.buf[:n]
                continue

            if len(self.buf) < HEADER_SIZE:
                break

            _, cmd, length = HEADER.unpack_from(self.buf)

            if cmd not in CMD_NAMES:
                out.append(("raw", bytes(self.buf[:HEADER_SIZE])))
                del self.buf[:HEADER_SIZE]
                continue

            if length > SANE_LEN:
                out.append(("raw", bytes(self.buf)))
                self.buf.clear()
                break

            size = HEADER_SIZE + length
            if len(self.buf) < size:
                break

            payload = bytes(self.buf[HEADER_SIZE:size])
            del self.buf[:size]
            out.append(("frame", Frame(cmd, length, payload)))

        return out

    def flush(self) -> List[Tuple[str, object]]:
        if not self.buf:
            return []
        rest = bytes(self.buf)
        self.buf.clear()
        return [("partial", rest)]


# --------------------------------------------------------------------------- #
# Connection: background reader + event queue + automatic heartbeat reply
# --------------------------------------------------------------------------- #

class Conn:
    def __init__(self, host: str, port: int, timeout: float = 5.0,
                 full_dump: bool = False):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.full_dump = full_dump

        self.sock: Optional[socket.socket] = None
        self.dec = Decoder()
        self.q: "queue.Queue[Tuple[str, object]]" = queue.Queue()

        self.alive = False
        self.error: Optional[BaseException] = None
        self._eof_seen = False
        self._closing = False

        # Reader thread (ALIVE replies) and shell thread both send:
        # serialize so packets never interleave on the wire.
        self._tx_lock = threading.Lock()

        self.chunk = 0
        self.chunk_delay = 0.0

    # -- connect / read ----------------------------------------------------

    def connect(self) -> "Conn":
        t0 = time.monotonic()
        self.sock = socket.create_connection((self.host, self.port),
                                             timeout=self.timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(None)
        self.alive = True

        log(f"{bold('●')} connected to {self.host}:{self.port} "
            f"({(time.monotonic() - t0) * 1000:.0f} ms)")

        threading.Thread(target=self._read_loop, daemon=True).start()
        return self

    def _read_loop(self) -> None:
        assert self.sock
        try:
            while True:
                data = self.sock.recv(4096)
                if not data:
                    break

                log(f"{green('←')} RX {len(data):>3}B  {short_hex(data)}")
                if self.full_dump:
                    log("\n" + hexdump(data, "           "))

                for kind, val in self.dec.feed(data):
                    self._handle(kind, val)

        except OSError as e:
            if not self._closing:
                self.error = e

        finally:
            for kind, val in self.dec.flush():
                self._handle(kind, val)

            self.alive = False
            if not self._closing:
                why = f" ({self.error})" if self.error else ""
                log(f"{red('✕')} connection closed by peer{why}")
            self.q.put(("eof", self.error))

    def _handle(self, kind: str, val: object) -> None:
        if kind == "frame":
            f: Frame = val  # type: ignore[assignment]
            log(f"  {green('↳')} {describe_frame(f)}")

            # Heartbeat: answer every ALIVE with an ALIVE, right away.
            if f.cmd == CMDS["ALIVE"]:
                try:
                    self.send(pack(CMDS["ALIVE"]), desc="auto-reply")
                except OSError as e:
                    log(red(f"failed to answer ALIVE: {e}"))
                return  # don't queue heartbeats, they're handled

        elif kind == "raw":
            log(f"  {yellow('↳ text/raw')} {bytes(val)!r}")  # type: ignore[arg-type]

        elif kind == "partial":
            log(f"  {yellow('↳ incomplete frame at EOF')} "
                f"{short_hex(bytes(val))}")  # type: ignore[arg-type]

        self.q.put((kind, val))

    # -- events ------------------------------------------------------------

    def next_event(self, timeout: float) -> Optional[Tuple[str, object]]:
        if self._eof_seen and self.q.empty():
            return ("eof", self.error)
        try:
            ev = self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None
        if ev[0] == "eof":
            self._eof_seen = True
        return ev

    def wait_frame(self, cmd: Optional[int], timeout: float) -> Optional[Frame]:
        deadline = time.monotonic() + timeout
        while True:
            ev = self.next_event(deadline - time.monotonic())
            if ev is None or ev[0] == "eof":
                return None
            if ev[0] == "frame" and (cmd is None or ev[1].cmd == cmd):  # type: ignore[union-attr]
                return ev[1]  # type: ignore[return-value]
            if time.monotonic() >= deadline:
                return None

    def collect(self, duration: float) -> List[Tuple[str, object]]:
        out: List[Tuple[str, object]] = []
        deadline = time.monotonic() + duration
        while True:
            ev = self.next_event(deadline - time.monotonic())
            if ev is None:
                return out
            out.append(ev)
            if ev[0] == "eof":
                return out

    # -- sending / closing -------------------------------------------------

    def send(self, data: bytes, desc: Optional[str] = None) -> None:
        assert self.sock

        n = self.chunk
        pieces = ([data[i:i + n] for i in range(0, len(data), n)]
                  if n > 0 else [data])
        info = desc if desc is not None else describe_bytes(data)

        with self._tx_lock:
            for i, piece in enumerate(pieces):
                self.sock.sendall(piece)

                part = f" (chunk {i + 1}/{len(pieces)})" if len(pieces) > 1 else ""
                log(f"{cyan('→')} TX {len(piece):>3}B  {short_hex(piece)}{part}")
                if i == 0 and info:
                    log(f"  {cyan('↳')} {info}")
                if self.full_dump:
                    log("\n" + hexdump(piece, "           "))

                if self.chunk_delay and i < len(pieces) - 1:
                    time.sleep(self.chunk_delay)

    def half_close(self) -> None:
        if self.sock:
            log(f"{yellow('⇥')} half-close (FIN, still reading)")
            self.sock.shutdown(socket.SHUT_WR)

    def abort(self) -> None:
        """Close with RST instead of FIN."""
        if self.sock:
            self._closing = True
            log(f"{yellow('✕')} abort (RST)")
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                 struct.pack("ii", 1, 0))
            try:
                self.sock.shutdown(socket.SHUT_RD)
            except OSError:
                pass
            self.sock.close()
            self.alive = False
            time.sleep(0.05)

    def close(self) -> None:
        if self.sock and not self._closing:
            self._closing = True
            log(dim("○ closing"))
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()
            self.alive = False


# --------------------------------------------------------------------------- #
# Parsing helpers
# --------------------------------------------------------------------------- #

def parse_cmd(token: str) -> int:
    t = token.strip().upper()
    return CMDS[t] if t in CMDS else int(token, 0) & 0xFF


def parse_hex(s: str) -> bytes:
    cleaned = (s.replace("0x", " ").replace("0X", " ")
                .replace(",", " ").replace(":", " "))
    return bytes.fromhex("".join(cleaned.split()))


# --------------------------------------------------------------------------- #
# Shell
# --------------------------------------------------------------------------- #

class Shell(cmdlib.Cmd):
    intro = (f"{bold('superV shell')} - type 'help' for commands, 'quit' to leave. "
             "Connection stays open; RX is printed live; ALIVE is answered automatically.")
    prompt = "superv> "

    def __init__(self, ns: argparse.Namespace):
        super().__init__()
        self.ns = ns
        self.conn: Optional[Conn] = None
        self.failed = False

    # -- helpers -----------------------------------------------------------

    def _need(self) -> Optional[Conn]:
        if self.conn is None or not self.conn.alive:
            print(red("not connected - use 'reconnect'"))
            return None
        return self.conn

    def _tx(self, data: bytes, desc: Optional[str] = None) -> None:
        c = self._need()
        if c:
            try:
                c.send(data, desc)
            except OSError as e:
                print(red(f"send failed: {e}"))

    def connect(self) -> bool:
        try:
            self.conn = Conn(self.ns.host, self.ns.port, self.ns.timeout,
                             self.ns.hexdump).connect()
            return True
        except OSError as e:
            print(red(f"connect failed: {e}"))
            self.failed = True
            return False

    def onecmd(self, line: str) -> bool:
        try:
            return super().onecmd(line)
        except (ValueError, OSError, IndexError, KeyError) as e:
            print(red(f"error: {e}"))
            return False

    def emptyline(self) -> bool:
        return False

    # -- commands ----------------------------------------------------------

    def do_ident(self, arg: str) -> None:
        """ident NAME - send IDENT with NAME (no trailing NUL)."""
        self._tx(pack(CMDS["IDENT"], arg.encode()))

    def do_status(self, arg: str) -> None:
        """status [RUNNING|PAUSED|WAITING|N] - send STATUS with a 1-byte state."""
        a = arg.strip().upper() or "RUNNING"
        state = STATES[a] if a in STATES else int(a, 0)
        self._tx(pack(CMDS["STATUS"], bytes([state & 0xFF])))

    def do_alive(self, arg: str) -> None:
        """alive - manually send an ALIVE (replies to the master's are automatic)."""
        self._tx(pack(CMDS["ALIVE"]))

    def do_custom(self, arg: str) -> None:
        """custom TEXT - send CUSTOM with TEXT."""
        self._tx(pack(CMDS["CUSTOM"], arg.encode()))

    def do_send(self, arg: str) -> None:
        """send CMD [text...|hex:DEADBEEF] [len=N]

        send ident hello
        send status hex:00
        send 0xff hex:00 01 02
        send ident AAAA len=1000
        """
        parts = shlex.split(arg)
        if not parts:
            raise ValueError("send requires a command")

        length = None
        rest = []
        for p in parts[1:]:
            if p.startswith("len="):
                length = int(p[4:], 0)
            else:
                rest.append(p)

        body = " ".join(rest)
        payload = parse_hex(body[4:]) if body.startswith("hex:") else body.encode()
        self._tx(pack(parse_cmd(parts[0]), payload, length))

    def do_raw(self, arg: str) -> None:
        """raw HEX - send raw bytes without any header."""
        self._tx(parse_hex(arg), "")

    def do_rawtext(self, arg: str) -> None:
        r"""rawtext TEXT - send raw text; escapes are interpreted (\r\n ...)."""
        self._tx(codecs.decode(arg, "unicode_escape").encode("latin-1"), "")

    def do_chunk(self, arg: str) -> None:
        """chunk N [DELAY_MS] - split following sends (chunk 0 disables)."""
        c = self._need()
        if c:
            p = arg.split()
            c.chunk = int(p[0]) if p else 0
            c.chunk_delay = float(p[1]) / 1000 if len(p) > 1 else 0.0
            print(f"chunk={c.chunk} delay={c.chunk_delay * 1000:.0f}ms")

    def do_wait(self, arg: str) -> None:
        """wait SECONDS - sleep."""
        time.sleep(float(arg or "1"))

    def do_expect(self, arg: str) -> None:
        """expect NAME|eof [SECS] - wait for a master frame (or EOF)."""
        p = arg.split()
        c = self._need()
        if not c or not p:
            self.failed = True
            return

        secs = float(p[1]) if len(p) > 1 else 5.0
        if p[0].lower() == "eof":
            ok = any(k == "eof" for k, _ in c.collect(secs))
        else:
            ok = c.wait_frame(parse_cmd(p[0]), secs) is not None

        print(green("expect OK") if ok else red("expect FAILED"))
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
        up = self.conn and self.conn.alive
        print(f"{'connected' if up else 'disconnected'}  "
              f"{self.ns.host}:{self.ns.port}")

    def do_hexdump(self, arg: str) -> None:
        """hexdump on|off - toggle full hexdumps."""
        if self.conn:
            self.conn.full_dump = arg.strip().lower() in ("on", "1", "true", "")
            print("hexdump " + ("on" if self.conn.full_dump else "off"))

    def do_source(self, arg: str) -> None:
        """source FILE - run commands from a file."""
        with open(arg.strip()) as fh:
            for line in fh:
                line = line.split("#", 1)[0].strip()
                if line:
                    print(dim(f"> {line}"))
                    if self.onecmd(line):
                        return

    def do_quit(self, arg: str) -> bool:
        """quit - leave."""
        if self.conn:
            self.conn.close()
        return True

    do_exit = do_quit
    do_EOF = do_quit


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(prog="superv.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-H", "--host", default=DEFAULT_HOST,
                    help=f"master address (default {DEFAULT_HOST})")
    ap.add_argument("-p", "--port", type=int, default=DEFAULT_PORT,
                    help=f"master port (default {DEFAULT_PORT})")
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="connect timeout in s (default 5)")
    ap.add_argument("-x", "--hexdump", action="store_true",
                    help="full hexdump of every TX/RX chunk")
    ap.add_argument("--no-color", action="store_true")
    ap.add_argument("-e", "--exec",
                    help="run ';'-separated commands then exit, "
                         "e.g. -e 'expect ident; ident bob; wait 2'")
    ap.add_argument("--script", help="run commands from a file then exit")
    ap.add_argument("-i", "--interactive", action="store_true",
                    help="stay in the shell after -e/--script")
    ns = ap.parse_args(argv)

    if ns.no_color:
        Style.on = False

    sh = Shell(ns)
    if not sh.connect():
        return 2

    try:
        if ns.exec:
            for part in ns.exec.split(";"):
                part = part.strip()
                if part:
                    print(dim(f"> {part}"))
                    if sh.onecmd(part):
                        break

        if ns.script:
            sh.onecmd(f"source {ns.script}")

        if (ns.exec or ns.script) and not ns.interactive:
            sh.onecmd("quit")
        else:
            sh.cmdloop()

    except KeyboardInterrupt:
        sh.onecmd("quit")
        return 130

    return 1 if sh.failed else 0


if __name__ == "__main__":
    sys.exit(main())