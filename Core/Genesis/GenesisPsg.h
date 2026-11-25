#pragma once
#include "pch.h"
#include "Genesis/GenesisTypes.h"
#include "Utilities/ISerializable.h"

class Emulator;
class SoundMixer;
class EmuSettings;
class GenesisConsole;
struct blip_t;

// Genesis SN76489 PSG - same chip as Core/SMS/SmsPsg, but:
//   * Clocked at master/240 (Z80 clock / 16 internal divider) instead of master/16.
//   * Mono (Genesis has no GameGear-style stereo panning register).
//   * Volume comes from GenesisConfig::PsgVolume.
//
// Algorithm adapted from Core/SMS/SmsPsg.cpp (native Mesen2, not ares).
class GenesisPsg final : public ISerializable
{
private:
	static constexpr int SampleRate = 96000;
	static constexpr int MaxSamples = 4000;
	//SMS uses the same 16-step attenuation LUT; verified identical to ares's
	//pow(2, level * -2.0/6.0) curve within rounding noise.
	static constexpr int16_t _volumeLut[16] = { 8192, 6507, 5168, 4105, 3261, 2590, 2058, 1642, 1298, 1031, 819, 651, 517, 410, 326, 0 };

	int16_t* _soundBuffer = nullptr;
	blip_t* _channel = nullptr;

	SoundMixer* _soundMixer = nullptr;
	EmuSettings* _settings = nullptr;
	GenesisConsole* _console = nullptr;

	GenesisPsgState _state = {};
	uint64_t _masterClock = 0;
	uint64_t _clockCounter = 0;
	int16_t _prevOutput = 0;

	void RunNoise(GenesisPsgNoiseChannel& noise);

public:
	GenesisPsg(Emulator* emu, GenesisConsole* console);
	~GenesisPsg();

	GenesisPsgState& GetState() { return _state; }

	void SetRegion(ConsoleRegion region);

	void Run();
	void PlayQueuedAudio();

	void Write(uint8_t value);

	void Serialize(Serializer& s) override;
};
