#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"
#include "Shared/MemoryType.h"
#include "Shared/CheatManager.h"
#include "Shared/MemoryOperationType.h"
#include "Debugger/AddressInfo.h"

class Emulator;
class GenesisConsole;
class GenesisM68K;
class GenesisZ80;
class GenesisVdp;
class GenesisPsg;
class GenesisYm2612;
class GenesisControlManager;
class GenesisEeprom;

// Genesis Memory Manager — M68K + Z80 bus arbiter.
//
// Handles the full 24-bit M68K address space and the 16-bit Z80 address space,
// routing reads/writes to the correct subsystem (ROM, RAM, VDP, YM2612, PSG,
// I/O ports, Z80 bus).  Also manages bus arbitration (Z80 bus request / grant),
// the Z80 bank register, and TMSS enable logic.
//
// Design follows the SmsMemoryManager pattern: raw pointers to subsystems
// (owned by GenesisConsole), no ares types.  The M68K bus uses word-granularity
// read/write callbacks set during Init(); the Z80 bus uses byte-granularity.

class GenesisMemoryManager final : public ISerializable
{
public:
	GenesisMemoryManager();
	~GenesisMemoryManager();

	void Init(Emulator* emu, GenesisConsole* console,
		GenesisM68K* m68k, GenesisZ80* z80, GenesisVdp* vdp,
		GenesisPsg* psg, GenesisYm2612* ym2612,
		GenesisControlManager* controlManager,
		uint8_t* rom, uint32_t romSize,
		uint8_t* sram, uint32_t sramSize,
		uint32_t sramStart, bool sramWritable, bool sramOddByte,
		bool useSsfMapper, uint8_t* romBank,
		bool useEeprom, GenesisEeprom* eeprom,
		uint8_t eepromRsda, uint8_t eepromWsda, uint8_t eepromWscl, bool eepromIgnoresWriteStrobes);

	void Reset();

	//--- M68K bus interface (word-granularity, set on GenesisM68K) ---
	uint16_t M68KRead(uint8_t upper, uint8_t lower, uint32_t address);
	void M68KWrite(uint8_t upper, uint8_t lower, uint32_t address, uint16_t data, CpuType cpuType);

	//--- Z80 bus interface (byte-granularity, set on GenesisZ80) ---
	uint8_t Z80Read(uint16_t address);
	void Z80Write(uint16_t address, uint8_t data);

	//--- Z80 bus arbitration ---
	bool IsBusGranted() const; //true when Z80 owns the bus

	//--- VDP DMA read (M68K bus, used during 68K->VDP DMA) ---
	uint16_t DmaRead(uint32_t address);

	//--- M68K interrupt polling ---
	void PollM68KInterrupts();
	bool PollM68KInterruptsBool(); //returns true if an interrupt was delivered

	//--- SRAM / Battery ---
	void LoadBattery();
	void SaveBattery();

	//--- Address translation (debugger / cheats) ---
	AddressInfo GetAbsoluteAddress(uint32_t addr, CpuType cpuType, bool forWrite = false);
	AddressInfo GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType);

	//--- Debug access (no side effects) ---
	uint8_t M68KDebugRead(uint32_t address);
	uint8_t Z80DebugRead(uint16_t address);
	void M68KDebugWrite(uint32_t address, uint8_t value);
	void Z80DebugWrite(uint16_t address, uint8_t value);
	void M68KPeekBlock(uint32_t start, uint8_t* dest);

	//--- ISerializable ---
	void Serialize(Serializer& s) override;

private:
	Emulator* _emu = nullptr;
	GenesisConsole* _console = nullptr;
	GenesisM68K* _m68k = nullptr;
	GenesisZ80* _z80 = nullptr;
	GenesisVdp* _vdp = nullptr;
	GenesisPsg* _psg = nullptr;
	GenesisYm2612* _ym2612 = nullptr;
	GenesisControlManager* _controlManager = nullptr;

	//Cartridge ROM (word-accessible, byte-swapped for 68000 big-endian)
	uint8_t* _rom = nullptr;
	uint32_t _romSize = 0;
	//ROM mirror mask (= _romSize - 1, since ROM is padded to power-of-2 in
	//InitCart). For non-banked carts, ROM is mirrored across the full 4MB
	//cartridge window using this mask, matching gpgx's cart.mask. A 2MB ROM
	//mirrors at 0x200000-0x3FFFFF; without this, reads beyond _romSize
	//return 0xFFFF (open bus), corrupting games that switch SRAM off and
	//read ROM from the 0x200000 region (e.g. Daikoukai Jidai II [CN]).
	//Not used for SSF2 banked carts (TranslateRomAddress handles banking).
	uint32_t _romMask = 0;

	//M68K work RAM (64KB)
	static constexpr uint32_t M68KRamSize = 0x10000;
	uint8_t* _m68kRam = nullptr;

	//Z80 RAM (8KB)
	static constexpr uint32_t Z80RamSize = 0x2000;
	uint8_t* _z80Ram = nullptr;

	//Cartridge SRAM (battery-backed)
	uint8_t* _sram = nullptr;
	uint32_t _sramSize = 0;
	uint32_t _sramStart = 0; //M68K address where SRAM window starts
	bool _sramWritable = true;
	bool _sramEnable = false; //for banked cartridges
	//True for odd-byte SRAM (D0-D7 only, /LDS-selected). SRAM chip index
	//is (m68kAddress - _sramStart) >> 1. Word reads return the byte
	//duplicated to both bytes (lram * 0x0101 in ares). Only lower-byte
	//writes are stored. Matches ares/md/cartridge/board/linear.cpp lram.
	bool _sramOddByte = false;
	uint8_t* _originalSram = nullptr;

	//SSF2-style bank switching (SEGA SSF mapper, also used by 4MB+ ROMs).
	//When _useSsfMapper is true, the 8 entries of _romBank[] (owned by
	//GenesisConsole, pointed to here) translate the upper 3 bits of the
	//M68K ROM address (bits 19-21 -> 512KB region index 0-7) into a 6-bit
	//bank number (0-31). The translated address is:
	//  byteOffset = (romBank[address >> 19] << 19) | (address & 0x7FFFF)
	//Bank 0 is fixed (identity mapping, not writable); banks 1-7 are set
	//by writing the lower 6 bits to 0xA130F2/4/6/8/A/C/E. The control
	//register at 0xA130F0 gates SRAM access (bit 0 = ramEnable,
	//bit 1 = ramWritable active-low). Ported from
	//ares/md/cartridge/board/banked.cpp.
	bool _useSsfMapper = false;
	uint8_t* _romBank = nullptr; //points to GenesisConsole::_romBank[8]

	//EEPROM (M24C I2C serial) save storage. When _useEeprom is true,
	//the SRAM address range is used for SDA/SCL bit-banging instead of
	//parallel SRAM. The EEPROM object is owned by GenesisConsole; this
	//is a non-owning pointer used for read/write/bit-bang access.
	//Reference: ares/md/cartridge/board/standard.cpp read/write for m24c.
	bool _useEeprom = false;
	GenesisEeprom* _eeprom = nullptr;
	uint8_t _eepromRsda = 0; //bit position of SDA on reads
	uint8_t _eepromWsda = 0; //bit position of SDA on writes
	uint8_t _eepromWscl = 1; //bit position of SCL on writes
	bool _eepromIgnoresWriteStrobes = false;
	uint8_t* _originalEeprom = nullptr; //snapshot for change detection

	//Z80 bank register (determines which 32KB window of the M68K bus
	//the Z80 sees at 0x8000-0xFFFF)
	uint16_t _z80Bank = 0;

	//Bus arbitration state
	bool _busreqLine = false; //M68K has requested the Z80 bus
	bool _busreqAck = false; //Z80 has acknowledged bus request
	bool _resetLine = false; //Z80 is held in reset

	//TMSS state
	bool _tmssEnable = false; //true if TMSS ROM is present
	bool _vdpEnable[2] = { true, true }; //TMSS VDP enable latch
	bool _romEnable = true; //TMSS ROM enable

	//M68K I/O register state
	struct IO
	{
		uint8_t version = 0; //0=Model 1, 1=Model 2+
	} _io;

	//--- Internal helpers ---
	uint16_t ReadRomWord(uint32_t address);
	uint16_t ReadSramWord(uint32_t address);
	void WriteSramWord(uint32_t address, uint16_t data, uint8_t upper, uint8_t lower);
	uint16_t ReadEepromWord(uint16_t data, uint8_t upper, uint8_t lower);
	void WriteEepromWord(uint16_t data, uint8_t upper, uint8_t lower);
	uint16_t ReadM68KIO(uint32_t address, uint16_t openBus);
	void WriteM68KIO(uint32_t address, uint8_t upper, uint8_t lower, uint16_t data, CpuType cpuType);
	void SetBusreq(bool line, CpuType cpuType);
	void SetReset(bool line, CpuType cpuType);
	//Apply SSF2 bank translation to a byte M68K ROM address.
	//Returns the corresponding byte offset into the _rom array.
	//When _useSsfMapper is false, returns the address unchanged.
	uint32_t TranslateRomAddress(uint32_t address) const;
	//Returns the M68K address one past the end of the SRAM window.
	//_sramSize is now the full address range (flat array, matching gpgx's
	//sram.sram[0x10000]), not the packed chip byte count.
	uint32_t GetSramEnd() const { return _sramStart + _sramSize; }

	//Z80 external bus access (through M68K bus, with arbitration)
	uint8_t Z80ReadExternal(uint32_t m68kAddress);
	void Z80WriteExternal(uint32_t m68kAddress, uint8_t data);
};
