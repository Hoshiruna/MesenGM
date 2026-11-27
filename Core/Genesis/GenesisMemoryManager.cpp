#include "pch.h"
#include "Genesis/GenesisMemoryManager.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisM68K.h"
#include "Genesis/GenesisZ80.h"
#include "Genesis/GenesisVdp.h"
#include "Genesis/GenesisPsg.h"
#include "Genesis/GenesisYm2612.h"
#include "Genesis/GenesisControlManager.h"
#include "Genesis/GenesisEeprom.h"
#include "Shared/Emulator.h"
#include "Shared/BatteryManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/CheatManager.h"
#include "Utilities/Serializer.h"

GenesisMemoryManager::GenesisMemoryManager()
{
	_m68kRam = new uint8_t[M68KRamSize];
	_z80Ram = new uint8_t[Z80RamSize];
	memset(_m68kRam, 0, M68KRamSize);
	memset(_z80Ram, 0, Z80RamSize);
}

GenesisMemoryManager::~GenesisMemoryManager()
{
	delete[] _m68kRam;
	delete[] _z80Ram;
	delete[] _originalSram;
	delete[] _originalEeprom;
}

void GenesisMemoryManager::Init(Emulator* emu, GenesisConsole* console,
	GenesisM68K* m68k, GenesisZ80* z80, GenesisVdp* vdp,
	GenesisPsg* psg, GenesisYm2612* ym2612,
	GenesisControlManager* controlManager,
	uint8_t* rom, uint32_t romSize,
	uint8_t* sram, uint32_t sramSize,
	uint32_t sramStart, bool sramWritable, bool sramOddByte,
	bool useSsfMapper, uint8_t* romBank,
	bool useEeprom, GenesisEeprom* eeprom,
	uint8_t eepromRsda, uint8_t eepromWsda, uint8_t eepromWscl, bool eepromIgnoresWriteStrobes)
{
	_emu = emu;
	_console = console;
	_m68k = m68k;
	_z80 = z80;
	_vdp = vdp;
	_psg = psg;
	_ym2612 = ym2612;
	_controlManager = controlManager;

	_rom = rom;
	_romSize = romSize;
	//ROM is padded to power-of-2 in InitCart, so _romSize-1 is a valid mask.
	//Guard against _romSize==0 (no ROM) to avoid a 0xFFFFFFFF mask.
	_romMask = _romSize > 0 ? _romSize - 1 : 0;
	_sram = sram;
	_sramSize = sramSize;
	_sramStart = sramStart;
	_sramWritable = sramWritable;
	_sramOddByte = sramOddByte;
	_useSsfMapper = useSsfMapper;
	_romBank = romBank;
	//On the SSF mapper, SRAM access is gated by bit 0 of the control
	//register at 0xA130F0 (ramEnable). ares initializes ramEnable=0 at
	//power-on, so SRAM is not accessible until the game explicitly enables
	//it. For non-banked carts, SRAM is always accessible when present.
	_sramEnable = _useSsfMapper ? false : (sramSize > 0);

	//EEPROM state (owned by GenesisConsole; we hold a pointer for
	//bit-banging access). When _useEeprom is true, the SRAM address
	//range is repurposed for SDA/SCL bit-banging.
	_useEeprom = useEeprom;
	_eeprom = eeprom;
	_eepromRsda = eepromRsda;
	_eepromWsda = eepromWsda;
	_eepromWscl = eepromWscl;
	_eepromIgnoresWriteStrobes = eepromIgnoresWriteStrobes;

	//Register memory regions with the emulator for debugger/cheat support
	_emu->RegisterMemory(MemoryType::GenesisM68KRam, _m68kRam, M68KRamSize);
	_emu->RegisterMemory(MemoryType::GenesisZ80Ram, _z80Ram, Z80RamSize);
	if(_rom) {
		_emu->RegisterMemory(MemoryType::GenesisCartridgeRom, _rom, _romSize);
	}
	if(_sram && _sramSize > 0) {
		_emu->RegisterMemory(MemoryType::GenesisCartridgeRam, _sram, _sramSize);
	}
	//Register the EEPROM data array as GenesisCartridgeRam so the
	//debugger's memory viewer can inspect save contents. The EEPROM is
	//not directly memory-mapped (access is via I2C bit-banging), but
	//exposing the raw bytes is useful for debugging saves.
	if(_useEeprom && _eeprom && _eeprom->size() > 0) {
		_emu->RegisterMemory(MemoryType::GenesisCartridgeRam, _eeprom->memory, _eeprom->size());
	}

	//Initialize RAM
	console->InitializeRam(_m68kRam, M68KRamSize);
	console->InitializeRam(_z80Ram, Z80RamSize);

	//Load battery (SRAM or EEPROM)
	LoadBattery();
	if(_sram && _sramSize > 0) {
		_originalSram = new uint8_t[_sramSize];
		memcpy(_originalSram, _sram, _sramSize);
	}
	if(_useEeprom && _eeprom && _eeprom->size() > 0) {
		uint32_t eepromSize = _eeprom->size();
		_originalEeprom = new uint8_t[eepromSize];
		memcpy(_originalEeprom, _eeprom->memory, eepromSize);
	}

	//Wire M68K bus callbacks
	_m68k->BusRead = [this](uint8_t upper, uint8_t lower, uint32_t address) -> uint16_t {
		return M68KRead(upper, lower, address);
	};
	_m68k->BusWrite = [this](uint8_t upper, uint8_t lower, uint32_t address, uint16_t data) {
		M68KWrite(upper, lower, address, data, CpuType::GenesisM68K);
	};
	_m68k->BusIdle = [this](uint32_t cycles) {
		_m68k->AddCycles(cycles);
	};
	_m68k->BusWait = [this](uint32_t cycles) {
		_m68k->AddCycles(cycles);
	};
	_m68k->CheckInterrupts = [this]() -> bool {
		if(!_vdp->GetVblankIrq() && !_vdp->GetHblankIrq() && !_vdp->GetExternalIrq()) {
			return false;
		}
		return PollM68KInterruptsBool();
	};

	//Wire Z80 bus callbacks
	_z80->BusRead = [this](uint16_t addr) -> uint8_t {
		return Z80Read(addr);
	};
	_z80->BusWrite = [this](uint16_t addr, uint8_t data) {
		Z80Write(addr, data);
	};
	_z80->BusWait = [this](uint32_t cycles) {
		_z80->AddCycles(cycles);
	};

	//Wire VDP DMA read callback
	_vdp->DmaRead = [this](uint32_t address) -> uint16_t {
		return DmaRead(address);
	};
}

void GenesisMemoryManager::Reset()
{
	_z80Bank = 0;
	_busreqLine = false;
	_busreqAck = false;
	_resetLine = false;
	_vdpEnable[0] = !_tmssEnable;
	_vdpEnable[1] = !_tmssEnable;
	_romEnable = !_tmssEnable;
	_io = {};
	_io.version = _tmssEnable ? 1 : 0;
}

// ============================================================================
// M68K bus read
// ============================================================================

uint16_t GenesisMemoryManager::M68KRead(uint8_t upper, uint8_t lower, uint32_t address)
{
	address &= 0x00FFFFFE; //word-align

	//0x000000-0x3FFFFF: Cartridge ROM
	if(address < 0x400000) {
		if(!_romEnable && _tmssEnable) {
			//TMSS: when ROM is disabled, reads go to TMSS BIOS (not implemented;
			//just return open bus for now)
			return 0xFFFF;
		}
		//EEPROM access: SDA bit is read from the configured bit position
		//of the word at _sramStart. The rest of the word is open bus.
		//Takes priority over SRAM (a cart has one or the other, never both).
		if(_useEeprom && _eeprom && address >= _sramStart && address < _sramStart + _sramSize) {
			return ReadEepromWord(0xFFFF, upper, lower);
		}
		//Banked cartridge: use bank mapping
		if(address >= _sramStart && address < GetSramEnd() && _sram && _sramEnable) {
			return ReadSramWord(address);
		}
		return ReadRomWord(address);
	}

	//0xA00000-0xA0FFFF: Z80 bus window
	if(address >= 0xA00000 && address <= 0xA0FFFF) {
		//Z80 RAM (0xA00000-0xA01FFF) is ALWAYS accessible by M68K regardless
		//of bus state — on real hardware, M68K can read/write Z80 RAM at any
		//time. SGDK's XGM driver polls z80ram[0] without requesting the bus.
		if(address <= 0xA01FFF) {
			uint32_t offset = address & 0x1FFF;
			uint8_t hi = upper ? _z80Ram[offset] : 0xFF;
			uint8_t lo = lower ? _z80Ram[offset | 1] : 0xFF;
			return ((uint16_t)hi << 8) | lo;
		}
		//Non-RAM accesses (YM2612, bank register) require bus ownership.
		//Return open bus when Z80 is running AND bus not granted to M68K.
		if(!_busreqAck && _resetLine) {
			return 0xFFFF;
		}
		//0xA04000-0xA040FF: YM2612
		if(address >= 0xA04000 && address <= 0xA040FF) {
			uint8_t status = _ym2612->ReadStatus(_console->GetCpuMasterClock(CpuType::GenesisM68K));
			return ((uint16_t)status << 8) | status;
		}
		return 0xFFFF;
	}

	//0xA10000-0xA1FFFF: I/O region
	if(address >= 0xA10000 && address <= 0xA1FFFF) {
		uint16_t result = ReadM68KIO(address, 0xFFFF);
		return result;
	}

	//0xC00000-0xDFFFFF: VDP
	if(address >= 0xC00000 && address <= 0xDFFFFF) {
		if(!_vdpEnable[0] || !_vdpEnable[1]) {
			return 0xFFFF;
		}
		//VDP data port: 0xC00000-0xC00003
		if(address <= 0xC00003) {
			return _vdp->ReadDataPort();
		}
		//VDP control port: 0xC00004-0xC00007
		if(address <= 0xC00007) {
			return _vdp->ReadControlPort();
		}
		//HV counter: 0xC00008-0xC0000B
		if(address <= 0xC0000B) {
			return _vdp->Read(address);
		}
		//Other VDP mirrors
		return _vdp->Read(address);
	}

	//0xE00000-0xFFFFFF: Work RAM (64KB, mirrored)
	if(address >= 0xE00000) {
		uint32_t offset = address & 0xFFFF;
		uint16_t data = ((uint16_t)_m68kRam[offset]) << 8 | _m68kRam[offset + 1];
		return data;
	}

	return 0xFFFF; //open bus
}

// ============================================================================
// M68K bus write
// ============================================================================

void GenesisMemoryManager::M68KWrite(uint8_t upper, uint8_t lower, uint32_t address, uint16_t data, CpuType cpuType)
{
	address &= 0x00FFFFFE;

	//0x000000-0x3FFFFF: Cartridge area (SRAM/EEPROM writes)
	if(address < 0x400000) {
		//EEPROM access: SCL/SDA bits are driven from the configured bit
		//positions of the word write, then the I2C state machine is
		//advanced by one SCL/SDA sample. Takes priority over SRAM.
		if(_useEeprom && _eeprom && address >= _sramStart && address < _sramStart + _sramSize) {
			WriteEepromWord(data, upper, lower);
			return;
		}
		if(address >= _sramStart && address < GetSramEnd() && _sram && _sramEnable) {
			WriteSramWord(address, data, upper, lower);
		}
		//ROM writes ignored
		return;
	}

	//0xA00000-0xA0FFFF: Z80 bus window
	if(address >= 0xA00000 && address <= 0xA0FFFF) {
		//Z80 RAM (0xA00000-0xA01FFF) is ALWAYS accessible by M68K regardless
		//of bus state — on real hardware, M68K can read/write Z80 RAM at any
		//time. SGDK's XGM driver writes command bytes without requesting the bus.
		if(address <= 0xA01FFF) {
			uint32_t offset = address & 0x1FFF;
			//Word writes (upper=1 && lower=1) must write BOTH bytes.
			//The old if/else only wrote the upper byte for word writes,
			//silently dropping the lower byte.
			if(upper) {
				_z80Ram[offset] = (data >> 8) & 0xFF;
			}
			if(lower) {
				_z80Ram[offset | 1] = data & 0xFF;
			}
			return;
		}
		//Non-RAM accesses (YM2612, bank register) require bus ownership.
		//Ignore the write when Z80 is running AND bus not granted to M68K.
		if(!_busreqAck && _resetLine) {
			return;
		}
		//0xA04000-0xA040FF: YM2612
		//YM2612 is 8-bit peripheral: register select = (address & 2) | (upper ? 0 : 1)
		//  bit 1 = port (0/1), bit 0 = address (0) vs data (1)
		if(address >= 0xA04000 && address <= 0xA040FF) {
			uint8_t reg = (address & 2) | (upper ? 0 : 1);
			uint8_t byte = upper ? ((data >> 8) & 0xFF) : (data & 0xFF);
			_ym2612->Write(reg, byte, _console->GetCpuMasterClock(cpuType));
		}
		return;
	}

	//0xA10000-0xA1FFFF: I/O region
	if(address >= 0xA10000 && address <= 0xA1FFFF) {
		WriteM68KIO(address, upper, lower, data, cpuType);
		return;
	}

	//0xC00000-0xDFFFFF: VDP
	if(address >= 0xC00000 && address <= 0xDFFFFF) {
		if(!_vdpEnable[0] || !_vdpEnable[1]) {
			return;
		}
		_vdp->Write(address, data, _console->GetCpuMasterClock(cpuType));
		return;
	}

	//0xE00000-0xFFFFFF: Work RAM
	if(address >= 0xE00000) {
		uint32_t offset = address & 0xFFFE;
		if(upper) {
			_m68kRam[offset] = (data >> 8) & 0xFF;
		}
		if(lower) {
			_m68kRam[offset + 1] = data & 0xFF;
		}
		return;
	}
}

// ============================================================================
// M68K I/O region read (0xA10000-0xA1FFFF)
// ============================================================================

uint16_t GenesisMemoryManager::ReadM68KIO(uint32_t address, uint16_t openBus)
{
	//0xA10000-0xA100FF: I/O ports (mirrored every 0x20)
	//Ares behavior: for word reads, lower byte is copied to upper byte
	//(data.byte(1) = data.byte(0)). This is critical for TMSS detection:
	//games do TST.L $A10008 to check if controller ports are initialized.
	//If upper byte is 0xFF (our old openBus behavior), the game thinks
	//TMSS BIOS already ran and skips the TMSS unlock, leading to a hang.
	if(address >= 0xA10000 && address <= 0xA100FF) {
		uint32_t reg = address & ~0x60; //mirror: a10020-a100ff -> a10000-a1001f
		uint8_t lo = 0;
		switch(reg) {
			case 0xA10000: {
				//Version register
				lo |= _io.version; //bit 0: 0=Model1, 1=Model2+
				lo |= 0x20; //bit 5: 1=no MegaCD
				lo |= (_console->GetRegion() == ConsoleRegion::Pal) ? 0x40 : 0x00; //bit 6: 1=PAL
				lo |= (_console->GetRegion() != ConsoleRegion::NtscJapan) ? 0x80 : 0x00; //bit 7: 1=export
				break;
			}
			case 0xA10002: lo = _controlManager->ReadPort(0); break;
			case 0xA10004: lo = _controlManager->ReadPort(1); break;
			case 0xA10006: lo = _controlManager->ReadPort(2); break;
			case 0xA10008: lo = _controlManager->ReadControl(0); break;
			case 0xA1000A: lo = _controlManager->ReadControl(1); break;
			case 0xA1000C: lo = _controlManager->ReadControl(2); break;
			default: return openBus;
		}
		//Mirror lower byte to upper byte (ares: data.byte(1) = data.byte(0))
		return ((uint16_t)lo << 8) | lo;
	}

	//0xA11100-0xA111FF: Z80 bus request status
	if(address >= 0xA11100 && address <= 0xA111FF) {
		//The status line is visible as D8 on word reads and D0 on byte reads
		//from the odd address ($A11101). Mirroring the bit across both bytes
		//supports both access patterns used by Genesis software.
		uint16_t data = openBus & 0xFEFE;
		if(!_busreqAck) {
			data |= 0x0101;
		}
		return data;
	}

	return openBus;
}

// ============================================================================
// M68K I/O region write (0xA10000-0xA1FFFF)
// ============================================================================

void GenesisMemoryManager::WriteM68KIO(uint32_t address, uint8_t upper, uint8_t lower, uint16_t data, CpuType cpuType)
{
	//0xA10000-0xA100FF: I/O ports
	if(address >= 0xA10000 && address <= 0xA100FF) {
		if(!lower) {
			return; //only lower byte writes are processed
		}
		uint32_t reg = address & ~0x60;
		switch(reg) {
			case 0xA10002:
				_controlManager->WritePort(0, data & 0xFF);
				break;
			case 0xA10004:
				_controlManager->WritePort(1, data & 0xFF);
				break;
			case 0xA10006:
				_controlManager->WritePort(2, data & 0xFF);
				break;
			case 0xA10008:
				_controlManager->WriteControl(0, data & 0xFF);
				break;
			case 0xA1000A:
				_controlManager->WriteControl(1, data & 0xFF);
				break;
			case 0xA1000C:
				_controlManager->WriteControl(2, data & 0xFF);
				break;
		}
		return;
	}

	//0xA11100-0xA111FF: Z80 bus request
	if(address >= 0xA11100 && address <= 0xA111FF) {
		if(!upper) {
			return; //upper byte only
		}
		SetBusreq((data >> 8) & 1, cpuType);
		return;
	}

	//0xA11200-0xA112FF: Z80 reset
	if(address >= 0xA11200 && address <= 0xA112FF) {
		if(!upper) {
			return;
		}
		SetReset((data >> 8) & 1, cpuType);
		return;
	}

	//0xA14000-0xA140FF: TMSS register
	if(address >= 0xA14000 && address <= 0xA140FF) {
		if(!_tmssEnable) {
			return;
		}
		if(address == 0xA14000) {
			if(upper && lower) {
				_vdpEnable[0] = (data == 0x5345); //"SE"
			}
		}
		if(address == 0xA14002) {
			if(upper && lower) {
				_vdpEnable[1] = (data == 0x4741); //"GA"
			}
		}
		return;
	}

	//0xA14100: TMSS ROM enable
	if(address >= 0xA14100 && address <= 0xA141FF) {
		if(!_tmssEnable) {
			return;
		}
		if(lower) {
			_romEnable = data & 1;
		}
		return;
	}

	//0xA130F0-0xA130FF: Cartridge banking/SRAM control register.
	//Two cart types respond to /TIME at this address:
	//  (1) 24MBit+SRAM carts (e.g. Story of Thor, Phantasy Star IV) — only
	//      CTRL0 (0xA130F0) is meaningful; it gates SRAM access on/off
	//      (bit 0 = ramEnable, bit 1 = ramWritable active-low). The game
	//      writes 0x02 to disable SRAM (so ROM is visible at 0x200000),
	//      0x01 to enable writable SRAM, 0x03 for read-only SRAM.
	//  (2) SSF2 mapper carts ("SEGA SSF") — CTRL0 gates SRAM, and CTRL1-7
	//      (0xA130F2-FE) select 512KB ROM banks for regions 1-7.
	//CTRL0 is always handled so 24MBit+SRAM carts can swap SRAM in/out.
	//Bank registers are only handled for SSF2 carts (_romBank != null).
	//For carts without SRAM, CTRL0 writes are harmless (no SRAM to gate).
	//Reference: ares/md/cartridge/board/banked.cpp::writeIO.
	if(address >= 0xA130F0 && address <= 0xA130FF) {
		if(!lower) {
			return; //ares: only lower-byte writes are processed
		}
		//0xA130F0: CTRL0 — ramEnable (bit 0), ramWritable active-low (bit 1)
		if(address == 0xA130F0) {
			_sramEnable = (data & 1) != 0;
			_sramWritable = (data & 2) == 0;
		}
		//0xA130F2-FE: CTRL1-7 — bank registers for 512KB regions 1-7.
		//Bank 0 (0x000000-0x07FFFF) is fixed at identity and not writable.
		//Only the lower 6 bits are used (banks 0-31, 512KB each = 16MB max).
		else if(_romBank) {
			uint32_t index = ((address - 0xA130F0) >> 1) & 7;
			if(index >= 1 && index <= 7) {
				_romBank[index] = (uint8_t)(data & 0x3F);
			}
		}
		return;
	}
}

// ============================================================================
// ROM access helpers
// ============================================================================

uint32_t GenesisMemoryManager::TranslateRomAddress(uint32_t address) const
{
	//SSF2 bank translation (ares/md/cartridge/board/banked.cpp::read):
	//   offset = romBank[address >> 19] << 19 | (address & 0x7FFFF)
	//Bits 19-21 of the M68K address select one of 8 512KB regions; the
	//bank register for that region replaces those bits with the bank number
	//(0-31). For non-banked carts, _romBank is null and the address passes
	//through unchanged.
	if(_useSsfMapper && _romBank) {
		uint32_t region = (address >> 19) & 7;
		return ((uint32_t)_romBank[region] << 19) | (address & 0x7FFFF);
	}
	//Mirror ROM across the 4MB cartridge window for non-banked carts.
	//gpgx (md_cart.c) sets cart.mask = romsize-1 and maps each 64KB block
	//as cart.rom + ((i<<16) & cart.mask), so a ROM smaller than 4MB
	//mirrors to fill the window. Without this, reads beyond _romSize
	//return 0xFFFF (open bus). This breaks games that use the standard
	//Sega mapper (0xA130F0) to switch SRAM off and read ROM from the
	//0x200000+ region — e.g. Daikoukai Jidai II [CN] (2MB ROM with
	//64KB SRAM at 0x200001-0x20FFFF) shows corrupted tiles because
	//the SRAM-disabled ROM reads return 0xFFFF instead of mirrored ROM.
	//Reference: gpgx/core/cart_hw/md_cart.c::md_cart_init (cart.mask).
	return address & _romMask;
}

uint16_t GenesisMemoryManager::ReadRomWord(uint32_t address)
{
	//TranslateRomAddress applies SSF2 bank translation (for banked carts)
	//or ROM mirroring via _romMask (for non-banked carts). The bounds check
	//is a safety net: for non-banked carts the mask already guarantees
	//offset < _romSize; for SSF2 carts a bank register could theoretically
	//point beyond the ROM, in which case we return open bus.
	uint32_t offset = TranslateRomAddress(address);
	if(offset + 1 >= _romSize) {
		return 0xFFFF;
	}
	return ((uint16_t)_rom[offset] << 8) | _rom[offset + 1];
}

uint16_t GenesisMemoryManager::ReadSramWord(uint32_t address)
{
	uint32_t offset = address - _sramStart;
	if(_sramOddByte) {
		//Odd-byte SRAM: use flat 64KB array indexed by offset within the
		//SRAM address range, matching gpgx sram.c::sram_read_word which
		//returns READ_WORD(sram.sram, address & 0xfffe).
		//Even indices hold the upper byte (initialized to 0xFF, may be
		//overwritten by word writes). Odd indices hold the SRAM data.
		uint32_t even = offset & 0xFFFE;
		if(even + 1 >= _sramSize) {
			return 0xFFFF;
		}
		return ((uint16_t)_sram[even] << 8) | _sram[even + 1];
	}
	//Word/byte SRAM: both bytes connected, byte-addressed.
	if(offset >= _sramSize) {
		return 0xFFFF;
	}
	uint16_t data = 0xFFFF;
	if(offset < _sramSize) {
		data = ((uint16_t)_sram[offset]) << 8;
		if(offset + 1 < _sramSize) {
			data |= _sram[offset + 1];
		}
	}
	return data;
}

void GenesisMemoryManager::WriteSramWord(uint32_t address, uint16_t data, uint8_t upper, uint8_t lower)
{
	if(!_sramWritable) {
		return;
	}
	uint32_t offset = address - _sramStart;
	if(_sramOddByte) {
		//Odd-byte SRAM: use flat 64KB array, matching gpgx sram.c::
		//sram_write_word which does WRITE_WORD(sram.sram, addr & 0xfffe,
		//data), storing BOTH bytes. On real hardware only D0-D7 (/LDS)
		//are connected, so even-byte writes do nothing. But gpgx stores
		//them anyway, and some games depend on this behavior.
		uint32_t even = offset & 0xFFFE;
		if(even + 1 >= _sramSize) {
			return;
		}
		if(upper) {
			_sram[even] = (data >> 8) & 0xFF;
		}
		if(lower) {
			_sram[even + 1] = data & 0xFF;
		}
		return;
	}
	//Word/byte SRAM: upper byte at _sram[offset], lower byte at [offset+1].
	if(upper && offset < _sramSize) {
		_sram[offset] = (data >> 8) & 0xFF;
	}
	if(lower && offset + 1 < _sramSize) {
		_sram[offset + 1] = data & 0xFF;
	}
}

// ============================================================================
// EEPROM (M24C) bit-bang access
// ============================================================================

uint16_t GenesisMemoryManager::ReadEepromWord(uint16_t data, uint8_t upper, uint8_t lower)
{
	//Read the SDA bit from the EEPROM and place it at the configured bit
	//position in the returned word. The upper nibble of the bit position
	//(rsda >> 3) selects the byte: 0 = low byte (lower), 1 = high byte
	//(upper). The rest of the word is left as-is (open bus, passed in as
	//`data`). Reference: ares/md/cartridge/board/standard.cpp::read.
	if(!_eeprom) {
		return data;
	}
	bool sda = _eeprom->read();
	if(upper && (_eepromRsda >> 3) == 1) {
		if(sda) {
			data |= (1 << _eepromRsda);
		} else {
			data &= ~(1 << _eepromRsda);
		}
	}
	if(lower && (_eepromRsda >> 3) == 0) {
		if(sda) {
			data |= (1 << _eepromRsda);
		} else {
			data &= ~(1 << _eepromRsda);
		}
	}
	return data;
}

void GenesisMemoryManager::WriteEepromWord(uint16_t data, uint8_t upper, uint8_t lower)
{
	//Drive SCL and SDA from the configured bit positions of the written
	//word, then advance the I2C state machine by one sample. The upper
	//nibble of each bit position (>>3) selects the byte: 0 = low, 1 = high.
	//Special case: wscl == 8 selects the 32Mbit Acclaim mapper where a
	//word write toggles eepromEnable (not implemented here).
	//Reference: ares/md/cartridge/board/standard.cpp::write.
	if(!_eeprom) {
		return;
	}
	if(_eepromWscl == 8 && upper && lower) {
		//32Mbit Acclaim mapper: control via word write. Not implemented;
		//EEPROM is always enabled in our default config.
		return;
	}
	bool writeClock = _eepromIgnoresWriteStrobes || (_eepromWscl < 8 ? lower : upper);
	bool writeData = _eepromIgnoresWriteStrobes || (_eepromWsda < 8 ? lower : upper);
	if(!writeClock && !writeData) {
		return;
	}
	_eeprom->clock = writeClock ? ((data >> _eepromWscl) & 1) != 0 : _eeprom->clock();
	_eeprom->data = writeData ? ((data >> _eepromWsda) & 1) != 0 : _eeprom->data();
	_eeprom->write();
}

// ============================================================================
// Z80 bus read
// ============================================================================

uint8_t GenesisMemoryManager::Z80Read(uint16_t address)
{
	//0x0000-0x1FFF: Z80 RAM (8KB, mirrored at 0x2000-0x3FFF)
	if(address <= 0x3FFF) {
		return _z80Ram[address & 0x1FFF];
	}

	//0x4000-0x5FFF: YM2612 — reading any address returns the status byte
	if(address >= 0x4000 && address <= 0x5FFF) {
		return _ym2612->ReadStatus(_console->GetCpuMasterClock(CpuType::GenesisZ80));
	}

	//0x6000-0x60FF: Bank register
	if(address >= 0x6000 && address <= 0x60FF) {
		return 0xFF; //write-only
	}

	//0x7F00-0x7FFF: VDP access
	if(address >= 0x7F00 && address <= 0x7FFF) {
		uint32_t m68kAddr = 0xC00000 | (address & 0xFF);
		return Z80ReadExternal(m68kAddr);
	}

	//0x8000-0xFFFF: M68K bus window (banked)
	if(address >= 0x8000) {
		uint32_t m68kAddr = (_z80Bank << 15) | (address & 0x7FFF);
		return Z80ReadExternal(m68kAddr);
	}

	return 0xFF;
}

// ============================================================================
// Z80 bus write
// ============================================================================

void GenesisMemoryManager::Z80Write(uint16_t address, uint8_t data)
{
	//0x0000-0x3FFF: Z80 RAM
	if(address <= 0x3FFF) {
		uint16_t off = address & 0x1FFF;
		_z80Ram[off] = data;
		return;
	}

	//0x4000-0x5FFF: YM2612
	if(address >= 0x4000 && address <= 0x5FFF) {
		_ym2612->Write(address & 3, data, _console->GetCpuMasterClock(CpuType::GenesisZ80));
		return;
	}

	//0x6000-0x60FF: Bank register
	if(address >= 0x6000 && address <= 0x60FF) {
		_z80Bank = ((data & 1) << 8) | (_z80Bank >> 1);
		return;
	}

	//0x7F00-0x7FFF: VDP access
	if(address >= 0x7F00 && address <= 0x7FFF) {
		uint32_t m68kAddr = 0xC00000 | (address & 0xFF);
		Z80WriteExternal(m68kAddr, data);
		return;
	}

	//0x8000-0xFFFF: M68K bus window (banked)
	if(address >= 0x8000) {
		uint32_t m68kAddr = (_z80Bank << 15) | (address & 0x7FFF);
		Z80WriteExternal(m68kAddr, data);
		return;
	}
}

// ============================================================================
// Z80 external bus access (through M68K bus)
// ============================================================================

uint8_t GenesisMemoryManager::Z80ReadExternal(uint32_t m68kAddress)
{
	//Z80 cannot read M68K RAM — returns open bus
	if(m68kAddress >= 0xE00000 && m68kAddress <= 0xFFFFFF) {
		return 0xFF;
	}

	//Z80 can access: ROM (0x000000-0x9FFFFF), I/O (0xA10000-0xA1FFFF),
	//VDP (0xC00000-0xC000FF)
	bool accessible =
		(m68kAddress < 0xA00000) ||
		(m68kAddress >= 0xA10000 && m68kAddress <= 0xA1FFFF) ||
		(m68kAddress >= 0xC00000 && m68kAddress <= 0xC000FF);

	if(!accessible) {
		return 0xFF;
	}

	//Perform a word read on the M68K bus and return the appropriate byte
	uint16_t word = M68KRead(1, 1, m68kAddress & ~1);
	if(m68kAddress & 1) {
		return word & 0xFF; //odd address = low byte
	} else {
		return (word >> 8) & 0xFF; //even address = high byte
	}
}

void GenesisMemoryManager::Z80WriteExternal(uint32_t m68kAddress, uint8_t data)
{
	//Z80 can write to: ROM area (for SRAM), I/O, VDP, M68K RAM
	bool accessible =
		(m68kAddress < 0xA00000) ||
		(m68kAddress >= 0xA10000 && m68kAddress <= 0xA1FFFF) ||
		(m68kAddress >= 0xC00000 && m68kAddress <= 0xC000FF) ||
		(m68kAddress >= 0xE00000 && m68kAddress <= 0xFFFFFF);

	if(!accessible) {
		return;
	}

	//Write as a word with the byte in both positions (ares pattern)
	uint16_t word = (data << 8) | data;
	if(m68kAddress & 1) {
		M68KWrite(0, 1, m68kAddress & ~1, word, CpuType::GenesisZ80);
	} else {
		M68KWrite(1, 0, m68kAddress, word, CpuType::GenesisZ80);
	}
}

// ============================================================================
// Bus arbitration
// ============================================================================

void GenesisMemoryManager::SetBusreq(bool line, CpuType cpuType)
{
	if(cpuType == CpuType::GenesisM68K && line != _busreqLine) {
		_console->RunZ80Until(_console->GetCpuMasterClock(cpuType));
	}
	_busreqLine = line;
	_busreqAck = line;
	_z80->SetBusreq(line);
}

void GenesisMemoryManager::SetReset(bool line, CpuType cpuType)
{
	if(cpuType == CpuType::GenesisM68K && line != _resetLine) {
		_console->RunZ80Until(_console->GetCpuMasterClock(cpuType));
	}
	//line=true: Z80 released from reset (running); line=false: Z80 held in reset
	_resetLine = line;
	_z80->SetReset(line);
}

bool GenesisMemoryManager::IsBusGranted() const
{
	return !_busreqAck;
}

// ============================================================================
// VDP DMA read
// ============================================================================

uint16_t GenesisMemoryManager::DmaRead(uint32_t address)
{
	//VDP DMA reads from the M68K bus (same as M68KRead but without
	//side effects like register access — just ROM/RAM reads)
	address &= 0x00FFFFFE;

	//0x000000-0x3FFFFF: Cartridge ROM (with SSF2 bank translation applied
	//inside ReadRomWord when _useSsfMapper is set).
	if(address < 0x400000) {
		//EEPROM region — DMA reads return the SDA bit (read() is const,
		//no side effects on the I2C state machine). Games don't normally
		//DMA from the EEPROM address, but this matches M68KRead behavior.
		if(_useEeprom && _eeprom && address >= _sramStart && address < _sramStart + _sramSize) {
			return ReadEepromWord(0xFFFF, 1, 1);
		}
		//SRAM region (only when enabled; ares checks sramAddr range first).
		if(address >= _sramStart && address < GetSramEnd() && _sram && _sramEnable) {
			return ReadSramWord(address);
		}
		return ReadRomWord(address);
	}

	//0x400000-0xBFFFFF: ROM mirror (non-banked carts only).
	//Banked carts (SSF2) do not decode this region — return open bus,
	//matching real hardware. ReadRomWord applies bank translation to
	//the masked address so a bank-switched region is still honored.
	if(address < 0xC00000) {
		if(_useSsfMapper) {
			return 0xFFFF;
		}
		return ReadRomWord(address & 0x3FFFFE);
	}

	//M68K RAM (64KB, mirrored across 0xE00000-0xFFFFFF)
	if(address >= 0xE00000) {
		uint32_t offset = address & 0xFFFF;
		return ((uint16_t)_m68kRam[offset] << 8) | _m68kRam[(offset + 1) & 0xFFFF];
	}

	//I/O region — some games do DMA from version register
	if(address >= 0xA10000 && address <= 0xA1FFFF) {
		return ReadM68KIO(address, 0xFFFF);
	}

	return 0xFFFF;
}

// ============================================================================
// M68K interrupt polling
// ============================================================================

void GenesisMemoryManager::PollM68KInterrupts()
{
	PollM68KInterruptsBool();
}

bool GenesisMemoryManager::PollM68KInterruptsBool()
{
	uint8_t ipl = _m68k->GetInterruptMask(); //current interrupt mask from SR

	//Vblank IRQ (level 6) — highest priority, check first
	if(_vdp->GetVblankIrq()) {
		if(6 > ipl) {
			_vdp->AcknowledgeIrq(6);
			_m68k->Interrupt(GenesisM68K::VLevel6, 6);
			return true;
		}
	}
	//Hblank IRQ (level 4)
	if(_vdp->GetHblankIrq() && 4 > ipl) {
		_vdp->AcknowledgeIrq(4);
		_m68k->Interrupt(GenesisM68K::VLevel4, 4);
		return true;
	}
	//External IRQ (level 2) — lightgun etc, rarely used
	if(_vdp->GetExternalIrq() && 2 > ipl) {
		_vdp->AcknowledgeIrq(2);
		_m68k->Interrupt(GenesisM68K::VLevel2, 2);
		return true;
	}
	return false; //no interrupt delivered
}

// ============================================================================
// Battery (SRAM/EEPROM) save/load
// ============================================================================

void GenesisMemoryManager::LoadBattery()
{
	//EEPROM: load the M24C memory array (size() bytes) from the .sav file.
	//Matches ares Interface::save which writes m24c.memory of m24c.size() bytes.
	if(_useEeprom && _eeprom && _eeprom->size() > 0) {
		_emu->GetBatteryManager()->LoadBattery(".sav", _eeprom->memory, _eeprom->size());
		return;
	}
	if(!_sram || _sramSize == 0) {
		return;
	}
	_emu->GetBatteryManager()->LoadBattery(".sav", _sram, _sramSize);
}

void GenesisMemoryManager::SaveBattery()
{
	//EEPROM: only write if the memory array has changed since load,
	//matching the SRAM change-detection optimization.
	if(_useEeprom && _eeprom && _eeprom->size() > 0) {
		uint32_t eepromSize = _eeprom->size();
		bool changed = (_originalEeprom && memcmp(_eeprom->memory, _originalEeprom, eepromSize) != 0);
		if(changed) {
			_emu->GetBatteryManager()->SaveBattery(".sav", _eeprom->memory, eepromSize);
		}
		return;
	}
	if(!_sram || _sramSize == 0) {
		return;
	}
	if(_originalSram && memcmp(_sram, _originalSram, _sramSize) != 0) {
		_emu->GetBatteryManager()->SaveBattery(".sav", _sram, _sramSize);
	}
}

// ============================================================================
// Address translation
// ============================================================================

AddressInfo GenesisMemoryManager::GetAbsoluteAddress(uint32_t addr, CpuType cpuType, bool forWrite)
{
	if(cpuType == CpuType::GenesisZ80) {
		addr &= 0xFFFF;
		if(addr < 0x4000) {
			return { (int32_t)(addr & 0x1FFF), MemoryType::GenesisZ80Ram };
		}
		if(addr < 0x8000) {
			return { -1, MemoryType::None };
		}
		addr = (_z80Bank << 15) | (addr & 0x7FFF);
		if((addr >= 0xA00000 && addr < 0xA10000) || (!forWrite && addr >= 0xE00000)) {
			return { -1, MemoryType::None };
		}
	}

	addr &= 0xFFFFFF;
	if(addr < 0x400000) {
		if((!_romEnable && _tmssEnable) || (_useEeprom && addr >= _sramStart && addr < GetSramEnd())) {
			return { -1, MemoryType::None };
		}
		if(_sram && _sramEnable && addr >= _sramStart && addr < GetSramEnd()) {
			uint32_t offset = addr - _sramStart;
			if(forWrite && (!_sramWritable || (_sramOddByte && !(addr & 1)))) {
				return { -1, MemoryType::None };
			}
			return { (int32_t)(_sramOddByte ? offset >> 1 : offset), MemoryType::GenesisCartridgeRam };
		}
		uint32_t offset = TranslateRomAddress(addr);
		if(offset < _romSize) {
			return { (int32_t)offset, MemoryType::GenesisCartridgeRom };
		}
	} else if(addr >= 0xE00000) {
		return { (int32_t)(addr & 0xFFFF), MemoryType::GenesisM68KRam };
	} else if(addr >= 0xA00000 && addr < 0xA02000) {
		return { (int32_t)(addr & 0x1FFF), MemoryType::GenesisZ80Ram };
	}

	return { -1, MemoryType::None };
}

AddressInfo GenesisMemoryManager::GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType)
{
	if(absAddress.Address < 0) {
		return { -1, MemoryType::None };
	}

	uint32_t offset = absAddress.Address;
	if(cpuType == CpuType::GenesisZ80 && absAddress.Type == MemoryType::GenesisZ80Ram && offset < Z80RamSize) {
		return { (int32_t)offset, MemoryType::GenesisZ80Memory };
	}

	auto translate = [&](uint32_t address) -> AddressInfo {
		MemoryType type = MemoryType::GenesisM68KMemory;
		if(cpuType == CpuType::GenesisZ80) {
			if((address >> 15) != _z80Bank) {
				return { -1, MemoryType::None };
			}
			type = MemoryType::GenesisZ80Memory;
			address = 0x8000 | (address & 0x7FFF);
		}
		AddressInfo mapped = GetAbsoluteAddress(address, cpuType);
		if(mapped.Type == absAddress.Type && mapped.Address == absAddress.Address) {
			return { (int32_t)address, type };
		}
		return { -1, MemoryType::None };
	};

	switch(absAddress.Type) {
		case MemoryType::GenesisCartridgeRom:
			if(offset < _romSize && offset < 0x1000000) {
				uint32_t stride = _useSsfMapper ? 0x80000 : _romSize;
				for(uint32_t address = offset % stride; address < 0x400000; address += stride) {
					AddressInfo relative = translate(address);
					if(relative.Address >= 0) {
						return relative;
					}
				}
			}
			break;
		case MemoryType::GenesisCartridgeRam:
			if(_sram && offset < _sramSize) {
				return translate(_sramStart + (_sramOddByte ? offset * 2 + 1 : offset));
			}
			break;
		case MemoryType::GenesisM68KRam:
			if(offset < M68KRamSize) {
				return translate(0xE00000 + offset);
			}
			break;
		case MemoryType::GenesisZ80Ram:
			if(offset < Z80RamSize) {
				return translate(0xA00000 + offset);
			}
			break;
		default: break;
	}
	return { -1, MemoryType::None };
}

// ============================================================================
// Debug access (no side effects)
// ============================================================================

uint8_t GenesisMemoryManager::M68KDebugRead(uint32_t address)
{
	address &= 0x00FFFFFF;

	//0x000000-0x3FFFFF: Cartridge ROM / SRAM / EEPROM
	if(address < 0x400000) {
		//EEPROM: return open bus for debug reads. The EEPROM is accessed
		//via I2C bit-banging, so a debug read can't return a meaningful
		//byte (the SDA bit is the only readable value, and reading it
		//here would not advance the state machine).
		if(_useEeprom && address >= _sramStart && address < _sramStart + _sramSize) {
			return 0xFF;
		}
		if(address >= _sramStart && address < GetSramEnd() && _sram && _sramEnable) {
			uint32_t offset = address - _sramStart;
			uint32_t index = _sramOddByte ? offset >> 1 : offset;
			return _sram[index];
		}
		//Apply SSF2 bank translation so the debugger sees the same byte
		//the CPU would see at this M68K address.
		uint32_t offset = TranslateRomAddress(address);
		return (_romEnable || !_tmssEnable) && offset < _romSize ? _rom[offset] : 0xFF;
	}

	//0xA00000-0xA01FFF: Z80 RAM
	if(address >= 0xA00000 && address <= 0xA01FFF) {
		return _z80Ram[address & 0x1FFF];
	}

	//0xE00000-0xFFFFFF: M68K work RAM (64KB, mirrored)
	if(address >= 0xE00000) {
		return _m68kRam[address & 0xFFFF];
	}

	return 0xFF;
}

uint8_t GenesisMemoryManager::Z80DebugRead(uint16_t address)
{
	//0x0000-0x3FFF: Z80 RAM (8KB, mirrored at 0x2000-0x3FFF)
	if(address <= 0x3FFF) {
		return _z80Ram[address & 0x1FFF];
	}

	//0x8000-0xFFFF: M68K bus window (banked) — read without side effects
	if(address >= 0x8000) {
		uint32_t m68kAddr = (_z80Bank << 15) | (address & 0x7FFF);
		//Z80 cannot read M68K RAM
		if(m68kAddr >= 0xE00000 && m68kAddr <= 0xFFFFFF) {
			return 0xFF;
		}
		bool accessible =
			(m68kAddr < 0xA00000) ||
			(m68kAddr >= 0xA10000 && m68kAddr <= 0xA1FFFF) ||
			(m68kAddr >= 0xC00000 && m68kAddr <= 0xC000FF);
		if(!accessible) {
			return 0xFF;
		}
		return M68KDebugRead(m68kAddr);
	}

	//Other regions (YM2612, bank register, VDP) — return open bus for debug
	return 0xFF;
}

void GenesisMemoryManager::M68KDebugWrite(uint32_t address, uint8_t value)
{
	address &= 0x00FFFFFF;

	//0x000000-0x3FFFFF: Cartridge area (SRAM/EEPROM writes)
	if(address < 0x400000) {
		//EEPROM: debug writes are silently ignored. The EEPROM is
		//accessed via I2C bit-banging; a direct byte write would
		//corrupt the I2C state. Use the memory viewer to edit EEPROM
		//bytes directly via the GenesisCartridgeRam registration.
		if(_useEeprom && address >= _sramStart && address < _sramStart + _sramSize) {
			return;
		}
		if(address >= _sramStart && address < GetSramEnd() && _sram && _sramEnable && _sramWritable) {
			uint32_t offset = address - _sramStart;
			uint32_t idx = _sramOddByte ? (offset >> 1) : offset;
			if(idx < _sramSize) {
				_sram[idx] = value;
			}
		}
		return;
	}

	//0xA00000-0xA01FFF: Z80 RAM
	if(address >= 0xA00000 && address <= 0xA01FFF) {
		_z80Ram[address & 0x1FFF] = value;
		return;
	}

	//0xE00000-0xFFFFFF: M68K work RAM (64KB, mirrored)
	if(address >= 0xE00000) {
		_m68kRam[address & 0xFFFF] = value;
		return;
	}
}

void GenesisMemoryManager::Z80DebugWrite(uint16_t address, uint8_t value)
{
	//0x0000-0x3FFF: Z80 RAM
	if(address <= 0x3FFF) {
		_z80Ram[address & 0x1FFF] = value;
		return;
	}

	//0x8000-0xFFFF: M68K bus window (banked) — write through to M68K bus
	if(address >= 0x8000) {
		uint32_t m68kAddr = (_z80Bank << 15) | (address & 0x7FFF);
		bool accessible =
			(m68kAddr < 0xA00000) ||
			(m68kAddr >= 0xA10000 && m68kAddr <= 0xA1FFFF) ||
			(m68kAddr >= 0xC00000 && m68kAddr <= 0xC000FF) ||
			(m68kAddr >= 0xE00000 && m68kAddr <= 0xFFFFFF);
		if(accessible) {
			M68KDebugWrite(m68kAddr, value);
		}
	}
}

void GenesisMemoryManager::M68KPeekBlock(uint32_t start, uint8_t* dest)
{
	for(uint32_t i = 0; i < 0x1000; i++) {
		dest[i] = M68KDebugRead(start + i);
	}
}

// ============================================================================
// Serialization
// ============================================================================

void GenesisMemoryManager::Serialize(Serializer& s)
{
	SV(_z80Bank);
	SV(_busreqLine);
	SV(_busreqAck);
	SV(_resetLine);
	SV(_sramEnable);
	SV(_sramWritable);
	SV(_romEnable);
	SV(_vdpEnable[0]);
	SV(_vdpEnable[1]);
	SV(_io.version);

	SVArray(_m68kRam, M68KRamSize);
	SVArray(_z80Ram, Z80RamSize);
	if(_sram && _sramSize > 0) {
		SVArray(_sram, _sramSize);
	}
}
