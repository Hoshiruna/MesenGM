#pragma once
#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Genesis/GenesisTypes.h"

class Emulator;
class GenesisConsole;

class GenesisDefaultVideoFilter : public BaseVideoFilter
{
private:
	GenesisConsole* _console = nullptr;

protected:
	FrameInfo GetFrameInfo() override
	{
		FrameInfo frameInfo = _baseFrameInfo;
		OverscanDimensions overscan = GetOverscan();
		frameInfo.Width -= overscan.Left + overscan.Right;
		frameInfo.Height -= overscan.Top + overscan.Bottom;
		return frameInfo;
	}

public:
	GenesisDefaultVideoFilter(Emulator* emu, GenesisConsole* console) : BaseVideoFilter(emu)
	{
		_console = console;
	}

	void ApplyFilter(uint16_t* ppuOutputBuffer) override
	{
		//Genesis VDP outputs 32-bit ARGB pixels directly.
		//The VDP framebuffer always uses a 320-pixel stride (MaxWidth),
		//even in H32 (256-wide) mode, so we must use 320 as the input stride.
		uint32_t* in = (uint32_t*)ppuOutputBuffer;
		uint32_t* out = GetOutputBuffer();

		OverscanDimensions overscan = GetOverscan();
		FrameInfo frame = _frameInfo;

		constexpr uint32_t inStride = 320; //GenesisVdp::MaxWidth
		for(uint32_t y = 0; y < frame.Height; y++) {
			uint32_t* src = in + (y + overscan.Top) * inStride + overscan.Left;
			uint32_t* dst = out + y * frame.Width;
			memcpy(dst, src, frame.Width * sizeof(uint32_t));
		}
	}
};
