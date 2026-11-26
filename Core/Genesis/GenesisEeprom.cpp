#include "pch.h"
#include "Genesis/GenesisEeprom.h"
#include "Utilities/Serializer.h"

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
		case Type::X24C02: return 256;
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
	if(_mode == Mode::Read && !clock() && _counter >= 1 && _counter <= 8) {
		//The next data bit is already valid before the host raises SCL.
		return ((_output >> (8 - _counter)) & 1) != 0;
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
		} else if(data.rise()) {
			_counter = 0;
			_mode = Mode::Standby;
		}
	}

	//On each SCL falling edge, advance the bit counter.
	//The counter wraps from 9 back to 1 (9 clocks per byte: 8 data + 1 ACK).
	if(clock.fall()) {
		if(_counter++ > 8) {
			_counter = 1;
		}
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
				} else if(select() != Acknowledge) {
					_mode = Mode::Standby;
				} else if(_device & 1) {
					//R/W bit = 1 → read immediately (current-address read)
					_mode = Mode::Read;
					_response = loadByte() ? 1 : 0;
				} else {
					//R/W bit = 0 → write: need bank (M24C32+) then address
					_mode = Uses16BitAddress() ? Mode::Bank : Mode::Address;
					_response = Acknowledge ? 1 : 0;
				}
				break;

			case Mode::Bank:
				if(_counter <= 8) {
					_bank = (uint8_t)((_bank << 1) | (data() ? 1 : 0));
				} else {
					_mode = Mode::Address;
					_response = Acknowledge ? 1 : 0;
				}
				break;

			case Mode::Address:
				if(_counter <= 8) {
					_address = (uint8_t)((_address << 1) | (data() ? 1 : 0));
				} else if(_type == Type::X24C01 && (_address & 1)) {
					//X24C01: address bit 0 = R/W. If read, read immediately.
					_mode = Mode::Read;
					_response = loadByte() ? 1 : 0;
				} else {
					_mode = Mode::Write;
					_response = Acknowledge ? 1 : 0;
				}
				break;

			case Mode::Read:
				if(_counter <= 8) {
					//Shift out the output byte MSB-first. Cast to uint32_t before
					//shifting so shifts by 8 (counter=0) are well-defined.
					_response = (uint8_t)(((uint32_t)_output >> (8 - _counter)) & 1);
				} else if(data() == Acknowledge) {
					//Master ACK → continue reading the next byte
					_address += (_type == Type::X24C01) ? 2 : 1;
					if(!_address) {
						_bank++;
					}
					_response = loadByte() ? 1 : 0;
				} else {
					//Master NACK → end read
					_mode = Mode::Standby;
				}
				break;

			case Mode::Write:
				if(_counter <= 8) {
					_input = (uint8_t)((_input << 1) | (data() ? 1 : 0));
				} else {
					_response = storeByte() ? 1 : 0;
					if(_type == Type::X24C01 || _type == Type::X24C02) {
						//Both Xicor chips write in four-byte pages. X24C01 packs R/W into its address.
						uint8_t addressStep = _type == Type::X24C01 ? 2 : 1;
						uint8_t pageOffsetMask = 4 * addressStep - 1;
						_address = (_address & ~pageOffsetMask) | ((_address + addressStep) & pageOffsetMask);
					} else if(!++_address) {
						_bank++;
					}
				}
				break;
		}
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
			if(_type == Type::X24C02 && ((_device >> 1) & 7) != _enable) {
				return !Acknowledge;
			}
			return Acknowledge;

		case Area::IDPage:
			if(!Uses16BitAddress()) {
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
	if(!Uses16BitAddress()) {
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
			if(!Uses16BitAddress()) {
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
				return !Acknowledge;
			}
			{
				uint32_t off = offset() & (size() - 1);
				memory[off] = _input;
			}
			return Acknowledge;

		case Area::IDPage:
			if(!writable) {
				return !Acknowledge;
			}
			if(!Uses16BitAddress()) {
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
	//Keep the chip type selected by the cartridge.
	Type savedType = _type;
	s.Stream(savedType, "_type");
	if(savedType != _type) {
		return;
	}
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
