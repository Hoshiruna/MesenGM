#pragma once
#include "pch.h"
#include "Shared/SettingTypes.h"
#include "Shared/BaseState.h"

// Genesis/Mega Drive native port - shared types.
//
// Strategy: this core is a native Mesen2 implementation that uses ares/md as
// algorithmic reference only. No ares headers are pulled in; everything is
// re-expressed through Mesen2 infrastructure (SV() serialization, SoundMixer,
// BaseControlManager, IConsole).

enum class GenesisRegion
{
	//Derived from GenesisConfig::Region + ROM header domestic flag
	Ntsc, //NTSC Genesis, master clock 53693175 Hz, ~59.92 fps
	Pal, //PAL Mega Drive, master clock 53203424 Hz, ~49.70 fps
};

// Master clock rates (same crystal as SMS, since Genesis VDP is SMS-compatible).
// NTSC: 53693175 Hz, PAL: 53203424 Hz.
// M68K clock = master / 7, Z80 clock = master / 15, VDP pixel = master / 10 (H40) or / 8 (H32).
inline constexpr uint32_t GenesisMasterClockNtsc = 53693175;
inline constexpr uint32_t GenesisMasterClockPal = 53203424;

// VDP visible area with full overscan. Matches ares visibleHeight():
// NTSC = 243 lines, PAL = 294 lines. Width: H32=256, H40=320.
inline constexpr uint32_t GenesisScreenWidthH32 = 256;
inline constexpr uint32_t GenesisScreenWidthH40 = 320;
inline constexpr uint32_t GenesisScreenHeightNtsc = 243;
inline constexpr uint32_t GenesisScreenHeightPal = 294;

// SN76489 PSG state (same chip as SMS, algorithm adapted from Core/SMS/SmsPsg).
// The Genesis clocks the SN76489 at Z80 clock = master/15, and the chip
// internally divides by 16, giving an effective tick rate of master/240.
struct GenesisPsgToneChannel
{
	uint16_t ReloadValue = 0;
	uint16_t Timer = 0;
	uint8_t Output = 0;
	uint8_t Volume = 0x0F;
};

struct GenesisPsgNoiseChannel
{
	uint16_t Timer = 0;
	uint16_t Lfsr = 0x8000;
	uint8_t LfsrInputBit = 0;
	uint8_t Control = 0;
	uint8_t Output = 0;
	uint8_t Volume = 0x0F;
};

struct GenesisPsgState
{
	GenesisPsgToneChannel Tone[3] = {};
	GenesisPsgNoiseChannel Noise = {};
	uint8_t SelectedReg = 0;
};

//--- Debugger/debugger state structs (BaseState subclasses) ---

struct GenesisM68KState : public BaseState
{
	uint32_t D[8];
	uint32_t A[8];
	uint32_t PC;
	uint16_t SR;
	uint32_t SSP;
	bool Stopped;
};

struct GenesisZ80State : public BaseState
{
	uint8_t A, Flags;
	uint8_t B, C, D, E, H, L;
	uint16_t IX, IY;
	uint16_t SP, PC;
	uint8_t I, R;
	bool Halted;
};

struct GenesisVdpState : public BaseState
{
	uint16_t VCounter;
	uint16_t HCounter;
	bool VBlank;
	bool HBlank;
	bool DisplayEnable;
	uint8_t Regs[24];
	uint16_t Cram[64];
};

struct GenesisState : public BaseState
{
	GenesisM68KState M68K;
	GenesisZ80State Z80;
	GenesisVdpState Vdp;
	uint32_t FrameCount;
	GenesisRegion Region;
};
