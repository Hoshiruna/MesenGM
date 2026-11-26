#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"

// M24C I2C serial EEPROM emulation for Genesis/Mega Drive cartridges.
//
// Ported from ares/component/eeprom/m24c/m24c.hpp and m24c.cpp.
// Used by cartridges that store saves on an I2C EEPROM (e.g. Light
// Crusader, Shadowrun, NBA Jam TE, WWF WrestleMania) instead of
// parallel SRAM. The EEPROM is accessed via SDA/SCL bit-banging
// through the cartridge's SRAM address range — the game writes SCL
// and SDA bit values to specific bit positions of word writes at the
// EEPROM address, and reads the SDA bit back from word reads.
//
// The M24C family covers X24C01 (128B) through M24C512 (64KB). The
// protocol is a simplified I2C state machine:
//   Standby → Device → [Bank] → Address → Read | Write
// Start condition: SDA falls while SCL is high.
// Stop condition:  SDA rises while SCL is high.
// Each byte is 9 clocks: 8 data bits (MSB first) + 1 ACK bit.
//
// Reference: ares/component/eeprom/m24c/m24c.cpp (207 LOC).
class GenesisEeprom final : public ISerializable
{
public:
	enum class Type : uint32_t
	{
		None,

		//8-bit address
		X24C01, // 128 cells =>   128 x 8-bit x 1-block

		//8-bit device + 8-bit address
		M24C01, // 1024 cells =>   128 x 8-bit x 1-block
		M24C02, // 2048 cells =>   256 x 8-bit x 1-block
		M24C04, // 4096 cells =>   256 x 8-bit x 2-blocks
		M24C08, // 8192 cells =>   256 x 8-bit x 4-blocks
		M24C16, // 16384 cells =>   256 x 8-bit x 8-blocks

		//8-bit device + 8-bit bank + 8-bit address
		M24C32, // 32768 cells =>  4096 x 8-bit x 1-block
		M24C64, // 65536 cells =>  8192 x 8-bit x 1-block
		M24C65, // 65536 cells =>  8192 x 8-bit x 1-block
		M24C128, //131072 cells => 16384 x 8-bit x 1-block
		M24C256, //262144 cells => 32768 x 8-bit x 1-block
		M24C512, //524288 cells => 65536 x 8-bit x 1-block

		//Keep the existing values for save states. X24C02 uses an 8-bit word address.
		X24C02,
	};

	enum class Mode : uint32_t
	{
		Standby,
		Device,
		Bank,
		Address,
		Read,
		Write,
	};

	enum Area : uint32_t
	{
		Memory = 0b1010,
		IDPage = 0b1011,
	};

	explicit operator bool() const { return size() != 0; }

	//Returns the EEPROM capacity in bytes (0 if no EEPROM loaded).
	uint32_t size() const;

	//Clears the type to None (no EEPROM).
	void reset();

	//Loads an EEPROM of the given type and erases its contents.
	//enableID selects which device address this chip responds to
	//(for multi-chip configurations; usually 0).
	void load(Type typeID, uint8_t enableID = 0);

	//Resets the I2C state machine to Standby (called on power-on).
	void power();

	//Returns the current SDA output bit. In Standby this is the
	//latched SDA line; otherwise it is the chip's response bit.
	bool read() const;

	//Advances the I2C state machine by one SCL/SDA sample. Called
	//after the host updates clock/data via the Line assignments.
	void write();

	//Fills the entire EEPROM (and ID page, if present) with `fill`.
	void erase(uint8_t fill = 0xFF);

	//--- ISerializable ---
	void Serialize(Serializer& s) override;

	//SCL/SDA line state. The host assigns these via `line = bool`
	//which latches the previous value for transition detection.
	//Public because the cartridge board code writes them directly
	//(matching ares's standard.cpp pattern).
	struct Line
	{
		bool lo() const { return !latch && !value; }
		bool hi() const { return latch && value; }
		bool fall() const { return latch && !value; }
		bool rise() const { return !latch && value; }

		bool operator()() const { return value != 0; }
		Line& operator=(bool data)
		{
			latch = value;
			value = data ? 1 : 0;
			return *this;
		}

		uint8_t latch = 1;
		uint8_t value = 1;
	};

	Line clock; //SCL
	Line data; //SDA
	uint8_t writable = 1; //!WP

	//EEPROM storage (public so BatteryManager can load/save directly,
	//matching ares's Interface::save which writes m24c.memory).
	uint8_t memory[65536];
	uint8_t idpage[32];
	uint8_t locked = 0;

private:
	bool Uses16BitAddress() const { return _type >= Type::M24C32 && _type <= Type::M24C512; }

	//Decodes the device address byte and returns whether this chip
	//should acknowledge (Memory and IDPage areas only; M24C16 and
	//below ignore the IDPage area).
	bool select() const;

	//Computes the byte offset into memory[] for the current
	//device/bank/address, based on the EEPROM type:
	//  X24C01:          address >> 1
	//  M24C01-M24C16:   (device >> 1) << 8 | address
	//  M24C32+:          (device >> 1) << 16 | bank << 8 | address
	uint32_t offset() const;

	//Loads the byte at the current offset into `output` and returns
	//Acknowledge. Used by the Read state.
	bool loadByte();

	//Stores `input` at the current offset (if writable) and returns
	//Acknowledge. Used by the Write state.
	bool storeByte();

	static constexpr bool Acknowledge = false;

	Type _type = Type::None;
	Mode _mode = Mode::Standby;
	uint8_t _enable = 0;
	uint8_t _counter = 0;
	uint8_t _device = 0;
	uint8_t _bank = 0;
	uint8_t _address = 0;
	uint8_t _input = 0;
	uint8_t _output = 0;
	uint8_t _response = 0;
};
