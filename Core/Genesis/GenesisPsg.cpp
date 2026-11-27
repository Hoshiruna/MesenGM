#include "pch.h"
#include "Genesis/GenesisPsg.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisYm2612.h"
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

	_soundBuffer = new int16_t[GenesisAudioBufferSize * 2];
	_channel = blip_new(GenesisAudioBufferSize);
	Reset();
}

GenesisPsg::~GenesisPsg()
{
	delete[] _soundBuffer;
	if(_channel) {
		blip_delete(_channel);
	}
}

void GenesisPsg::Reset()
{
	//Same power-on state as ares's SN76489::power() and SmsPsg::SmsPsg().
	_state = {};
	_state.Noise.Lfsr = 0x8000;
	_state.Noise.Volume = 0x0F;
	_state.Tone[0].Volume = 0x0F;
	_state.Tone[1].Volume = 0x0F;
	_state.Tone[2].Volume = 0x0F;

	_masterClock = _console->GetMasterClock();
	ResetAudioOutput();
}

void GenesisPsg::ResetAudioOutput()
{
	_clockCounter = 0;
	_prevOutput = 0;
	blip_set_rates(_channel, _console->GetMasterClockRate(), GenesisAudioSampleRate);
	blip_clear(_channel);
	_lowPass = {};
	//Use BlastEm's default analog low-pass cutoff.
	_lowPass.SetCutoffFrequency(3390, GenesisAudioSampleRate);
}

void GenesisPsg::SetRegion(ConsoleRegion region)
{
	//Region change implies a master-clock-rate change; rebuild blip mapping.
	ResetAudioOutput();
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

void GenesisPsg::Run(uint64_t masterClock)
{
	constexpr uint32_t clocksPerTick = GenesisZ80ClockDivider * 16;
	//BlastEm peaks at 2340 per PSG voice and scales FM by about two thirds.
	//43% of this table gives a similar PSG level relative to FM.
	constexpr int32_t outputGainPercent = 43;
	int32_t psgVolume = (int32_t)std::min(_settings->GetGenesisConfig().PsgVolume, 100u);
	int32_t volumeScale = psgVolume * outputGainPercent;

	while(_masterClock + clocksPerTick <= masterClock) {
		int32_t output = 0;
		for(auto& tone : _state.Tone) {
			if(tone.Timer == 0 || --tone.Timer == 0) {
				tone.Output ^= 1;
				tone.Timer = tone.ReloadValue;
			}

			output += tone.Output * _volumeLut[tone.Volume];
		}

		RunNoise(_state.Noise);
		output += _state.Noise.Output * _volumeLut[_state.Noise.Volume];
		output = output * volumeScale / (100 * 100);

		_clockCounter += clocksPerTick;
		_masterClock += clocksPerTick;

		if(_prevOutput != output) {
			blip_add_delta(_channel, _clockCounter, output - _prevOutput);
			_prevOutput = output;
		}
	}
}

void GenesisPsg::PlayQueuedAudio(GenesisYm2612& ym2612)
{
	blip_end_frame(_channel, _clockCounter);

	uint32_t sampleCount = blip_read_samples(_channel, _soundBuffer, GenesisAudioBufferSize, 0);

	for(uint32_t i = 0; i < sampleCount; i++) {
		_soundBuffer[i] = (int16_t)_lowPass.Process(_soundBuffer[i]);
	}

	//Expand mono to stereo in place, starting at the end of the buffer.
	for(int32_t i = (int32_t)sampleCount - 1; i >= 0; i--) {
		_soundBuffer[i * 2 + 0] = _soundBuffer[i];
		_soundBuffer[i * 2 + 1] = _soundBuffer[i];
	}

	//Mix both chips over the same master-clock interval before output resampling.
	ym2612.MixAudio(_soundBuffer, sampleCount, _masterClock - _clockCounter, _clockCounter);
	_soundMixer->PlayAudioBuffer(_soundBuffer, sampleCount, GenesisAudioSampleRate);
	_clockCounter = 0;
}

void GenesisPsg::Write(uint8_t value, uint64_t masterClock)
{
	Run(masterClock);

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
	if(!s.IsSaving()) {
		ResetAudioOutput();
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
	}
}
