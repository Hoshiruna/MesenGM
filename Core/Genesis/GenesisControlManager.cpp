#include "pch.h"
#include "Genesis/GenesisControlManager.h"
#include "Genesis/Input/GenesisController.h"
#include "Genesis/GenesisConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Utilities/Serializer.h"

GenesisControlManager::GenesisControlManager(Emulator* emu, GenesisConsole* console)
	: BaseControlManager(emu, CpuType::GenesisM68K), _console(console)
{
}

shared_ptr<BaseControlDevice> GenesisControlManager::CreateControllerDevice(ControllerType type, uint8_t port)
{
	shared_ptr<BaseControlDevice> device;

	KeyMappingSet keys;
	GenesisConfig& cfg = _emu->GetSettings()->GetGenesisConfig();
	switch(port) {
		default:
		case 0: keys = cfg.Port1.Keys; break;
		case 1: keys = cfg.Port2.Keys; break;
	}

	switch(type) {
		default:
		case ControllerType::None: break;
		case ControllerType::GenesisController:
		case ControllerType::GenesisController6Buttons:
			device = std::make_shared<GenesisController>(_emu, _console, type, port, keys, port >= 3 || _ports[port].GetThLevel() != 0);
			break;
	}

	return device;
}

void GenesisControlManager::UpdateControlDevices()
{
	GenesisConfig& cfg = _emu->GetSettings()->GetGenesisConfig();
	if(_emu->GetSettings()->IsEqual(_prevConfig, cfg) && _controlDevices.size() > 0) {
		return;
	}

	auto lock = _deviceLock.AcquireSafe();
	ClearDevices();

	for(int i = 0; i < 2; i++) {
		ControllerType type = (i == 0) ? cfg.Port1.Type : cfg.Port2.Type;
		shared_ptr<BaseControlDevice> device = CreateControllerDevice(type, i);
		if(device) {
			RegisterControlDevice(device);
		}
	}

	_prevConfig = cfg;
}

void GenesisControlManager::ResetControllerHandshakes()
{
	for(shared_ptr<BaseControlDevice>& device : _controlDevices) {
		if(device->GetControllerType() == ControllerType::GenesisController6Buttons) {
			static_cast<GenesisController*>(device.get())->ResetHandshake();
		}
	}
}

uint8_t GenesisControlManager::ReadPort(uint8_t port)
{
	SetInputReadFlag();
	if(port >= 3) {
		return 0x7F;
	}

	//Get device input data
	uint8_t inputData = 0x7F; //default: all bits pulled up
	for(shared_ptr<BaseControlDevice>& device : _controlDevices) {
		if(device->IsConnected() && device->GetPort() == port) {
			inputData &= device->ReadRam(0);
		}
	}

	//Merge with data latch (only output bits from latch are driven)
	PortState& ps = _ports[port];
	uint8_t outputMask = 0x80 | ps.control; //bit 7 always output, plus control bits
	uint8_t merged = (ps.dataLatch & outputMask) | (inputData & ~outputMask);
	return merged & 0x7F; //bit 7 unused
}

uint8_t GenesisControlManager::ReadControl(uint8_t port)
{
	if(port >= 3) {
		return 0x00;
	}
	return _ports[port].control;
}

void GenesisControlManager::WritePort(uint8_t port, uint8_t data)
{
	if(port >= 3) {
		return;
	}
	_ports[port].dataLatch = data & 0x7F;
	UpdateTh(port, true);
}

void GenesisControlManager::WriteControl(uint8_t port, uint8_t data)
{
	if(port >= 3) {
		return;
	}
	_ports[port].control = data & 0x7F;
	UpdateTh(port, false);
}

void GenesisControlManager::UpdateTh(uint8_t port, bool wroteData)
{
	const PortState& state = _ports[port];
	for(shared_ptr<BaseControlDevice>& device : _controlDevices) {
		if(device->IsConnected() && device->GetPort() == port) {
			if(device->GetControllerType() == ControllerType::GenesisController6Buttons) {
				device->WriteRam(0, state.GetThLevel());
			} else if(wroteData) {
				device->WriteRam(0, state.dataLatch);
			}
		}
	}
}

void GenesisControlManager::Serialize(Serializer& s)
{
	BaseControlManager::Serialize(s);
	for(int i = 0; i < 3; i++) {
		SVI(_ports[i].control);
		SVI(_ports[i].dataLatch);
		SVI(_ports[i].dataLines);
	}

	if(!s.IsSaving()) {
		UpdateControlDevices();
	}

	for(uint8_t i = 0; i < _controlDevices.size(); i++) {
		SVI(_controlDevices[i]);
	}
}
