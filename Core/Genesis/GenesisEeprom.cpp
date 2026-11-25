#include "pch.h"
#include "Genesis/GenesisEeprom.h"
#include "Utilities/Serializer.h"
#include <cstdio>

#define GENESIS_DBG(fmt, ...) std::fprintf(stderr, "[GENESIS] " fmt "\n", __VA_ARGS__)

// Ported from ares/component/eeprom/m24c/m24c.cpp.
// All nall types (n1/n3/n8/u8/u32) are replaced with stdint types,
// and the auto->return-type syntax is replaced with plain C++.
// The private helpers load()/store() are renamed to loadByte()/storeByte()
// to avoid confusion with the public load(Type) method.

uint32_t GenesisEeprom::size() const
{
	switch(_type) {
		default: return 0;
		case Type::X24C01: return 128;
		case Type::M24C01: return 128;
		case Type::M24C02: return 256;
		case Type::M24C04: return 512;
		case Type::M24C08: return 1024;
		case Type::M24C16: return 2048;
		case Type::M24C32: return 4096;
		case Type::M24C64: return 8192;
		case Type::M24C65: return 8192;
		case Type::M24C128: return 16384;
		case Type::M24C256: return 32768;
		case Type::M24C512: return 65536;
	}
}

void GenesisEeprom::reset()
{
	_type = Type::None;
}

void GenesisEeprom::load(Type typeID, uint8_t enableID)
{
	_type = typeID;
	_enable = enableID;
	erase();
}

void GenesisEeprom::power()
{
	_mode = Mode::Standby;
	//Reset SCL/SDA lines to idle-high (I2C bus idle state). Explicit
	//assignment avoids relying on aggregate-init with default member
	//initializers, which would otherwise need C++14 aggregate rules.
	clock.latch = 1;
	clock.value = 1;
	data.latch = 1;
	data.value = 1;
	_counter = 0;
	_device = (uint8_t)(Area::Memory << 4); //0xA0 — standard EEPROM device address
	_bank = 0;
	_address = 0;
	_input = 0;
	_output = 0;
	_response = Acknowledge ? 1 : 0; //Acknowledge == false == 0
	writable = 1;
}

bool GenesisEeprom::read() const
{
	if(_mode == Mode::Standby) {
		return data();
	}
	return _response != 0;
}

void GenesisEeprom::write()
{
	auto phase = _mode;

	//Start/stop condition detection: SDA transitions while SCL is high.
	//Start (SDA fall) → begin a new transaction.
	//Stop  (SDA rise) → end the current transaction.
	if(clock.hi()) {
		if(data.fall()) {
			_counter = 0;
			_mode = (_type == Type::X24C01) ? Mode::Address : Mode::Device;
			GENESIS_DBG("[START] SCL=1 SDA fell → mode=%d counter=%d", (int)_mode, _counter);
		} else if(data.rise()) {
			_counter = 0;
			_mode = Mode::Standby;
			GENESIS_DBG("[STOP]  SCL=1 SDA rose → mode=Standby counter=%d", _counter);
		}
	}

	//On each SCL falling edge, advance the bit counter.
	//The counter wraps from 9 back to 1 (9 clocks per byte: 8 data + 1 ACK).
	if(clock.fall()) {
		uint8_t prev = _counter;
		if(_counter++ > 8) {
			_counter = 1;
		}
		GENESIS_DBG("[SCLfall] prev_counter=%d new_counter=%d mode=%d SDA=%d",
			prev,
			_counter,
			(int)phase,
			data() ? 1 : 0);
	}

	//On each SCL rising edge, process the state machine using the mode
	//that was active at the start of this write() call (`phase`). This
	//ensures start/stop transitions don't affect the current byte's
	//processing until the next bit.
	if(clock.rise()) {
		switch(phase) {
			case Mode::Device:
				if(_counter <= 8) {
					_device = (uint8_t)((_device << 1) | (data() ? 1 : 0));
					GENESIS_DBG("[DEVbit] counter=%d bit=%d device=0x%02X", _counter, data() ? 1 : 0, _device);
				} else if(select() != Acknowledge) {
					_mode = Mode::Standby;
					GENESIS_DBG("[DEVack] NO ACK (select failed) device=0x%02X → Standby", _device);
				} else if(_device & 1) {
					//R/W bit = 1 → read immediately (current-address read)
					_mode = Mode::Read;
					_response = loadByte() ? 1 : 0;
					GENESIS_DBG("[DEVack] ACK device=0x%02X R/W=READ → Read mode offset=%u output=0x%02X",
						_device,
						offset(),
						_output);
				} else {
					//R/W bit = 0 → write: need bank (M24C32+) then address
					_mode = (_type <= Type::M24C16) ? Mode::Address : Mode::Bank;
					_response = Acknowledge ? 1 : 0;
					GENESIS_DBG("[DEVack] ACK device=0x%02X R/W=WRITE → %s mode",
						_device,
						(_type <= Type::M24C16) ? "Address" : "Bank");
				}
				break;

			case Mode::Bank:
				if(_counter <= 8) {
					_bank = (uint8_t)((_bank << 1) | (data() ? 1 : 0));
					GENESIS_DBG("[BNKbit] counter=%d bit=%d bank=0x%02X", _counter, data() ? 1 : 0, _bank);
				} else {
					_mode = Mode::Address;
					_response = Acknowledge ? 1 : 0;
					GENESIS_DBG("[BNKack] ACK bank=0x%02X → Address mode", _bank);
				}
				break;

			case Mode::Address:
				if(_counter <= 8) {
					_address = (uint8_t)((_address << 1) | (data() ? 1 : 0));
					GENESIS_DBG("[ADRbit] counter=%d bit=%d address=0x%02X", _counter, data() ? 1 : 0, _address);
				} else if(_type == Type::X24C01 && (_address & 1)) {
					//X24C01: address bit 0 = R/W. If read, read immediately.
					_mode = Mode::Read;
					_response = loadByte() ? 1 : 0;
					GENESIS_DBG("[ADRack] X24C01 read → Read mode offset=%u output=0x%02X", offset(), _output);
				} else {
					_mode = Mode::Write;
					_response = Acknowledge ? 1 : 0;
					GENESIS_DBG("[ADRack] ACK address=0x%02X → Write mode offset=%u", _address, offset());
				}
				break;

			case Mode::Read:
				if(_counter <= 8) {
					//Shift out the output byte MSB-first. Cast to uint32_t before
					//shifting so shifts by 8 (counter=0) are well-defined.
					_response = (uint8_t)(((uint32_t)_output >> (8 - _counter)) & 1);
					GENESIS_DBG("[RDbit]  counter=%d output=0x%02X response(SDA)=%d", _counter, _output, _response);
				} else if(data() == Acknowledge) {
					//Master ACK → continue reading the next byte
					_address += (_type == Type::X24C01) ? 2 : 1;
					if(!_address) {
						_bank++;
					}
					_response = loadByte() ? 1 : 0;
					GENESIS_DBG("[RDack]  master ACK → next byte offset=%u output=0x%02X", offset(), _output);
				} else {
					//Master NACK → end read
					_mode = Mode::Standby;
					GENESIS_DBG("%s", "[RDnack] master NACK → Standby");
				}
				break;

			case Mode::Write:
				if(_counter <= 8) {
					_input = (uint8_t)((_input << 1) | (data() ? 1 : 0));
					GENESIS_DBG("[WRbit]  counter=%d bit=%d input=0x%02X", _counter, data() ? 1 : 0, _input);
				} else {
					_response = storeByte() ? 1 : 0;
					_address += (_type == Type::X24C01) ? 2 : 1;
					if(!_address) {
						_bank++;
					}
					GENESIS_DBG("[WRack]  stored input=0x%02X at offset=%u response(ACK)=%d",
						_input,
						offset() - ((_type == Type::X24C01) ? 2 : 1),
						_response);
				}
				break;
		}
	}

	//Log when NO transition happened (idle — both SCL/SDA unchanged).
	//This helps identify if the game is stuck or not accessing EEPROM.
	if(!clock.hi() && !clock.fall() && !clock.rise() && !data.fall() && !data.rise()) {
		//Uncomment to log idle samples (very noisy):
		//GENESIS_DBG("[IDLE]   SCL=%d SDA=%d mode=%d counter=%d (no transition)",
		//	clock() ? 1 : 0, data() ? 1 : 0, (int)phase, _counter);
	}
}

void GenesisEeprom::erase(uint8_t fill)
{
	for(auto& byte : memory) {
		byte = fill;
	}
	for(auto& byte : idpage) {
		byte = fill;
	}
	locked = 0;
}

bool GenesisEeprom::select() const
{
	switch(_device >> 4) {
		case Area::Memory:
			return Acknowledge;

		case Area::IDPage:
			if(_type <= Type::M24C16) {
				return !Acknowledge;
			}
			return Acknowledge;

		default:
			return !Acknowledge;
	}
}

uint32_t GenesisEeprom::offset() const
{
	if(_type == Type::X24C01) {
		return _address >> 1;
	}
	if(_type <= Type::M24C16) {
		return (_device >> 1) << 8 | _address;
	}
	return (_device >> 1) << 16 | _bank << 8 | _address;
}

bool GenesisEeprom::loadByte()
{
	switch(_device >> 4) {
		case Area::Memory:
			_output = memory[offset() & (size() - 1)];
			return Acknowledge;

		case Area::IDPage:
			if(_type <= Type::M24C16) {
				return !Acknowledge;
			}
			_output = idpage[_address & (sizeof(idpage) - 1)];
			return Acknowledge;

		default:
			return !Acknowledge;
	}
}

bool GenesisEeprom::storeByte()
{
	switch(_device >> 4) {
		case Area::Memory:
			if(!writable) {
				GENESIS_DBG("[STORE]  NOT WRITABLE! offset=%u input=0x%02X (write-protected)", offset(), _input);
				return !Acknowledge;
			}
			{
				uint32_t off = offset() & (size() - 1);
				uint8_t prev = memory[off];
				memory[off] = _input;
				GENESIS_DBG("[STORE]  *** WROTE BYTE *** offset=%u (=0x%X) prev=0x%02X new=0x%02X",
					off,
					off,
					prev,
					_input);
			}
			return Acknowledge;

		case Area::IDPage:
			if(!writable) {
				return !Acknowledge;
			}
			if(_type <= Type::M24C16) {
				return !Acknowledge;
			}
			if(locked) {
				return !Acknowledge;
			}
			idpage[_address & (sizeof(idpage) - 1)] = _input;
			return Acknowledge;

		default:
			return !Acknowledge;
	}
}

void GenesisEeprom::Serialize(Serializer& s)
{
	//Type must be serialized first so size() returns the correct value
	//for the memory array below (used as the SVArray count).
	SV(_type);
	SV(_mode);
	SV(_enable);
	SV(_counter);
	SV(_device);
	SV(_bank);
	SV(_address);
	SV(_input);
	SV(_output);
	SV(_response);
	SV(writable);
	SV(locked);

	//Only serialize the used portion of memory[] to keep save states
	//small (1KB for M24C08 vs 64KB for the full array). When loading,
	//_type is already restored so size() returns the correct count.
	uint32_t memSize = size();
	SVArray(memory, memSize);
	SVArray(idpage, (uint32_t)32);

	//Line state (SCL/SDA latch+value)
	SV(clock.latch);
	SV(clock.value);
	SV(data.latch);
	SV(data.value);
}
