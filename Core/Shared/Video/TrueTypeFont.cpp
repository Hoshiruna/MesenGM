#include "pch.h"
#include <cstring>
#include <limits>
#include "Shared/Video/TrueTypeFont.h"

#ifdef _MSC_VER
	#pragma warning(push, 0)
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "Utilities/Font/stb_truetype.h"

#ifdef _MSC_VER
	#pragma warning(pop)
#endif

namespace
{
	class BigEndianFontReader
	{
	private:
		const vector<uint8_t>& _data;

	public:
		BigEndianFontReader(const vector<uint8_t>& data) : _data(data) {}

		bool HasRange(size_t offset, size_t length) const
		{
			return offset <= _data.size() && length <= _data.size() - offset;
		}

		optional<uint8_t> ReadUInt8(size_t offset) const
		{
			if(!HasRange(offset, 1)) {
				return std::nullopt;
			}
			return _data[offset];
		}

		optional<uint16_t> ReadUInt16(size_t offset) const
		{
			if(!HasRange(offset, 2)) {
				return std::nullopt;
			}
			return (uint16_t)(((uint16_t)_data[offset] << 8) | _data[offset + 1]);
		}

		optional<uint32_t> ReadUInt32(size_t offset) const
		{
			if(!HasRange(offset, 4)) {
				return std::nullopt;
			}
			return ((uint32_t)_data[offset] << 24) | ((uint32_t)_data[offset + 1] << 16) | ((uint32_t)_data[offset + 2] << 8) | _data[offset + 3];
		}

		bool HasTag(size_t offset, const char tag[4]) const
		{
			return HasRange(offset, 4) && std::memcmp(_data.data() + offset, tag, 4) == 0;
		}
	};

	struct EmbeddedBitmapMetrics
	{
		uint8_t Height = 0;
		uint8_t Width = 0;
		int8_t BearingX = 0;
		int8_t BearingY = 0;
		uint8_t Advance = 0;
	};
}

TrueTypeFont::~TrueTypeFont() = default;

shared_ptr<TrueTypeFont> TrueTypeFont::Load(const string& filename, uint32_t pixelSize, bool monochrome, uint32_t faceIndex, string& error)
{
	shared_ptr<TrueTypeFont> font(new TrueTypeFont());
	ifstream input(filename, ios::binary | ios::ate);
	if(!input) {
		error = "could not open font file";
		return nullptr;
	}

	std::streamoff fileSize = input.tellg();
	if(fileSize < 4 || fileSize > (std::streamoff)MaxFontSize) {
		error = "font file must be between 4 bytes and 64 MB";
		return nullptr;
	}

	font->_fontData.resize((size_t)fileSize);
	input.seekg(0, ios::beg);
	if(!input.read((char*)font->_fontData.data(), fileSize)) {
		error = "could not read font file";
		return nullptr;
	}

	if(faceIndex > (uint32_t)std::numeric_limits<int>::max()) {
		error = "font face index is out of range";
		return nullptr;
	}

	int offset = stbtt_GetFontOffsetForIndex(font->_fontData.data(), (int)faceIndex);
	if(offset < 0) {
		error = "font face was not found";
		return nullptr;
	}

	font->_fontInfo.reset(new stbtt_fontinfo());
	if(!stbtt_InitFont(font->_fontInfo.get(), font->_fontData.data(), offset)) {
		error = "file is not a supported TrueType font";
		return nullptr;
	}

	font->_scale = stbtt_ScaleForPixelHeight(font->_fontInfo.get(), (float)pixelSize);
	font->_monochrome = monochrome;
	if(monochrome) {
		// stb_truetype ignores embedded bitmap strikes, so read a matching 1-bit strike directly.
		font->FindEmbeddedBitmapStrike((size_t)offset, pixelSize);
	}

	int ascent = 0;
	int descent = 0;
	int lineGap = 0;
	stbtt_GetFontVMetrics(font->_fontInfo.get(), &ascent, &descent, &lineGap);
	font->_baseline = (int)std::ceil(ascent * font->_scale);
	font->_lineHeight = (uint32_t)std::max(1, (int)std::ceil((ascent - descent + lineGap) * font->_scale));

	int spaceAdvance = 0;
	int leftBearing = 0;
	stbtt_GetCodepointHMetrics(font->_fontInfo.get(), ' ', &spaceAdvance, &leftBearing);
	font->_spaceAdvance = std::max(1, (int)std::round(spaceAdvance * font->_scale));
	return font;
}

shared_ptr<const TrueTypeGlyph> TrueTypeFont::GetGlyph(int codepoint)
{
	std::lock_guard<std::mutex> lock(_glyphLock);
	auto cachedGlyph = _glyphCache.find(codepoint);
	if(cachedGlyph != _glyphCache.end()) {
		return cachedGlyph->second;
	}

	shared_ptr<TrueTypeGlyph> glyph(new TrueTypeGlyph());
	if(_monochrome && LoadEmbeddedBitmapGlyph(codepoint, *glyph)) {
		_glyphCache.emplace(codepoint, glyph);
		return glyph;
	}

	int advance = 0;
	int leftBearing = 0;
	stbtt_GetCodepointHMetrics(_fontInfo.get(), codepoint, &advance, &leftBearing);
	glyph->Advance = std::max(0, (int)std::round(advance * _scale));

	int x2 = 0;
	int y2 = 0;
	stbtt_GetCodepointBitmapBox(_fontInfo.get(), codepoint, _scale, _scale, &glyph->XOffset, &glyph->YOffset, &x2, &y2);
	int64_t width = (int64_t)x2 - glyph->XOffset;
	int64_t height = (int64_t)y2 - glyph->YOffset;
	if(width > 0 && width <= 4096 && height > 0 && height <= 4096 && width * height <= MaxGlyphPixelCount) {
		glyph->Width = (int)width;
		glyph->Height = (int)height;
		glyph->Pixels.resize((size_t)glyph->Width * glyph->Height);
		stbtt_MakeCodepointBitmap(_fontInfo.get(), glyph->Pixels.data(), glyph->Width, glyph->Height, glyph->Width, _scale, _scale, codepoint);
		if(_monochrome) {
			// Keep covered pixels when the font has no embedded bitmap for this glyph.
			for(uint8_t& pixel : glyph->Pixels) {
				pixel = pixel > 0 ? 255 : 0;
			}
		}
	}

	_glyphCache.emplace(codepoint, glyph);
	return glyph;
}

bool TrueTypeFont::FindEmbeddedBitmapStrike(size_t fontOffset, uint32_t pixelSize)
{
	BigEndianFontReader reader(_fontData);
	optional<uint16_t> tableCount = reader.ReadUInt16(fontOffset + 4);
	if(!tableCount || !reader.HasRange(fontOffset + 12, (size_t)*tableCount * 16)) {
		return false;
	}

	size_t bitmapDataOffset = 0;
	size_t bitmapDataLength = 0;
	size_t indexTableOffset = 0;
	size_t indexTableLength = 0;
	for(uint16_t i = 0; i < *tableCount; i++) {
		size_t recordOffset = fontOffset + 12 + (size_t)i * 16;
		optional<uint32_t> tableOffset = reader.ReadUInt32(recordOffset + 8);
		optional<uint32_t> tableLength = reader.ReadUInt32(recordOffset + 12);
		if(!tableOffset || !tableLength || !reader.HasRange(*tableOffset, *tableLength)) {
			continue;
		}
		if(reader.HasTag(recordOffset, "EBDT")) {
			bitmapDataOffset = *tableOffset;
			bitmapDataLength = *tableLength;
		} else if(reader.HasTag(recordOffset, "EBLC")) {
			indexTableOffset = *tableOffset;
			indexTableLength = *tableLength;
		}
	}

	if(bitmapDataLength == 0 || indexTableLength < 8) {
		return false;
	}
	optional<uint32_t> indexTableVersion = reader.ReadUInt32(indexTableOffset);
	optional<uint32_t> strikeCount = reader.ReadUInt32(indexTableOffset + 4);
	if(!indexTableVersion || *indexTableVersion != 0x00020000 || !strikeCount || *strikeCount > (indexTableLength - 8) / 48) {
		return false;
	}

	for(uint32_t i = 0; i < *strikeCount; i++) {
		size_t strikeOffset = indexTableOffset + 8 + (size_t)i * 48;
		optional<uint32_t> indexArrayOffset = reader.ReadUInt32(strikeOffset);
		optional<uint32_t> subTableCount = reader.ReadUInt32(strikeOffset + 8);
		optional<uint16_t> startGlyphIndex = reader.ReadUInt16(strikeOffset + 40);
		optional<uint16_t> endGlyphIndex = reader.ReadUInt16(strikeOffset + 42);
		optional<uint8_t> ppemX = reader.ReadUInt8(strikeOffset + 44);
		optional<uint8_t> ppemY = reader.ReadUInt8(strikeOffset + 45);
		optional<uint8_t> bitDepth = reader.ReadUInt8(strikeOffset + 46);
		if(!indexArrayOffset || !subTableCount || !startGlyphIndex || !endGlyphIndex || !ppemX || !ppemY || !bitDepth) {
			continue;
		}
		if(*ppemX != pixelSize || *ppemY != pixelSize || *bitDepth != 1 || *indexArrayOffset > indexTableLength) {
			continue;
		}

		size_t indexSubTableArrayOffset = indexTableOffset + *indexArrayOffset;
		size_t remainingIndexBytes = indexTableLength - *indexArrayOffset;
		if(*subTableCount > remainingIndexBytes / 8) {
			continue;
		}

		_embeddedBitmapStrike.BitmapDataOffset = bitmapDataOffset;
		_embeddedBitmapStrike.BitmapDataLength = bitmapDataLength;
		_embeddedBitmapStrike.IndexTableOffset = indexTableOffset;
		_embeddedBitmapStrike.IndexTableLength = indexTableLength;
		_embeddedBitmapStrike.IndexSubTableArrayOffset = indexSubTableArrayOffset;
		_embeddedBitmapStrike.IndexSubTableCount = *subTableCount;
		_embeddedBitmapStrike.StartGlyphIndex = *startGlyphIndex;
		_embeddedBitmapStrike.EndGlyphIndex = *endGlyphIndex;
		_embeddedBitmapStrike.IsValid = true;
		return true;
	}
	return false;
}

bool TrueTypeFont::LoadEmbeddedBitmapGlyph(int codepoint, TrueTypeGlyph& glyph) const
{
	if(!_embeddedBitmapStrike.IsValid) {
		return false;
	}

	int glyphIndexValue = stbtt_FindGlyphIndex(_fontInfo.get(), codepoint);
	if(glyphIndexValue < _embeddedBitmapStrike.StartGlyphIndex || glyphIndexValue > _embeddedBitmapStrike.EndGlyphIndex) {
		return false;
	}
	uint16_t glyphIndex = (uint16_t)glyphIndexValue;
	BigEndianFontReader reader(_fontData);

	for(uint32_t i = 0; i < _embeddedBitmapStrike.IndexSubTableCount; i++) {
		size_t arrayEntryOffset = _embeddedBitmapStrike.IndexSubTableArrayOffset + (size_t)i * 8;
		optional<uint16_t> firstGlyphIndex = reader.ReadUInt16(arrayEntryOffset);
		optional<uint16_t> lastGlyphIndex = reader.ReadUInt16(arrayEntryOffset + 2);
		optional<uint32_t> additionalOffset = reader.ReadUInt32(arrayEntryOffset + 4);
		if(!firstGlyphIndex || !lastGlyphIndex || !additionalOffset || glyphIndex < *firstGlyphIndex || glyphIndex > *lastGlyphIndex) {
			continue;
		}

		size_t arrayOffsetInTable = _embeddedBitmapStrike.IndexSubTableArrayOffset - _embeddedBitmapStrike.IndexTableOffset;
		size_t remainingTableBytes = _embeddedBitmapStrike.IndexTableLength - arrayOffsetInTable;
		if(*additionalOffset > remainingTableBytes || remainingTableBytes - *additionalOffset < 8) {
			return false;
		}
		size_t subTableOffset = _embeddedBitmapStrike.IndexSubTableArrayOffset + *additionalOffset;
		size_t subTableOffsetInTable = subTableOffset - _embeddedBitmapStrike.IndexTableOffset;
		size_t subTableBytesRemaining = _embeddedBitmapStrike.IndexTableLength - subTableOffsetInTable;
		optional<uint16_t> indexFormat = reader.ReadUInt16(subTableOffset);
		optional<uint16_t> imageFormat = reader.ReadUInt16(subTableOffset + 2);
		optional<uint32_t> imageDataOffset = reader.ReadUInt32(subTableOffset + 4);
		if(!indexFormat || !imageFormat || !imageDataOffset) {
			return false;
		}

		uint32_t glyphDataOffset = 0;
		uint32_t glyphDataLength = 0;
		EmbeddedBitmapMetrics indexMetrics;
		bool hasIndexMetrics = false;
		uint32_t glyphOffsetInRange = glyphIndex - *firstGlyphIndex;
		if(*indexFormat == 1 || *indexFormat == 3) {
			size_t offsetSize = *indexFormat == 1 ? 4 : 2;
			size_t offsetCount = (subTableBytesRemaining - 8) / offsetSize;
			if(offsetCount < 2 || glyphOffsetInRange >= offsetCount - 1) {
				return false;
			}
			size_t offsetsOffset = subTableOffset + 8 + (size_t)glyphOffsetInRange * offsetSize;
			optional<uint32_t> firstOffset;
			optional<uint32_t> nextOffset;
			if(*indexFormat == 1) {
				firstOffset = reader.ReadUInt32(offsetsOffset);
				nextOffset = reader.ReadUInt32(offsetsOffset + 4);
			} else {
				optional<uint16_t> first = reader.ReadUInt16(offsetsOffset);
				optional<uint16_t> next = reader.ReadUInt16(offsetsOffset + 2);
				if(first && next) {
					firstOffset = *first;
					nextOffset = *next;
				}
			}
			if(!firstOffset || !nextOffset || *nextOffset < *firstOffset) {
				return false;
			}
			glyphDataOffset = *firstOffset;
			glyphDataLength = *nextOffset - *firstOffset;
		} else if(*indexFormat == 2 || *indexFormat == 5) {
			if(subTableBytesRemaining < (*indexFormat == 2 ? 20 : 24)) {
				return false;
			}
			optional<uint32_t> imageSize = reader.ReadUInt32(subTableOffset + 8);
			optional<uint8_t> height = reader.ReadUInt8(subTableOffset + 12);
			optional<uint8_t> width = reader.ReadUInt8(subTableOffset + 13);
			optional<uint8_t> bearingX = reader.ReadUInt8(subTableOffset + 14);
			optional<uint8_t> bearingY = reader.ReadUInt8(subTableOffset + 15);
			optional<uint8_t> advance = reader.ReadUInt8(subTableOffset + 16);
			if(!imageSize || !height || !width || !bearingX || !bearingY || !advance) {
				return false;
			}
			indexMetrics = { *height, *width, (int8_t)*bearingX, (int8_t)*bearingY, *advance };
			hasIndexMetrics = true;
			glyphDataLength = *imageSize;

			uint32_t imageIndex = glyphOffsetInRange;
			if(*indexFormat == 5) {
				optional<uint32_t> glyphCount = reader.ReadUInt32(subTableOffset + 20);
				if(!glyphCount || *glyphCount > (subTableBytesRemaining - 24) / 2) {
					return false;
				}
				bool found = false;
				for(uint32_t glyphNumber = 0; glyphNumber < *glyphCount; glyphNumber++) {
					optional<uint16_t> currentGlyphIndex = reader.ReadUInt16(subTableOffset + 24 + (size_t)glyphNumber * 2);
					if(!currentGlyphIndex) {
						return false;
					}
					if(*currentGlyphIndex == glyphIndex) {
						imageIndex = glyphNumber;
						found = true;
						break;
					}
				}
				if(!found) {
					return false;
				}
			}
			uint64_t offset = (uint64_t)imageIndex * *imageSize;
			if(offset > std::numeric_limits<uint32_t>::max()) {
				return false;
			}
			glyphDataOffset = (uint32_t)offset;
		} else if(*indexFormat == 4) {
			if(subTableBytesRemaining < 16) {
				return false;
			}
			optional<uint32_t> glyphCount = reader.ReadUInt32(subTableOffset + 8);
			if(!glyphCount || *glyphCount > (subTableBytesRemaining - 16) / 4) {
				return false;
			}
			bool found = false;
			for(uint32_t glyphNumber = 0; glyphNumber < *glyphCount; glyphNumber++) {
				size_t entryOffset = subTableOffset + 12 + (size_t)glyphNumber * 4;
				optional<uint16_t> currentGlyphIndex = reader.ReadUInt16(entryOffset);
				optional<uint16_t> firstOffset = reader.ReadUInt16(entryOffset + 2);
				optional<uint16_t> nextOffset = reader.ReadUInt16(entryOffset + 6);
				if(!currentGlyphIndex || !firstOffset || !nextOffset) {
					return false;
				}
				if(*currentGlyphIndex == glyphIndex) {
					if(*nextOffset < *firstOffset) {
						return false;
					}
					glyphDataOffset = *firstOffset;
					glyphDataLength = *nextOffset - *firstOffset;
					found = true;
					break;
				}
			}
			if(!found) {
				return false;
			}
		} else {
			return false;
		}

		uint64_t bitmapOffset = (uint64_t)*imageDataOffset + glyphDataOffset;
		if(bitmapOffset > _embeddedBitmapStrike.BitmapDataLength || glyphDataLength > _embeddedBitmapStrike.BitmapDataLength - bitmapOffset) {
			return false;
		}
		size_t glyphDataPosition = _embeddedBitmapStrike.BitmapDataOffset + (size_t)bitmapOffset;
		EmbeddedBitmapMetrics metrics;
		size_t metricsSize = 0;
		bool byteAligned = false;
		if(*imageFormat == 1 || *imageFormat == 2) {
			optional<uint8_t> height = reader.ReadUInt8(glyphDataPosition);
			optional<uint8_t> width = reader.ReadUInt8(glyphDataPosition + 1);
			optional<uint8_t> bearingX = reader.ReadUInt8(glyphDataPosition + 2);
			optional<uint8_t> bearingY = reader.ReadUInt8(glyphDataPosition + 3);
			optional<uint8_t> advance = reader.ReadUInt8(glyphDataPosition + 4);
			if(!height || !width || !bearingX || !bearingY || !advance) {
				return false;
			}
			metrics = { *height, *width, (int8_t)*bearingX, (int8_t)*bearingY, *advance };
			metricsSize = 5;
			byteAligned = *imageFormat == 1;
		} else if(*imageFormat == 5 && hasIndexMetrics) {
			metrics = indexMetrics;
		} else if(*imageFormat == 6 || *imageFormat == 7) {
			optional<uint8_t> height = reader.ReadUInt8(glyphDataPosition);
			optional<uint8_t> width = reader.ReadUInt8(glyphDataPosition + 1);
			optional<uint8_t> bearingX = reader.ReadUInt8(glyphDataPosition + 2);
			optional<uint8_t> bearingY = reader.ReadUInt8(glyphDataPosition + 3);
			optional<uint8_t> advance = reader.ReadUInt8(glyphDataPosition + 4);
			if(!height || !width || !bearingX || !bearingY || !advance) {
				return false;
			}
			metrics = { *height, *width, (int8_t)*bearingX, (int8_t)*bearingY, *advance };
			metricsSize = 8;
			byteAligned = *imageFormat == 6;
		} else {
			return false;
		}

		size_t rowByteCount = ((size_t)metrics.Width + 7) / 8;
		size_t bitmapByteCount = byteAligned ? rowByteCount * metrics.Height : ((size_t)metrics.Width * metrics.Height + 7) / 8;
		if(metricsSize > glyphDataLength || bitmapByteCount > glyphDataLength - metricsSize || (size_t)metrics.Width * metrics.Height > MaxGlyphPixelCount) {
			return false;
		}

		glyph.Width = metrics.Width;
		glyph.Height = metrics.Height;
		glyph.XOffset = metrics.BearingX;
		glyph.YOffset = -metrics.BearingY;
		glyph.Advance = metrics.Advance;
		glyph.Pixels.resize((size_t)glyph.Width * glyph.Height);
		size_t bitmapPosition = glyphDataPosition + metricsSize;
		for(int row = 0; row < glyph.Height; row++) {
			for(int column = 0; column < glyph.Width; column++) {
				size_t bitOffset = byteAligned ? (size_t)row * rowByteCount * 8 + column : (size_t)row * glyph.Width + column;
				uint8_t bits = _fontData[bitmapPosition + bitOffset / 8];
				glyph.Pixels[(size_t)row * glyph.Width + column] = (bits & (0x80 >> (bitOffset % 8))) ? 255 : 0;
			}
		}
		return true;
	}
	return false;
}

vector<int> TrueTypeFont::DecodeUtf8(const string& text) const
{
	vector<int> codepoints;
	for(size_t i = 0; i < text.size();) {
		uint8_t first = (uint8_t)text[i];
		if(first < 0x80) {
			codepoints.push_back(first);
			i++;
			continue;
		}

		int codepoint = 0;
		size_t length = 0;
		int minimum = 0;
		if((first & 0xE0) == 0xC0) {
			codepoint = first & 0x1F;
			length = 2;
			minimum = 0x80;
		} else if((first & 0xF0) == 0xE0) {
			codepoint = first & 0x0F;
			length = 3;
			minimum = 0x800;
		} else if((first & 0xF8) == 0xF0) {
			codepoint = first & 0x07;
			length = 4;
			minimum = 0x10000;
		}

		bool valid = length > 0 && i + length <= text.size();
		for(size_t j = 1; valid && j < length; j++) {
			uint8_t next = (uint8_t)text[i + j];
			if((next & 0xC0) != 0x80) {
				valid = false;
			} else {
				codepoint = (codepoint << 6) | (next & 0x3F);
			}
		}

		if(!valid || codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
			codepoints.push_back(0xFFFD);
			i++;
		} else {
			codepoints.push_back(codepoint);
			i += length;
		}
	}
	return codepoints;
}

TrueTypeTextLayout TrueTypeFont::Layout(const string& text, uint32_t maxWidth, bool includeGlyphs)
{
	TrueTypeTextLayout layout;
	layout.LineHeight = _lineHeight;

	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t widestLine = 0;
	int previousCodepoint = 0;

	auto finishLine = [&]() {
		if(includeGlyphs) {
			layout.Lines.push_back({ (int)y, x });
		}
		widestLine = std::max(widestLine, x);
		x = 0;
		y += _lineHeight;
		previousCodepoint = 0;
	};

	for(int codepoint : DecodeUtf8(text)) {
		if(codepoint == '\r') {
			continue;
		}
		if(codepoint == '\n') {
			finishLine();
			continue;
		}
		if(codepoint == '\t') {
			uint32_t tabWidth = (uint32_t)_spaceAdvance * 4;
			uint32_t nextX = ((x / tabWidth) + 1) * tabWidth;
			if(maxWidth > 0 && nextX > maxWidth && x > 0) {
				finishLine();
				nextX = tabWidth;
			}
			x = nextX;
			previousCodepoint = 0;
			continue;
		}

		shared_ptr<const TrueTypeGlyph> glyph = GetGlyph(codepoint);
		int kerning = previousCodepoint == 0 ? 0 : (int)std::round(stbtt_GetCodepointKernAdvance(_fontInfo.get(), previousCodepoint, codepoint) * _scale);
		int64_t nextX = (int64_t)x + kerning + glyph->Advance;
		if(maxWidth > 0 && nextX > maxWidth && x > 0) {
			finishLine();
			kerning = 0;
			nextX = glyph->Advance;
		}

		int glyphX = (int)x + kerning + glyph->XOffset;
		if(includeGlyphs && glyph->Width > 0 && glyph->Height > 0) {
			layout.Glyphs.push_back({ glyphX, (int)y + _baseline + glyph->YOffset, glyph });
		}
		x = (uint32_t)std::max<int64_t>(0, nextX);
		previousCodepoint = codepoint;
	}

	finishLine();
	layout.Size.X = widestLine;
	layout.Size.Y = y;
	return layout;
}
