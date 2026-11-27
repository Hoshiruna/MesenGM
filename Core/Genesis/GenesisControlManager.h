#pragma once
#include "pch.h"
#include "Shared/SettingTypes.h"
#include "Shared/BaseControlManager.h"

class GenesisConsole;
class Emulator;

// Genesis Control Manager — 3 I/O ports with TH/TR/TL handshake.
//
// Port layout (per ares/md/controller/port.cpp):
//   Port 0 ($A10002/$A10008): Controller port 1
//   Port 1 ($A10004/$A1000A): Controller port 2
//   Port 2 ($A10006/$A1000C): Extension port
//
// Each port has a data register (read: device data, write: TH line)
// and a control register (bit 6 = TH direction: 0=input, 1=output).
// When TH is configured as output, the data latch drives it.

class GenesisControlManager final : public BaseControlManager
{
private:
	GenesisConsole* _console = nullptr;
	GenesisConfig _prevConfig = {};

	//Per-port state (3 ports)
	struct PortState
	{
		uint8_t control = 0x00; //control register: bit6=TH direction
		uint8_t dataLatch = 0x7F; //data latch (written by M68K)
		uint8_t dataLines = 0x7F; //current data line state

		uint8_t GetThLevel() const { return (control & 0x40) ? dataLatch & 0x40 : 0x40; }
	} _ports[3];

	void UpdateTh(uint8_t port, bool wroteData);

public:
	GenesisControlManager(Emulator* emu, GenesisConsole* console);

	shared_ptr<BaseControlDevice> CreateControllerDevice(ControllerType type, uint8_t port) override;

	void UpdateControlDevices() override;
	void ResetControllerHandshakes();

	//Called by GenesisMemoryManager for M68K I/O reads/writes.
	//Port index: 0=port1, 1=port2, 2=extension.
	uint8_t ReadPort(uint8_t port);
	uint8_t ReadControl(uint8_t port);
	void WritePort(uint8_t port, uint8_t data);
	void WriteControl(uint8_t port, uint8_t data);

	void Serialize(Serializer& s) override;
};
