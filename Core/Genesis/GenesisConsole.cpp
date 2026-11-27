#include "pch.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisControlManager.h"
#include "Genesis/GenesisPsg.h"
#include "Genesis/GenesisYm2612.h"
#include "Genesis/GenesisM68K.h"
#include "Genesis/GenesisZ80.h"
#include "Genesis/GenesisVdp.h"
#include "Genesis/GenesisMemoryManager.h"
#include "Genesis/GenesisDefaultVideoFilter.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/EventType.h"
#include "Shared/BatteryManager.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/RewindManager.h"
#include "Shared/NotificationManager.h"
#include "Utilities/Serializer.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/CRC32.h"
#include "Utilities/StringUtilities.h"
#include <cmath>

GenesisConsole::GenesisConsole(Emulator* emu)
{
	_emu = emu;
	_controlManager = unique_ptr<GenesisControlManager>(new GenesisControlManager(_emu, this));
	_psg = unique_ptr<GenesisPsg>(new GenesisPsg(_emu, this));
	_ym2612 = unique_ptr<GenesisYm2612>(new GenesisYm2612(_emu, this));
	UpdateRegion();
}

GenesisConsole::~GenesisConsole()
{
	if(_sram) {
		delete[] _sram;
	}
}

static bool HasSsfMapperHeader(const uint8_t* system)
{
	return memcmp(system, "SEGASSF", 7) == 0 || memcmp(system, "SEGA SSF", 8) == 0;
}

bool GenesisConsole::HasBinRomHeader(VirtualFile& romFile)
{
	const vector<uint8_t>& data = romFile.GetData();
	if(data.size() < 0x200) {
		return false;
	}

	const uint8_t* system = data.data() + 0x100;
	return memcmp(system, "SEGA MEGA DRIVE", 15) == 0 || memcmp(system, "SEGA GENESIS", 12) == 0 || HasSsfMapperHeader(system);
}

LoadRomResult GenesisConsole::LoadRom(VirtualFile& romFile)
{
	vector<uint8_t> romData;
	romFile.ReadFile(romData);

	if(romData.size() < 0x200) {
		return LoadRomResult::Failure;
	}

	//Strip SMD interleaved header if present (512-byte header)
	if(romData.size() > 512 && (romData.size() % 0x4000) == 512) {
		//SMD format: deinterleave 16KB blocks
		vector<uint8_t> deinterleaved;
		deinterleaved.resize(romData.size() - 512);
		uint32_t blockSize = 0x4000;
		uint32_t numBlocks = (uint32_t)(deinterleaved.size() / blockSize);
		for(uint32_t block = 0; block < numBlocks; block++) {
			for(uint32_t i = 0; i < blockSize; i++) {
				//SMD interleaves odd/even bytes: first 8KB = even bytes, second 8KB = odd bytes
				if(i < blockSize / 2) {
					deinterleaved[block * blockSize + i * 2 + 1] = romData[512 + block * blockSize + i];
				} else {
					deinterleaved[block * blockSize + (i - blockSize / 2) * 2] = romData[512 + block * blockSize + i];
				}
			}
		}
		romData = std::move(deinterleaved);
	}

	_filename = romFile.GetFileName();
	_romFormat = RomFormat::Genesis;

	//Parse the ROM header for SRAM info and region detection
	ParseRomHeader(romData);

	//Initialize cartridge (pad ROM to power-of-2, allocate SRAM)
	InitCart(romData);

	//Create all subsystems
	_vdp = unique_ptr<GenesisVdp>(new GenesisVdp(_emu, this));
	_m68k = unique_ptr<GenesisM68K>(new GenesisM68K());
	_z80 = unique_ptr<GenesisZ80>(new GenesisZ80());
	_m68k->SetEmulator(_emu);
	_z80->SetEmulator(_emu);
	_memoryManager = unique_ptr<GenesisMemoryManager>(new GenesisMemoryManager());

	//The Acclaim 16M board ignores the byte write strobes.
	bool acclaim16M = _eepromType == GenesisEeprom::Type::X24C02;
	_memoryManager->Init(
		_emu, this, _m68k.get(), _z80.get(), _vdp.get(), _psg.get(), _ym2612.get(), _controlManager.get(), _romData.data(), (uint32_t)_romData.size(), _sram, _sramSize, _sramStart, _sramWritable, _sramOddByte, _banked, _romBank, (bool)_eeprom, &_eeprom, _eepromRsda, _eepromWsda, _eepromWscl, acclaim16M);

	//Wire M68K pointer to VDP
	_vdp->SetM68K(_m68k.get());

	//Power on all subsystems
	_vdp->Power();
	_m68k->Power();
	_z80->Power();

	UpdateRegion();

	return LoadRomResult::Success;
}

void GenesisConsole::ParseRomHeader(vector<uint8_t>& romData)
{
	if(romData.size() < 0x200) {
		return;
	}

	//The system identifier at $100 can explicitly request the SSF mapper.
	if(HasSsfMapperHeader(romData.data() + 0x100)) {
		_banked = true;
	}

	string productCode = StringUtilities::Trim(string(reinterpret_cast<const char*>(romData.data() + 0x182), 9));
	if(productCode == "G-4524") {
		//Ninja Burai Densetsu uses the Sega board, with SDA on D0 in both directions.
		_eepromType = GenesisEeprom::Type::X24C01;
	} else if(productCode == "T-081326") {
		//NBA Jam (USA/Europe) uses Acclaim 16M, with SDA output on D1.
		_eepromType = GenesisEeprom::Type::X24C02;
		_eepromRsda = 1;
	}

	//SRAM info from ROM header at offset $1B0-$1BF.
	//The correct Genesis header layout (matching gpgx/Genesis Plus GX):
	// $1B0: 'R' (0x52)
	// $1B1: 'A' (0x41)
	// $1B2: type byte 1 — %1x1yz000 (backup, even/odd address selection)
	//       yz=10 even-only, yz=11 odd-only, yz=00 both, yz=01 other(EEPROM)
	// $1B3: type byte 2 — %abc00000 (001=SRAM, 010=EEPROM)
	// $1B4: SRAM start address (big-endian 32-bit)
	// $1B8: SRAM end address (big-endian 32-bit)
	//
	// IMPORTANT: A previous version of this code read sramStart from $1B2
	// and sramEnd from $1B6 (2 bytes earlier than the correct offsets),
	// which caused the type bytes to be mixed into the address fields.
	// For ROMs with non-zero type bytes (e.g. Chinese fan translations
	// like Daikoukai Jidai II [CN] with type=0xF0 0x20), this produced
	// garbage addresses and fell back to a default 8KB SRAM. The correct
	// offsets parse the header properly: e.g. Daikoukai Jidai II [CN]
	// has start=0x200001, end=0x20FFFF → 64KB SRAM. Reference:
	// gpgx/core/cart_hw/sram.c::sram_init().
	//
	// Odd-byte SRAM detection: the yz bits in type byte 1 indicate the
	// address selection, but many ROMs (including official titles like
	// Light Crusader) have incorrect yz bits. A more reliable indicator
	// is the start address parity: odd start (e.g. 0x200001) means the
	// SRAM chip is /LDS-selected (D0-D7 only, odd-byte access). We use
	// both signals: yz=11 OR odd start address.
	if(romData.size() > 0x1BB) {
		if(romData[0x1B0] == 'R' && romData[0x1B1] == 'A') {
			uint8_t typeByte1 = romData[0x1B2];
			uint32_t sramStart = ((uint32_t)romData[0x1B4] << 24) | ((uint32_t)romData[0x1B5] << 16) |
				((uint32_t)romData[0x1B6] << 8) | (uint32_t)romData[0x1B7];
			uint32_t sramEnd = ((uint32_t)romData[0x1B8] << 24) | ((uint32_t)romData[0x1B9] << 16) |
				((uint32_t)romData[0x1BA] << 8) | (uint32_t)romData[0x1BB];

			//Odd-byte SRAM: yz=11 (bits 4+3 both set) in type byte 1,
			//OR start address is odd (SRAM chip wired to D0-D7 via /LDS).
			bool yzOdd = (typeByte1 & 0x18) == 0x18;
			_sramOddByte = yzOdd || ((sramStart & 1) != 0);

			//Validate the SRAM range, matching gpgx sram_init() logic:
			//  - start >= 0x800000 → invalid, force default 64KB at $200000
			//  - start > end OR range >= 64KB → cap end to start + 0xFFFF
			if(sramStart >= 0x800000) {
				sramStart = 0x200000;
				sramEnd = 0x20FFFF;
			} else if(sramStart > sramEnd || (sramEnd - sramStart) >= 0x10000) {
				sramEnd = sramStart + 0xFFFF;
			}

			_sramStart = sramStart & ~1;
			_sramSize = (sramEnd - _sramStart) + 1;

			//The old code used a packed 32KB array (_sramSize halved) and
			//hardcoded 0xFF in the upper byte for reads. This matches real
			//hardware but differs from gpgx, causing tile corruption in
			//games that write word values with non-0xFF upper bytes and
			//read them back.
			//Cap at 64KB (largest standard Genesis SRAM)
			if(_sramSize > 0x10000) {
				_sramSize = 0x10000;
			}
			_sramEnable = true;
		}
	}

	//Detect region from ROM header
	//Offset $1F0: domestic/overseas region string (e.g. "JUE" = Japan+US+Europe)
	if(romData.size() > 0x1F4) {
		for(int i = 0; i < 16 && romData[0x1F0 + i]; i++) {
			_romRegion += (char)romData[0x1F0 + i];
		}
	}
}

void GenesisConsole::InitCart(vector<uint8_t>& romData)
{
	//Pad ROM to power-of-2 size (matching SMS pattern)
	uint32_t power = (uint32_t)std::log2(romData.size());
	if(romData.size() > ((uint64_t)1 << power)) {
		uint32_t newSize = 1 << (power + 1);
		romData.insert(romData.end(), newSize - romData.size(), 0xFF);
	}

	//Enable SSF2 bank switching for ROMs larger than 4MB. The standard
	//cartridge ROM window is 0x000000-0x3FFFFF (4MB); ROMs beyond that
	//size (e.g. Super Street Fighter II at 5MB) require bank switching
	//to expose the upper 1MB via 512KB swappable banks. This is also
	//auto-enabled by ParseRomHeader when the system type is "SEGA SSF".
	//Reference: ares/md/cartridge/board/banked.cpp.
	if(romData.size() > 0x400000) {
		_banked = true;
	}

	//Initialize bank registers (default: identity mapping). ares
	//banked.cpp::power() sets romBank[index] = index for all 8 entries,
	//so on power-on each 512KB region maps to itself.
	for(int i = 0; i < 8; i++) {
		_romBank[i] = i;
	}

	if(_eepromType != GenesisEeprom::Type::None) {
		//Both supported boards map the serial lines across the upper 2 MiB of the cartridge window.
		_sramStart = 0x200000;
		_sramSize = 0x200000;
		_eeprom.load(_eepromType);
		_eeprom.power();
		//No parallel SRAM on EEPROM carts
		_sram = nullptr;
		_sramEnable = false;
	} else if(_sramSize > 0 && _sramEnable) {
		_sram = new uint8_t[_sramSize];
		//Initialize SRAM to 0xFF (unprogrammed SRAM reads as 0xFF on real
		//hardware). gpgx does the same: memset(sram.sram, 0xFF, 0x10000).
		//The old 0x00 initialization caused odd-byte SRAM word reads to
		//return 0xFF00 instead of 0xFFFF on first boot (no save file),
		//breaking games like Daikoukai Jidai II [CN] that check SRAM
		//contents for uninitialized state (0xFF patterns).
		memset(_sram, 0xFF, _sramSize);
	} else {
		_sram = nullptr;
		_sramSize = 0;
		_sramEnable = false;
	}

	//Store ROM data
	_romData = std::move(romData);
}

void GenesisConsole::InitializeRam(void* data, uint32_t length)
{
	EmuSettings* settings = _emu->GetSettings();
	RamState ramState = settings->GetGenesisConfig().RamPowerOnState;
	settings->InitializeRam(ramState, data, length);
}

void GenesisConsole::Reset()
{
	if(_m68k) {
		_m68k->Power();
	}
	if(_z80) {
		_z80->Power();
	}
	if(_vdp) {
		_vdp->Reset();
	}
	if(_memoryManager) {
		_memoryManager->Reset();
	}
	//Reset bank registers to identity mapping on soft reset, matching
	//ares banked.cpp::power(reset) which runs on both power-on and reset.
	for(int i = 0; i < 8; i++) {
		_romBank[i] = i;
	}
	//Reset the EEPROM I2C state machine to Standby (preserves memory
	//contents). Matches ares standard.cpp::power(reset) which calls
	//m24c.power() on both power-on and reset.
	if(_eeprom) {
		_eeprom.power();
	}
	_frameCount = 0;
	_masterClock = 0;
	_m68kClock = 0;
	_z80Clock = 0;
	_psg->Reset();
	_ym2612->Reset();
	_controlManager->ResetControllerHandshakes();
	UpdateRegion();
}

void GenesisConsole::RunFrame()
{
	if(!_m68k || !_vdp || !_memoryManager) {
		_frameCount++;
		return;
	}

	if(_configuredRegion != _emu->GetSettings()->GetGenesisConfig().Region) {
		UpdateRegion();
	}

	//Update controller devices and poll input state for this frame
	_controlManager->UpdateControlDevices();
	_controlManager->UpdateInputState();

	_vdp->BeginFrame();
	_emu->ProcessEvent(EventType::StartFrame, CpuType::GenesisM68K);
	_emu->ProcessEvent(EventType::StartFrame, CpuType::GenesisZ80);

	//Genesis frame timing:
	//NTSC: 262 scanlines per frame, ~59.92 fps
	//PAL:  313 scanlines per frame, ~49.70 fps
	uint32_t scanlinesPerFrame = (_genesisRegion == GenesisRegion::Pal) ? 313 : 262;
	constexpr uint32_t masterClocksPerScanline = 3420;

	//In real hardware, Z80 INT is connected to VDP VBlank and is
	//level-sensitive. However, the ares reference uses a timing hack
	//(apu.setINT(0) in the HBlank handler) that clears Z80 INT shortly
	//after VBlank sets it, ensuring only one interrupt per frame.
	//Without this, the Z80 would take multiple interrupts per VBlank
	//period (IsVblank() stays true for ~30 scanlines), because after
	//the SMPS interrupt handler does EI, INT is still asserted and
	//another interrupt is taken immediately.
	//Match ares behavior by asserting Z80 IRQ only on the rising edge
	//of VBlank (0→1 transition), for exactly one scanline.
	bool prevVblank = false;

	for(uint32_t line = 0; line < scanlinesPerFrame; line++) {
		uint64_t scanlineStart = _masterClock;
		_masterClock += masterClocksPerScanline;

		//Run VDP for one scanline (renders pixels, generates Hblank/Vblank)
		_vdp->RunScanline();

		//Assert Z80 IRQ only on the rising edge of VBlank (one scanline).
		//This matches the ares timing hack where apu.setINT(0) is called
		//in the HBlank handler, clearing INT shortly after VBlank sets it.
		if(_z80) {
			bool vblank = _vdp->IsVblank();
			bool irqEdge = vblank && !prevVblank;
			_z80->SetIrq(irqEdge);

			prevVblank = vblank;
		}

		RunScanlineCpus(scanlineStart);

		_psg->Run(_masterClock);
		_ym2612->Run(_masterClock);

		//NOTE: YM2612 Timer IRQ is NOT connected to Z80 NMI.
		//The ares reference implementation never calls apu.setNMI() — the
		//YM2612 timer IRQ pin is simply not wired to the Z80 in the Genesis.
		//Games that need timer-driven audio use one of:
		//  1. Polling the YM2612 status register (e.g. Batman & Robin)
		//  2. VDP VBlank IRQ → Z80 IRQ (level-sensitive, via apu.setINT)
		//Connecting YM2612 timer edges to Z80 NMI causes spurious NMIs that
		//corrupt the stack of drivers that don't have an NMI handler at 0x0066.
	}

	_psg->PlayQueuedAudio(*_ym2612);

	_frameCount++;

	//Send the rendered frame to the video decoder
	PpuFrameInfo ppuFrame = GetPpuFrame();
	RenderedFrame frame(
		ppuFrame.FrameBuffer,
		ppuFrame.Width,
		ppuFrame.Height,
		1.0,
		ppuFrame.FrameCount,
		_controlManager->GetPortStates());

	bool rewinding = _emu->GetRewindManager()->IsRewinding();
	_emu->GetVideoDecoder()->UpdateFrame(frame, rewinding, rewinding);
	if(!_emu->IsRunAheadFrame()) {
		_vdp->MarkFrameSubmitted();
	}
	_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::PpuFrameDone);
	_emu->ProcessEndOfFrame();
}

void GenesisConsole::RunScanlineCpus(uint64_t scanlineStart)
{
	uint32_t m68kCyclesPerScanline = (uint32_t)((_masterClock - scanlineStart) / GenesisM68kClockDivider);
	//Carry each CPU's instruction overrun into the next scanline.
	while(_m68kClock < _masterClock || _z80Clock < _masterClock) {
		if(_z80Clock <= _m68kClock) {
			RunZ80Instruction();
			continue;
		}

		uint32_t m68kCycle = (uint32_t)((_m68kClock - scanlineStart) / GenesisM68kClockDivider);
		_vdp->SetM68kCyclePosition(m68kCycle, m68kCyclesPerScanline);
		uint32_t cycles = std::max(1u, _m68k->ExecuteInstruction());
		cycles += _vdp->ConsumeBusPenalty();
		_m68kClock += (uint64_t)cycles * GenesisM68kClockDivider;
		if(_m68k->IsStopped()) {
			//The VDP can next wake the 68000 when it starts another scanline.
			_m68kClock = std::max(_m68kClock, _masterClock);
		}
	}
}

void GenesisConsole::RunZ80Instruction()
{
	uint32_t cycles = std::max(1u, _z80->ExecuteInstruction());
	_z80Clock += (uint64_t)cycles * GenesisZ80ClockDivider;
}

void GenesisConsole::RunZ80Until(uint64_t masterClock)
{
	while(_z80Clock < masterClock) {
		RunZ80Instruction();
	}
}

void GenesisConsole::SaveBattery()
{
	if(_memoryManager) {
		_memoryManager->SaveBattery();
	}
}

BaseControlManager* GenesisConsole::GetControlManager()
{
	return _controlManager.get();
}

vector<CpuType> GenesisConsole::GetCpuTypes()
{
	if(_m68k) {
		return { CpuType::GenesisM68K, CpuType::GenesisZ80 };
	}
	return {};
}

double GenesisConsole::GetFps()
{
	return _genesisRegion == GenesisRegion::Pal ? 49.701 : 59.923;
}

PpuFrameInfo GenesisConsole::GetPpuFrame()
{
	PpuFrameInfo frame {};
	if(_vdp) {
		frame.FrameBuffer = (uint8_t*)_vdp->GetScreenPixels();
		frame.Width = _vdp->GetScreenWidth();
		frame.Height = _vdp->GetScreenHeight();
		frame.FrameBufferSize = GenesisScreenWidthH40 * frame.Height * sizeof(uint32_t);
		frame.FrameCount = _vdp->GetFrameCount();
		frame.ScanlineCount = _vdp->GetVisibleHeight();
		frame.FirstScanline = 0;
	} else {
		static vector<uint32_t> blackFrame(GenesisScreenWidthH40 * GenesisScreenHeightV28, 0);
		frame.FrameBuffer = (uint8_t*)blackFrame.data();
		frame.Width = GenesisScreenWidthH40;
		frame.Height = GenesisScreenHeightV28;
		frame.FrameBufferSize = static_cast<uint32_t>(blackFrame.size() * sizeof(uint32_t));
		frame.FrameCount = _frameCount;
		frame.ScanlineCount = GenesisScreenHeightNtsc;
		frame.FirstScanline = 0;
	}
	return frame;
}

BaseVideoFilter* GenesisConsole::GetVideoFilter(bool getDefaultFilter)
{
	return new GenesisDefaultVideoFilter(_emu);
}

uint64_t GenesisConsole::GetMasterClock()
{
	return _masterClock;
}

uint64_t GenesisConsole::GetCpuMasterClock(CpuType cpuType) const
{
	if(cpuType == CpuType::GenesisZ80) {
		return _z80Clock + (uint64_t)_z80->GetInstructionCycles() * GenesisZ80ClockDivider;
	}
	return _m68kClock + (uint64_t)_m68k->GetInstructionCycles() * GenesisM68kClockDivider;
}

uint32_t GenesisConsole::GetMasterClockRate()
{
	return (_genesisRegion == GenesisRegion::Pal) ? GenesisMasterClockPal : GenesisMasterClockNtsc;
}

AddressInfo GenesisConsole::GetAbsoluteAddress(AddressInfo& relAddress)
{
	if(_memoryManager) {
		CpuType cpuType = relAddress.Type == MemoryType::GenesisZ80Memory ? CpuType::GenesisZ80 : CpuType::GenesisM68K;
		return _memoryManager->GetAbsoluteAddress((uint32_t)relAddress.Address, cpuType);
	}
	return { -1, MemoryType::None };
}

AddressInfo GenesisConsole::GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType)
{
	if(_memoryManager) {
		return _memoryManager->GetRelativeAddress(absAddress, cpuType);
	}
	return { -1, MemoryType::None };
}

void GenesisConsole::GetConsoleState(BaseState& state, ConsoleType consoleType)
{
	auto& gs = (GenesisState&)state;
	gs.FrameCount = _frameCount;
	gs.Region = _genesisRegion;

	if(_m68k) {
		GetCpuState(gs.M68K);
	}

	if(_z80) {
		GetCpuState(gs.Z80);
	}

	if(_vdp) {
		gs.Vdp.VCounter = _vdp->GetVCounter();
		gs.Vdp.HCounter = _vdp->GetHCounter();
		gs.Vdp.VBlank = _vdp->IsVblank();
		gs.Vdp.HBlank = _vdp->IsHblank();
		gs.Vdp.DisplayEnable = _vdp->IsDisplayEnable();
		for(uint32_t i = 0; i < 64; i++) {
			gs.Vdp.Cram[i] = _vdp->DebugReadCRAM(i);
		}
	}
}

void GenesisConsole::GetCpuState(GenesisM68KState& state)
{
	for(int i = 0; i < 8; i++) {
		state.D[i] = _m68k->GetDataReg(i);
		state.A[i] = _m68k->GetAddrReg(i);
	}
	state.PC = _m68k->GetInstructionPC();
	state.SR = _m68k->GetSR();
	state.SSP = _m68k->GetSSP();
	state.Stopped = _m68k->IsStopped();
	state.CycleCount = GetCpuMasterClock(CpuType::GenesisM68K) / GenesisM68kClockDivider;
}

void GenesisConsole::GetCpuState(GenesisZ80State& state)
{
	state.A = _z80->GetA();
	state.Flags = _z80->GetFlags();
	state.B = _z80->GetB();
	state.C = _z80->GetC();
	state.D = _z80->GetD();
	state.E = _z80->GetE();
	state.H = _z80->GetH();
	state.L = _z80->GetL();
	state.IX = _z80->GetIX();
	state.IY = _z80->GetIY();
	state.SP = _z80->GetSP();
	state.PC = _z80->GetPC();
	state.I = _z80->GetI();
	state.R = _z80->GetR();
	state.Halted = _z80->IsHalted();
	state.CycleCount = GetCpuMasterClock(CpuType::GenesisZ80) / GenesisZ80ClockDivider;
}

void GenesisConsole::UpdateRegion()
{
	const GenesisConfig& cfg = _emu->GetSettings()->GetGenesisConfig();
	_configuredRegion = cfg.Region;
	switch(_configuredRegion) {
		case ConsoleRegion::Auto: {
			//Parse ROM header region string at offset $1F0.
			//Characters: J=Japan, U=USA, E=Europe, K=Korea, etc.
			//Priority:
			//  - Europe-only ('E' without 'J' or 'U') → PAL
			//  - Japan present, US absent ('J' without 'U', incl. 'JE')
			//    → NTSC-J (domestic). Many JE games (e.g. Bare Knuckle 2)
			//    are Japanese-origin and enforce a region lockout against
			//    export (US) hardware: "developed for use outside north
			//    and south america". Domestic (bit 7 = 0) satisfies them.
			//  - Otherwise (US-only or multi-region incl. 'U') → NTSC-U (export)
			//This is critical because the version register at $A10001 returns
			//bit 7 = 0 (domestic/Japan) only when region == NtscJapan.
			//Japan-only games check this bit and show a region lockout
			//("DEVELOPED FOR USE ONLY WITH NTSC MEGA DRIVE SYSTEMS")
			//if they detect an export console.
			ConsoleRegion region = ConsoleRegion::Ntsc;
			if(_romRegion.find('E') != string::npos &&
				_romRegion.find('U') == string::npos &&
				_romRegion.find('J') == string::npos) {
				region = ConsoleRegion::Pal;
			} else if(_romRegion.find('J') != string::npos &&
				_romRegion.find('U') == string::npos) {
				//J present, U absent → domestic NTSC-J. This covers both
				//'J' (Japan-only) and 'JE' (Japan+Europe). We prefer
				//NtscJapan over Pal for 'JE' because the game is
				//Japanese-origin and runs at NTSC timing (262 lines/60Hz).
				region = ConsoleRegion::NtscJapan;
			}
			//Fall back to filename tags when the ROM header region string
			//is empty or doesn't contain any of J/U/E.
			if(_romRegion.find_first_of("JUEjue") == string::npos) {
				string filename = StringUtilities::ToLower(_filename);
				if(filename.find("(europe)") != string::npos || filename.find("(e)") != string::npos ||
					filename.find("[e]") != string::npos) {
					region = ConsoleRegion::Pal;
				} else if(filename.find("(japan)") != string::npos || filename.find("(j)") != string::npos ||
					filename.find("[j]") != string::npos) {
					region = ConsoleRegion::NtscJapan;
				} else if(filename.find("(usa)") != string::npos || filename.find("(u)") != string::npos ||
					filename.find("[u]") != string::npos) {
					region = ConsoleRegion::Ntsc;
				} else {
					//No header region AND no recognizable filename tag —
					//typically a headerless pirate/hack ROM (no "SEGA" at
					//0x100, region string at 0x1F0 all zeros, e.g. Chinese
					//hacks tagged "[CN]"). Default to NtscJapan (domestic)
					//so the version register reports export=0: Japanese
					//domestic games enforce a region lockout against export
					//(US) hardware with messages like "developed for use
					//outside north and south america", and many headerless
					//ROMs are Japanese-origin. US games rarely lock out
					//domestic hardware, so this is the safer default.
					region = ConsoleRegion::NtscJapan;
				}
			}
			_region = region;
			_genesisRegion = (region == ConsoleRegion::Pal) ? GenesisRegion::Pal : GenesisRegion::Ntsc;
			break;
		}
		case ConsoleRegion::Ntsc:
		case ConsoleRegion::NtscJapan:
			_region = _configuredRegion;
			_genesisRegion = GenesisRegion::Ntsc;
			break;
		case ConsoleRegion::Pal:
			_region = ConsoleRegion::Pal;
			_genesisRegion = GenesisRegion::Pal;
			break;
		default:
			_region = ConsoleRegion::Ntsc;
			_genesisRegion = GenesisRegion::Ntsc;
			break;
	}
	_model = cfg.Model;

	if(_psg) {
		_psg->SetRegion(_region);
	}
	if(_ym2612) {
		_ym2612->SetRegion(_region);
	}
	//VDP must also be told the region — it defaults to NTSC in its
	//constructor and otherwise never learns it. Without this, the VDP
	//status register reports NTSC (bit 0 = 0), runs 262 scanlines/frame
	//instead of 313, and uses NTSC VBlank timing. PAL-strict ROMs (e.g.
	//Titan Overdrive 2) detect this and refuse to run with messages
	//like "THIS DEMO REQUIRES PAL/50HZ".
	if(_vdp) {
		_vdp->SetRegion(_region);
	}
}

void GenesisConsole::Serialize(Serializer& s)
{
	SV(_m68k);
	SV(_z80);
	SV(_vdp);
	SV(_memoryManager);
	SV(_controlManager);
	SV(_psg);
	SV(_ym2612);

	SV(_frameCount);
	SV(_masterClock);
	if(s.GetFormat() != SerializeFormat::Map) {
		if(!s.IsSaving()) {
			_m68kClock = _masterClock;
			_z80Clock = _masterClock;
		}
		SV(_m68kClock);
		SV(_z80Clock);
		if(!s.IsSaving()) {
			//RunFrame starts a new scanline after loading a state saved mid-line.
			_m68kClock = std::max(_m68kClock, _masterClock);
			_z80Clock = std::max(_z80Clock, _masterClock);
		}
	}
	SV(_sramEnable);
	SV(_sramWritable);
	SV(_banked);
	for(int i = 0; i < 8; i++) {
		SVI(_romBank[i]);
	}

	//The ROM selects the board. The saved flag only tells us whether chip state is present.
	bool hasEepromState = s.IsSaving() && _eeprom;
	s.Stream(hasEepromState, "_useEeprom");
	if(_eeprom && hasEepromState) {
		SV(_eeprom);
	} else if(_eeprom && !s.IsSaving()) {
		_eeprom.power();
	}
	if(_eeprom && s.IsSaving()) {
		SV(_eepromRsda);
		SV(_eepromWsda);
		SV(_eepromWscl);
	}
}
