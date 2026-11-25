#include "pch.h"
#include "Genesis/GenesisPsg.h"
#include "Genesis/GenesisConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/Serializer.h"
#include "Utilities/Audio/blip_buf.h"

GenesisPsg::GenesisPsg(Emulator* emu, GenesisConsole* console)
{
	_console = console;
	_soundMixer = emu->GetSoundMixer();
	_settings = emu->GetSettings();

	//Same power-on state as ares's SN76489::power() and SmsPsg::SmsPsg().
	_state.Noise.Lfsr = 0x8000;
	_state.Noise.Volume = 0x0F;
	_state.Tone[0].Volume = 0x0F;
	_state.Tone[1].Volume = 0x0F;
	_state.Tone[2].Volume = 0x0F;

	//Buffer holds MaxSamples mono reads from blip_buf, then expanded in-place
	//to MaxSamples*2 interleaved stereo for SoundMixer::PlayAudioBuffer.
	_soundBuffer = new int16_t[GenesisPsg::MaxSamples * 2];
	memset(_soundBuffer, 0, GenesisPsg::MaxSamples * 2 * sizeof(int16_t));

	_channel = blip_new(GenesisPsg::MaxSamples);
	blip_clear(_channel);
	blip_set_rates(_channel, _console->GetMasterClockRate(), GenesisPsg::SampleRate);
}

GenesisPsg::~GenesisPsg()
{
	delete[] _soundBuffer;
	if(_channel) {
		blip_delete(_channel);
	}
}

void GenesisPsg::SetRegion(ConsoleRegion region)
{
	//Region change implies a master-clock-rate change; rebuild blip mapping.
	blip_clear(_channel);
	blip_set_rates(_channel, _console->GetMasterClockRate(), GenesisPsg::SampleRate);
}

void GenesisPsg::RunNoise(GenesisPsgNoiseChannel& noise)
{
	if(noise.Timer == 0 || --noise.Timer == 0) {
		noise.LfsrInputBit ^= 1;
		switch(noise.Control & 0x03) {
			case 0: noise.Timer = 0x10; break;
			case 1: noise.Timer = 0x20; break;
			case 2: noise.Timer = 0x40; break;
			case 3: noise.Timer = _state.Tone[2].ReloadValue; break;
		}

		if(noise.LfsrInputBit) {
			bool useBit3 = noise.Control & 0x04;
			uint16_t newBit = (noise.Lfsr & 0x01) ^ (useBit3 & ((noise.Lfsr >> 3) & 0x01));
			noise.Lfsr = (newBit << 15) | (noise.Lfsr >> 1);
			noise.Output = noise.Lfsr & 0x01;
		}
	}
}

void GenesisPsg::Run()
{
	uint64_t runTo = _console->GetMasterClock();
	uint32_t psgVolume = _settings->GetGenesisConfig().PsgVolume;

	//Genesis SN76489 ticks at master/240 (Z80 clock master/15, then /16 chip-internal).
	//Each iteration advances _masterClock by 240 master clocks = 1 PSG tick.
	while(_masterClock + 240 < runTo) {
		int16_t output = 0;
		int16_t channelOutput;
		for(int i = 0; i < 3; i++) {
			if(_state.Tone[i].Timer == 0 || --_state.Tone[i].Timer == 0) {
				_state.Tone[i].Output ^= 1;
				_state.Tone[i].Timer = _state.Tone[i].ReloadValue;
			}

			//Volume scale matches ares's 0.625 mono coefficient (per-channel /4 * 0.625 = /6.4).
			//We approximate with /100 * PsgVolume for parity with SmsPsg's per-channel handling.
			channelOutput = _state.Tone[i].Output * _volumeLut[_state.Tone[i].Volume] * psgVolume / 100;
			output += channelOutput;
		}

		RunNoise(_state.Noise);
		channelOutput = _state.Noise.Output * _volumeLut[_state.Noise.Volume] * psgVolume / 100;
		output += channelOutput;

		_clockCounter += 240;
		_masterClock += 240;

		if(_prevOutput != output) {
			blip_add_delta(_channel, _clockCounter, output - _prevOutput);
			_prevOutput = output;
		}
	}

	if(_clockCounter >= 20000) {
		PlayQueuedAudio();
	}
}

void GenesisPsg::PlayQueuedAudio()
{
	blip_end_frame(_channel, _clockCounter);

	//Mono: read into a flat buffer (no interleave).
	uint32_t sampleCount = (uint32_t)blip_read_samples(_channel, _soundBuffer, GenesisPsg::MaxSamples, 0);

	//Reshape into the interleaved stereo format SoundMixer expects by duplicating
	//each sample across L+R. The buffer is sized MaxSamples (mono) which is half
	//of MaxSamples*2 (stereo), so this in-place expansion from the end is safe.
	for(int32_t i = (int32_t)sampleCount - 1; i >= 0; i--) {
		_soundBuffer[i * 2 + 0] = _soundBuffer[i];
		_soundBuffer[i * 2 + 1] = _soundBuffer[i];
	}

	_soundMixer->PlayAudioBuffer(_soundBuffer, sampleCount, GenesisPsg::SampleRate);
	_clockCounter = 0;
}

void GenesisPsg::Write(uint8_t value)
{
	Run();

	if(value & 0x80) {
		_state.SelectedReg = (value >> 4) & 0x07;
	}

	uint8_t channel = (_state.SelectedReg >> 1) & 0x03;
	bool volReg = _state.SelectedReg & 0x01;

	switch(channel) {
		case 0:
		case 1:
		case 2:
			if(volReg) {
				_state.Tone[channel].Volume = value & 0x0F;
			} else {
				if(value & 0x80) {
					_state.Tone[channel].ReloadValue = (_state.Tone[channel].ReloadValue & 0x3F0) | (value & 0x0F);
				} else {
					_state.Tone[channel].ReloadValue = (_state.Tone[channel].ReloadValue & 0x0F) | ((value & 0x3F) << 4);
				}
			}
			break;

		case 3:
			if(volReg) {
				_state.Noise.Volume = value & 0x0F;
			} else {
				_state.Noise.Control = value & 0x07;
				_state.Noise.Lfsr = 0x8000;
			}
			break;
	}
}

void GenesisPsg::Serialize(Serializer& s)
{
	if(s.IsSaving()) {
		Run();
	} else {
		_clockCounter = 0;
		blip_clear(_channel);
	}

	SV(_state.SelectedReg);

	SV(_state.Noise.Timer);
	SV(_state.Noise.Lfsr);
	SV(_state.Noise.LfsrInputBit);
	SV(_state.Noise.Control);
	SV(_state.Noise.Output);
	SV(_state.Noise.Volume);

	for(int i = 0; i < 3; i++) {
		SVI(_state.Tone[i].ReloadValue);
		SVI(_state.Tone[i].Timer);
		SVI(_state.Tone[i].Output);
		SVI(_state.Tone[i].Volume);
	}

	if(s.GetFormat() != SerializeFormat::Map) {
		//Hide these entries from the Lua API.
		SV(_masterClock);
		SV(_clockCounter);
		SV(_prevOutput);
	}
}
