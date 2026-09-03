#pragma once
#include "pch.h"
#include <mutex>
#include "Shared/Video/DrawCommand.h"

struct stbtt_fontinfo;

struct TrueTypeGlyph
{
	int Width = 0;
	int Height = 0;
	int XOffset = 0;
	int YOffset = 0;
	int Advance = 0;
	vector<uint8_t> Pixels;
};

struct TrueTypeGlyphPlacement
{
	int X = 0;
	int Y = 0;
	shared_ptr<const TrueTypeGlyph> Glyph;
};

struct TrueTypeTextLine
{
	int Y = 0;
	uint32_t Width = 0;
};

struct TrueTypeTextLayout
{
	TextSize Size = {};
	uint32_t LineHeight = 0;
	vector<TrueTypeGlyphPlacement> Glyphs;
	vector<TrueTypeTextLine> Lines;
};

class TrueTypeFont
{
private:
	static constexpr size_t MaxFontSize = 64 * 1024 * 1024;

	vector<uint8_t> _fontData;
	unique_ptr<stbtt_fontinfo> _fontInfo;
	unordered_map<int, shared_ptr<const TrueTypeGlyph>> _glyphCache;
	std::mutex _glyphLock;
	float _scale = 0;
	uint32_t _lineHeight = 0;
	int _baseline = 0;
	int _spaceAdvance = 0;
	bool _monochrome = false;

	TrueTypeFont() = default;
	shared_ptr<const TrueTypeGlyph> GetGlyph(int codepoint);
	vector<int> DecodeUtf8(const string& text) const;

public:
	~TrueTypeFont();

	static shared_ptr<TrueTypeFont> Load(const string& filename, uint32_t pixelSize, bool monochrome, uint32_t faceIndex, string& error);
	TrueTypeTextLayout Layout(const string& text, uint32_t maxWidth, bool includeGlyphs);
};
