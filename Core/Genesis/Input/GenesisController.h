#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Utilities/Serializer.h"

// Sega 3-button Control Pad (standard Genesis/Mega Drive controller).
//
// Protocol: the M68K writes to the TH/TL control lines (bits 6,5 of the I/O
// port control register). When TH (select) = 0: read Up,Down,1C,Start.
// When TH (select) = 1: read Up,Down,Left,Right,A,B. Active-low outputs.
//
// Port mapping (per ares/md/controller/control-pad):
//   $A10003 Data port 1  (read: controller data; write: TH select)
//   $A10005 Data port 2
//   $A10009 Control port 1 (write: direction bits, bit6=TH output enable)
//   $A1000B Control port 2

class GenesisController : public BaseControlDevice
{
private:
	uint32_t _turboSpeed = 0;
	bool _select = false; //TH line state (written by M68K)

	//Direction conflict resolution (same as ares)
	bool _yHold = false;
	bool _xHold = false;
	bool _upLatch = false;
	bool _downLatch = false;
	bool _leftLatch = false;
	bool _rightLatch = false;

protected:
	string GetKeyNames() override
	{
		return "UDLRABCs";
	}

	void InternalSetStateFromInput() override
	{
		for(KeyMapping& keyMapping : _keyMappings) {
			//Turbo
			uint8_t turboFreq = 1 << (4 - _turboSpeed);
			bool turboOn = (uint8_t)(_emu->GetFrameCount() % turboFreq) < turboFreq / 2;
			if(turboOn) {
				SetPressedState(Buttons::A, keyMapping.TurboA);
				SetPressedState(Buttons::B, keyMapping.TurboB);
				SetPressedState(Buttons::C, keyMapping.TurboX);
			}

			SetPressedState(Buttons::Up, keyMapping.Up);
			SetPressedState(Buttons::Down, keyMapping.Down);
			SetPressedState(Buttons::Left, keyMapping.Left);
			SetPressedState(Buttons::Right, keyMapping.Right);
			SetPressedState(Buttons::A, keyMapping.A);
			SetPressedState(Buttons::B, keyMapping.B);
			SetPressedState(Buttons::C, keyMapping.X);
			SetPressedState(Buttons::Start, keyMapping.Start);

			//U+D / L+R conflict resolution (same as ares)
			bool up = IsPressed(Buttons::Up);
			bool down = IsPressed(Buttons::Down);
			bool left = IsPressed(Buttons::Left);
			bool right = IsPressed(Buttons::Right);

			if(!(up && down)) {
				_yHold = false;
				_upLatch = up;
				_downLatch = down;
			} else if(!_yHold) {
				_yHold = true;
				std::swap(_upLatch, _downLatch);
			}

			if(!(left && right)) {
				_xHold = false;
				_leftLatch = left;
				_rightLatch = right;
			} else if(!_xHold) {
				_xHold = true;
				std::swap(_leftLatch, _rightLatch);
			}
		}
	}

	void RefreshStateBuffer() override {}

public:
	enum Buttons
	{
		Up = 0,
		Down,
		Left,
		Right,
		A,
		B,
		C,
		Start
	};

	GenesisController(Emulator* emu, uint8_t port, KeyMappingSet keyMappings)
		: BaseControlDevice(emu, ControllerType::GenesisController, port, keyMappings)
	{
		_turboSpeed = keyMappings.TurboSpeed;
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		//Genesis controller data read (active-low, 6 bits)
		//Per ares ControlPad::readData():
		//  select=0: bit0=up, bit1=down, bit2-3=0 (grounded), bit4=a, bit5=start
		//  select=1: bit0=up, bit1=down, bit2=left, bit3=right, bit4=b, bit5=c
		//After ~data (active-low): 0=released, 1=not pressed.
		//Bit 6 (TH) is NOT driven by the controller — it is an input to the
		//controller. Return 1 for bit 6 so that when the IOPort merges device
		//data with TH-as-input, the TH line reads as pulled-high (1), matching
		//real hardware. The IOPort overrides bit 6 from the data latch when TH
		//is configured as output.
		uint8_t data = 0x7F; //bits 0-5 active-low (1=released), bit 6=1 (TH not driven)

		if(!_select) {
			//TH = 0 (select low)
			if(_upLatch) {
				data &= ~0x01;
			}
			if(_downLatch) {
				data &= ~0x02;
			}
			data &= ~0x0C; //bits 2-3 grounded (0) per ares
			if(IsPressed(Buttons::A)) {
				data &= ~0x10;
			}
			if(IsPressed(Buttons::Start)) {
				data &= ~0x20;
			}
		} else {
			//TH = 1 (select high)
			if(_upLatch) {
				data &= ~0x01;
			}
			if(_downLatch) {
				data &= ~0x02;
			}
			if(_leftLatch) {
				data &= ~0x04;
			}
			if(_rightLatch) {
				data &= ~0x08;
			}
			if(IsPressed(Buttons::B)) {
				data &= ~0x10;
			}
			if(IsPressed(Buttons::C)) {
				data &= ~0x20;
			}
		}

		return data;
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		//TH select line is bit 6 of the data written to the port
		_select = (value >> 6) & 1;
	}

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_select);
		SV(_yHold);
		SV(_xHold);
		SV(_upLatch);
		SV(_downLatch);
		SV(_leftLatch);
		SV(_rightLatch);
	}

	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "a", Buttons::A },
			{ "b", Buttons::B },
			{ "c", Buttons::C },
			{ "up", Buttons::Up },
			{ "down", Buttons::Down },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
			{ "start", Buttons::Start },
		};
	}
};
