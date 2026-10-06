#pragma once

#include <Arduino.h>
#include <Adafruit_ST7735.h>

#include <superv/protocol.hpp>

class Master;

class Interface {
public:
	Interface();

	void begin();
	void loop(Master& master);

private:
	struct ButtonState {
		bool rawPressed = false;
		bool stablePressed = false;
		unsigned long changedAtMs = 0;
	};

	void readEncoder();
	void readButtons(Master& master);
	void refreshPeerLabels(Master& master);
	void sendSelectedCommand(Master& master, uint8_t peerIndex);
	void draw();

	Adafruit_ST7735 _display;
	ButtonState _buttons[4];
	char _peerLabels[4][9] = {
			"P1 ANIM1",
			"P2 ANIM2",
			"P3 ANIM3",
			"P4 ANIM4",
	};
	int32_t _encoderSteps = 0;
	uint8_t _selectedCommand = 0;
	const char* _feedback = nullptr;
	uint16_t _feedbackColor = 0;
	unsigned long _feedbackUntilMs = 0;
};
