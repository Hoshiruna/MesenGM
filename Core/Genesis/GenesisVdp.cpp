#include "pch.h"
#include "Genesis/GenesisVdp.h"
#include "Genesis/GenesisM68K.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisPsg.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Utilities/Serializer.h"

// Yamaha YM7101 VDP — native Mesen2 port.
// Algorithm ported from ares/md/vdp/*.cpp, adapted to Mesen2's explicit
// clock-advance model (no co-routine scheduler). Each RunScanline() call
// processes one horizontal scanline worth of slots and renders pixels if
// the line is in the visible region.

GenesisVdp::GenesisVdp(Emulator* emu, GenesisConsole* console)
	: _emu(emu), _console(console)
{
	_framebuffer.resize(MaxWidth * MaxHeight, 0);
	DmaRead = [](uint32_t) -> uint16_t { return 0xFFFF; };
	_region = ConsoleRegion::Ntsc;
}

void GenesisVdp::Power()
{
	memset(_vram, 0, sizeof(_vram));
	memset(_vsram, 0, sizeof(_vsram));
	memset(_cram, 0, sizeof(_cram));
	_vramMode = 0;
	_vramRefreshing = false;
	_cramBusActive = false;
	_cramBusData = 0;
	_busPenalty = 0;
	_command = {};
	_io = {};
	_latch = {};
	_state = {};
	_irq = {};
	_testAddress = 0;
	_frameCount = 0;
	_fifo.Power();
	_prefetch.Power();
	_dma.Power();
	_layers.Power();
	_window.Power();
	_layerA.Power();
	_layerB.Power();
	_sprite.Power();
	_dac.Power();
	UpdateScreenParams();
}

void GenesisVdp::Reset()
{
	_command = {};
	_io = {};
	_latch = {};
	_state = {};
	_irq = {};
	_testAddress = 0;
	_busPenalty = 0;
	_fifo.Power();
	_prefetch.Power();
	_dma.Power();
	_layers.Power();
	_window.Power();
	_layerA.Power();
	_layerB.Power();
	_sprite.Power();
	_dac.Power();
	UpdateScreenParams();
}

void GenesisVdp::UpdateScreenParams()
{
	//Treat any non-PAL region as NTSC. The MD VDP only distinguishes NTSC
	//vs PAL for timing; NtscJapan differs from Ntsc only in the version
	//register's export flag (handled in ReadM68KIO via _console->GetRegion()).
	if(_region != ConsoleRegion::Pal && V28()) {
		_state.topline = 0x1E5;
		_state.bottomline = 0x0EA;
	}
	if(_region != ConsoleRegion::Pal && V30()) {
		_state.topline = 0x000;
		_state.bottomline = 0x1FF;
	}
	if(_region == ConsoleRegion::Pal && V28()) {
		_state.topline = 0x1CA;
		_state.bottomline = 0x102;
	}
	if(_region == ConsoleRegion::Pal && V30()) {
		_state.topline = 0x1D2;
		_state.bottomline = 0x10A;
	}
}

//--- Scanline execution ---

void GenesisVdp::RunScanline()
{
	_latch.displayWidth = _io.displayWidth;
	_latch.clockSelect = _io.clockSelect;
	_state.edclkPos = 0;

	if(H32()) {
		MainH32();
	} else {
		MainH40();
	}

	if(_state.vcounter == _state.bottomline) {
		_latch.interlace = _io.interlaceMode & 1;
		_latch.overscan = _io.overscan;
		_frameCount++;
		_state.field ^= 1;
		UpdateScreenParams();
	}
}

//--- Timing helpers ---

void GenesisVdp::Htick()
{
	//ares uses n8 hcounter (8-bit) which wraps 0xFF→0x00 naturally.
	//Mesen2 uses uint32_t, so mask with 0xFF to mimic the n8 wrap behavior.
	//Without this, hcounter goes 0xFF→0x100→0x101→...→0x1D2 and never returns
	//to 0x00, breaking Vedge()/Hblank()/Vtick() timing and causing off-screen
	//pixel positions in Direct Color DMA.
	_state.hcounter = (_state.hcounter + 1) & 0xFF;
	if(H40()) {
		if(_state.hcounter == 0x00) {
			Vedge();
		} else if(_state.hcounter == 0x05) {
			Hblank(0);
		} else if(_state.hcounter == 0xA5) {
			Vtick();
		} else if(_state.hcounter == 0xB3) {
			Hblank(1);
		} else if(_state.hcounter == 0xB6) {
			_state.hcounter = 0xE4;
		}
	} else {
		if(_state.hcounter == 0x00) {
			Vedge();
		} else if(_state.hcounter == 0x05) {
			Hblank(0);
		} else if(_state.hcounter == 0x85) {
			Vtick();
		} else if(_state.hcounter == 0x93) {
			Hblank(1);
		} else if(_state.hcounter == 0x94) {
			_state.hcounter = 0xE9;
		}
	}
	IrqPoll();
}

void GenesisVdp::Vtick()
{
	if(_state.vblank) {
		_irq.hblank.counter = _irq.hblank.frequency;
	} else if(_irq.hblank.counter-- == 0) {
		_irq.hblank.counter = _irq.hblank.frequency;
		_irq.hblank.pending = 1;
		_irq.delay = H40() ? 3 : 2;
	}
	if(_state.vcounter == _state.bottomline) {
		_state.vcounter = _state.topline;
	} else {
		_state.vcounter++;
	}
	_state.vcounter &= 0x1FF; //9-bit wrap (ares n9 behavior)
	VblankCheck();
}

void GenesisVdp::Hblank(bool line)
{
	_state.hblank = line;
}

void GenesisVdp::Vblank(bool line)
{
	uint8_t prev = _state.vblank;
	uint8_t newTransitioned = _state.vblank ^ (line ? 1 : 0);
	_irq.vblank.transitioned |= newTransitioned;
	if(_state.vblank > (line ? 1 : 0)) {
		_state.topline = _state.vcounter;
	}
	_state.vblank = line ? 1 : 0;
}

void GenesisVdp::Vedge()
{
	if(!_irq.vblank.transitioned) {
		return;
	}
	_irq.vblank.transitioned = 0;
	if(_state.vblank) {
		_irq.vblank.pending = 1;
		_irq.delay = H40() ? 3 : 2;
	}
}

void GenesisVdp::VblankCheck()
{
	if(V28()) {
		if(_state.vcounter == 0x0E0) {
			Vblank(1);
		}
		if(_state.vcounter == 0x1FF) {
			Vblank(0);
		}
	}
	if(V30()) {
		if(_state.vcounter == 0x0F0) {
			Vblank(1);
		}
		if(_state.vcounter == 0x1FF) {
			Vblank(0);
		}
	}
}

void GenesisVdp::IrqPoll()
{
	if(_irq.delay) {
		_irq.delay--;
	}
}

void GenesisVdp::Slot()
{
	_state.rambusy = 0;
	if(!(_state.rambusy = _fifo.Run(*this))) {
		_state.rambusy = _prefetch.Run(*this);
	}
}

//--- Tick + slot helpers ---

template<bool H40>
void GenesisVdp::TickAndSlot()
{
	//Match ares tick() order exactly (main.cpp lines 28-74):
	//  dma.run → fullslotStep → htick → cram.bus dot → displayEnable latch
	//       → fifo.tick → dma.fetch → vram.refreshing=0 → preload--
	//       → rambusy=1
	//Then Mesen2-specific Slot() (fifo.run / prefetch.run).
	_dma.Run(*this);
	Htick();
	//Cram bus dot BEFORE _fifo.Tick — ares checks latency > 1 (pre-tick value).
	if(_cramBusActive) {
		_dac.Dot(*this, _state.hcounter * 2 + 1, _cramBusData);
		if((_latch.displayEnable && !_state.vblank) || _fifo.slots[0].empty() || _fifo.slots[0].target != 3) {
			_cramBusActive = false;
		} else {
			if(_fifo.slots[0].latency > 1 || _vramRefreshing) {
				_cramBusData = _cram[_io.backgroundColor];
			}
			_dac.Dot(*this, _state.hcounter * 2 + 2, _cramBusData);
		}
	}
	if(_latch.displayEnable > _io.displayEnable || _fifo.empty()) {
		_latch.displayEnable = _io.displayEnable;
	}
	_fifo.Tick();
	if(_dma.active && !_vramRefreshing) {
		_dma.Fetch(*this);
	}
	_vramRefreshing = false;
	if(_dma.active && _dma.preload > 0) {
		_dma.preload--;
	}
	_state.rambusy = 1;
}

template<bool H40>
void GenesisVdp::TickAndRefresh()
{
	//Match ares tick() refresh slot order (main.cpp lines 28-74 with _refresh=true):
	//  dma.run → fullslotStep → htick → cram.bus dot → displayEnable latch
	//       → vram.refreshing=1 → preload=h40?6:4 → rambusy=1
	//NOTE: ares does NOT call fifo.tick() in refresh slots. Mesen2 previously
	//called _fifo.Tick() here, which is a bug — FIFO advances during refresh
	//when it shouldn't, breaking DMA timing.
	_dma.Run(*this);
	Htick();
	if(_cramBusActive) {
		_dac.Dot(*this, _state.hcounter * 2 + 1, _cramBusData);
		if((_latch.displayEnable && !_state.vblank) || _fifo.slots[0].empty() || _fifo.slots[0].target != 3) {
			_cramBusActive = false;
		} else {
			if(_fifo.slots[0].latency > 1 || _vramRefreshing) {
				_cramBusData = _cram[_io.backgroundColor];
			}
			_dac.Dot(*this, _state.hcounter * 2 + 2, _cramBusData);
		}
	}
	if(_latch.displayEnable > _io.displayEnable || _fifo.empty()) {
		_latch.displayEnable = _io.displayEnable;
	}
	_vramRefreshing = true;
	if(_dma.active && _dma.preload > 0) {
		_dma.preload = H40 ? 6 : 4;
	}
	//Don't clear _vramRefreshing here — ares leaves vram.refreshing=1 until
	//the next non-refresh slot checks and clears it (in TickAndSlot).
	_state.rambusy = 1;
}

//--- Framebuffer management ---

void GenesisVdp::ClearFramebuffer()
{
	uint32_t bgColor = Color(_cram[_io.backgroundColor], 1);
	std::fill(_framebuffer.begin(), _framebuffer.end(), bgColor);
}

//--- Get line buffer for current scanline ---

uint32_t* GenesisVdp::GetLineBuffer()
{
	uint32_t y = _state.vcounter;

	//Pixel blanking range — matches ares vdp.cpp pixels():
	//NTSC:          y >= 0x0E8 && y < 0x1F5  → nullptr
	//PAL overscan:  y >= 0x108 && y < 0x1E2  → nullptr
	//PAL no-oversc: y >= 0x100 && y < 0x1DA  → nullptr
	//Treat any non-PAL region (incl. NtscJapan) as NTSC.
	if(_region != ConsoleRegion::Pal) {
		if(y >= 0x0E8 && y < 0x1F5) {
			return nullptr;
		}
	} else {
		if(_latch.overscan && y >= 0x108 && y < 0x1E2) {
			return nullptr;
		}
		if(!_latch.overscan && y >= 0x100 && y < 0x1DA) {
			return nullptr;
		}
	}

	//Map vcounter to framebuffer row — matches ares vdp.cpp pixels():
	//1. Collapse the blank gap by subtracting the vsync region size
	//2. Add top border offset to center active display
	//3. Modulo by visibleHeight (243 NTSC, 294 PAL)
	if(_region != ConsoleRegion::Pal) {
		if(y >= 0x0E8) {
			y -= (0x1F5 - 0x0E8); //y -= 0x10D
		}
		y += 11;
		y = y % 243;
	} else {
		if(_latch.overscan) {
			if(y >= 0x108) {
				y -= (0x1E2 - 0x108); //y -= 0xDA
			}
			y += 30;
			y = y % 294;
		} else {
			if(y >= 0x100) {
				y -= (0x1DA - 0x100); //y -= 0xDA
			}
			y += 38;
			y = y % 294;
		}
	}

	//Safety: if y exceeds MaxHeight, skip (shouldn't happen)
	if(y >= MaxHeight) {
		return nullptr;
	}
	return _framebuffer.data() + y * MaxWidth;
}

//--- Main scanline loops ---

void GenesisVdp::MainH32()
{
	uint32_t* lineBuf = GetLineBuffer();
	_dac.pixels = lineBuf;
	_dac.active = lineBuf ? lineBuf : nullptr;
	_state.hcounter = 0;
	Vedge(); //hcounter=0 is where Vedge fires in Htick()

	_sprite.Begin();
	if(lineBuf) {
		Blocks<false, true>();
	} else {
		Blocks<false, false>();
	}

	TickAndSlot<false>();
	Slot();
	TickAndSlot<false>();
	Slot();

	_layers.VscrollFetch(*this);
	_sprite.End();

	for(auto c = 0; c < 4; c++) {
		TickAndSlot<false>();
		_sprite.PatternFetch(*this, c + 0);
	}
	for(auto c = 0; c < 13; c++) {
		TickAndSlot<false>();
		_sprite.PatternFetch(*this, c + 4);
		_sprite.Scan(*this);
	}
	TickAndSlot<false>();
	Slot();
	_window.Begin();
	for(auto c = 0; c < 9; c++) {
		TickAndSlot<false>();
		_sprite.PatternFetch(*this, c + 17);
		_sprite.Scan(*this);
	}
	TickAndSlot<false>();
	Slot();

	_layerA.Begin();
	_layerB.Begin();

	TickAndSlot<false>();
	_layers.HscrollFetch(*this);
	TickAndSlot<false>();
	_sprite.PatternFetch(*this, 26);
	_sprite.Scan(*this);
	TickAndSlot<false>();
	_sprite.PatternFetch(*this, 27);
	_sprite.Scan(*this);
	TickAndSlot<false>();
	_sprite.PatternFetch(*this, 28);
	_sprite.Scan(*this);
	TickAndSlot<false>();
	_sprite.PatternFetch(*this, 29);
	_sprite.Scan(*this);

	_layers.VscrollFetchIndexed(*this, -1);
	_layerA.AttributesFetch(*this);
	_layerB.AttributesFetch(*this);
	_window.AttributesFetch(*this, -1);

	TickAndSlot<false>();
	_layerA.MappingFetch(*this, -1);
	if(!IsDisplayEnable()) {
		TickAndRefresh<false>();
	} else {
		TickAndSlot<false>();
		_sprite.PatternFetch(*this, 30);
		_sprite.Scan(*this);
	}
	TickAndSlot<false>();
	_layerA.PatternFetch(*this, 0);
	_sprite.Scan(*this);
	TickAndSlot<false>();
	_layerA.PatternFetch(*this, 1);
	_sprite.Scan(*this);
	TickAndSlot<false>();
	_layerB.MappingFetch(*this, -1);
	TickAndSlot<false>();
	_sprite.PatternFetch(*this, 31);
	_sprite.Scan(*this);
	TickAndSlot<false>();
	_layerB.PatternFetch(*this, 0);
	_sprite.Scan(*this);
	TickAndSlot<false>();
	_layerB.PatternFetch(*this, 1);
	_sprite.Scan(*this);
}

void GenesisVdp::MainH40()
{
	uint32_t* lineBuf = GetLineBuffer();
	_dac.pixels = lineBuf;
	_dac.active = lineBuf ? lineBuf : nullptr;
	_state.hcounter = 0;
	Vedge(); //hcounter=0 is where Vedge fires in Htick()

	_sprite.Begin();
	if(lineBuf) {
		Blocks<true, true>();
	} else {
		Blocks<true, false>();
	}

	TickAndSlot<true>();
	Slot();
	TickAndSlot<true>();
	Slot();

	_layers.VscrollFetch(*this);
	_sprite.End();

	for(auto c = 0; c < 4; c++) {
		TickAndSlot<true>();
		_sprite.PatternFetch(*this, c + 0);
	}
	for(auto c = 0; c < 19; c++) {
		TickAndSlot<true>();
		_sprite.PatternFetch(*this, c + 4);
		_sprite.Scan(*this);
	}
	TickAndSlot<true>();
	Slot();
	for(auto c = 0; c < 11; c++) {
		TickAndSlot<true>();
		_sprite.PatternFetch(*this, c + 23);
		_sprite.Scan(*this);
	}

	_layerA.Begin();
	_layerB.Begin();
	_window.Begin();

	TickAndSlot<true>();
	_layers.HscrollFetch(*this);
	TickAndSlot<true>();
	_sprite.PatternFetch(*this, 34);
	_sprite.Scan(*this);
	TickAndSlot<true>();
	_sprite.PatternFetch(*this, 35);
	_sprite.Scan(*this);
	TickAndSlot<true>();
	_sprite.PatternFetch(*this, 36);
	_sprite.Scan(*this);
	TickAndSlot<true>();
	_sprite.PatternFetch(*this, 37);
	_sprite.Scan(*this);

	_layers.VscrollFetchIndexed(*this, -1);
	_layerA.AttributesFetch(*this);
	_layerB.AttributesFetch(*this);
	_window.AttributesFetch(*this, -1);

	TickAndSlot<true>();
	_layerA.MappingFetch(*this, -1);
	if(!IsDisplayEnable()) {
		TickAndRefresh<true>();
	} else {
		TickAndSlot<true>();
		_sprite.PatternFetch(*this, 38);
		_sprite.Scan(*this);
	}
	TickAndSlot<true>();
	_layerA.PatternFetch(*this, 0);
	_sprite.Scan(*this);
	TickAndSlot<true>();
	_layerA.PatternFetch(*this, 1);
	_sprite.Scan(*this);
	TickAndSlot<true>();
	_layerB.MappingFetch(*this, -1);
	TickAndSlot<true>();
	_sprite.PatternFetch(*this, 39);
	_sprite.Scan(*this);
	TickAndSlot<true>();
	_layerB.PatternFetch(*this, 0);
	_sprite.Scan(*this);
	TickAndSlot<true>();
	_layerB.PatternFetch(*this, 1);
	_sprite.Scan(*this);
}

template<bool H40, bool DrawPixels>
void GenesisVdp::Blocks()
{
	bool top = (_state.vcounter == _state.topline);
	uint32_t blockCount = H40 ? 20 : 16;
	for(uint32_t block = 0; block < blockCount; block++) {
		_layers.VscrollFetchIndexed(*this, block);
		_layerA.AttributesFetch(*this);
		_layerB.AttributesFetch(*this);
		_window.AttributesFetch(*this, block);

		TickAndSlot<H40>();
		_layerA.MappingFetch(*this, block);
		if((block & 3) == 3) {
			TickAndRefresh<H40>();
		} else {
			TickAndSlot<H40>();
			Slot();
		}

		bool den = IsDisplayEnable();
		TickAndSlot<H40>();
		_layerA.PatternFetch(*this, block * 2 + 2);
		TickAndSlot<H40>();
		_layerA.PatternFetch(*this, block * 2 + 3);
		TickAndSlot<H40>();
		_layerB.MappingFetch(*this, block);
		TickAndSlot<H40>();
		_sprite.MappingFetch(*this, block);
		TickAndSlot<H40>();
		_layerB.PatternFetch(*this, block * 2 + 2);
		TickAndSlot<H40>();
		_layerB.PatternFetch(*this, block * 2 + 3);

		if(DrawPixels) {
			if(!den || top) {
				for(uint32_t p = 0; p < 16; p++) {
					_dac.Pixel<H40, false>(*this, block * 16 + p);
				}
			} else {
				for(uint32_t p = 0; p < 16; p++) {
					_dac.Pixel<H40, true>(*this, block * 16 + p);
				}
			}
		}
	}
}

//--- Bus interface ---

uint16_t GenesisVdp::ComputeVirtualHvCounter() const
{
	uint16_t vc = _state.vcounter;
	if(_io.interlaceMode & 1) {
		if(_io.interlaceMode & 2) {
			vc <<= 1;
		}
		vc = (vc & ~1) | ((_state.vcounter >> 8) & 1);
	}

	//If no M68K cycle position info, fall back to frozen hcounter.
	if(_m68kCyclesPerScanline == 0) {
		return (vc << 8) | (_state.hcounter & 0xFF);
	}

	uint32_t totalTicks = H40() ? 210 : 171;
	uint32_t virtualTick = (uint64_t)_m68kCycleInScanline * totalTicks / _m68kCyclesPerScanline;

	uint8_t hc;
	if(H40()) {
		if(virtualTick <= 0xB5) {
			hc = virtualTick & 0xFF;
		} else {
			hc = (virtualTick + 0x2E) & 0xFF;
		}
	} else {
		if(virtualTick <= 0x93) {
			hc = virtualTick & 0xFF;
		} else {
			hc = (virtualTick + 0x55) & 0xFF;
		}
	}

	return (vc << 8) | hc;
}

uint16_t GenesisVdp::Read(uint32_t address)
{
	uint32_t decode = address & 0x1E; //bits 1-4

	//Bus throttling: during active display, 68K must wait for an external
	//slot to access VDP registers. Add penalty for all non-PSG addresses.
	if(!_state.vblank && _latch.displayEnable && decode != 0x10 && decode != 0x12 && decode != 0x14 && decode != 0x16) {
		_busPenalty += 16;
	}

	//Update FIFO drain cycle tracking for non-control-port reads so that
	//cycles spent on data port / HV counter reads don't accumulate as
	//FIFO drain credit. ReadControlPort handles its own cycle tracking.
	if(decode != 0x04 && decode != 0x06) {
		_lastFifoDrainCycle = _m68kCycleInScanline;
	}

	switch(decode) {
		case 0x00:
		case 0x02: return ReadDataPort(); //0xC00000-0xC00003
		case 0x04:
		case 0x06: return ReadControlPort(); //0xC00004-0xC00007
		case 0x08:
		case 0x0A:
		case 0x0C:
		case 0x0E: { //0xC00008-0xC0000F
			if(_io.counterLatch) {
				return _state.counterLatchValue;
			}
			return ComputeVirtualHvCounter();
		}
		default: break;
	}
	return 0xFFFF;
}

void GenesisVdp::Write(uint32_t address, uint16_t data)
{
	//Genesis VDP address decode (mirrored every 32 bytes within 0xC00000-0xDFFFFF):
	//  0xC00000-0xC00003: Data port
	//  0xC00004-0xC00007: Control port
	//  0xC00008-0xC0000F: HV counter (read-only)
	//  0xC00010-0xC00017: PSG
	//  0xC00018-0xC0001B: Test address
	//  0xC0001C-0xC0001F: Test data
	uint32_t decode = address & 0x1E; //bits 1-4 (bit 0 = byte select, ignored)

	//Bus throttling: during active display, 68K must wait for an external
	//slot to access VDP registers. Add penalty for all non-PSG addresses.
	if(!_state.vblank && _latch.displayEnable && decode != 0x10 && decode != 0x12 && decode != 0x14 && decode != 0x16) {
		_busPenalty += 16;
	}

	//Update FIFO drain cycle tracking — cycles spent on VDP writes don't
	//accumulate as FIFO drain credit (VRAM bus is busy during writes).
	_lastFifoDrainCycle = _m68kCycleInScanline;

	switch(decode) {
		case 0x00:
		case 0x02: WriteDataPort(data); break; //0xC00000-0xC00003
		case 0x04:
		case 0x06:
			WriteControlPort(data);
			break; //0xC00004-0xC00007
		//0x08-0x0E: HV counter (read-only, ignore writes)
		case 0x10:
		case 0x12:
		case 0x14:
		case 0x16: {
			//PSG: ares forwards to psg.write(data.byte(0)) for word writes.
			//Only the low byte of the word is used.
			_console->GetPsg()->Write(data & 0xFF);
			break;
		}
		case 0x18:
		case 0x1A: _testAddress = data & 0xF; break;
		case 0x1C:
		case 0x1E: {
			switch(_testAddress) {
				case 0x0:
					//ares io.cpp:88-97 — test register 0 layout:
					//  bit 6:      DAC disable layers
					//  bit 7-8:    DAC force layer
					//  bit 9:      PSG volume override
					//  bit 10-11:  PSG volume channel
					//  bit 12:     sprite disable phase 1 (visible scan)
					//  bit 13:     sprite disable phase 2 (mapping fetch)
					//  bit 14:     sprite disable phase 3 (pattern fetch)
					_dac.disableLayers = (data >> 6) & 1;
					_dac.forceLayer = (data >> 7) & 3;
					_sprite.disablePhase1 = (data >> 12) & 1;
					_sprite.disablePhase2 = (data >> 13) & 1;
					_sprite.disablePhase3 = (data >> 14) & 1;
					break;
			}
			break;
		}
	}
}

uint16_t GenesisVdp::ReadDataPort()
{
	_command.latch = 0;
	_command.ready = 0;
	//When reading CRAM/VSRAM, force-complete any pending CRAM/VSRAM DMA.
	//DrainFifo normally skips CRAM DMA to preserve Direct Color DMA entries
	//for scanline processing. But when the M68K reads CRAM/VSRAM, the DMA
	//must complete first so the read returns the DMA-written data.
	//Note: read targets are 8 (CRAM) and 4 (VSRAM), but DMA write targets
	//are 3 (CRAM) and 5 (VSRAM). Check _dma.target for the DMA write target.
	if(_command.pending && (_dma.target == 3 || _dma.target == 5)) {
		DrainFifo(true);
	}
	//Clear latency on all FIFO entries to allow processing during the prefetch
	//loop below. In ares's co-routine model, the M68K's readDataPort() yields
	//via cpu.wait(1), letting the VDP tick latency down and process entries
	//before the prefetch reads. In Mesen2's sequential model, latency is only
	//decremented during RunScanline, so without this, FIFO entries would block
	//in FIFO::Run() with latency>0, leaving VSRAM/CRAM/VRAM reads stale and
	//fifo.slots[0].data holding unprocessed entry data instead of the correct
	//stale value from previously-processed entries.
	for(auto& s : _fifo.slots) {
		if(!s.empty()) {
			s.latency = 0;
		}
	}
	while(!_prefetch.full()) {
		Slot();
	}
	_command.address = (_command.address + _command.increment) & 0x1FFFF;
	_command.ready = 0;
	_prefetch.Read(_command.target, _command.address);
	return _prefetch.slot.data;
}

void GenesisVdp::WriteDataPort(uint16_t data)
{
	_command.latch = 0;
	_command.ready = 1;

	//Simulate VDP progress when DMA Fill is active.
	//In ares's co-routine model, the M68K and VDP run concurrently: while
	//the M68K executes a delay loop between the seed write and the update
	//write, the VDP processes slots — draining the seed FIFO entry and
	//running several DMA Fill iterations. When the M68K finally writes the
	//data port, the FIFO seed entry has already been processed (updating
	//_dma.data via FIFO::Advance) and the DMA Fill has progressed.
	//In Mesen2's sequential model, the VDP runs a full scanline BEFORE the
	//M68K, so DMA Fill doesn't progress between M68K writes. Both FIFO
	//entries (seed + update) are processed at once during status register
	//polling, causing the DMA Fill to use only the last data value.
	//Fix: simulate VDP slot progress proportional to M68K cycles elapsed
	//since the last VDP access. This drains the seed FIFO entry (updating
	//_dma.data) and runs several DMA Fill iterations before the new write.
	//Must happen BEFORE saving target/address, so the new FIFO entry is
	//created at the current DMA Fill position (synced from _dma.address).
	if(_command.pending && _dma.mode == 2 && _m68kCyclesPerScanline > 0) {
		//Clear latency and preload so FIFO entries can be processed
		for(auto& s : _fifo.slots) {
			if(!s.empty()) {
				s.latency = 0;
			}
		}
		if(_dma.active && _dma.preload > 0) {
			_dma.preload = 0;
		}

		//Accumulate cycle credit for elapsed M68K cycles
		int32_t cycleDiff = (int32_t)_m68kCycleInScanline - (int32_t)_lastFifoDrainCycle;
		if(cycleDiff < 0) {
			_fifoDrainCredit = 0;
		} else {
			_fifoDrainCredit += cycleDiff;
		}
		_lastFifoDrainCycle = _m68kCycleInScanline;

		//Process slots: each slot is ~(_m68kCyclesPerScanline/18) cycles.
		//Slot order matches ares: if rambusy, clear it (slot consumed);
		//else if FIFO has data, run 1 byte (sets rambusy); else if DMA Fill
		//can run, call Fill (sets rambusy). This rambusy alternation makes
		//DMA Fill run every OTHER slot (1 fill per ~54 M68K cycles).
		uint32_t cyclesPerSlot = _m68kCyclesPerScanline / 18;
		if(cyclesPerSlot == 0) {
			cyclesPerSlot = 1;
		}
		int safety = 0;
		while(_fifoDrainCredit >= (int32_t)cyclesPerSlot && safety < 0x40000) {
			if(_state.rambusy) {
				_state.rambusy = 0;
				_fifoDrainCredit -= cyclesPerSlot;
				safety++;
				continue;
			}
			if(!_fifo.empty()) {
				if(_fifo.Run(*this)) {
					_state.rambusy = 1;
					_fifoDrainCredit -= cyclesPerSlot;
					safety++;
					continue;
				}
				break;
			}
			if(_command.pending && !_dma.wait) {
				_dma.Fill(*this);
				_fifoDrainCredit -= cyclesPerSlot;
				safety++;
				continue;
			}
			break;
		}
		_state.rambusy = 0;

		//Sync command.address with DMA address so the new FIFO entry is
		//created at the current DMA Fill position. In ares, DMA Fill uses
		//vdp.command.address directly, so this sync is implicit. In Mesen2,
		//_dma.address is saved at DMA trigger time and incremented by Fill,
		//while _command.address was only incremented by the seed write.
		//Only sync when _dma.wait == 0 (DMA Fill has started). When wait == 1
		//(seed not yet processed), the M68K may have modified _command.address
		//via partial CP writes between DMA trigger and seed write — syncing
		//would overwrite those modifications with the stale trigger address.
		//(TestDMAFillControlPortWrites sub-test 6: partial CP write changes
		//address from 0x8002 to 0x8008, but sync would revert it to 0x8002.)
		if(_command.pending && !_dma.wait) {
			_command.address = _dma.address;
		}
	}

	//Save command address/target before any FIFO operation, because DMA
	//execution modifies _command.address and potentially _command.target.
	uint8_t target = _command.target;
	uint32_t address = _command.address;
	//On real hardware (ares fifo.cpp write()), when the FIFO is full and
	//the 68K writes to the data port, the VDP holds the 68K bus while it
	//processes slots until exactly 1 FIFO entry completes (Advance called),
	//freeing 1 slot. The 68K write then fills that slot, so the FIFO
	//remains full. We must drain only 1 entry, not all — draining all
	//makes the FIFO appear empty in the status register, which is wrong.
	if(_fifo.full()) {
		_busPenalty += 16;
		//Clear latency on all entries (they've had time to expire)
		for(auto& s : _fifo.slots) {
			if(!s.empty()) {
				s.latency = 0;
			}
		}
		//Clear DMA preload to allow FIFO drain (same as DrainFifo)
		if(_dma.active && _dma.preload > 0) {
			_dma.preload = 0;
		}
		//Drain exactly 1 entry: call Run() until Advance() fires and
		//shifts the FIFO, freeing slot 3. For VRAM mode 0, this takes
		//2 Run() calls (lower byte, then upper byte + Advance).
		//For CRAM/VSRAM, 1 call suffices.
		int safety = 0;
		while(_fifo.full() && safety < 16) {
			if(!_fifo.Run(*this)) {
				break;
			}
			safety++;
		}
		_state.rambusy = 0;
	}
	_fifo.Write(target, address, data);
	_command.address = (_command.address + _command.increment) & 0x1FFFF;
}

uint16_t GenesisVdp::ReadControlPort()
{
	_command.latch = 0;
	//In ares, the VDP runs concurrently with the M68K, processing slots
	//that drain FIFO entries. In our sequential model, the FIFO is frozen
	//during M68K execution. We simulate VDP progress by draining FIFO
	//bytes proportional to M68K cycles elapsed since the last VDP access.
	//The VDP processes ~18 slots per scanline (one byte per slot), so
	//1 byte drains per ~(_m68kCyclesPerScanline/18) M68K cycles.
	for(auto& s : _fifo.slots) {
		if(!s.empty()) {
			s.latency = 0;
		}
	}
	//Decrement DMA preload to simulate time passing (ares main.cpp line 70)
	if(_dma.active && _dma.preload > 0) {
		_dma.preload--;
	}
	//Update display enable latch before building status register.
	if(_latch.displayEnable > _io.displayEnable || _fifo.empty()) {
		_latch.displayEnable = _io.displayEnable;
	}
	//Cycle-based FIFO drain: accumulate M68K cycle credit and drain bytes
	//proportional to elapsed time. This prevents the FIFO from draining
	//too fast (1 byte per status read) which breaks FIFO wait state tests.
	if(_m68kCyclesPerScanline > 0) {
		int32_t cycleDiff = (int32_t)_m68kCycleInScanline - (int32_t)_lastFifoDrainCycle;
		if(cycleDiff < 0) {
			//New scanline — reset credit (VDP already processed this scanline's slots)
			_fifoDrainCredit = 0;
		} else {
			_fifoDrainCredit += cycleDiff;
		}
		_lastFifoDrainCycle = _m68kCycleInScanline;
		uint32_t cyclesPerByte = _m68kCyclesPerScanline / 18;
		if(cyclesPerByte > 0) {
			while(_fifoDrainCredit >= (int32_t)cyclesPerByte && !_fifo.empty()) {
				if(!_fifo.Run(*this)) {
					_fifoDrainCredit = 0;
					break;
				}
				_fifoDrainCredit -= cyclesPerByte;
				_state.rambusy = 0;
			}
		}
	} else {
		if(_fifo.Run(*this)) {
			_state.rambusy = 0;
		}
	}
	//Advance DMA Fill even when FIFO is empty (ongoing Fill iterations).
	//After Advance() sets _dma.wait=0, subsequent Fill calls must proceed
	//on each status register read until DMA length reaches 0.
	if(_command.pending && !_dma.wait && _dma.mode == 2 && _fifo.empty() && !_state.rambusy) {
		_dma.Fill(*this);
		_state.rambusy = 0;
	}
	//Advance DMA Copy during status reads. Copy writes directly to VRAM
	//(not through FIFO), so without this it only progresses during
	//RunScanline's TickAndSlot calls. Processing one Copy step per status
	//read ensures the DMA completes within the test's wait window.
	if(_command.pending && !_dma.wait && _dma.mode == 3 && !_state.rambusy) {
		_dma.Copy(*this);
		_state.rambusy = 0;
	}
	uint16_t result = 0;
	result |= (_region == ConsoleRegion::Pal) ? 1 : 0;
	//Bit 1 = DMA pending (ares: command.pending). This is the "DMA busy"
	//flag polled by software to wait for DMA completion. Mesen2 previously
	//returned _state.hblankOccurred here, which is incorrect — hblankOccurred
	//is an internal flag (set when HBlank transitions, cleared on read)
	//and not part of the status register. The bug caused the Direct Color
	//DMA test ROM's wait-for-DMA loop to exit prematurely.
	result |= (_command.pending & 1) << 1;
	//Compute virtual hblank from M68K cycle position within the scanline.
	//In Mesen2's sequential model, the VDP has already processed the full
	//scanline, so _state.hblank is frozen at its end-of-scanline value (1).
	//We map the M68K cycle position to an hcounter tick to derive the
	//correct hblank state the M68K would see if VDP ran concurrently.
	//H40: 210 ticks/scanline; hblank clears at tick 5, sets at tick 179.
	//H32: 171 ticks/scanline; hblank clears at tick 5, sets at tick 147.
	uint8_t virtualHblank = _state.hblank;
	if(_m68kCyclesPerScanline > 0) {
		uint32_t totalTicks = H40() ? 210 : 171;
		uint32_t hblankSetTick = H40() ? 179 : 147;
		uint32_t virtualTick = (uint64_t)_m68kCycleInScanline * totalTicks / _m68kCyclesPerScanline;
		virtualHblank = (virtualTick < 5 || virtualTick >= hblankSetTick) ? 1 : 0;
	}
	result |= (virtualHblank & 1) << 2;
	result |= ((_state.vblank || !IsDisplayEnable()) ? 1 : 0) << 3;
	result |= ((_io.interlaceMode & 1) && _state.field) ? (1 << 4) : 0;
	result |= (_sprite.collision & 1) << 5;
	result |= (_sprite.overflow & 1) << 6;
	result |= (_irq.vblank.pending & 1) << 7;
	result |= (_fifo.full() ? 1 : 0) << 8;
	result |= (_fifo.empty() ? 1 : 0) << 9;
	if(_m68k) {
		result |= _m68k->GetIrc() & 0xFC00;
	}
	_sprite.collision = 0;
	_sprite.overflow = 0;
	//Only clear VBlank pending on status read when the M68K has interrupts
	//masked (IPL >= 6). When IPL >= 6, the VBlank IRQ (level 6) cannot be
	//delivered, so the M68K must use status polling to detect VBlank —
	//clearing pending allows the polling loop to see bit 7 transition.
	//When IPL < 6, the IRQ will be delivered via CheckInterrupts on the
	//next instruction, so pending must be preserved. This prevents IRQ
	//loss when the M68K reads VDP status inside an HBlank ISR (IPL=4)
	//just before VBlank would preempt — the original unconditional clear
	//lost the VBlank IRQ in that case, causing the TiTAN Overdrive
	//screen-freeze regression.
	if(_m68k && _m68k->GetInterruptMask() >= 6) {
		_irq.vblank.pending = 0;
	}
	return result;
}

void GenesisVdp::WriteControlPort(uint16_t data)
{
	//Pre-write DrainFifo removed: draining before the control port write
	//would flush FIFO entries (including the DMA Fill value) before the
	//DMA trigger can process them. The FIFO now retains entries until
	//drained by the post-LATCH2 DrainFifo (for DMA trigger) or by
	//RunScanline (for normal rendering).

	//If a DMA is still pending (e.g., CRAM DMA was skipped by DrainFifo
	//to preserve Direct Color DMA entries), complete it now before the
	//new command modifies _command state. In ares, the DMA runs
	//concurrently and completes before the M68K writes a new command.
	//Use forceCram=true to ensure CRAM DMA completes.
	if(_command.pending) {
		DrainFifo(true);
	}

	if(_command.latch) {
		_command.latch = 0;
		_command.address = (_command.address & 0x3FFF) | ((data & 7) << 14);
		_command.target = (_command.target & 0x03) | (((data >> 4) & 3) << 2);
		_command.ready = ((data >> 6) & 1) | (_command.target & 1);
		uint8_t prevPending = _command.pending;
		_command.pending |= ((data >> 7) & 1) & _dma.enable;
		//When DMA is newly triggered, save target/address/increment.
		//In ares, M68K is blocked during DMA so command.target/address
		//can't change. In Mesen2's sequential model, M68K runs before
		//VDP and can modify these values between DMA setup and VDP
		//execution, corrupting the DMA destination.
		if(_command.pending && !prevPending) {
			_dma.target = _command.target;
			_dma.address = _command.address;
			_dma.increment = _command.increment;
		}
		_prefetch.Read(_command.target, _command.address);
		if(_command.pending && _dma.mode != 2) {
			if(_dma.mode < 2) {
				_dma.preload = 7;
			}
			_dma.read = 0;
			_dma.wait = 0;
		}
		_dma.Synchronize(*this);
		//In ares, M68K and VDP run as co-routines: after M68K writes the control
		//port, it yields and the VDP gets time to process DMA. In our sequential
		//model, we must call DrainFifo here to give the VDP time to start the DMA.
		//Without this, DMA mode 0/1 (memory-to-VDP) never executes because
		//WriteControlPort is the only place that triggers it (via pending=1).
		//Only force-complete DMA Load (mode 0/1) here. DMA Fill (mode 2) and
		//Copy (mode 3) must remain pending so the DMA busy flag (bit 1) is set
		//in subsequent status reads — VDPFIFOTesting expects bit 1=1 immediately
		//after a Copy/Fill trigger. Mode 2 Fill is advanced by ReadControlPort's
		//Fill handler; mode 3 Copy is processed during RunScanline's TickAndSlot.
		if(_dma.mode <= 1) {
			DrainFifo();
		}
		return;
	}

	_command.target = (_command.target & 0x0C) | ((data >> 14) & 3);
	_command.ready = 1;

	if(((data >> 14) & 3) != 2) {
		_command.address = (_command.address & ~0x3FFF) | (data & 0x3FFF);
		_command.latch = 1;
		return;
	}

	uint8_t reg = (data >> 8) & 0x1F;
	if(!_io.videoMode5 && reg > 0x0A) {
		return;
	}

	switch(reg) {
		case 0x00:
			_io.displayOverlayEnable = data & 1;
			if(!_io.counterLatch && (data & 2)) {
				_state.counterLatchValue = ComputeVirtualHvCounter();
			}
			_io.counterLatch = (data >> 1) & 1;
			_io.videoMode4 = (data >> 2) & 1;
			_irq.hblank.enable = (data >> 4) & 1;
			_irq.delay = H40() ? 3 : 2;
			_io.leftColumnBlank = (data >> 5) & 1;
			break;
		case 0x01: {
			_io.videoMode5 = (data >> 2) & 1;
			_io.overscan = (data >> 3) & 1;
			_dma.enable = (data >> 4) & 1;
			_irq.vblank.enable = (data >> 5) & 1;
			_irq.delay = H40() ? 3 : 2;
			_io.displayEnable = (data >> 6) & 1;
			_vramMode = (data >> 7) & 1;
			VblankCheck();
			break;
		}
		case 0x02: _layerA.nametableAddress = (_layerA.nametableAddress & 0x0FFF) | ((data & 0x78) << 9); break;
		case 0x03: _window.nametableAddress = (_window.nametableAddress & 0x03FF) | ((data & 0x7E) << 9); break;
		case 0x04: _layerB.nametableAddress = (_layerB.nametableAddress & 0x0FFF) | ((data & 0x0F) << 12); break;
		case 0x05: _sprite.nametableAddress = (_sprite.nametableAddress & 0x00FF) | ((data & 0xFF) << 8); break;
		case 0x06: _sprite.generatorAddress = (_sprite.generatorAddress & 0x7FFF) | ((data & 0x20) << 10); break;
		case 0x07: _io.backgroundColor = data & 0x3F; break;
		case 0x0A: _irq.hblank.frequency = data & 0xFF; break;
		case 0x0B:
			_layers.hscrollMode = data & 3;
			_layers.vscrollMode = (data >> 2) & 1;
			_irq.external.enable = (data >> 3) & 1;
			break;
		case 0x0C:
			_io.displayWidth = data & 1;
			_io.interlaceMode = (data >> 1) & 3;
			_io.shadowHighlightEnable = (data >> 3) & 1;
			_io.externalColorEnable = (data >> 4) & 1;
			_io.hsync = (data >> 5) & 1;
			_io.vsync = (data >> 6) & 1;
			_io.clockSelect = (data >> 7) & 1;
			break;
		case 0x0D: _layers.hscrollAddress = (data & 0x7F) << 9; break;
		case 0x0E:
			_layerA.generatorAddress = (_layerA.generatorAddress & 0x7FFF) | ((data & 1) << 15);
			_layerB.generatorAddress = (_layerB.generatorAddress & 0x7FFF) | (((data & 0x11) == 0x11) ? 0x8000 : 0);
			break;
		case 0x0F: _command.increment = data & 0xFF; break;
		case 0x10:
			_layers.nametableWidth = data & 3;
			_layers.nametableHeight = (data >> 4) & 3;
			break;
		case 0x11:
			_window.io.hoffset = (data & 0x1F) << 4;
			_window.io.hdirection = (data >> 7) & 1;
			break;
		case 0x12:
			_window.io.voffset = (data & 0x1F) << 3;
			_window.io.vdirection = (data >> 7) & 1;
			break;
		case 0x13: _dma.length = (_dma.length & 0xFF00) | (data & 0xFF); break;
		case 0x14: _dma.length = (_dma.length & 0x00FF) | ((data & 0xFF) << 8); break;
		case 0x15: _dma.source = (_dma.source & 0x3FFF00) | (data & 0xFF); break;
		case 0x16: _dma.source = (_dma.source & 0x3F00FF) | ((data & 0xFF) << 8); break;
		case 0x17:
			_dma.source = (_dma.source & 0x00FFFF) | ((data & 0x3F) << 16);
			_dma.mode = (data >> 6) & 3;
			break;
	}
}

//--- IRQ interface ---

bool GenesisVdp::GetVblankIrq() const
{
	return _irq.vblank.enable && _irq.vblank.pending && !_irq.delay;
}
bool GenesisVdp::GetHblankIrq() const
{ return _irq.hblank.enable && _irq.hblank.pending; }
bool GenesisVdp::GetExternalIrq() const
{ return _irq.external.enable && _irq.external.pending; }

void GenesisVdp::AcknowledgeIrq(uint8_t level)
{
	if(level == 2) {
		_irq.external.pending = 0;
	}
	//Level 4 ack = HBlank interrupt. Always clear HBlank pending.
	//Previously this checked VBlank pending first and cleared VBlank instead,
	//which caused VBlank ISR to never fire when HBlank preempted VBlank during
	//the delay window (e.g., after M68K writes to VDP mode registers which
	//set _irq.delay). This hung Comix Zone's fade effect.
	if(level == 4) {
		_irq.hblank.pending = 0;
	}
	if(level == 6 && _irq.vblank.enable) {
		_irq.vblank.pending = 0;
	}
}

//--- VRAM access ---

uint16_t GenesisVdp::VramReadWord(const uint16_t* vram, uint8_t mode, uint16_t address)
{
	if(mode == 0) {
		return vram[address & 0x7FFF];
	}
	uint16_t offset = (address >> 1 & 0x7E00) | (address & 0x01FE) | (address >> 9 & 1);
	uint8_t data = vram[offset & 0x7FFF] >> ((address & 1) ? 0 : 8);
	return data | (data << 8);
}

void GenesisVdp::VramWriteWord(uint16_t* vram, uint8_t mode, uint16_t address, uint16_t data)
{
	if(mode == 0) {
		vram[address & 0x7FFF] = data;
	} else {
		uint16_t offset = (address >> 1 & 0x7E00) | (address & 0x01FE) | (address >> 9 & 1);
		if(address & 1) {
			vram[offset & 0x7FFF] = (vram[offset & 0x7FFF] & 0xFF00) | (data & 0xFF);
		} else {
			vram[offset & 0x7FFF] = (vram[offset & 0x7FFF] & 0x00FF) | ((data & 0xFF) << 8);
		}
	}
}

uint8_t GenesisVdp::VramReadByte(const uint16_t* vram, uint8_t mode, uint32_t address)
{
	uint16_t word = VramReadWord(vram, mode, (uint16_t)(address >> 1));
	return (address & 1) ? (word & 0xFF) : (word >> 8);
}

void GenesisVdp::VramWriteByte(uint16_t* vram, uint8_t mode, uint32_t address, uint8_t data)
{
	uint16_t word = VramReadWord(vram, mode, (uint16_t)(address >> 1));
	if(address & 1) {
		word = (word & 0xFF00) | data;
	} else {
		word = (word & 0x00FF) | (data << 8);
	}
	VramWriteWord(vram, mode, (uint16_t)(address >> 1), word);
}

//--- FIFO ---

//Drain all FIFO entries and run DMA to completion.
//Called before M68K reads/writes VDP to simulate the interleaved
//execution model where VDP processes FIFO entries and DMA while M68K is running.
//In ares, M68K and VDP run as co-routines: when M68K yields, VDP runs
//slots that drain FIFO and advance DMA. In our sequential model, we must
//do this eagerly before each VDP access.
void GenesisVdp::DrainFifo(bool forceCram)
{
	//Track whether DMA was pending at entry — used to sync command
	//address after DMA completion.
	bool dmaWasPending = _command.pending;

	//Clear latency on all FIFO entries (they've had time to expire)
	for(auto& s : _fifo.slots) {
		if(!s.empty()) {
			s.latency = 0;
		}
	}
	//Clear DMA preload — DrainFifo simulates enough VDP slots for
	//all pending operations to complete. The preload counter blocks
	//FIFO::Run(), so we must clear it to allow FIFO drain and DMA
	//Load to proceed.
	if(_dma.active && _dma.preload > 0) {
		_dma.preload = 0;
	}

	//Run FIFO drain + DMA in a loop until both are idle.
	//This simulates multiple VDP slots running between M68K accesses.
	int safety = 0;
	while(safety < 0x40000) {
		bool progress = false;

		//Clear latency on all FIFO entries before each Run call.
		//DMA::Load adds entries with latency=2 (from FIFO::Write),
		//which blocks FIFO::Run. Since DrainFifo simulates enough
		//time for all operations to complete, latency is irrelevant.
		for(auto& s : _fifo.slots) {
			if(!s.empty()) {
				s.latency = 0;
			}
		}

		//Drain one FIFO entry.
		//Skip CRAM-targeted entries (target==3) unless forceCram is set.
		//CRAM entries need scanline processing for Direct Color DMA —
		//each CRAM[0] write must be processed one-at-a-time during
		//TickAndSlot's dac.dot() calls. DrainFifo processing them all
		//at once would leave only the last value in _cramBusData.
		bool skipFifo = false;
		if(!_fifo.slots[0].empty() && _fifo.slots[0].target == 3 && !forceCram) {
			skipFifo = true;
		}
		if(!skipFifo && _fifo.Run(*this)) {
			progress = true;
			_state.rambusy = 0; //Simulate slot completing
			continue; //FIFO had data, drain more first
		}

		//If DMA is pending, try to advance it
		if(_command.pending && !_dma.wait) {
			//For mode 0/1 (bus->VDP), need to fetch + load
			if(_dma.mode <= 1) {
				_dma.Synchronize(*this);
				if(_dma.active) {
					//Skip DMA Load for CRAM target (target==3) unless forceCram.
					//CRAM DMA Load entries must remain in FIFO for scanline
					//processing — Direct Color DMA mechanism relies on each
					//CRAM[0] write being processed one-at-a-time during
					//TickAndSlot's dac.dot() calls. If DrainFifo processes
					//them all upfront, only the last value survives in
					//_cramBusData and the bitmap is not displayed.
					//CRAM/VSRAM reads via ReadDataPort use forceCram=true.
					if(_dma.target == 3 && !forceCram) {
						//Don't advance CRAM DMA here — let it flow through FIFO
					} else {
						//Fetch DMA source data if needed
						if(!_dma.read) {
							auto address = ((_dma.mode & 1) << 23) | (_dma.source << 1);
							_dma.data = DmaRead(address);
							_dma.read = 1;
						}
						//Run DMA Load (writes to FIFO)
						if(!_fifo.full() && _dma.read) {
							_dma.Load(*this);
							progress = true;
							_state.rambusy = 0;
							continue;
						}
					}
				}
			}
			//For mode 2 (fill), writes directly to VRAM/CRAM/VSRAM
			else if(_dma.mode == 2) {
				if(_fifo.empty() && !_state.rambusy) {
					_dma.Fill(*this);
					progress = true;
					_state.rambusy = 0; //Simulate slot completing
					continue;
				}
			}
			//For mode 3 (copy), reads VRAM then writes VRAM
			else if(_dma.mode == 3) {
				if(!_state.rambusy) {
					_dma.Copy(*this);
					progress = true;
					_state.rambusy = 0; //Simulate slot completing
					continue;
				}
			}
		}

		if(!progress) {
			break;
		}
		safety++;
	}

	//If DMA completed during this drain, sync command address with DMA address.
	//In ares, DMA uses vdp.command.address directly, so after DMA completes,
	//command.address reflects the final DMA address. In Mesen2, DMA uses a
	//separate _dma.address (saved at DMA trigger time to prevent M68K from
	//corrupting it during sequential execution), so we must sync back to
	//_command.address for subsequent data port writes to go to the correct
	//address. This is required for TestDMATransferBusLock where M68K issues
	//interleaved long-word writes (low word to control port triggers DMA,
	//high word to data port must go to the post-DMA address).
	if(dmaWasPending && !_command.pending) {
		_command.address = _dma.address;
	}

	//Clear rambusy since all pending operations are done
	_state.rambusy = 0;
}

void GenesisVdp::FIFO::Tick()
{
	for(auto& s : slots) {
		if(!s.empty() && s.latency > 0) {
			s.latency--;
		}
	}
}

void GenesisVdp::FIFO::Advance(GenesisVdp& vdp)
{
	if(vdp._command.pending && vdp._dma.mode == 2) {
		if(slots[0].target == 1) {
			vdp._dma.data = slots[0].data;
		} else {
			vdp._dma.data = slots[1].data;
		}
		//Sync DMA state from command state.
		//In ares, DMA::Fill uses vdp.command.target/address/increment
		//directly (no saved copies). In Mesen2, we save copies at DMA
		//trigger time to prevent M68K corruption in the sequential model,
		//but this breaks cases where the M68K intentionally modifies
		//registers between DMA trigger and seed write.
		//
		//TestDMAFillControlPortWrites sub-tests 3-6 exploit this:
		//- Sub-test 3: register write $8F02 sets target=2 (invalid, because
		//  writeControlPort modifies target before the register write path).
		//  The fill runs without writing (switch falls through), but length
		//  still decrements. This matches hardware behavior.
		//- Sub-tests 4/5/6: register write $8F02 sets increment=2. The fill
		//  must use the new increment value.
		//
		//Syncing here (when the seed FIFO entry is processed by Advance)
		//ensures the DMA fill sees the current command state, matching ares.
		vdp._dma.address = vdp._command.address;
		vdp._dma.target = vdp._command.target;
		vdp._dma.increment = vdp._command.increment;
		vdp._dma.read = 1;
		vdp._dma.wait = 0;
	}
	std::swap(slots[0], slots[1]);
	std::swap(slots[1], slots[2]);
	std::swap(slots[2], slots[3]);
}

bool GenesisVdp::FIFO::Run(GenesisVdp& vdp)
{
	if(empty()) {
		return false;
	}
	if(slots[0].latency > 0) {
		return false;
	}
	if(vdp._dma.active && vdp._dma.preload > 0) {
		return false;
	}

	if(slots[0].target == 1 && vdp._vramMode == 0) {
		if(slots[0].lower) {
			slots[0].lower = 0;
			VramWriteByte(vdp._vram, vdp._vramMode, slots[0].address ^ 1, slots[0].data & 0xFF);
			//ares VRAM::writeByte → VRAM::write → sprite.write — update sprite cache
			vdp._sprite.VramWrite(vdp, slots[0].address >> 1, VramReadWord(vdp._vram, vdp._vramMode, slots[0].address >> 1));
			return true;
		}
		if(slots[0].upper) {
			slots[0].upper = 0;
			VramWriteByte(vdp._vram, vdp._vramMode, slots[0].address, slots[0].data >> 8);
			vdp._sprite.VramWrite(vdp, slots[0].address >> 1, VramReadWord(vdp._vram, vdp._vramMode, slots[0].address >> 1));
			Advance(vdp);
			return true;
		}
	}

	if(slots[0].target == 3) {
		//ares CRAM::write(n6 address, ...) — address is 6-bit, wraps naturally.
		//Mesen2 must explicitly mask with 0x3F to mimic n6 behavior.
		//Without this, DMA writes to CRAM addr >= 64 are skipped, so only
		//the first 128 words (64 pairs) of a 44352-word DMA land in CRAM.
		uint16_t addr = (slots[0].address >> 1) & 0x3F;
		uint16_t cramVal = ((slots[0].data >> 1) & 7) | (((slots[0].data >> 5) & 7) << 3) | (((slots[0].data >> 9) & 7) << 6);
		vdp._cram[addr] = cramVal;
		vdp._cramBusData = cramVal;
		vdp._cramBusActive = true;
	} else if(slots[0].target == 5) {
		//ares VSRAM::write(n6 address, ...) — address is 6-bit, wraps naturally.
		//Addresses >= 40 are ignored (VSRAM only has 40 entries).
		uint16_t addr = (slots[0].address >> 1) & 0x3F;
		if(addr < VSRAMSize) {
			vdp._vsram[addr] = slots[0].data & 0x7FF;
		}
	} else if(slots[0].target == 1 && vdp._vramMode == 1) {
		VramWriteByte(vdp._vram, vdp._vramMode, slots[0].address | 1, slots[0].data & 0xFF);
		vdp._sprite.VramWrite(vdp, slots[0].address >> 1, VramReadWord(vdp._vram, vdp._vramMode, slots[0].address >> 1));
	}

	slots[0].lower = 0;
	slots[0].upper = 0;
	Advance(vdp);
	return true;
}

void GenesisVdp::FIFO::Write(uint8_t target, uint32_t address, uint16_t data)
{
	for(auto& s : slots) {
		if(s.empty()) {
			s.target = target;
			s.address = address;
			s.data = data;
			s.upper = 1;
			s.lower = 1;
			s.latency = 2;
			return;
		}
	}
}

void GenesisVdp::FIFO::Power()
{
	for(auto& s : slots) {
		s = {};
	}
}

//--- Prefetch ---

bool GenesisVdp::Prefetch::Run(GenesisVdp& vdp)
{
	if(full()) {
		return false;
	}

	if(vdp._command.target == 0 && vdp._vramMode == 0) {
		if(!slot.lower) {
			slot.lower = 1;
			slot.data = (slot.data & 0xFF00) | VramReadByte(vdp._vram, vdp._vramMode, (vdp._command.address & ~1) | 1);
			return true;
		}
		if(!slot.upper) {
			slot.data = (slot.data & 0x00FF) | (VramReadByte(vdp._vram, vdp._vramMode, vdp._command.address & ~1) << 8);
			slot.upper = 1;
			vdp._command.ready = 1;
			return true;
		}
	}

	if(vdp._command.target == 0 && vdp._vramMode == 1) {
		slot.lower = 1;
		slot.upper = 1;
		uint8_t b = VramReadByte(vdp._vram, vdp._vramMode, vdp._command.address | 1);
		slot.data = b | (b << 8);
		vdp._command.ready = 1;
		return true;
	}

	if(vdp._command.target == 4) {
		slot.lower = 1;
		slot.upper = 1;
		//ares VSRAM::read(n6 address) — n6 masks to 6 bits (0-63),
		//then wraps addresses >= 40 to 0. Without this mask, reads at
		//command addresses >= 0x80 go out of bounds and return 0.
		uint16_t addr = (vdp._command.address >> 1) & 0x3F;
		if(addr >= VSRAMSize) {
			addr = 0;
		}
		slot.data = vdp._vsram[addr] & 0x7FF;
		slot.data |= vdp._fifo.slots[0].data & 0xF800;
		vdp._command.ready = 1;
		return true;
	}

	if(vdp._command.target == 8) {
		slot.lower = 1;
		slot.upper = 1;
		//ares CRAM::read(n6 address) — n6 masks to 6 bits (0-63).
		//Without this mask, reads at command addresses >= 0x80 go out
		//of bounds and return 0, mismatching the write path which masks.
		uint16_t addr = (vdp._command.address >> 1) & 0x3F;
		uint16_t cramR = vdp._cram[addr];
		slot.data = ((cramR & 7) << 1) | (((cramR >> 3) & 7) << 5) | (((cramR >> 6) & 7) << 9);
		slot.data = slot.data & 0x0EEE | vdp._fifo.slots[0].data & ~0x0EEE;
		vdp._command.ready = 1;
		return true;
	}

	if(vdp._command.target == 12) {
		slot.lower = 1;
		slot.upper = 1;
		slot.data = VramReadByte(vdp._vram, vdp._vramMode, vdp._command.address ^ 1);
		slot.data |= vdp._fifo.slots[0].data & 0xFF00;
		vdp._command.ready = 1;
		return true;
	}

	slot.lower = 1;
	slot.upper = 1;
	vdp._command.ready = 1;
	return true;
}

void GenesisVdp::Prefetch::Read(uint8_t target, uint32_t address)
{
	if(target & 1) {
		return;
	}
	slot.upper = 0;
	slot.lower = 0;
}

void GenesisVdp::Prefetch::Power()
{
	slot = {};
	slot.upper = 1;
	slot.lower = 1;
}

//--- DMA ---

void GenesisVdp::DMA::Synchronize(GenesisVdp& vdp)
{
	if(vdp._command.pending && !wait && mode <= 1) {
		active = 1;
	} else {
		active = 0;
	}
}

void GenesisVdp::DMA::Fetch(GenesisVdp& vdp)
{
	if(active && !read) {
		auto address = ((mode & 1) << 23) | (source << 1);
		data = vdp.DmaRead(address);
		read = 1;
	}
}

bool GenesisVdp::DMA::Run(GenesisVdp& vdp)
{
	if(!vdp._command.pending || wait) {
		return false;
	}
	if(mode <= 1 && !vdp._fifo.full() && read) {
		Load(vdp);
		return true;
	} else if(mode == 2 && vdp._fifo.empty() && !vdp._state.rambusy) {
		Fill(vdp);
		return true;
	} else if(mode == 3 && !vdp._state.rambusy) {
		Copy(vdp);
		return true;
	}
	return false;
}

void GenesisVdp::DMA::Load(GenesisVdp& vdp)
{
	read = 0;
	vdp._fifo.Write(target, address, data);
	source = (source & 0x3F0000) | ((source + 1) & 0xFFFF);
	address = (address + increment) & 0x1FFFF;
	if(--length == 0) {
		vdp._command.pending = 0;
		wait = 1;
		preload = 0;
		//Sync command address with final DMA address. DrainFifo does
		//this for non-CRAM DMA, but CRAM DMA now completes naturally
		//in RunScanline (not via DrainFifo), so we must sync here.
		vdp._command.address = address;
		Synchronize(vdp);
	}
}

void GenesisVdp::DMA::Fill(GenesisVdp& vdp)
{
	switch(target) {
		case 1:
			VramWriteByte(vdp._vram, vdp._vramMode, address ^ 1, data >> 8);
			vdp._sprite.VramWrite(vdp, address >> 1, VramReadWord(vdp._vram, vdp._vramMode, address >> 1));
			break;
		case 3: {
			uint16_t addr = (address >> 1) & 0x3F;
			uint16_t cramVal = ((data >> 1) & 7) | (((data >> 5) & 7) << 3) | (((data >> 9) & 7) << 6);
			vdp._cram[addr] = cramVal;
			break;
		}
		case 5: {
			uint16_t addr = (address >> 1) & 0x3F;
			if(addr < VSRAMSize) {
				vdp._vsram[addr] = data & 0x7FF;
			}
			break;
		}
	}
	vdp._state.rambusy = 1;
	source = (source & 0x3F0000) | ((source + 1) & 0xFFFF);
	address = (address + increment) & 0x1FFFF;
	if(--length == 0) {
		vdp._command.pending = 0;
		wait = 1;
		Synchronize(vdp);
	}
}

void GenesisVdp::DMA::Copy(GenesisVdp& vdp)
{
	if(!read) {
		read = 1;
		data = VramReadByte(vdp._vram, vdp._vramMode, source ^ 1);
		vdp._state.rambusy = 1;
		return;
	}
	read = 0;
	VramWriteByte(vdp._vram, vdp._vramMode, address ^ 1, data & 0xFF);
	vdp._sprite.VramWrite(vdp, address >> 1, VramReadWord(vdp._vram, vdp._vramMode, address >> 1));
	vdp._state.rambusy = 1;
	source = (source & 0x3F0000) | ((source + 1) & 0xFFFF);
	address = (address + increment) & 0x1FFFF;
	if(--length == 0) {
		vdp._command.pending = 0;
		wait = 1;
		Synchronize(vdp);
	}
}

void GenesisVdp::DMA::Power()
{
	active = 0;
	mode = 0;
	source = 0;
	length = 0;
	data = 0;
	wait = 1;
	read = 0;
	enable = 0;
	preload = 0;
	target = 0;
	address = 0;
	increment = 0;
}

//--- Layers ---

void GenesisVdp::Layers::HscrollFetch(GenesisVdp& vdp)
{
	if(!vdp.IsDisplayEnable()) {
		vdp.Slot();
		return;
	}
	static const uint32_t mask[] = { 0u, 7u, ~7u, ~0u };
	uint16_t address = hscrollAddress;
	address += ((uint8_t)vdp._state.vcounter & mask[hscrollMode]) << 1;
	vdp._layerA.hscroll = VramReadWord(vdp._vram, vdp._vramMode, address);
	vdp._layerB.hscroll = VramReadWord(vdp._vram, vdp._vramMode, address + 1);
}

void GenesisVdp::Layers::VscrollFetch(GenesisVdp& vdp)
{
	if(vscrollMode == 1) {
		return;
	}
	vdp._layerA.vscroll = vdp._vsram[0] & 0x7FF;
	vdp._layerB.vscroll = vdp._vsram[1] & 0x7FF;
}

void GenesisVdp::Layers::VscrollFetchIndexed(GenesisVdp& vdp, int32_t index)
{
	if(vscrollMode == 0) {
		return;
	}
	if(index == -1) {
		uint16_t val = vdp.H40() ? (vdp._vsram[38] & vdp._vsram[39]) : 0;
		vdp._layerA.vscroll = vdp._layerB.vscroll = val & 0x7FF;
	} else {
		uint16_t address = index << 1;
		vdp._layerA.vscroll = vdp._vsram[address & 63] & 0x7FF;
		vdp._layerB.vscroll = vdp._vsram[(address + 1) & 63] & 0x7FF;
	}
}

void GenesisVdp::Layers::Power()
{
	hscrollMode = 0;
	hscrollAddress = 0;
	vscrollMode = 0;
	nametableWidth = 0;
	nametableHeight = 0;
}

//--- Window ---

void GenesisVdp::Window::Begin()
{
	latch.hoffset = io.hoffset;
	latch.hdirection = io.hdirection;
	latch.voffset = io.voffset;
	latch.vdirection = io.vdirection;
}

void GenesisVdp::Window::AttributesFetch(GenesisVdp& vdp, int32_t attributesIndex)
{
	if(attributesIndex == -1) {
		vdp._layerA.windowed[0] = 0;
		vdp._layerA.windowed[1] = 0;
		return;
	}

	int32_t x = attributesIndex << 4;
	int32_t y = vdp._state.vcounter;
	vdp._layerA.windowed[0] = vdp._layerA.windowed[1];
	vdp._layerA.windowed[1] = (x < (int32_t)latch.hoffset) ^ latch.hdirection || (y < (int32_t)latch.voffset) ^ latch.vdirection;
	if(!vdp._layerA.windowed[1]) {
		return;
	}

	vdp._layerA.attributes.address = vdp.H40() ? (nametableAddress & ~0x400) : (nametableAddress & ~0u);
	vdp._layerA.attributes.hmask = vdp.H40() ? 63 : 31;
	vdp._layerA.attributes.vmask = 31;
	vdp._layerA.attributes.hscroll = 0;
	vdp._layerA.attributes.vscroll = 0;
}

void GenesisVdp::Window::Power()
{
	latch = {};
	io = {};
	nametableAddress = 0;
}

//--- Layer ---

void GenesisVdp::Layer::Begin()
{
	for(auto& p : pixels) {
		p = {};
	}
}

void GenesisVdp::Layer::AttributesFetch(GenesisVdp& vdp)
{
	attributes.address = nametableAddress;
	attributes.hmask = 32 * (1 + vdp._layers.nametableWidth) - 1;
	attributes.vmask = 32 * (1 + vdp._layers.nametableHeight) - 1;
	attributes.hscroll = hscroll;
	attributes.vscroll = vscroll;

	if(vdp._layers.nametableHeight == 2) {
		attributes.vmask = (32 * (1 + 3) - 1) & ~((1 << 6));
	}
	if(vdp._layers.nametableWidth == 2) {
		attributes.hmask = 31;
		attributes.vmask = 0;
	}
}

void GenesisVdp::Layer::MappingFetch(GenesisVdp& vdp, int32_t mappingIndex)
{
	if(!vdp.IsDisplayEnable()) {
		vdp.Slot();
		return;
	}

	bool interlace = vdp._io.interlaceMode == 3;
	auto x = mappingIndex * 16;
	auto y = (int32_t)vdp._state.vcounter;
	if(interlace) {
		y = y << 1 | vdp._state.field;
	}

	x -= attributes.hscroll & ~15;
	y += attributes.vscroll;

	auto tileX = (x >> 3) & attributes.hmask;
	auto tileY = ((y >> (3 + interlace)) & attributes.vmask);
	uint16_t address = attributes.address + (uint16_t)(tileY * (1 + attributes.hmask) + tileX);
	for(auto& mapping : mappings) {
		auto data = VramReadWord(vdp._vram, vdp._vramMode, address++);
		mapping.address = (data & 0x7FF) << (4 + interlace);
		mapping.hflip = (data >> 11) & 1;
		mapping.palette = (data >> 13) & 3;
		mapping.priority = (data >> 15) & 1;

		auto pixelY = y & (7 + interlace * 8);
		if((data >> 12) & 1) {
			pixelY ^= (7 + interlace * 8);
		}
		mapping.address += pixelY << 1;

		uint32_t extra = mapping.priority << 2 | mapping.palette;
		extra |= extra << 4;
		extra |= extra << 8;
		extra |= extra << 16;
		extras = (extras << 32) | extra;
	}
}

void GenesisVdp::Layer::PatternFetch(GenesisVdp& vdp, uint32_t patternIndex)
{
	if(!vdp.IsDisplayEnable()) {
		vdp.Slot();
		return;
	}

	auto& mapping = mappings[patternIndex & 1];
	uint16_t address = mapping.address;
	uint16_t hi = VramReadWord(vdp._vram, vdp._vramMode, generatorAddress | address);
	uint16_t lo = VramReadWord(vdp._vram, vdp._vramMode, generatorAddress | (address + 1));
	uint32_t data = ((uint32_t)hi << 16) | lo;
	if(mapping.hflip) {
		data = Hflip(data);
	}
	colors = (colors << 32) | data;

	if(patternIndex & 1) {
		uint32_t pixelCount = (patternIndex >> 1) << 4;
		for(int32_t index = 15; index >= 0; index--) {
			uint32_t shift = (index + (attributes.hscroll & 15)) * 4;
			if(windowed[0] && !windowed[1] && (hscroll & 15)) {
				if(15 - index < (hscroll & 15)) {
					shift += 64;
				}
			}
			uint8_t color = (colors >> shift) & 0xF;
			uint8_t extra = (extras >> shift) & 0xF;
			if(color) {
				color |= (extra & 3) << 4;
			}
			pixels[pixelCount++] = { color, (uint8_t)(extra >> 2) };
		}
	}
}

void GenesisVdp::Layer::Power()
{
	hscroll = 0;
	vscroll = 0;
	generatorAddress = 0;
	nametableAddress = 0;
	attributes = {};
	for(auto& p : pixels) {
		p = {};
	}
	colors = 0;
	extras = 0;
	windowed[0] = 0;
	windowed[1] = 0;
	for(auto& m : mappings) {
		m = {};
	}
}

//--- Sprite ---

void GenesisVdp::Sprite::VramWrite(GenesisVdp& vdp, uint16_t address, uint16_t data)
{
	auto baseAddress = nametableAddress;
	if(vdp.H40()) {
		baseAddress &= ~0x1FF;
	}
	address -= baseAddress;
	uint32_t limit = vdp.H40() ? 80 : 64;
	if(address >= limit * 8 / 2) {
		return;
	}
	auto& object = cache[address >> 2];
	switch(address & 3) {
		case 0: object.y = data & 0x3FF; break;
		case 1:
			object.link = data & 0x7F;
			object.height = (data >> 8) & 3;
			object.width = (data >> 10) & 3;
			break;
	}
}

void GenesisVdp::Sprite::Begin()
{
	for(auto& m : mappings) {
		m.valid = 0;
	}
	mappingCount = 0;
}

void GenesisVdp::Sprite::End()
{
	for(auto& p : pixels) {
		p.color = 0;
	}
	visibleLink = 0;
	visibleCount = 0;
	visibleStop = 0;
	patternIndex = 0;
	patternSlice = 0;
	patternCount = 0;
	maskActive = 0;
}

void GenesisVdp::Sprite::MappingFetch(GenesisVdp& vdp, uint32_t)
{
	if(!vdp.IsDisplayEnable()) {
		vdp.Slot();
		return;
	}

	//ares: if(test.disablePhase2) return;
	if(disablePhase2) {
		return;
	}

	if(visibleCount++ < LineObjectLimit(vdp.H40())) {
		return;
	}

	bool interlace = vdp._io.interlaceMode == 3;
	//ares: y = 129 + (i9)vcounter()  — 9-bit signed (bit 8 is sign bit)
	int32_t vc = vdp._state.vcounter;
	if(vc & 0x100) {
		vc -= 0x200;
	}
	int32_t y = 129 + vc;
	if(interlace) {
		y = y << 1 | vdp._state.field;
	}

	if(mappingCount >= 21) {
		return;
	}
	auto id = visible[mappingCount];
	if(id >= 80) {
		return;
	}
	auto& object = cache[id];
	auto height = (1 + object.height) << (3 + interlace);

	auto baseAddress = nametableAddress;
	if(vdp.H40()) {
		baseAddress &= ~0x1FF;
	}

	auto& mapping = mappings[mappingCount++];
	auto address = baseAddress + id * 4 + 2;
	uint16_t d2 = VramReadWord(vdp._vram, vdp._vramMode, address);
	uint16_t d3 = VramReadWord(vdp._vram, vdp._vramMode, address + 1);

	mapping.valid = 1;
	mapping.width = object.width;
	mapping.height = object.height;
	mapping.address = (d2 & 0x7FF) << (4 + interlace);
	mapping.hflip = (d2 >> 11) & 1;
	mapping.palette = (d2 >> 13) & 3;
	mapping.priority = (d2 >> 15) & 1;
	mapping.x = d3 & 0x1FF;

	y = y - (object.y & (interlace ? 1023 : 511));
	if((d2 >> 12) & 1) {
		y = (height - 1) - y;
	}
	y &= 31;

	mapping.address += (y >> (3 + interlace)) << (4 + interlace);
	mapping.address += (y & (7 + interlace * 8)) << 1;
}

void GenesisVdp::Sprite::PatternFetch(GenesisVdp& vdp, uint32_t)
{
	if(!vdp.IsDisplayEnable()) {
		vdp.Slot();
		return;
	}

	//ares: if(test.disablePhase3) mappings[patternIndex].valid = 0;
	if(disablePhase3 && patternIndex < 21) {
		mappings[patternIndex].valid = 0;
	}

	bool interlace = vdp._io.interlaceMode == 3;

	if(patternIndex < 21 && mappings[patternIndex].valid) {
		auto& object = mappings[patternIndex];
		auto width = (1 + object.width) << 3;
		auto height = (1 + object.height) << (3 + interlace);

		if(!maskActive) {
			if(maskCheck && !object.x) {
				maskActive = 1;
			} else {
				uint32_t x = patternSlice * 8;
				if(object.hflip) {
					x = (width - 1) - x;
				}

				uint32_t tileX = x >> 3;
				uint32_t tileNumber = tileX * (height >> (3 + interlace));
				uint16_t tileAddress = object.address + (tileNumber << (4 + interlace));

				uint16_t hi = VramReadWord(vdp._vram, vdp._vramMode, generatorAddress | tileAddress);
				uint16_t lo = VramReadWord(vdp._vram, vdp._vramMode, generatorAddress | (tileAddress + 1));
				uint32_t data = ((uint32_t)hi << 16) | lo;
				if(object.hflip) {
					data = Hflip(data);
				}
				for(uint32_t i = 0; i < 8; i++) {
					//ares: n9 x = object.x + patternSlice*8 + index - 128
					//n9 is 9-bit unsigned (0-511), wraps on negative/overflow
					uint32_t px = (object.x + patternSlice * 8 + i - 128) & 0x1FF;
					uint8_t color = data >> 28;
					data <<= 4;
					if(pixels[px].solid()) {
						if(color) {
							collision = 1;
						}
					} else {
						color |= object.palette << 4;
						pixels[px] = { color, object.priority };
					}
				}
				if(object.x) {
					maskCheck = 1;
				}
			}
		}

		if(++patternSlice >= (1 + object.width)) {
			patternSlice = 0;
			patternIndex++;
		}
	} else {
		maskCheck = 0;
	}
}

void GenesisVdp::Sprite::Scan(GenesisVdp& vdp)
{
	if(!vdp.IsDisplayEnable()) {
		return;
	}

	//ares: if(test.disablePhase1) visibleStop = 1;
	if(disablePhase1) {
		visibleStop = 1;
	}

	bool interlace = vdp._io.interlaceMode == 3;
	//ares: y = 129 + (i9)vcounter()  — 9-bit signed (bit 8 is sign bit)
	int32_t vc = vdp._state.vcounter;
	if(vc & 0x100) {
		vc -= 0x200;
	}
	int32_t y = 129 + vc;
	if(interlace) {
		y = y << 1 | vdp._state.field;
	}

	for(int index = 0; index < 2; index++) {
		if(visibleStop) {
			break;
		}
		auto id = visibleLink;
		if(id >= FrameObjectLimit(vdp.H40())) {
			visibleStop = 1;
			break;
		}
		auto& object = cache[id];
		visibleLink = object.link;
		if(!visibleLink || visibleLink >= FrameObjectLimit(vdp.H40())) {
			visibleStop = 1;
		}

		auto objectY = object.y & (interlace ? 1023 : 511);
		auto height = (1 + object.height) << (3 + interlace);
		if(y < (int32_t)objectY) {
			continue;
		}
		if(y >= (int32_t)(objectY + height)) {
			continue;
		}

		if(visibleCount < LineObjectLimit(vdp.H40())) {
			visible[visibleCount++] = id;
		} else {
			visibleStop = 1;
		}
	}
}

void GenesisVdp::Sprite::Power()
{
	generatorAddress = 0;
	nametableAddress = 0;
	collision = 0;
	overflow = 0;
	disablePhase1 = 0;
	disablePhase2 = 0;
	disablePhase3 = 0;
	for(auto& p : pixels) {
		p = {};
	}
	for(auto& c : cache) {
		c = {};
	}
	for(auto& m : mappings) {
		m = {};
	}
	mappingCount = 0;
	maskCheck = 0;
	maskActive = 0;
	patternIndex = 0;
	patternSlice = 0;
	patternCount = 0;
	for(auto& v : visible) {
		v = 0;
	}
	visibleLink = 0;
	visibleCount = 0;
	visibleStop = 0;
}

//--- DAC ---

template<bool H40, bool Draw>
void GenesisVdp::DAC::Pixel(GenesisVdp& vdp, uint32_t x)
{
	if(!Draw || !active) {
		if(active) {
			*active++ = Color(vdp._cram[vdp._io.backgroundColor], 1);
		}
		return;
	}

	GenesisVdp::Pixel g = { vdp._io.backgroundColor, 0, 1 };
	GenesisVdp::Pixel a = vdp._layerA.PixelAt(x);
	GenesisVdp::Pixel b = vdp._layerB.PixelAt(x);
	GenesisVdp::Pixel s = vdp._sprite.PixelAt(x);

	if(disableLayers == 1) {
		if(forceLayer == 1) {
			g = s;
		}
		if(forceLayer == 2) {
			g = a;
		}
		if(forceLayer == 3) {
			g = b;
		}
		a = {};
		b = {};
		s = {};
	}

	auto& bg = (a.above() || (a.solid() && !b.above())) ? a : b.solid() ? b :
																								 g;
	auto& fg = (s.above() || (s.solid() && !b.above() && !a.above())) ? s : bg;

	auto pixel = fg;
	uint8_t mode = 1; //0=shadow, 1=normal, 2=highlight

	if(vdp._io.shadowHighlightEnable) {
		mode = a.priority || b.priority;
		if(&fg == &s) {
			switch(s.color) {
				case 0x0E:
				case 0x1E:
				case 0x2E: mode = 1; break;
				case 0x3E:
					mode += 1;
					pixel = bg;
					break;
				case 0x3F:
					mode = 0;
					pixel = bg;
					break;
				default: mode |= s.priority; break;
			}
		}
	}

	//Debug register AND mask mode (ares dac.cpp:39-52):
	//When disableLayers == 0 and forceLayer != 0, the forced layer acts as
	//a bitwise AND mask on the composited pixel's 6-bit color. If the
	//pixel is the backdrop (nothing was drawn), it is first replaced with
	//the mask layer's pixel, then ANDed. This enables the layer-blending
	//effects in Titan Overdrive 2 (logo screen, blue circles, etc.).
	//Note: the sprite transparent-pixel quirk (PatternFetch writes palette
	//bits into transparent sprite pixels) ensures the AND mask uses the
	//last evaluated sprite's palette when all sprite pixels are transparent.
	if(disableLayers == 0) {
		if(forceLayer == 1) {
			if(pixel.backdrop) {
				pixel = s;
			}
			pixel.color &= s.color;
		}
		if(forceLayer == 2) {
			if(pixel.backdrop) {
				pixel = a;
			}
			pixel.color &= a.color;
		}
		if(forceLayer == 3) {
			if(pixel.backdrop) {
				pixel = b;
			}
			pixel.color &= b.color;
		}
	}

	*active++ = Color(vdp._cram[pixel.color & 0x3F], mode);
}

template<bool H40>
void GenesisVdp::DAC::Output(uint32_t color)
{
	if(active) {
		*active++ = color;
	}
}

void GenesisVdp::DAC::FillLeftBorder(GenesisVdp& vdp)
{
	if(!active) {
		return;
	}
	uint32_t bgColor = Color(vdp._cram[vdp._io.backgroundColor], 1);
	uint32_t count = vdp.H40() ? 320 : 256;
	uint32_t borderLeft = vdp.H40() ? 13 * 4 : 13 * 5;
	uint32_t visibleWidth = vdp.H40() ? 320 : 256;
	//Left border fills up to the visible area
	for(uint32_t i = 0; i < borderLeft && i < visibleWidth; i++) {
		*active++ = bgColor;
	}
}

void GenesisVdp::DAC::FillRightBorder(GenesisVdp& vdp)
{
	if(!active) {
		return;
	}
	uint32_t bgColor = Color(vdp._cram[vdp._io.backgroundColor], 1);
	uint32_t visibleWidth = vdp.H40() ? 320 : 256;
	//Right border fills remaining pixels after the visible area
	//In our flat framebuffer, we just fill to the end of the line
	uint32_t pos = (uint32_t)(active - vdp._framebuffer.data());
	uint32_t lineStart = pos - (pos % MaxWidth);
	uint32_t lineEnd = lineStart + MaxWidth;
	while(active < (uint32_t*)vdp._framebuffer.data() + lineEnd) {
		*active++ = bgColor;
	}
}

void GenesisVdp::DAC::Power()
{
	disableLayers = 0;
	forceLayer = 0;
	pixels = nullptr;
	active = nullptr;
}

void GenesisVdp::DAC::Dot(GenesisVdp& vdp, uint16_t hpos, uint16_t cramColor)
{
	//Direct Color DMA: write cram bus data directly to framebuffer at hpos.
	//Mesen2's framebuffer is 1:1 (320/256 pixels, no duplication, no border).
	//ares uses a wide framebuffer with 4x/5x duplication and 13-dot left border;
	//Mesen2's visible area starts at pixel 0, so:
	//   x = hpos - (hposMin + 13) = hpos - hposStart
	//H40: hposStart = 0x00d + 13 = 0x01A, width = 320
	//H32: hposStart = 0x00b + 13 = 0x018, width = 256
	if(!pixels) {
		return;
	}

	uint32_t x;
	uint16_t hposStart;
	uint32_t maxWidth;
	if(vdp.H40()) {
		hposStart = 0x01A;
		maxWidth = 320;
	} else {
		hposStart = 0x018;
		maxWidth = 256;
	}
	if(hpos < hposStart) {
		return;
	}
	x = hpos - hposStart;
	if(x >= maxWidth) {
		return;
	}

	pixels[x] = Color(cramColor, 1);
}

//--- Color conversion ---

uint32_t GenesisVdp::Color(uint16_t cramColor, uint8_t mode)
{
	uint32_t R = cramColor & 7;
	uint32_t G = (cramColor >> 3) & 7;
	uint32_t B = (cramColor >> 6) & 7;

	static const uint32_t lookup[4][8] = {
		{ 0, 29, 52, 70, 87, 101, 116, 130 },
		{ 0, 52, 87, 116, 144, 172, 206, 255 },
		{ 130, 144, 158, 172, 187, 206, 228, 255 },
		{},
	};

	mode &= 3;
	return 0xFF000000 | (lookup[mode][R] << 16) | (lookup[mode][G] << 8) | lookup[mode][B];
}

uint32_t GenesisVdp::Hflip(uint32_t data)
{
	data = (data >> 16 & 0x0000FFFF) | (data << 16 & 0xFFFF0000);
	data = (data >> 8 & 0x00FF00FF) | (data << 8 & 0xFF00FF00);
	data = (data >> 4 & 0x0F0F0F0F) | (data << 4 & 0xF0F0F0F0);
	return data;
}

//--- Serialization ---

void GenesisVdp::Serialize(Serializer& s)
{
	SVArray(_vram, VRAMSize);
	SVArray(_vsram, VSRAMSize);
	SVArray(_cram, CRAMSize);
	SV(_vramMode);
	SV(_vramRefreshing);
	SV(_cramBusActive);
	SV(_cramBusData);

	SV(_command.latch);
	SV(_command.target);
	SV(_command.ready);
	SV(_command.pending);
	SV(_command.address);
	SV(_command.increment);

	SV(_io.displayOverlayEnable);
	SV(_io.counterLatch);
	SV(_io.videoMode4);
	SV(_io.leftColumnBlank);
	SV(_io.videoMode5);
	SV(_io.overscan);
	SV(_io.displayEnable);
	SV(_io.backgroundColor);
	SV(_io.displayWidth);
	SV(_io.interlaceMode);
	SV(_io.shadowHighlightEnable);
	SV(_io.externalColorEnable);
	SV(_io.hsync);
	SV(_io.vsync);
	SV(_io.clockSelect);

	SV(_latch.interlace);
	SV(_latch.overscan);
	SV(_latch.displayWidth);
	SV(_latch.clockSelect);
	SV(_latch.displayEnable);

	SV(_state.counterLatchValue);
	SV(_state.hcounter);
	SV(_state.vcounter);
	SV(_state.field);
	SV(_state.hblank);
	SV(_state.vblank);
	SV(_state.rambusy);
	SV(_state.edclkPos);
	SV(_state.topline);
	SV(_state.bottomline);

	SV(_irq.external.enable);
	SV(_irq.external.pending);
	SV(_irq.hblank.enable);
	SV(_irq.hblank.pending);
	SV(_irq.hblank.counter);
	SV(_irq.hblank.frequency);
	SV(_irq.vblank.enable);
	SV(_irq.vblank.pending);
	SV(_irq.vblank.transitioned);
	SV(_irq.delay);

	for(int i = 0; i < 4; i++) {
		SVI(_fifo.slots[i].target);
		SVI(_fifo.slots[i].address);
		SVI(_fifo.slots[i].data);
		SVI(_fifo.slots[i].upper);
		SVI(_fifo.slots[i].lower);
		SVI(_fifo.slots[i].latency);
	}

	SV(_prefetch.slot.target);
	SV(_prefetch.slot.address);
	SV(_prefetch.slot.data);
	SV(_prefetch.slot.upper);
	SV(_prefetch.slot.lower);
	SV(_prefetch.slot.latency);

	SV(_dma.active);
	SV(_dma.mode);
	SV(_dma.source);
	SV(_dma.length);
	SV(_dma.data);
	SV(_dma.wait);
	SV(_dma.read);
	SV(_dma.enable);
	SV(_dma.preload);
	SV(_dma.target);
	SV(_dma.address);
	SV(_dma.increment);

	SV(_layers.hscrollMode);
	SV(_layers.hscrollAddress);
	SV(_layers.vscrollMode);
	SV(_layers.nametableWidth);
	SV(_layers.nametableHeight);

	SV(_window.latch.hoffset);
	SV(_window.latch.hdirection);
	SV(_window.latch.voffset);
	SV(_window.latch.vdirection);
	SV(_window.io.hoffset);
	SV(_window.io.hdirection);
	SV(_window.io.voffset);
	SV(_window.io.vdirection);
	SV(_window.nametableAddress);

	SV(_layerA.hscroll);
	SV(_layerA.vscroll);
	SV(_layerA.generatorAddress);
	SV(_layerA.nametableAddress);
	SV(_layerA.attributes.address);
	SV(_layerA.attributes.hmask);
	SV(_layerA.attributes.vmask);
	SV(_layerA.attributes.hscroll);
	SV(_layerA.attributes.vscroll);
	SV(_layerA.windowed[0]);
	SV(_layerA.windowed[1]);
	for(int i = 0; i < 2; i++) {
		SVI(_layerA.mappings[i].address);
		SVI(_layerA.mappings[i].hflip);
		SVI(_layerA.mappings[i].palette);
		SVI(_layerA.mappings[i].priority);
	}

	SV(_layerB.hscroll);
	SV(_layerB.vscroll);
	SV(_layerB.generatorAddress);
	SV(_layerB.nametableAddress);
	SV(_layerB.attributes.address);
	SV(_layerB.attributes.hmask);
	SV(_layerB.attributes.vmask);
	SV(_layerB.attributes.hscroll);
	SV(_layerB.attributes.vscroll);
	SV(_layerB.windowed[0]);
	SV(_layerB.windowed[1]);
	for(int i = 0; i < 2; i++) {
		SVI(_layerB.mappings[i].address);
		SVI(_layerB.mappings[i].hflip);
		SVI(_layerB.mappings[i].palette);
		SVI(_layerB.mappings[i].priority);
	}

	SV(_sprite.generatorAddress);
	SV(_sprite.nametableAddress);
	SV(_sprite.collision);
	SV(_sprite.overflow);
	SV(_sprite.disablePhase1);
	SV(_sprite.disablePhase2);
	SV(_sprite.disablePhase3);
	SV(_sprite.mappingCount);
	SV(_sprite.maskCheck);
	SV(_sprite.maskActive);
	SV(_sprite.patternIndex);
	SV(_sprite.patternSlice);
	SV(_sprite.patternCount);
	SV(_sprite.visibleLink);
	SV(_sprite.visibleCount);
	SV(_sprite.visibleStop);
	for(int i = 0; i < 80; i++) {
		SVI(_sprite.cache[i].y);
		SVI(_sprite.cache[i].link);
		SVI(_sprite.cache[i].height);
		SVI(_sprite.cache[i].width);
	}
	for(int i = 0; i < 21; i++) {
		SVI(_sprite.mappings[i].valid);
		SVI(_sprite.mappings[i].width);
		SVI(_sprite.mappings[i].height);
		SVI(_sprite.mappings[i].address);
		SVI(_sprite.mappings[i].hflip);
		SVI(_sprite.mappings[i].palette);
		SVI(_sprite.mappings[i].priority);
		SVI(_sprite.mappings[i].x);
	}
	for(int i = 0; i < 20; i++) {
		SVI(_sprite.visible[i]);
	}

	SV(_dac.disableLayers);
	SV(_dac.forceLayer);
	SV(_testAddress);
	SV(_frameCount);
}

// Explicit template instantiations
template void GenesisVdp::TickAndSlot<true>();
template void GenesisVdp::TickAndSlot<false>();
template void GenesisVdp::TickAndRefresh<true>();
template void GenesisVdp::TickAndRefresh<false>();
template void GenesisVdp::Blocks<true, true>();
template void GenesisVdp::Blocks<true, false>();
template void GenesisVdp::Blocks<false, true>();
template void GenesisVdp::Blocks<false, false>();
template void GenesisVdp::DAC::Pixel<true, true>(GenesisVdp&, uint32_t);
template void GenesisVdp::DAC::Pixel<true, false>(GenesisVdp&, uint32_t);
template void GenesisVdp::DAC::Pixel<false, true>(GenesisVdp&, uint32_t);
template void GenesisVdp::DAC::Pixel<false, false>(GenesisVdp&, uint32_t);
template void GenesisVdp::DAC::Output<true>(uint32_t);
template void GenesisVdp::DAC::Output<false>(uint32_t);
