#pragma once
#include "pch.h"

enum class CpuType : uint8_t
{
	Snes,
	Spc,
	NecDsp,
	Sa1,
	Gsu,
	Cx4,
	St018,
	Gameboy,
	Nes,
	Pce,
	Sms,
	Gba,
	Ws,
	GenesisM68K,
	GenesisZ80
};

class CpuTypeUtilities
{
public:
	static constexpr bool SupportsDebugger(CpuType type)
	{
		return type != CpuType::GenesisM68K && type != CpuType::GenesisZ80;
	}

	static constexpr int GetCpuTypeCount()
	{
		return (int)CpuType::GenesisZ80 + 1;
	}
};