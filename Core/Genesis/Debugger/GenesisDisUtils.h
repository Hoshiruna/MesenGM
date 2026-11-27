#pragma once
#include "pch.h"

namespace GenesisDisUtils
{
	constexpr int M68KInstructionSize = 10;
	uint8_t DecodeM68K(const uint8_t* bytes, uint32_t pc, string* output = nullptr);

	inline bool IsCall(uint16_t opcode)
	{
		return (opcode & 0xFFC0) == 0x4E80 || (opcode & 0xFF00) == 0x6100;
	}

	inline bool IsReturn(uint16_t opcode)
	{
		return opcode == 0x4E73 || opcode == 0x4E75 || opcode == 0x4E77;
	}

	inline bool IsUnconditionalJump(uint16_t opcode)
	{
		return (opcode & 0xFFC0) == 0x4EC0 || (opcode & 0xFF00) == 0x6000 || IsReturn(opcode);
	}

	inline bool IsConditionalJump(uint16_t opcode)
	{
		return (opcode >= 0x6200 && opcode < 0x7000) || (opcode & 0xF0F8) == 0x50C8;
	}
}
