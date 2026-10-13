#pragma once
#include "pch.h"
#include "NES/APU/NesApu.h"
#include "NES/NesConsole.h"
#include "Utilities/Serializer.h"
#include <algorithm>
#include <cmath>

// Rinco FSG2's FIFO and MSM6585 decoder. Behavioral reference: NintendulatorNRS
// src-mappers/src/iNES/MMC3-based/mapper594.cpp and Hardware/sound/s_MSM6585.cpp.
class RincoFsg2Audio final : public ISerializable
{
private:
	static constexpr uint32_t ClockRate = 1789773;
	NesConsole* _console;
	uint8_t _fifo[1024] = {};
	uint16_t _readPos = 0;
	uint16_t _writePos = 0;
	uint8_t _input = 0;
	bool _lowNibble = false;
	int16_t _signal = -2;
	int8_t _stepIndex = 0;
	uint32_t _clock = 0;
	uint32_t _rate = 4000;
	int32_t _lastOutput = 0;
	double _gain[2] = {};
	double _a1[2] = {};
	double _a2[2] = {};
	double _delay1[2] = {};
	double _delay2[2] = {};

	void UpdateFilter()
	{
		// Two biquads form a fourth-order Butterworth filter at 40% of the ADPCM rate.
		constexpr double pi = 3.14159265358979323846;
		double angle = 2.0 * pi * (_rate * 0.4) / ClockRate;
		for(int i = 0; i < 2; i++) {
			double alpha = std::sin(angle) * std::sin(pi * (2 * i + 1) / 8.0);
			double divisor = 1.0 + alpha;
			_gain[i] = (1.0 - std::cos(angle)) / (2.0 * divisor);
			_a1[i] = -2.0 * std::cos(angle) / divisor;
			_a2[i] = (1.0 - alpha) / divisor;
		}
	}

	void DecodeNibble()
	{
		uint8_t nibble;
		if(_lowNibble) {
			nibble = _input & 0x0F;
		} else {
			_input = 0;
			if(_readPos != _writePos) {
				_input = _fifo[_readPos];
				_readPos = (_readPos + 1) & 0x3FF;
			}
			nibble = _input >> 4;
		}
		_lowNibble = !_lowNibble;

		static constexpr int16_t steps[49] = {
			16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552
		};
		static constexpr int8_t indexChanges[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
		int step = steps[_stepIndex];
		int delta = step / 8;
		if(nibble & 1) {
			delta += step / 4;
		}
		if(nibble & 2) {
			delta += step / 2;
		}
		if(nibble & 4) {
			delta += step;
		}
		_signal = std::clamp<int>(_signal + (nibble & 8 ? -delta : delta), -2048, 2047);
		_stepIndex = std::clamp<int>(_stepIndex + indexChanges[nibble & 7], 0, 48);
	}

protected:
	void Serialize(Serializer& s) override
	{
		SVArray(_fifo, 1024);
		SV(_readPos);
		SV(_writePos);
		SV(_input);
		SV(_lowNibble);
		SV(_signal);
		SV(_stepIndex);
		SV(_clock);
		SV(_rate);
		SV(_lastOutput);
		SVArray(_delay1, 2);
		SVArray(_delay2, 2);
		if(!s.IsSaving()) {
			UpdateFilter();
		}
	}

public:
	RincoFsg2Audio(NesConsole* console) : _console(console)
	{
		UpdateFilter();
	}

	void Reset()
	{
		// The reference preserves the playback rate, nibble phase and filter history on reset.
		_readPos = _writePos = 0;
		_clock = 0;
		_signal = -2;
		_stepIndex = 0;
		_lastOutput = 0;
	}

	uint8_t ReadStatus() const
	{
		return ((_writePos - _readPos + 1024) & 0x3FF) >= 512 ? 0x00 : 0x40;
	}

	void Write(uint16_t addr, uint8_t value)
	{
		if(addr & 1) {
			uint32_t rate = 4000 << (value >> 6);
			if(rate != _rate) {
				_rate = rate;
				UpdateFilter();
				memset(_delay1, 0, sizeof(_delay1));
				memset(_delay2, 0, sizeof(_delay2));
			}
			_readPos = _writePos = 0;
		} else {
			// Match the reference's ring-buffer wrap, including writes past the status threshold.
			_fifo[_writePos] = value;
			_writePos = (_writePos + 1) & 0x3FF;
		}
	}

	void Clock()
	{
		_clock += _rate;
		if(_clock >= ClockRate) {
			_clock -= ClockRate;
			DecodeNibble();
		}

		// Keep the filter history in the same units as existing save states.
		double sample = _signal * 4.0 + 1e-15;
		for(int i = 0; i < 2; i++) {
			double output = sample * _gain[i] + _delay1[i];
			_delay1[i] = sample * 2.0 * _gain[i] - output * _a1[i] + _delay2[i];
			_delay2[i] = sample * _gain[i] - output * _a2[i];
			sample = output;
		}
		// Leave headroom for the NES channels by reducing ADPCM gain without clipping its waveform.
		int32_t output = (int32_t)(sample * 0.25);
		// Use the same expansion mixer channel as the other FIFO audio board, UNL-DripGame.
		if(_console->GetApu()->IsApuEnabled()) {
			_console->GetApu()->AddExpansionAudioDelta(AudioChannel::VRC7, output - _lastOutput);
			_lastOutput = output;
		}
	}
};
