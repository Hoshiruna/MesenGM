#pragma once
#include "pch.h"
#include "NES/Mappers/Nintendo/MMC3.h"
#include "NES/Mappers/Audio/RincoFsg2Audio.h"

// NES 2.0 mapper 594. Behavioral reference: NintendulatorNRS mapper594.cpp.
class RincoFsg2 : public MMC3
{
private:
	uint8_t _outerRegisters[4] = {};
	unique_ptr<RincoFsg2Audio> _audio;

	uint16_t GetPrgOuterBank() const
	{
		return (_outerRegisters[2] & 0x40 ? 0xC0 : 0) | (_outerRegisters[2] & 0x80 ? 0x100 : 0);
	}

protected:
	bool AllowRegisterRead() override { return true; }
	bool EnableCpuClockHook() override { return true; }
	bool ForceMmc3RevAIrqs() override { return true; }

	void InitMapper() override
	{
		_audio.reset(new RincoFsg2Audio(_console));
		MMC3::InitMapper();
		AddRegisterRange(0x5000, 0x5FFF, MemoryOperation::Any);
		RemoveRegisterRange(0x8000, 0xFFFF, MemoryOperation::Read);
	}

	void Reset(bool softReset) override
	{
		memset(_outerRegisters, 0, sizeof(_outerRegisters));
		_audio->Reset();
		if(!softReset) {
			ResetMmc3();
		}
		_console->GetCpu()->ClearIrqSource(IRQSource::External);
		UpdateState();
		UpdateMirroring();
	}

	void UpdatePrgMapping() override
	{
		MMC3::UpdatePrgMapping();
		SetCpuMemoryMapping(0x6000, 0x7FFF, _outerRegisters[0] | GetPrgOuterBank(), PrgMemoryType::PrgRom);
	}

	void SelectPrgPage(uint16_t slot, uint16_t page, PrgMemoryType memoryType) override
	{
		MMC3::SelectPrgPage(slot, (page & 0x3F) | GetPrgOuterBank(), memoryType);
	}

	void SelectChrPage(uint16_t slot, uint16_t page, ChrMemoryType memoryType) override
	{
		uint16_t mask = _outerRegisters[2] & 0xC0 ? 0xFF : 0x1FF;
		uint16_t outerBank = (_outerRegisters[2] & 0x40 ? 0x200 : 0) | (_outerRegisters[2] & 0x80 ? 0x300 : 0);
		MMC3::SelectChrPage(slot, ((page | ((slot << 6) & 0x100)) & mask) | (outerBank & ~mask), memoryType);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		return _audio->ReadStatus();
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr < 0x6000) {
			_audio->Write(addr, value);
		} else if((addr & 0xD000) == 0x9000) {
			_outerRegisters[((addr >> 12) & 2) | (addr & 1)] = value;
			UpdateState();
			UpdateMirroring();
		} else {
			MMC3::WriteRegister(addr, value);
		}
	}

	void ProcessCpuClock() override
	{
		MMC3::ProcessCpuClock();
		_audio->Clock();
	}

	void Serialize(Serializer& s) override
	{
		MMC3::Serialize(s);
		SVArray(_outerRegisters, 4);
		SV(_audio);
		if(!s.IsSaving()) {
			UpdateState();
			UpdateMirroring();
		}
	}
};
