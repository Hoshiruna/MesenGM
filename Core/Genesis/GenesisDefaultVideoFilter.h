#pragma once
#include "pch.h"
#include "Shared/Emulator.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Genesis/GenesisTypes.h"

class GenesisDefaultVideoFilter : public BaseVideoFilter
{
protected:
	void ApplyFilter(uint16_t* ppuOutputBuffer) override
	{
		//The VDP writes ARGB pixels with an H40 stride in both modes.
		const uint32_t* input = reinterpret_cast<const uint32_t*>(ppuOutputBuffer);
		uint32_t* output = GetOutputBuffer();
		OverscanDimensions overscan = GetOverscan();
		constexpr uint32_t black = 0xFF000000u;
		uint32_t sourceWidth = _baseFrameInfo.Width;
		uint32_t sourceHeight = _baseFrameInfo.Height;
		//Center the cropped H32 image in the same viewport as H40.
		uint32_t sidePadding = (GenesisScreenWidthH40 - sourceWidth) / 2;
		uint32_t croppedWidth = sourceWidth - overscan.Left - overscan.Right;
		uint32_t displayHeight = _frameInfo.Height + overscan.Top + overscan.Bottom;
		uint32_t topPadding = (displayHeight - sourceHeight) / 2;
		for(uint32_t y = 0; y < _frameInfo.Height; y++) {
			uint32_t* outputRow = output + y * _frameInfo.Width;
			uint32_t displayY = y + overscan.Top;
			if(displayY < topPadding || displayY >= topPadding + sourceHeight) {
				std::fill_n(outputRow, _frameInfo.Width, black);
				continue;
			}

			const uint32_t* sourceRow = input + (displayY - topPadding) * GenesisScreenWidthH40;
			std::fill_n(outputRow, sidePadding, black);
			memcpy(outputRow + sidePadding, sourceRow + overscan.Left, croppedWidth * sizeof(uint32_t));
			std::fill_n(outputRow + sidePadding + croppedWidth, sidePadding, black);
		}
	}

public:
	using BaseVideoFilter::BaseVideoFilter;

	FrameInfo GetDisplayFrameInfo() const override
	{
		constexpr uint32_t palDisplayHeight = 240;
		uint32_t height = _baseFrameInfo.Height;
		if(_emu->GetRegion() == ConsoleRegion::Pal) {
			//Keep the same viewport when PAL games switch between V28 and V30.
			height = std::max(height, palDisplayHeight);
		}
		return { GenesisScreenWidthH40, height };
	}
};
