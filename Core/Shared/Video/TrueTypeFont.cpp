#include "pch.h"
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
	int advance = 0;
	int leftBearing = 0;
	stbtt_GetCodepointHMetrics(_fontInfo.get(), codepoint, &advance, &leftBearing);
	glyph->Advance = std::max(0, (int)std::round(advance * _scale));

	int x2 = 0;
	int y2 = 0;
	stbtt_GetCodepointBitmapBox(_fontInfo.get(), codepoint, _scale, _scale, &glyph->XOffset, &glyph->YOffset, &x2, &y2);
	int64_t width = (int64_t)x2 - glyph->XOffset;
	int64_t height = (int64_t)y2 - glyph->YOffset;
	if(width > 0 && width <= 4096 && height > 0 && height <= 4096 && width * height <= 16 * 1024 * 1024) {
		glyph->Width = (int)width;
		glyph->Height = (int)height;
		glyph->Pixels.resize((size_t)glyph->Width * glyph->Height);
		stbtt_MakeCodepointBitmap(_fontInfo.get(), glyph->Pixels.data(), glyph->Width, glyph->Height, glyph->Width, _scale, _scale, codepoint);
		if(_monochrome) {
			for(uint8_t& pixel : glyph->Pixels) {
				pixel = pixel >= 128 ? 255 : 0;
			}
		}
	}

	_glyphCache.emplace(codepoint, glyph);
	return glyph;
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
