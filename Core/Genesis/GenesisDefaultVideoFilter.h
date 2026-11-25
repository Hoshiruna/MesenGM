#pragma once
#include "pch.h"
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
		uint32_t sourceWidth = _baseFrameInfo.Width;
		for(uint32_t y = 0; y < _frameInfo.Height; y++) {
			const uint32_t* sourceRow = input + (y + overscan.Top) * GenesisScreenWidthH40;
			uint32_t* outputRow = output + y * _frameInfo.Width;
			if(sourceWidth == GenesisScreenWidthH40) {
				memcpy(outputRow, sourceRow + overscan.Left, _frameInfo.Width * sizeof(uint32_t));
			} else {
				//Sample H32 at the center of each output pixel after cropping in display coordinates.
				for(uint32_t x = 0; x < _frameInfo.Width; x++) {
					uint32_t sourceX = ((x + overscan.Left) * sourceWidth + sourceWidth / 2) / GenesisScreenWidthH40;
					outputRow[x] = sourceRow[sourceX];
				}
			}
		}
	}

public:
	using BaseVideoFilter::BaseVideoFilter;

	FrameInfo GetDisplayFrameInfo() const override
	{
		return { GenesisScreenWidthH40, _baseFrameInfo.Height };
	}
};
