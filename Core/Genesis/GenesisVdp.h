#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"
#include "Shared/SettingTypes.h"

class Emulator;
class GenesisConsole;

// Portable 128-bit unsigned integer for VDP pattern accumulation.
// ares uses u128 (128-bit) for colors/extras; Mesen2 originally ported these
// as uint64_t, which caused horizontal scroll artifacts ("shutters") because
// the shift formula (index + hscroll_fine) * 4 can reach 120, exceeding 64 bits.
#if defined(__SIZEOF_INT128__) || defined(__GNUC__) || defined(__clang__)
using uint128_t = __uint128_t;
#else
struct uint128_t
{
	uint64_t lo = 0;
	uint64_t hi = 0;
	uint128_t() = default;
	uint128_t(int v) : lo(static_cast<uint64_t>(v)), hi(0) {}
	uint128_t operator<<(int n) const
	{
		if(n == 0) {
			return *this;
		}
		if(n >= 64) {
			return { 0, lo << (n - 64) };
		}
		return { lo << n, (hi << n) | (lo >> (64 - n)) };
	}
	uint128_t operator|(uint32_t v) const { return { lo | v, hi }; }
	uint64_t operator>>(int n) const
	{
		if(n >= 128) {
			return 0;
		}
		if(n >= 64) {
			return hi >> (n - 64);
		}
		if(n == 0) {
			return lo;
		}
		return (hi << (64 - n)) | (lo >> n);
	}
};
#endif

// Yamaha YM7101 VDP — native Mesen2 port.
// Algorithm ported from ares/md/vdp with Mesen2 infrastructure (SV, uint32_t
// framebuffer). Uses bus callbacks for M68K bus access (DMA load) instead of
// ares's Thread/synchronize model. Clock is advanced explicitly by the console's
// RunFrame loop, not by a co-routine scheduler.

class GenesisVdp final : public ISerializable
{
public:
	GenesisVdp(Emulator* emu, GenesisConsole* console);
	~GenesisVdp() = default;

	void Power();
	void Reset();

	//Run the VDP for one scanline. The caller (GenesisConsole::RunFrame via
	//GenesisMemoryManager) is responsible for calling this 262/313 times per
	//frame. Internally, this advances the hcounter through the entire line,
	//performs slot work (FIFO, DMA, prefetch, VRAM access), and renders
	//pixels into _framebuffer if the line is in the visible region.
	void RunScanline();

	//M68K/Z80 bus interface — register reads/writes at 0xC00000-0xC0001F.
	uint16_t Read(uint32_t address);
	void Write(uint32_t address, uint16_t data);

	//Data port access (separate for the memory manager which may split 16-bit
	//access into two 8-bit calls).
	uint16_t ReadDataPort();
	void WriteDataPort(uint16_t data);
	uint16_t ReadControlPort();
	void WriteControlPort(uint16_t data);
	void DrainFifo(bool forceCram = false);

	//Interrupt outputs — polled by the M68K interrupt controller.
	bool GetVblankIrq() const;
	bool GetHblankIrq() const;
	bool GetExternalIrq() const;
	void AcknowledgeIrq(uint8_t level);

	//Hblank / Vblank status for Z80 bus arbitration.
	bool IsHblank() const { return _state.hblank; }
	bool IsVblank() const { return _state.vblank; }
	bool IsDisplayEnable() const { return _latch.displayEnable && !_state.vblank; }
	bool IsRefreshing() const { return _vramRefreshing; }

	//Bus throttling — during active display, VDP register accesses incur
	//a penalty because the 68K must wait for an available external slot.
	//The M68K cycle loop calls ConsumeBusPenalty() to account for this.
	uint32_t ConsumeBusPenalty()
	{
		uint32_t p = _busPenalty;
		_busPenalty = 0;
		return p;
	}

	//Track M68K cycle position within the current scanline so that
	//ReadControlPort can compute a virtual hblank state. In Mesen2's
	//sequential model, the VDP processes a full scanline before the M68K
	//runs, so _state.hblank is frozen at its end-of-scanline value (1).
	//This makes the M68K always see hblank=1, breaking polling loops that
	//expect hblank to toggle. By mapping M68K cycles to hcounter position,
	//we can compute what hblank SHOULD be at the M68K's current time.
	void SetM68kCyclePosition(uint32_t cycle, uint32_t total)
	{
		_m68kCycleInScanline = cycle;
		_m68kCyclesPerScanline = total;
	}

	//Framebuffer access for GenesisConsole::GetPpuFrame.
	uint32_t* GetFramebuffer() { return _framebuffer.data(); }
	void ClearFramebuffer();
	uint32_t GetScreenWidth() const { return _io.displayWidth ? 320 : 256; }
	uint32_t GetScreenHeight() const { return _region == ConsoleRegion::Pal ? 294 : 243; }
	uint32_t GetFrameCount() const { return _frameCount; }
	uint16_t GetVCounter() const { return _state.vcounter; }
	uint16_t GetHCounter() const { return _state.hcounter; }
	uint16_t DebugReadCRAM(uint32_t index) const { return index < CRAMSize ? _cram[index] : 0; }
	uint16_t DebugReadVRAM(uint32_t index) const { return index < VRAMSize ? _vram[index] : 0; }
	uint16_t DebugReadVSRAM(uint32_t index) const { return index < VSRAMSize ? _vsram[index] : 0; }
	void DebugWriteVRAM(uint32_t index, uint16_t data)
	{
		if(index < VRAMSize) {
			_vram[index] = data;
		}
	}
	void DebugWriteVSRAM(uint32_t index, uint16_t data)
	{
		if(index < VSRAMSize) {
			_vsram[index] = data & 0x7FF;
		}
	}
	void DebugWriteCRAM(uint32_t index, uint16_t data)
	{
		if(index < CRAMSize) {
			_cram[index] = data & 0x1FF;
		}
	}
	uint8_t GetBackgroundColor() const { return _io.backgroundColor; }

	//Region (PAL/NTSC) setter. The VDP defaults to NTSC in its constructor;
	//the console calls this from UpdateRegion() once ROM-header region
	//detection has settled on PAL vs NTSC. Affects: status register bit 0
	//(PAL flag read by games to detect 50Hz), VBlank topline/bottomline,
	//scanline count per frame (313 PAL / 262 NTSC), framebuffer height,
	//and pixel blanking ranges. Must be called before Power() for correct
	//initial VBlank lines, but is also re-applied every frame at the
	//bottomline transition via UpdateScreenParams().
	void SetRegion(ConsoleRegion region)
	{
		_region = region;
		UpdateScreenParams();
	}

	//Clock helpers. The VDP clock is the master clock; we track cycle counts
	//relative to it. In H40 mode, 4 mclks/pixel; in H32 mode, 5 mclks/pixel.
	uint32_t GetHcounter() const { return _state.hcounter; }
	uint32_t GetVcounter() const { return _state.vcounter; }
	bool GetField() const { return _state.field; }

	//DMA bus request callback — used by the memory manager to service 68K->VDP DMA.
	//When a DMA load is active, the VDP needs to read from the 68K bus.
	std::function<uint16_t(uint32_t address)> DmaRead;

	//M68K reference for debug logging
	class GenesisM68K* _m68k = nullptr;
	void SetM68K(class GenesisM68K* m68k) { _m68k = m68k; }

	//ISerializable
	void Serialize(Serializer& s) override;

private:
	Emulator* _emu;
	GenesisConsole* _console;
	ConsoleRegion _region = ConsoleRegion::Ntsc;

	//--- Framebuffer ---
	//Max size: 320x294 (H40, PAL full overscan) * 4 bytes (XRGB8888).
	static constexpr uint32_t MaxWidth = 320;
	static constexpr uint32_t MaxHeight = 294; //PAL full overscan
	vector<uint32_t> _framebuffer; //XRGB8888
	uint32_t _frameCount = 0;

	//--- Video RAM ---
	static constexpr uint32_t VRAMSize = 32768; //64KB in 16-bit words
	uint16_t _vram[VRAMSize] = {};
	uint32_t _vramSize = VRAMSize;
	uint8_t _vramMode = 0; //0=64KB, 1=128KB
	bool _vramRefreshing = false;

	//Vertical Scroll RAM
	static constexpr uint32_t VSRAMSize = 40;
	uint16_t _vsram[VSRAMSize] = {}; //11-bit values stored in bits 0-10

	//Color RAM (CRAM) — 64 entries of 9-bit color (3 bits R, 3 G, 3 B)
	static constexpr uint32_t CRAMSize = 64;
	uint16_t _cram[CRAMSize] = {}; //9-bit values stored in bits 0-8
	bool _cramBusActive = false;
	uint16_t _cramBusData = 0;

	//Bus throttling penalty accumulator
	uint32_t _busPenalty = 0;

	//M68K cycle position within the current scanline (set by console)
	uint32_t _m68kCycleInScanline = 0;
	uint32_t _m68kCyclesPerScanline = 0;

	//FIFO drain cycle tracking — simulates VDP processing slots between
	//M68K status register reads. The VDP drains ~18 bytes per scanline
	//(one per slot). We accumulate M68K cycle credit and drain 1 byte
	//per ~(_m68kCyclesPerScanline/18) cycles.
	uint32_t _lastFifoDrainCycle = 0;
	int32_t _fifoDrainCredit = 0;

	//--- Command/IO state ---
	struct Command
	{
		uint8_t latch = 0; //write-half toggle
		uint8_t target = 0; //CD0-CD3
		uint8_t ready = 0; //CD4
		uint8_t pending = 0; //CD5
		uint32_t address = 0; //A0-A16 (17-bit)
		uint8_t increment = 0; //auto-increment value
	} _command;

	struct IO
	{
		//$00 mode register 1
		uint8_t displayOverlayEnable = 0;
		uint8_t counterLatch = 0;
		uint8_t videoMode4 = 0;
		uint8_t leftColumnBlank = 0;

		//$01 mode register 2
		uint8_t videoMode5 = 0;
		uint8_t overscan = 0; //0=224 lines, 1=240 lines
		uint8_t displayEnable = 0;

		//$07 background color
		uint8_t backgroundColor = 0;

		//$0C mode register 4
		uint8_t displayWidth = 0; //0=H32, 1=H40
		uint8_t interlaceMode = 0;
		uint8_t shadowHighlightEnable = 0;
		uint8_t externalColorEnable = 0;
		uint8_t hsync = 0;
		uint8_t vsync = 0;
		uint8_t clockSelect = 0; //0=DCLK, 1=EDCLK
	} _io;

	struct Latch
	{
		uint8_t interlace = 0;
		uint8_t overscan = 0;
		uint8_t displayWidth = 0;
		uint8_t clockSelect = 0;
		uint8_t displayEnable = 0;
	} _latch;

	struct State
	{
		uint16_t counterLatchValue = 0;
		uint32_t hcounter = 0;
		uint32_t vcounter = 0;
		uint8_t field = 0;
		uint8_t hblank = 0;
		uint8_t vblank = 0;
		uint8_t rambusy = 0;
		uint32_t edclkPos = 0;
		uint32_t topline = 0;
		uint32_t bottomline = 0;
	} _state;

	//--- IRQ ---
	struct IRQ
	{
		struct External
		{
			uint8_t enable = 0, pending = 0;
		} external;
		struct Hblank
		{
			uint8_t enable = 0, pending = 0;
			uint8_t counter = 0, frequency = 0;
		} hblank;
		struct Vblank
		{
			uint8_t enable = 0, pending = 0, transitioned = 0;
		} vblank;
		uint8_t delay = 0;
	} _irq;

	//--- FIFO ---
	struct Slot
	{
		uint8_t target = 0; //CD1-3 + read/write
		uint32_t address = 0; //17-bit
		uint16_t data = 0;
		uint8_t upper = 0; //byte(1) valid
		uint8_t lower = 0; //byte(0) valid
		uint8_t latency = 0;

		bool empty() const { return !upper && !lower; }
		bool full() const { return upper && lower; }
	};

	struct FIFO
	{
		Slot slots[4] = {};
		void Tick();
		void Advance(class GenesisVdp& vdp);
		bool Run(class GenesisVdp& vdp);
		void Write(uint8_t target, uint32_t address, uint16_t data);
		void Power();
		bool empty() const { return slots[0].empty(); }
		bool full() const { return !slots[3].empty(); }
	} _fifo;

	//--- Prefetch ---
	struct Prefetch
	{
		Slot slot = {};
		bool Run(class GenesisVdp& vdp);
		void Read(uint8_t target, uint32_t address);
		void Power();
		bool empty() const { return slot.empty(); }
		bool full() const { return slot.full(); }
	} _prefetch;

	//--- DMA ---
	struct DMA
	{
		uint8_t active = 0;
		uint8_t mode = 0;
		uint32_t source = 0; //22-bit
		uint16_t length = 0;
		uint16_t data = 0;
		uint8_t wait = 1;
		uint8_t read = 0;
		uint8_t enable = 0;
		uint8_t preload = 0;
		//Saved at DMA start — in ares, M68K is blocked during DMA so
		//command.target/address/increment can't change. In Mesen2's
		//sequential model, M68K runs before VDP and can modify these
		//values between DMA setup and VDP execution. We save our own
		//copy to prevent corruption.
		uint8_t target = 0;
		uint32_t address = 0;
		uint16_t increment = 0;

		void Synchronize(class GenesisVdp& vdp);
		void Fetch(class GenesisVdp& vdp);
		bool Run(class GenesisVdp& vdp);
		void Load(class GenesisVdp& vdp);
		void Fill(class GenesisVdp& vdp);
		void Copy(class GenesisVdp& vdp);
		void Power();
	} _dma;

	//--- Layer rendering ---
	struct Pixel
	{
		uint8_t color = 0; //6-bit
		uint8_t priority = 0; //1-bit
		uint8_t backdrop = 0; //1-bit

		bool solid() const { return color & 0xF; }
		bool above() const { return priority == 1 && solid(); }
		bool below() const { return priority == 0 && solid(); }
	};

	struct Attributes
	{
		uint32_t address = 0; //15-bit
		uint16_t hmask = 0;
		uint16_t vmask = 0;
		uint16_t hscroll = 0; //10-bit
		uint16_t vscroll = 0; //10-bit
	};

	struct LayerMapping
	{
		uint32_t address = 0; //15-bit
		uint8_t hflip = 0;
		uint8_t palette = 0; //2-bit
		uint8_t priority = 0;
	};

	struct Layer
	{
		uint16_t hscroll = 0;
		uint16_t vscroll = 0;
		uint16_t generatorAddress = 0;
		uint16_t nametableAddress = 0;
		Attributes attributes = {};
		Pixel pixels[352] = {};
		uint128_t colors = 0;
		uint128_t extras = 0;
		uint8_t windowed[2] = {};
		LayerMapping mappings[2] = {};

		void Begin();
		void AttributesFetch(class GenesisVdp& vdp);
		void MappingFetch(class GenesisVdp& vdp, int32_t mappingIndex);
		void PatternFetch(class GenesisVdp& vdp, uint32_t patternIndex);
		Pixel PixelAt(uint32_t x) const { return pixels[16 + x]; }
		void Power();
	} _layerA, _layerB;

	struct Layers
	{
		uint8_t hscrollMode = 0;
		uint16_t hscrollAddress = 0;
		uint8_t vscrollMode = 0;
		uint8_t nametableWidth = 0;
		uint8_t nametableHeight = 0;

		void HscrollFetch(class GenesisVdp& vdp);
		void VscrollFetch(class GenesisVdp& vdp);
		void VscrollFetchIndexed(class GenesisVdp& vdp, int32_t index);
		void Power();
	} _layers;

	struct Window
	{
		struct Latch
		{
			uint16_t hoffset = 0;
			uint8_t hdirection = 0;
			uint16_t voffset = 0;
			uint8_t vdirection = 0;
		} latch;
		struct IO
		{
			uint16_t hoffset = 0;
			uint8_t hdirection = 0;
			uint16_t voffset = 0;
			uint8_t vdirection = 0;
		} io;
		uint16_t nametableAddress = 0;

		void Begin();
		void AttributesFetch(class GenesisVdp& vdp, int32_t attributesIndex);
		void Power();
	} _window;

	//--- Sprite ---
	struct SpriteMapping
	{
		uint8_t valid = 0;
		uint8_t width = 0;
		uint8_t height = 0;
		uint32_t address = 0; //15-bit
		uint8_t hflip = 0;
		uint8_t palette = 0; //2-bit
		uint8_t priority = 0;
		uint16_t x = 0; //9-bit
	};

	struct SpriteCache
	{
		uint16_t y = 0; //10-bit
		uint8_t link = 0; //7-bit
		uint8_t height = 0; //2-bit
		uint8_t width = 0; //2-bit
	};

	struct Sprite
	{
		uint16_t generatorAddress = 0;
		uint16_t nametableAddress = 0;
		uint8_t collision = 0;
		uint8_t overflow = 0;
		Pixel pixels[512] = {};

		SpriteCache cache[80] = {};
		SpriteMapping mappings[21] = {};
		uint8_t mappingCount = 0;

		uint8_t maskCheck = 0;
		uint8_t maskActive = 0;
		uint8_t patternIndex = 0;
		uint8_t patternSlice = 0;
		uint8_t patternCount = 0;

		uint8_t visible[20] = {};
		uint8_t visibleLink = 0;
		uint8_t visibleCount = 0;
		uint8_t visibleStop = 0;

		//Test register bits (test address 0, bits 12-14)
		uint8_t disablePhase1 = 0; //disables sprite visible-scan phase
		uint8_t disablePhase2 = 0; //disables sprite mapping-fetch phase
		uint8_t disablePhase3 = 0; //disables sprite pattern-fetch phase

		void VramWrite(class GenesisVdp& vdp, uint16_t address, uint16_t data);
		void Begin();
		void End();
		void MappingFetch(class GenesisVdp& vdp, uint32_t index);
		void PatternFetch(class GenesisVdp& vdp, uint32_t index);
		void Scan(class GenesisVdp& vdp);
		Pixel PixelAt(uint32_t x) const { return pixels[x]; }
		void Power();

		uint32_t LineObjectLimit(bool h40) const { return h40 ? 20 : 16; }
		uint32_t FrameObjectLimit(bool h40) const { return h40 ? 80 : 64; }
	} _sprite;

	//--- DAC (pixel output) ---
	struct DAC
	{
		uint8_t disableLayers = 0;
		uint8_t forceLayer = 0;
		uint32_t* pixels = nullptr; //pointer to start of current scanline in framebuffer
		uint32_t* active = nullptr; //pointer to next pixel write position

		template<bool H40, bool Draw> void Pixel(class GenesisVdp& vdp, uint32_t x);
		template<bool H40> void Output(uint32_t color);
		void FillLeftBorder(class GenesisVdp& vdp);
		void FillRightBorder(class GenesisVdp& vdp);
		void Dot(class GenesisVdp& vdp, uint16_t hpos, uint16_t cramColor);
		void Power();
	} _dac;

	//--- Test register ---
	uint8_t _testAddress = 0;

	//--- Internal helpers ---
	inline bool H40() const { return _latch.displayWidth == 1; }
	inline bool H32() const { return _latch.displayWidth == 0; }
	inline bool V28() const { return _io.overscan == 0; }
	inline bool V30() const { return _io.overscan == 1; }

	//Compute a virtual HV counter value from the M68K's cycle position
	//within the current scanline. In Mesen2's sequential model, the VDP
	//has already processed the full scanline, so _state.hcounter is frozen
	//at its end-of-scanline value. This maps M68K cycles to an hcounter
	//tick to derive the live HV counter the M68K would see if the VDP
	//ran concurrently.
	//H40: 210 ticks/scanline; ticks 0-0xB5 → hcounter=tick, ticks 0xB6-0xD1 → hcounter=tick+0x2E
	//H32: 171 ticks/scanline; ticks 0-0x93 → hcounter=tick, ticks 0x94-0xAA → hcounter=tick+0x55
	uint16_t ComputeVirtualHvCounter() const;

	uint32_t FrameHeight() const { return (_region == ConsoleRegion::Pal) ? 313 : 262; }

	//IRQ polling
	void IrqPoll();
	void VblankCheck();

	//Counter/timing
	void Htick();
	void Vtick();
	void Hblank(bool line);
	void Vblank(bool line);
	void Vedge();

	//Slot execution
	void Slot();

	//Main scanline rendering
	void MainH32();
	void MainH40();
	template<bool H40, bool DrawPixels> void Blocks();

	//Color conversion (CRAM 9-bit -> XRGB8888)
	static uint32_t Color(uint16_t cramColor, uint8_t mode);

	//hflip utility
	static uint32_t Hflip(uint32_t data);

	//Tick helpers (process one slot = 2 pixels of work)
	template<bool H40> void TickAndSlot();
	template<bool H40> void TickAndRefresh();

	//Get line buffer pointer for current scanline (null if blanked)
	uint32_t* GetLineBuffer();

	//VRAM access helpers (static so sub-structs can call them)
	static uint16_t VramReadWord(const uint16_t* vram, uint8_t mode, uint16_t address);
	static void VramWriteWord(uint16_t* vram, uint8_t mode, uint16_t address, uint16_t data);
	static uint8_t VramReadByte(const uint16_t* vram, uint8_t mode, uint32_t address);
	static void VramWriteByte(uint16_t* vram, uint8_t mode, uint32_t address, uint8_t data);

	//Update topline/bottomline based on region/overscan
	void UpdateScreenParams();
};
