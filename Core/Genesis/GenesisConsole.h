#pragma once
#include "pch.h"
#include "Shared/Interfaces/IConsole.h"
#include "Shared/SettingTypes.h"
#include "Genesis/GenesisTypes.h"
#include "Genesis/GenesisEeprom.h"

class Emulator;
class VirtualFile;
class BaseControlManager;
class GenesisControlManager;
class GenesisPsg;
class GenesisYm2612;
class GenesisM68K;
class GenesisZ80;
class GenesisVdp;
class GenesisMemoryManager;

class GenesisConsole final : public IConsole
{
private:
	Emulator* _emu = nullptr;

	//Subsystem instances (created in LoadRom)
	unique_ptr<GenesisControlManager> _controlManager;
	unique_ptr<GenesisPsg> _psg;
	unique_ptr<GenesisYm2612> _ym2612;
	unique_ptr<GenesisM68K> _m68k;
	unique_ptr<GenesisZ80> _z80;
	unique_ptr<GenesisVdp> _vdp;
	unique_ptr<GenesisMemoryManager> _memoryManager;

	//ROM data (owned by the console, passed to memory manager)
	vector<uint8_t> _romData;
	uint8_t* _sram = nullptr;
	uint32_t _sramSize = 0;
	uint32_t _sramStart = 0x200000; //default SRAM window at 2MB boundary
	bool _sramWritable = true;
	bool _sramEnable = false;
	//True if SRAM is wired to D0-D7 only (odd-byte access). The SRAM
	//chip is selected by /LDS, A0 is ignored, and the chip's byte index
	//is (m68kAddress - _sramStart) >> 1. Matches ares's lram[address>>1]
	//pattern (linear.cpp/standard.cpp). Derived from ROM header type
	//byte 1 (0x1B2) yz bits (yz=11 = odd-only) OR odd start address.
	bool _sramOddByte = false;
	bool _banked = false; //true if cartridge uses banked mapping
	uint8_t _romBank[8] = {}; //bank registers for banked cartridges

	//Recognized EEPROM boards use the cartridge save window for SDA and SCL.
	GenesisEeprom::Type _eepromType = GenesisEeprom::Type::None;
	GenesisEeprom _eeprom;
	//SDA/SCL bit positions within the 16-bit M68K word at _sramStart.
	//rsda: bit position of SDA on reads. wsda/wscl: bit positions of
	//SDA/SCL on writes. The upper nibble (>>3) selects the byte (0=low,
	//1=high).
	uint8_t _eepromRsda = 0;
	uint8_t _eepromWsda = 0;
	uint8_t _eepromWscl = 1;

	RomFormat _romFormat = RomFormat::Genesis;
	ConsoleRegion _region = ConsoleRegion::Ntsc;
	ConsoleRegion _configuredRegion = ConsoleRegion::Auto;
	GenesisRegion _genesisRegion = GenesisRegion::Ntsc;
	GenesisModel _model = GenesisModel::Model1;
	string _filename;
	string _romRegion; //region string from ROM header (e.g. "JUE")

	uint32_t _frameCount = 0;
	uint64_t _masterClock = 0;
	uint64_t _m68kClock = 0;
	uint64_t _z80Clock = 0;

	void UpdateRegion();
	void ParseRomHeader(vector<uint8_t>& romData);
	void InitCart(vector<uint8_t>& romData);
	void RunScanlineCpus(uint64_t scanlineStart);
	void RunZ80Instruction();

public:
	static vector<string> GetSupportedExtensions() { return { ".md", ".gen", ".smd" }; }
	static vector<string> GetSupportedSignatures() { return {}; }
	static bool HasBinRomHeader(VirtualFile& romFile);

	GenesisConsole(Emulator* emu);
	virtual ~GenesisConsole();

	LoadRomResult LoadRom(VirtualFile& romFile) override;

	void Reset() override;
	void RunFrame() override;

	void SaveBattery() override;

	BaseControlManager* GetControlManager() override;
	ConsoleRegion GetRegion() override { return _region; }
	ConsoleType GetConsoleType() override { return ConsoleType::Genesis; }
	vector<CpuType> GetCpuTypes() override;
	RomFormat GetRomFormat() override { return _romFormat; }
	double GetFps() override;
	uint32_t GetFrameCount() override { return _frameCount; }
	PpuFrameInfo GetPpuFrame() override;
	BaseVideoFilter* GetVideoFilter(bool getDefaultFilter) override;

	uint64_t GetMasterClock() override;
	uint32_t GetMasterClockRate() override;
	uint64_t GetCpuMasterClock(CpuType cpuType) const;
	void RunZ80Until(uint64_t masterClock);

	AudioTrackInfo GetAudioTrackInfo() override { return {}; }
	void ProcessAudioPlayerAction(AudioPlayerActionParams p) override {}

	AddressInfo GetAbsoluteAddress(AddressInfo& relAddress) override;
	AddressInfo GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType) override;
	void GetConsoleState(BaseState& state, ConsoleType consoleType) override;

	GenesisRegion GetGenesisRegion() { return _genesisRegion; }
	GenesisModel GetModel() { return _model; }
	GenesisPsg* GetPsg() { return _psg.get(); }
	GenesisYm2612* GetYm2612() { return _ym2612.get(); }
	GenesisVdp* GetVdp() { return _vdp.get(); }
	GenesisM68K* GetM68K() { return _m68k.get(); }
	GenesisZ80* GetZ80() { return _z80.get(); }
	GenesisMemoryManager* GetMemoryManager() { return _memoryManager.get(); }

	void InitializeRam(void* data, uint32_t length);

	void Serialize(Serializer& s) override;
};
