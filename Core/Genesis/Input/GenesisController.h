#pragma once
#include "pch.h"
#include "Genesis/GenesisConsole.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Utilities/Serializer.h"

// Sega 3-button and 6-button Control Pads. The console drives TH on bit 6.
class GenesisController : public BaseControlDevice
{
private:
	GenesisConsole* _console;
	bool _sixButton;
	uint32_t _turboSpeed;
	bool _select;
	uint8_t _thRises = 0;
	uint64_t _lastThRiseClock = 0;

	void ExpireHandshake(uint64_t clock)
	{
		// The Fighting Pad returns to the initial phase after 1.6 ms without a TH rising edge.
		const uint64_t timeout = _console->GetMasterClockRate() * 1600ull / 1'000'000;
		if(clock - _lastThRiseClock >= timeout) {
			_thRises = 0;
		}
	}

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
		return _sixButton ? "UDLRABCsXYZM" : "UDLRABCs";
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
				if(_sixButton) {
					SetPressedState(Buttons::X, keyMapping.TurboY);
					SetPressedState(Buttons::Y, keyMapping.TurboL);
					SetPressedState(Buttons::Z, keyMapping.TurboR);
				}
			}

			SetPressedState(Buttons::Up, keyMapping.Up);
			SetPressedState(Buttons::Down, keyMapping.Down);
			SetPressedState(Buttons::Left, keyMapping.Left);
			SetPressedState(Buttons::Right, keyMapping.Right);
			SetPressedState(Buttons::A, keyMapping.A);
			SetPressedState(Buttons::B, keyMapping.B);
			SetPressedState(Buttons::C, keyMapping.X);
			SetPressedState(Buttons::Start, keyMapping.Start);
			if(_sixButton) {
				SetPressedState(Buttons::X, keyMapping.Y);
				SetPressedState(Buttons::Y, keyMapping.L);
				SetPressedState(Buttons::Z, keyMapping.R);
				SetPressedState(Buttons::Mode, keyMapping.Select);
			}

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
		Start,
		X,
		Y,
		Z,
		Mode
	};

	GenesisController(Emulator* emu, GenesisConsole* console, ControllerType type, uint8_t port, KeyMappingSet keyMappings, bool thHigh)
		: BaseControlDevice(emu, type, port, keyMappings),
		  _console(console),
		  _sixButton(type == ControllerType::GenesisController6Buttons),
		  _turboSpeed(keyMappings.TurboSpeed),
		  _select(_sixButton && thHigh)
	{
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		if(_thRises != 0) {
			ExpireHandshake(_console->GetMasterClock());
		}

		uint8_t data = 0x7F;
		auto activeLow = [&](uint8_t bit, bool pressed) {
			if(pressed) {
				data &= ~(1u << bit);
			}
		};

		if(_select) {
			if(_thRises == 3) {
				activeLow(0, IsPressed(Buttons::Z));
				activeLow(1, IsPressed(Buttons::Y));
				activeLow(2, IsPressed(Buttons::X));
				activeLow(3, IsPressed(Buttons::Mode));
			} else {
				activeLow(0, _upLatch);
				activeLow(1, _downLatch);
				activeLow(2, _leftLatch);
				activeLow(3, _rightLatch);
			}
			activeLow(4, IsPressed(Buttons::B));
			activeLow(5, IsPressed(Buttons::C));
		} else {
			if(_thRises == 2) {
				// The third TH-low read identifies a 6-button pad.
				data &= ~0x0F;
			} else if(_thRises != 3) {
				activeLow(0, _upLatch);
				activeLow(1, _downLatch);
				data &= ~0x0C;
			}
			activeLow(4, IsPressed(Buttons::A));
			activeLow(5, IsPressed(Buttons::Start));
		}
		return data;
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		bool select = (value & 0x40) != 0;
		if(_sixButton && select && !_select) {
			uint64_t clock = _console->GetMasterClock();
			ExpireHandshake(clock);
			if(_thRises < 4) {
				_thRises++;
			}
			_lastThRiseClock = clock;
		}
		_select = select;
	}

	void ResetHandshake()
	{
		_thRises = 0;
		_lastThRiseClock = 0;
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
		if(_sixButton) {
			if(!s.IsSaving()) {
				ResetHandshake();
			}
			SV(_thRises);
			SV(_lastThRiseClock);
		}
	}

	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		vector<DeviceButtonName> names = {
			{ "a", Buttons::A },
			{ "b", Buttons::B },
			{ "c", Buttons::C },
			{ "up", Buttons::Up },
			{ "down", Buttons::Down },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
			{ "start", Buttons::Start },
		};
		if(_sixButton) {
			names.insert(names.end(), {
												  { "x", Buttons::X },
												  { "y", Buttons::Y },
												  { "z", Buttons::Z },
												  { "mode", Buttons::Mode },
											  });
		}
		return names;
	}
};
