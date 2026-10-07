#include <interface.hpp>

#include <superv/logging.hpp>
#include <superv/master.hpp>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint8_t TFT_CS_PIN = 10;
constexpr uint8_t TFT_DC_PIN = 8;
constexpr uint8_t TFT_RESET_PIN = 9;
constexpr uint8_t TFT_BACKLIGHT_PIN = 7;
constexpr uint8_t TFT_SD_CS_PIN = 4;

constexpr uint8_t ENCODER_A_PIN = 2;
constexpr uint8_t ENCODER_B_PIN = 3;
constexpr uint8_t BUTTON_PINS[4] = {A0, A1, A2, A3};
constexpr unsigned long BUTTON_DEBOUNCE_MS = 30;

struct CommandOption {
	const char* label;
	Protocol::Command command;
};

constexpr CommandOption commandText[] = {
		{"IDENT", Protocol::Command::IDENT},
		{"STATUS", Protocol::Command::STATUS},
		{"ALIVE", Protocol::Command::ALIVE},
		{"START", Protocol::Command::START},
		{"STOP", Protocol::Command::STOP},
		{"PAUSE", Protocol::Command::PAUSE},
		{"RESET", Protocol::Command::RESET},
};
constexpr uint8_t COMMAND_COUNT = sizeof(commandText) / sizeof(commandText[0]);

constexpr int8_t ENCODER_TRANSITIONS[16] = {
		0, -1, 1, 0,
		1, 0, 0, -1,
		-1, 0, 0, 1,
		0, 1, -1, 0,
};

volatile uint8_t encoderState = 0;
volatile int32_t encoderTransitionsPending = 0;

void handleEncoderChange() {
	const uint8_t state =
			(static_cast<uint8_t>(digitalRead(ENCODER_A_PIN)) << 1) |
			static_cast<uint8_t>(digitalRead(ENCODER_B_PIN));
	const uint8_t transition = (encoderState << 2) | state;
	encoderTransitionsPending += ENCODER_TRANSITIONS[transition];
	encoderState = state;
}
}  // namespace

Interface::Interface()
		: _display(TFT_CS_PIN, TFT_DC_PIN, TFT_RESET_PIN) {}

void Interface::begin() {
	pinMode(ENCODER_A_PIN, INPUT_PULLUP);
	pinMode(ENCODER_B_PIN, INPUT_PULLUP);
	for (uint8_t index = 0; index < 4; ++index) {
		pinMode(BUTTON_PINS[index], INPUT_PULLUP);
		_buttons[index].changedAtMs = millis();
	}

	pinMode(TFT_BACKLIGHT_PIN, OUTPUT);
	digitalWrite(TFT_BACKLIGHT_PIN, HIGH);
	pinMode(TFT_SD_CS_PIN, OUTPUT);
	digitalWrite(TFT_SD_CS_PIN, HIGH);

	encoderState =
			(static_cast<uint8_t>(digitalRead(ENCODER_A_PIN)) << 1) |
			static_cast<uint8_t>(digitalRead(ENCODER_B_PIN));
	attachInterrupt(
			digitalPinToInterrupt(ENCODER_A_PIN),
			handleEncoderChange,
			CHANGE);
	attachInterrupt(
			digitalPinToInterrupt(ENCODER_B_PIN),
			handleEncoderChange,
			CHANGE);

	_display.initR(INITR_BLACKTAB);
	_display.setRotation(0);
	_display.fillScreen(ST77XX_BLACK);
	draw();
	LOGI("Supervisor display ready");
}

void Interface::loop(Master& master) {
	updatePeerNames(master);
	readEncoder();
	readButtons(master);

	if (_feedback != nullptr &&
			static_cast<long>(millis() - _feedbackUntilMs) >= 0) {
		_feedback = nullptr;
		draw();
	}
}

void Interface::updatePeerNames(Master& master) {
	bool changed = false;
	for (uint8_t index = 0; index < 4; ++index) {
		char name[sizeof(_peerNames[index])];
		if (!master.getPeerName(index, name, sizeof(name))) {
			snprintf(name, sizeof(name), "anim%u", index + 1);
		}

		if (strncmp(_peerNames[index], name, sizeof(_peerNames[index])) != 0) {
			strncpy(_peerNames[index], name, sizeof(_peerNames[index]) - 1);
			_peerNames[index][sizeof(_peerNames[index]) - 1] = '\0';
			changed = true;
		}
	}

	if (changed) {
		draw();
	}
}

void Interface::readEncoder() {
	noInterrupts();
	const int32_t transitions = encoderTransitionsPending;
	encoderTransitionsPending = 0;
	interrupts();

	_encoderSteps += transitions;
	const int32_t detents = _encoderSteps / 4;
	_encoderSteps %= 4;
	if (detents == 0) {
		return;
	}

	const int32_t commandOffset =
			((detents % COMMAND_COUNT) + COMMAND_COUNT) % COMMAND_COUNT;
	_selectedCommand =
			(_selectedCommand + commandOffset) % COMMAND_COUNT;
	_feedback = nullptr;
	draw();
	Serial.print("Encoder detent, command ");
	Serial.println(_selectedCommand + 1);
}

void Interface::readButtons(Master& master) {
	const unsigned long now = millis();

	for (uint8_t index = 0; index < 4; ++index) {
		const bool pressed = digitalRead(BUTTON_PINS[index]) == LOW;
		ButtonState& button = _buttons[index];

		if (pressed != button.rawPressed) {
			button.rawPressed = pressed;
			button.changedAtMs = now;
		}

		if (now - button.changedAtMs >= BUTTON_DEBOUNCE_MS &&
				button.stablePressed != button.rawPressed) {
			button.stablePressed = button.rawPressed;
			if (button.stablePressed) {
				sendSelectedCommand(master, index);
			}
		}
	}
}

void Interface::sendSelectedCommand(Master& master, uint8_t peerIndex) {
	const bool sent = master.sendCommandToPeer(
			peerIndex,
			commandText[_selectedCommand].command);

	_feedback = sent ? "ORDRE ENVOYE" : "ANIMATION ABSENTE";
	_feedbackColor = sent ? ST77XX_GREEN : ST77XX_RED;
	_feedbackUntilMs = millis() + 1800;
	draw();

	if (sent) {
		LOGI("Sent %s to animation %u", commandText[_selectedCommand].label,
				 peerIndex + 1);
	} else {
		LOGW("Animation %u is not connected", peerIndex + 1);
	}
}

void Interface::draw() {
	_display.fillScreen(ST77XX_BLACK);
	_display.setTextWrap(false);

	_display.fillRect(0, 0, 128, 22, ST77XX_BLUE);
	_display.setTextColor(ST77XX_WHITE);
	_display.setTextSize(1);
	_display.setCursor(6, 7);
	_display.print("SUPERVISEUR");

	_display.setTextSize(1);
	_display.setTextColor(ST77XX_CYAN);
	_display.setCursor(6, 31);
	_display.print("COMMANDE");
	_display.setCursor(96, 31);
	_display.print(_selectedCommand + 1);
	_display.print("/");
	_display.print(COMMAND_COUNT);

	_display.drawRoundRect(4, 43, 120, 38, 5, ST77XX_WHITE);
	_display.setTextSize(2);
	_display.setTextColor(ST77XX_YELLOW);
	int16_t textX = 0;
	int16_t textY = 0;
	uint16_t textWidth = 0;
	uint16_t textHeight = 0;
	_display.getTextBounds(
			commandText[_selectedCommand].label,
			0,
			0,
			&textX,
			&textY,
			&textWidth,
			&textHeight);
	_display.setCursor((128 - textWidth) / 2, 55);
	_display.print(commandText[_selectedCommand].label);

	_display.setTextSize(1);
	_display.setTextColor(ST77XX_WHITE);
	_display.setCursor(6, 94);
	_display.print("BOUTON = ENVOI PEER");

	for (uint8_t index = 0; index < 4; ++index) {
		const int16_t x = 1 + (index % 2) * 64;
		const int16_t y = 101 + (index / 2) * 23;
		_display.drawRoundRect(x, y, 62, 19, 4, ST77XX_CYAN);
		_display.setTextColor(ST77XX_WHITE);
		_display.setTextSize(1);
		char visibleName[11];
		strncpy(visibleName, _peerNames[index], sizeof(visibleName) - 1);
		visibleName[sizeof(visibleName) - 1] = '\0';
		int16_t nameX = 0;
		int16_t nameY = 0;
		uint16_t nameWidth = 0;
		uint16_t nameHeight = 0;
		_display.getTextBounds(
				visibleName, 0, 0, &nameX, &nameY, &nameWidth, &nameHeight);
		_display.setCursor(x + (62 - nameWidth) / 2, y + 6);
		_display.print(visibleName);
	}

	if (_feedback != nullptr) {
		_display.fillRect(0, 145, 128, 15, ST77XX_BLACK);
		_display.setTextColor(_feedbackColor);
		_display.setTextSize(1);
		_display.setCursor(6, 149);
		_display.print(_feedback);
	}
}
