#include "pch.h"
#include "GenesisM68K.h"
#include "Utilities/Serializer.h"
#include "Shared/Emulator.h"
#include <bitset>

#ifdef _MSC_VER
	#define GENESIS_UNREACHABLE() __assume(0)
#else
	#define GENESIS_UNREACHABLE() __builtin_unreachable()
#endif

// ============================================================================
// GenesisM68K - Native Mesen2 port of ares M68000 interpreter.
// All algorithms ported verbatim from ares/component/processor/m68000.
// Type conversions: n1→bool/uint8_t, n3/n4→uint8_t, n8→uint8_t, n16→uint16_t,
// n24→uint32_t, n32→uint32_t. ares serializer→SV(). Virtual bus→callbacks.
// ============================================================================

//--- Helpers ---
static inline void m68k_swap(uint32_t& a, uint32_t& b)
{
	uint32_t t = a;
	a = b;
	b = t;
}

// ============================================================================
// m68000.cpp - Power, Supervisor, Exception, Interrupt
// ============================================================================

void GenesisM68K::Power()
{
	for(auto& dr : _r.d) {
		dr = 0;
	}
	for(auto& ar : _r.a) {
		ar = 0;
	}
	_r.sp = 0;
	_r.pc = 0;
	_r.c = 0;
	_r.v = 0;
	_r.z = 0;
	_r.n = 0;
	_r.x = 0;
	_r.i = 7;
	_r.s = 1;
	_r.t = 0;
	_r.irc = 0x4e71; //nop
	_r.ir = 0x4e71;
	_r.ird = 0x4e71;
	_r.stop = false;
	_r.reset = false;
	BuildInstructionTable();

	//Load reset vector: SSP from $000000, PC from $000004
	//On a real M68000, after RESET the processor reads the initial
	//supervisor stack pointer and program counter from the vector table.
	//This matches ares CPU::main() handling of Interrupt::Reset.
	uint16_t v0 = BusRead(1, 1, 0);
	uint16_t v2 = BusRead(1, 1, 2);
	uint16_t v4 = BusRead(1, 1, 4);
	uint16_t v6 = BusRead(1, 1, 6);
	_r.a[7] = (uint32_t)v0 << 16 | (uint32_t)v2;
	_r.pc = (uint32_t)v4 << 16 | (uint32_t)v6;
	_r.sp = _r.a[7];

	//Fill the prefetch pipeline with two prefetches (matching ares)
	Prefetch();
	Prefetch();
}

bool GenesisM68K::Supervisor()
{
	if(_r.s) {
		return true;
	}
	Exception(ExUnprivileged, VUnprivileged);
	return false;
}

void GenesisM68K::Exception(uint32_t exception, uint32_t vector, uint32_t priority)
{
	_r.stop = false;

	//register setup (+6 cyc)
	BusIdle(6);
	uint32_t pc = _r.pc - 4;
	uint32_t sr = ReadSR();
	if(!_r.s) {
		m68k_swap(_r.a[7], _r.sp);
	}
	_r.s = 1;
	_r.t = 0;

	//push pc low (+4 cyc)
	Push<Word>(pc & 0x0000ffff);

	//external interrupt handling
	if(exception == ExInterrupt) {
		BusIdle(14 + 4); //autovector cycles (approximated for Megadrive)
		_r.i = priority;
	}

	//push pc high & sr (+8 cyc)
	Push<Long>(sr << 16 | pc >> 16);

	//read vector address (+8 cyc)
	_r.pc = Read<Long>(vector << 2);

	//prefetch (+8 cyc)
	Prefetch();
	if(exception == ExInterrupt) {
		BusIdle(2);
	}
	Prefetch();
}

void GenesisM68K::Interrupt(uint32_t vector, uint32_t priority)
{
	uint32_t oldPc = _r.pc - 4;
	Exception(ExInterrupt, vector, priority);
	if(_emu) {
		_emu->ProcessInterrupt<CpuType::GenesisM68K>(oldPc, _r.pc, false);
	}
}

uint32_t GenesisM68K::ExecuteInstruction()
{
	_cycleAccum = 0;

	if(_emu) {
		_emu->ProcessInstruction<CpuType::GenesisM68K>();
	}

	//In ares, CPU::main() checks for pending interrupts before every instruction.
	//We replicate this by calling CheckInterrupts() here. This is critical for
	//correct VBlank delivery: the VDP sets the VBlank pending flag during its
	//scanline execution, and the M68K must detect this on the next instruction
	//that has SR.i low enough to accept the interrupt.
	if(CheckInterrupts) {
		CheckInterrupts();
	}

	if(!_r.stop) {
		_r.ird = _r.ir;
		_instructionTable[_r.ird]();
	} else {
		if(_emu) {
			_emu->ProcessHaltedCpu<CpuType::GenesisM68K>();
		}
		BusWait(1);
	}
	return _cycleAccum;
}

// ============================================================================
// registers.cpp
// ============================================================================

template<uint32_t Size> uint32_t GenesisM68K::Read(DataRegister reg)
{
	if constexpr(Size == Byte) {
		return (uint8_t)_r.d[reg.number];
	}
	if constexpr(Size == Word) {
		return (uint16_t)_r.d[reg.number];
	}
	if constexpr(Size == Long) {
		return (uint32_t)_r.d[reg.number];
	}
	GENESIS_UNREACHABLE();
}

template<uint32_t Size> void GenesisM68K::Write(DataRegister reg, uint32_t data)
{
	if constexpr(Size == Byte) {
		_r.d[reg.number] = (_r.d[reg.number] & ~0xff) | (data & 0xff);
	}
	if constexpr(Size == Word) {
		_r.d[reg.number] = (_r.d[reg.number] & ~0xffff) | (data & 0xffff);
	}
	if constexpr(Size == Long) {
		_r.d[reg.number] = data;
	}
}

template<uint32_t Size> uint32_t GenesisM68K::Read(AddressRegister reg)
{
	if constexpr(Size == Byte) {
		return (int8_t)_r.a[reg.number];
	}
	if constexpr(Size == Word) {
		return (int16_t)_r.a[reg.number];
	}
	if constexpr(Size == Long) {
		return (int32_t)_r.a[reg.number];
	}
	GENESIS_UNREACHABLE();
}

template<uint32_t Size> void GenesisM68K::Write(AddressRegister reg, uint32_t data)
{
	if constexpr(Size == Byte) {
		_r.a[reg.number] = (int8_t)data;
	}
	if constexpr(Size == Word) {
		_r.a[reg.number] = (int16_t)data;
	}
	if constexpr(Size == Long) {
		_r.a[reg.number] = (int32_t)data;
	}
}

uint8_t GenesisM68K::ReadCCR()
{
	return _r.c << 0 | _r.v << 1 | _r.z << 2 | _r.n << 3 | _r.x << 4;
}

uint16_t GenesisM68K::ReadSR() const
{
	return _r.c << 0 | _r.v << 1 | _r.z << 2 | _r.n << 3 | _r.x << 4 | _r.i << 8 | _r.s << 13 | _r.t << 15;
}

uint16_t GenesisM68K::GetSR() const
{ return ReadSR(); }

void GenesisM68K::SetSR(uint16_t sr)
{ WriteSR(sr); }

void GenesisM68K::WriteCCR(uint8_t ccr)
{
	_r.c = bit(ccr, 0);
	_r.v = bit(ccr, 1);
	_r.z = bit(ccr, 2);
	_r.n = bit(ccr, 3);
	_r.x = bit(ccr, 4);
}

void GenesisM68K::WriteSR(uint16_t sr)
{
	WriteCCR(sr);
	if(_r.s != bit(sr, 13)) {
		m68k_swap(_r.a[7], _r.sp);
	}
	_r.i = bits(sr, 8, 10);
	_r.s = bit(sr, 13);
	_r.t = bit(sr, 15);
}

// ============================================================================
// memory.cpp
// ============================================================================

template<uint32_t Size> uint32_t GenesisM68K::Read(uint32_t address)
{
	if constexpr(Size == Byte) {
		BusWait(4);
		if(address & 1) {
			return BusRead(0, 1, address & ~1) & 0xFF; // /LDS
		} else {
			return (BusRead(1, 0, address & ~1) >> 8) & 0xFF; // /UDS
		}
	} else if constexpr(Size == Word) {
		BusWait(4);
		return BusRead(1, 1, address & ~1);
	} else if constexpr(Size == Long) {
		BusWait(4);
		uint32_t data = BusRead(1, 1, (address + 0) & ~1) << 16;
		BusWait(4);
		return data | BusRead(1, 1, (address + 2) & ~1) << 0;
	}
	GENESIS_UNREACHABLE();
}

template<uint32_t Size, bool Order> void GenesisM68K::Write(uint32_t address, uint32_t data)
{
	if constexpr(Size == Byte) {
		BusWait(4);
		if(address & 1) {
			BusWrite(0, 1, address & ~1, (data << 8) | ((uint8_t)data << 0)); // /LDS
		} else {
			BusWrite(1, 0, address & ~1, (data << 8) | ((uint8_t)data << 0)); // /UDS
		}
	} else if constexpr(Size == Word) {
		BusWait(4);
		BusWrite(1, 1, address & ~1, data);
	} else if constexpr(Size == Long) {
		if constexpr(Order) {
			BusWait(4);
			BusWrite(1, 1, (address + 2) & ~1, data >> 0);
			BusWait(4);
			BusWrite(1, 1, (address + 0) & ~1, data >> 16);
		} else {
			BusWait(4);
			BusWrite(1, 1, (address + 0) & ~1, data >> 16);
			BusWait(4);
			BusWrite(1, 1, (address + 2) & ~1, data >> 0);
		}
	}
}

template<uint32_t Size> uint32_t GenesisM68K::Extension()
{
	if constexpr(Size == Byte) {
		BusWait(4);
		_r.ir = _r.irc;
		_r.irc = BusRead(1, 1, _r.pc & ~1);
		_r.pc += 2;
		return (uint8_t)_r.ir;
	} else if constexpr(Size == Word) {
		BusWait(4);
		_r.ir = _r.irc;
		_r.irc = BusRead(1, 1, _r.pc & ~1);
		_r.pc += 2;
		return _r.ir;
	} else if constexpr(Size == Long) {
		auto hi = Extension<Word>();
		auto lo = Extension<Word>();
		return hi << 16 | lo << 0;
	}
	GENESIS_UNREACHABLE();
}

uint16_t GenesisM68K::Prefetch()
{
	BusWait(4);
	_r.ir = _r.irc;
	_r.irc = BusRead(1, 1, _r.pc & ~1);
	_r.pc += 2;
	return _r.ir;
}

uint16_t GenesisM68K::Prefetched()
{
	_r.ir = _r.irc;
	_r.irc = 0x0000;
	_r.pc += 2;
	return _r.ir;
}

template<uint32_t Size> uint32_t GenesisM68K::Pop()
{
	auto data = Read<Size>((uint32_t)_r.a[7]);
	_r.a[7] += bytes<Size>();
	return data;
}

template<uint32_t Size> void GenesisM68K::Push(uint32_t data)
{
	_r.a[7] -= bytes<Size>();
	return Write<Size, Reverse>((uint32_t)_r.a[7], data);
}

// ============================================================================
// effective-address.cpp
// ============================================================================

uint32_t GenesisM68K::Prefetched(EffectiveAddress& ea)
{
	if(ea.valid) {
		return ea.address;
	}
	ea.valid = true;

	switch(ea.mode) {
		case AddressRegisterIndirect:
			return ea.address = _r.a[ea.reg];

		case AddressRegisterIndirectWithDisplacement: {
			BusIdle(2);
			return ea.address = _r.a[ea.reg] + (int16_t)Prefetched();
		}

		case AddressRegisterIndirectWithIndex: {
			BusIdle(6);
			auto extension = Prefetched();
			auto index = (extension & 0x8000) ? _r.a[(extension >> 12) & 7] : _r.d[(extension >> 12) & 7];
			if(!(extension & 0x800)) {
				index = (int16_t)index;
			}
			return ea.address = _r.a[ea.reg] + index + (int8_t)extension;
		}

		case AbsoluteShortIndirect: {
			BusIdle(2);
			return ea.address = (int16_t)Prefetched();
		}

		case AbsoluteLongIndirect: {
			auto hi = Prefetch();
			auto lo = Prefetch();
			return ea.address = hi << 16 | lo << 0;
		}

		case ProgramCounterIndirectWithDisplacement: {
			BusIdle(2);
			auto base = _r.pc - 2;
			return ea.address = base + (int16_t)Prefetched();
		}

		case ProgramCounterIndirectWithIndex: {
			BusIdle(6);
			auto base = _r.pc - 2;
			auto extension = Prefetched();
			auto index = (extension & 0x8000) ? _r.a[(extension >> 12) & 7] : _r.d[(extension >> 12) & 7];
			if(!(extension & 0x800)) {
				index = (int16_t)index;
			}
			return ea.address = base + index + (int8_t)extension;
		}
	}
	return ea.address = 0;
}

template<uint32_t Size> uint32_t GenesisM68K::Fetch(EffectiveAddress& ea)
{
	if(ea.valid) {
		return ea.address;
	}
	ea.valid = true;

	switch(ea.mode) {
		case DataRegisterDirect:
			return ea.address = Read<Size>(DataRegister { ea.reg });

		case AddressRegisterDirect:
			return ea.address = Read<Size>(AddressRegister { ea.reg });

		case AddressRegisterIndirect:
			return ea.address = Read<Long>(AddressRegister { ea.reg });

		case AddressRegisterIndirectWithPostIncrement:
			return ea.address = Read<Long>(AddressRegister { ea.reg });

		case AddressRegisterIndirectWithPreDecrement:
			return ea.address = Read<Long>(AddressRegister { ea.reg });

		case AddressRegisterIndirectWithDisplacement:
			return ea.address = Read<Long>(AddressRegister { ea.reg }) + (int16_t)Extension<Word>();

		case AddressRegisterIndirectWithIndex: {
			BusIdle(2);
			auto data = Extension<Word>();
			auto index = (data & 0x8000) ? Read<Long>(AddressRegister { data >> 12 }) : Read<Long>(DataRegister { data >> 12 });
			if(!(data & 0x800)) {
				index = (int16_t)index;
			}
			return ea.address = Read<Long>(AddressRegister { ea.reg }) + index + (int8_t)data;
		}

		case AbsoluteShortIndirect:
			return ea.address = (int16_t)Extension<Word>();

		case AbsoluteLongIndirect:
			return ea.address = Extension<Long>();

		case ProgramCounterIndirectWithDisplacement: {
			auto base = _r.pc - 2;
			return ea.address = base + (int16_t)Extension<Word>();
		}

		case ProgramCounterIndirectWithIndex: {
			BusIdle(2);
			auto base = _r.pc - 2;
			auto data = Extension<Word>();
			auto index = (data & 0x8000) ? Read<Long>(AddressRegister { data >> 12 }) : Read<Long>(DataRegister { data >> 12 });
			if(!(data & 0x800)) {
				index = (int16_t)index;
			}
			return ea.address = base + index + (int8_t)data;
		}

		case Immediate:
			return ea.address = Extension<Size>();
	}
	return ea.address = 0;
}

template<uint32_t Size, bool Hold, bool Fast> uint32_t GenesisM68K::Read(EffectiveAddress& ea)
{
	Fetch<Size>(ea);

	switch(ea.mode) {
		case DataRegisterDirect:
			return clip<Size>(ea.address);

		case AddressRegisterDirect:
			return sign<Size>(ea.address);

		case AddressRegisterIndirect:
			return Read<Size>(ea.address);

		case AddressRegisterIndirectWithPostIncrement: {
			auto address = ea.address + (ea.reg == 7 && Size == Byte ? bytes<Word>() : bytes<Size>());
			auto data = Read<Size>(ea.address);
			if(!Hold) {
				Write<Long>(AddressRegister { ea.reg }, ea.address = address);
			}
			return data;
		}

		case AddressRegisterIndirectWithPreDecrement: {
			if(!Fast) {
				BusIdle(2);
			}
			auto address = ea.address - (ea.reg == 7 && Size == Byte ? bytes<Word>() : bytes<Size>());
			auto data = Read<Size>(address);
			if(!Hold) {
				Write<Long>(AddressRegister { ea.reg }, ea.address = address);
			}
			return data;
		}

		case AddressRegisterIndirectWithDisplacement:
			return Read<Size>(ea.address);

		case AddressRegisterIndirectWithIndex:
			return Read<Size>(ea.address);

		case AbsoluteShortIndirect:
			return Read<Size>(ea.address);

		case AbsoluteLongIndirect:
			return Read<Size>(ea.address);

		case ProgramCounterIndirectWithDisplacement:
			return Read<Size>(ea.address);

		case ProgramCounterIndirectWithIndex:
			return Read<Size>(ea.address);

		case Immediate:
			return clip<Size>(ea.address);
	}
	return 0;
}

template<uint32_t Size, bool Hold> void GenesisM68K::Write(EffectiveAddress& ea, uint32_t data)
{
	Fetch<Size>(ea);

	switch(ea.mode) {
		case DataRegisterDirect:
			return Write<Size>(DataRegister { ea.reg }, data);

		case AddressRegisterDirect:
			return Write<Size>(AddressRegister { ea.reg }, data);

		case AddressRegisterIndirect:
			return Write<Size>(ea.address, data);

		case AddressRegisterIndirectWithPostIncrement: {
			auto address = ea.address + (ea.reg == 7 && Size == Byte ? bytes<Word>() : bytes<Size>());
			Write<Size>(ea.address, data);
			if(!Hold) {
				Write<Long>(AddressRegister { ea.reg }, ea.address = address);
			}
			return;
		}

		case AddressRegisterIndirectWithPreDecrement: {
			auto address = ea.address - (ea.reg == 7 && Size == Byte ? bytes<Word>() : bytes<Size>());
			Write<Size, Reverse>(address, data);
			if(!Hold) {
				Write<Long>(AddressRegister { ea.reg }, ea.address = address);
			}
			return;
		}

		case AddressRegisterIndirectWithDisplacement:
			return Write<Size>(ea.address, data);

		case AddressRegisterIndirectWithIndex:
			return Write<Size>(ea.address, data);

		case AbsoluteShortIndirect:
			return Write<Size>(ea.address, data);

		case AbsoluteLongIndirect:
			return Write<Size>(ea.address, data);

		case ProgramCounterIndirectWithDisplacement:
			return Write<Size>(ea.address, data);

		case ProgramCounterIndirectWithIndex:
			return Write<Size>(ea.address, data);

		case Immediate:
			return;
	}
}

// ============================================================================
// conditions.cpp
// ============================================================================

bool GenesisM68K::Condition(uint32_t condition)
{
	switch(condition) {
		case 0: return true; //T
		case 1: return false; //F
		case 2: return !_r.c && !_r.z; //HI
		case 3: return _r.c || _r.z; //LS
		case 4: return !_r.c; //CC,HS
		case 5: return _r.c; //CS,LO
		case 6: return !_r.z; //NE
		case 7: return _r.z; //EQ
		case 8: return !_r.v; //VC
		case 9: return _r.v; //VS
		case 10: return !_r.n; //PL
		case 11: return _r.n; //MI
		case 12: return _r.n == _r.v; //GE
		case 13: return _r.n != _r.v; //LT
		case 14: return _r.n == _r.v && !_r.z; //GT
		case 15: return _r.n != _r.v || _r.z; //LE
	}
	GENESIS_UNREACHABLE();
}

// ============================================================================
// algorithms.cpp
// ============================================================================

template<uint32_t Size, bool ExtendFlag> uint32_t GenesisM68K::ADD(uint32_t source, uint32_t target)
{
	target = clip<Size>(target);
	source = clip<Size>(source);
	uint32_t result = target + source + (ExtendFlag ? _r.x : 0);
	uint32_t carries = target ^ source ^ result;
	uint32_t overflow = (target ^ result) & (source ^ result);

	_r.c = (carries ^ overflow) & msb<Size>();
	_r.v = overflow & msb<Size>();
	_r.z = clip<Size>(result) ? 0 : (ExtendFlag ? _r.z : 1);
	_r.n = sign<Size>(result) < 0;
	_r.x = _r.c;

	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::AND(uint32_t source, uint32_t target)
{
	uint32_t result = target & source;
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::ASL(uint32_t result, uint32_t shift)
{
	bool carry = false;
	uint32_t overflow = 0;
	for(uint32_t i = 0; i < shift; i++) {
		carry = result & msb<Size>();
		uint32_t before = result;
		result <<= 1;
		overflow |= before ^ result;
	}
	_r.c = carry;
	_r.v = sign<Size>(overflow) < 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	if(shift) {
		_r.x = _r.c;
	}
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::ASR(uint32_t result, uint32_t shift)
{
	bool carry = false;
	uint32_t overflow = 0;
	for(uint32_t i = 0; i < shift; i++) {
		carry = result & lsb<Size>();
		uint32_t before = result;
		result = sign<Size>(result) >> 1;
		overflow |= before ^ result;
	}
	_r.c = carry;
	_r.v = sign<Size>(overflow) < 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	if(shift) {
		_r.x = _r.c;
	}
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::CMP(uint32_t source, uint32_t target)
{
	target = clip<Size>(target);
	source = clip<Size>(source);
	uint32_t result = target - source;
	uint32_t carries = target ^ source ^ result;
	uint32_t overflow = (target ^ result) & (source ^ target);
	_r.c = (carries ^ overflow) & msb<Size>();
	_r.v = overflow & msb<Size>();
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::EOR(uint32_t source, uint32_t target)
{
	uint32_t result = target ^ source;
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::LSL(uint32_t result, uint32_t shift)
{
	bool carry = false;
	for(uint32_t i = 0; i < shift; i++) {
		carry = result & msb<Size>();
		result <<= 1;
	}
	_r.c = carry;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	if(shift) {
		_r.x = _r.c;
	}
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::LSR(uint32_t result, uint32_t shift)
{
	bool carry = false;
	for(uint32_t i = 0; i < shift; i++) {
		carry = result & lsb<Size>();
		result >>= 1;
	}
	_r.c = carry;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	if(shift) {
		_r.x = _r.c;
	}
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::OR(uint32_t source, uint32_t target)
{
	uint32_t result = target | source;
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::ROL(uint32_t result, uint32_t shift)
{
	bool carry = false;
	for(uint32_t i = 0; i < shift; i++) {
		carry = result & msb<Size>();
		result = result << 1 | static_cast<uint32_t>(carry);
	}
	_r.c = carry;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::ROR(uint32_t result, uint32_t shift)
{
	bool carry = false;
	for(uint32_t i = 0; i < shift; i++) {
		carry = result & lsb<Size>();
		result >>= 1;
		if(carry) {
			result |= msb<Size>();
		}
	}
	_r.c = carry;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::ROXL(uint32_t result, uint32_t shift)
{
	bool carry = _r.x;
	for(uint32_t i = 0; i < shift; i++) {
		bool extend = carry;
		carry = result & msb<Size>();
		result = result << 1 | static_cast<uint32_t>(extend);
	}
	_r.c = carry;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	_r.x = _r.c;
	return clip<Size>(result);
}

template<uint32_t Size> uint32_t GenesisM68K::ROXR(uint32_t result, uint32_t shift)
{
	bool carry = _r.x;
	for(uint32_t i = 0; i < shift; i++) {
		bool extend = carry;
		carry = result & lsb<Size>();
		result >>= 1;
		if(extend) {
			result |= msb<Size>();
		}
	}
	_r.c = carry;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
	_r.x = _r.c;
	return clip<Size>(result);
}

template<uint32_t Size, bool ExtendFlag> uint32_t GenesisM68K::SUB(uint32_t source, uint32_t target)
{
	target = clip<Size>(target);
	source = clip<Size>(source);
	uint32_t result = target - source - (ExtendFlag ? _r.x : 0);
	uint32_t carries = target ^ source ^ result;
	uint32_t overflow = (target ^ result) & (source ^ target);
	_r.c = (carries ^ overflow) & msb<Size>();
	_r.v = overflow & msb<Size>();
	_r.z = clip<Size>(result) ? 0 : (ExtendFlag ? _r.z : 1);
	_r.n = sign<Size>(result) < 0;
	_r.x = _r.c;
	return result;
}

// ============================================================================
// instructions.cpp (ported verbatim from ares)
// ============================================================================

void GenesisM68K::instructionABCD(EffectiveAddress from, EffectiveAddress with)
{
	auto source = Read<Byte>(from);
	auto target = Read<Byte, Hold, Fast>(with);
	auto result = source + target + _r.x;
	bool c = false, v = false;
	if(((target ^ source ^ result) & 0x10) || (result & 0x0f) >= 0x0a) {
		auto previous = result;
		result += 0x06;
		v = v || ((~previous & result) & 0x80) != 0;
	}
	if(result >= 0xa0) {
		auto previous = result;
		result += 0x60;
		c = true;
		v = v || ((~previous & result) & 0x80) != 0;
	}
	Prefetch();
	Write<Byte>(with, result);
	if(with.mode == DataRegisterDirect) {
		BusIdle(2);
	}
	_r.c = c;
	_r.v = v;
	_r.z = clip<Byte>(result) ? 0 : _r.z;
	_r.n = sign<Byte>(result) < 0;
	_r.x = _r.c;
}

template<uint32_t Size> void GenesisM68K::instructionADD(EffectiveAddress from, DataRegister with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size>(with);
	auto result = ADD<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(from.mode == DataRegisterDirect || from.mode == AddressRegisterDirect || from.mode == Immediate) {
			BusIdle(4);
		} else {
			BusIdle(2);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionADD(DataRegister from, EffectiveAddress with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size, Hold>(with);
	auto result = ADD<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionADDA(EffectiveAddress from, AddressRegister with)
{
	auto source = sign<Size>(Read<Size>(from));
	auto target = Read<Long>(with);
	Prefetch();
	Write<Long>(with, source + target);
	if constexpr(Size == Long) {
		const bool fastSource = from.mode == DataRegisterDirect || from.mode == AddressRegisterDirect || from.mode == Immediate;
		BusIdle(fastSource ? 4 : 2);
	} else {
		BusIdle(4);
	}
}

template<uint32_t Size> void GenesisM68K::instructionADDI(EffectiveAddress with)
{
	auto source = Extension<Size>();
	auto target = Read<Size, Hold>(with);
	auto result = ADD<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(4);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionADDQ(uint8_t immediate, EffectiveAddress with)
{
	auto source = (uint32_t)immediate;
	auto target = Read<Size, Hold>(with);
	auto result = ADD<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(4);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionADDQ(uint8_t immediate, AddressRegister with)
{
	auto result = Read<Long>(with) + immediate;
	Prefetch();
	Write<Long>(with, result);
	BusIdle(4);
}

template<uint32_t Size> void GenesisM68K::instructionADDX(EffectiveAddress from, EffectiveAddress with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size, Hold, Fast>(with);
	auto result = ADD<Size, Extend>(source, target);
	if constexpr(Size == Long) {
		if(with.mode == AddressRegisterIndirectWithPreDecrement) {
			Write<Word>(with, result >> 0);
			Prefetch();
			Write<Word>(with, result >> 16);
		} else {
			Prefetch();
			Write<Long>(with, result);
			BusIdle(4);
		}
	} else {
		Prefetch();
		Write<Size>(with, result);
	}
}

template<uint32_t Size> void GenesisM68K::instructionAND(EffectiveAddress from, DataRegister with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size>(with);
	auto result = AND<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(from.mode == DataRegisterDirect || from.mode == Immediate) {
			BusIdle(4);
		} else {
			BusIdle(2);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionAND(DataRegister from, EffectiveAddress with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size, Hold>(with);
	auto result = AND<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionANDI(EffectiveAddress with)
{
	auto source = Extension<Size>();
	auto target = Read<Size, Hold>(with);
	auto result = AND<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(4);
		}
	}
}

void GenesisM68K::instructionANDI_TO_CCR()
{
	auto data = Extension<Word>();
	WriteCCR(ReadCCR() & data);
	BusIdle(8);
	Read<Word>(_r.pc);
	Prefetch();
}

void GenesisM68K::instructionANDI_TO_SR()
{
	if(Supervisor()) {
		auto data = Extension<Word>();
		WriteSR(ReadSR() & data);
		BusIdle(8);
		Read<Word>(_r.pc);
		Prefetch();
	}
}

template<uint32_t Size> void GenesisM68K::instructionASL(uint8_t count, DataRegister with)
{
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ASL<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionASL(DataRegister from, DataRegister with)
{
	auto count = Read<Long>(from) & 63;
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ASL<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

void GenesisM68K::instructionASL(EffectiveAddress with)
{
	auto result = ASL<Word>(Read<Word, Hold>(with), 1);
	Prefetch();
	Write<Word>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionASR(uint8_t count, DataRegister with)
{
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ASR<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionASR(DataRegister from, DataRegister with)
{
	auto count = Read<Long>(from) & 63;
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ASR<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

void GenesisM68K::instructionASR(EffectiveAddress with)
{
	auto result = ASR<Word>(Read<Word, Hold>(with), 1);
	Prefetch();
	Write<Word>(with, result);
}

void GenesisM68K::instructionBCC(uint8_t test, uint8_t displacement)
{
	if(!Condition(test)) {
		BusIdle(4);
		if(!displacement) {
			Prefetch();
		}
	} else {
		BusIdle(2);
		auto offset = displacement ? (int8_t)displacement : (int16_t)Prefetched() - 2;
		_r.pc -= 2;
		_r.pc += offset;
		Prefetch();
	}
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionBCHG(DataRegister bitreg, EffectiveAddress with)
{
	auto index = Read<Size>(bitreg) & (bits_<Size>() - 1);
	auto test = Read<Size, Hold>(with);
	_r.z = bit(test, index) == 0;
	test ^= (1 << index);
	Prefetch();
	Write<Size>(with, test);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(index < 16 ? 2 : 4);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionBCHG(EffectiveAddress with)
{
	auto index = Extension<Word>() & (bits_<Size>() - 1);
	auto test = Read<Size, Hold>(with);
	_r.z = bit(test, index) == 0;
	test ^= (1 << index);
	Prefetch();
	Write<Size>(with, test);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(index < 16 ? 2 : 4);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionBCLR(DataRegister bitreg, EffectiveAddress with)
{
	auto index = Read<Size>(bitreg) & (bits_<Size>() - 1);
	auto test = Read<Size, Hold>(with);
	_r.z = bit(test, index) == 0;
	test &= ~(1 << index);
	Prefetch();
	Write<Size>(with, test);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(index < 16 ? 4 : 6);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionBCLR(EffectiveAddress with)
{
	auto index = Extension<Word>() & (bits_<Size>() - 1);
	auto test = Read<Size, Hold>(with);
	_r.z = bit(test, index) == 0;
	test &= ~(1 << index);
	Prefetch();
	Write<Size>(with, test);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(index < 16 ? 4 : 6);
		}
	}
}

void GenesisM68K::instructionBRA(uint8_t displacement)
{
	BusIdle(2);
	auto offset = displacement ? (int8_t)displacement : (int16_t)Prefetched() - 2;
	_r.pc -= 2;
	_r.pc += offset;
	Prefetch();
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionBSET(DataRegister bitreg, EffectiveAddress with)
{
	auto index = Read<Size>(bitreg) & (bits_<Size>() - 1);
	auto test = Read<Size, Hold>(with);
	_r.z = bit(test, index) == 0;
	test |= (1 << index);
	Prefetch();
	Write<Size>(with, test);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(index < 16 ? 2 : 4);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionBSET(EffectiveAddress with)
{
	auto index = Extension<Word>() & (bits_<Size>() - 1);
	auto test = Read<Size, Hold>(with);
	_r.z = bit(test, index) == 0;
	test |= (1 << index);
	Prefetch();
	Write<Size>(with, test);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(index < 16 ? 2 : 4);
		}
	}
}

void GenesisM68K::instructionBSR(uint8_t displacement)
{
	BusIdle(2);
	auto offset = displacement ? (int8_t)displacement : (int16_t)Prefetched() - 2;
	_r.pc -= 2;
	Push<Long>(_r.pc);
	_r.pc += offset;
	Prefetch();
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionBTST(DataRegister bitreg, EffectiveAddress with)
{
	auto index = Read<Size>(bitreg) & (bits_<Size>() - 1);
	auto test = Read<Size>(with);
	_r.z = bit(test, index) == 0;
	Prefetch();
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(2);
		}
	}
	if constexpr(Size == Byte) {
		if(with.mode == Immediate) {
			BusIdle(2);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionBTST(EffectiveAddress with)
{
	auto index = Extension<Word>() & (bits_<Size>() - 1);
	auto test = Read<Size>(with);
	_r.z = bit(test, index) == 0;
	Prefetch();
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(2);
		}
	}
}

void GenesisM68K::instructionCHK(DataRegister compare, EffectiveAddress maximum)
{
	auto source = Read<Word>(compare);
	auto target = Read<Word>(maximum);
	CMP<Word>(source, target);
	BusIdle(2);
	bool bound = _r.n || _r.v;
	CMP<Word>(0, source);
	BusIdle(2);
	if(bound) {
		Prefetched();
		return Exception(ExBoundsCheck, VBoundsCheck);
	}
	BusIdle(2);
	if(_r.n) {
		Prefetched();
		return Exception(ExBoundsCheck, VBoundsCheck);
	}
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionCLR(EffectiveAddress with)
{
	Read<Size, Hold>(with);
	Prefetch();
	Write<Size>(with, 0);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect || with.mode == AddressRegisterDirect) {
			BusIdle(2);
		}
	}
	_r.c = 0;
	_r.v = 0;
	_r.z = 1;
	_r.n = 0;
}

template<uint32_t Size> void GenesisM68K::instructionCMP(EffectiveAddress from, DataRegister with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size>(with);
	CMP<Size>(source, target);
	Prefetch();
	if constexpr(Size == Long) {
		BusIdle(2);
	}
}

template<uint32_t Size> void GenesisM68K::instructionCMPA(EffectiveAddress from, AddressRegister with)
{
	auto source = sign<Size>(Read<Size>(from));
	auto target = Read<Long>(with);
	CMP<Long>(source, target);
	Prefetch();
	BusIdle(2);
}

template<uint32_t Size> void GenesisM68K::instructionCMPI(EffectiveAddress with)
{
	auto source = Extension<Size>();
	auto target = Read<Size>(with);
	CMP<Size>(source, target);
	Prefetch();
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(2);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionCMPM(EffectiveAddress from, EffectiveAddress with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size>(with);
	CMP<Size>(source, target);
	Prefetch();
}

void GenesisM68K::instructionDBCC(uint8_t test, DataRegister with)
{
	BusIdle(2);
	_r.pc -= 2;
	if(!Condition(test)) {
		auto disp = sign<Word>(_r.irc);
		_r.pc += disp;
		Prefetch();
		uint16_t result = Read<Word>(with);
		Write<Word>(with, result - 1);
		if(result) {
			Prefetch();
			return;
		} else {
			_r.pc -= disp;
		}
	} else {
		BusIdle(2);
		_r.pc += 2;
	}
	Prefetch();
	Prefetch();
}

void GenesisM68K::instructionDIVS(EffectiveAddress from, DataRegister with)
{
	uint32_t dividend = Read<Long>(with), odividend = dividend;
	uint32_t divisor = Read<Word>(from) << 16, odivisor = divisor;
	if(divisor == 0) {
		BusIdle(4);
		Prefetched();
		return Exception(ExDivisionByZero, VDivisionByZero);
	}
	if(divisor >> 31) {
		divisor = 0u - divisor;
	}
	if(dividend >> 31) {
		dividend = 0u - dividend;
		BusIdle(2);
	}
	_r.c = 0;
	_r.v = dividend >= divisor;
	if(_r.v) {
		_r.z = 0;
		_r.n = 1;
		BusIdle(12);
		Prefetch();
		return;
	}
	uint16_t quotient = 0;
	bool carry = 0;
	uint32_t ticks = 12 + 8;
	for(uint32_t i = 0; i < 15; i++) {
		dividend = dividend << 1;
		quotient = quotient << 1 | static_cast<uint16_t>(carry);
		carry = dividend >= divisor;
		if(carry) {
			dividend -= divisor;
		}
		ticks += !carry ? 8 : 6;
	}
	quotient = quotient << 1 | static_cast<uint16_t>(carry);
	dividend = dividend << 1;
	carry = dividend >= divisor;
	if(carry) {
		dividend -= divisor;
	}
	quotient = quotient << 1 | static_cast<uint16_t>(carry);
	ticks += 4;
	if(odivisor >> 31) {
		ticks += 4;
		if(odividend >> 31) {
			if(quotient >> 15) {
				_r.v = 1;
			}
			dividend = 0u - dividend;
		} else {
			quotient = -quotient;
			if(quotient && !(quotient >> 15)) {
				_r.v = 1;
			}
		}
	} else if(odividend >> 31) {
		ticks += 6;
		quotient = -quotient;
		if(quotient && !(quotient >> 15)) {
			_r.v = 1;
		}
		dividend = 0u - dividend;
	} else {
		ticks += 2;
		if(quotient >> 15) {
			_r.v = 1;
		}
	}
	if(_r.v) {
		_r.z = 0;
		_r.n = 1;
		BusIdle(ticks);
		Prefetch();
		return;
	}
	_r.z = clip<Word>(quotient) == 0;
	_r.n = sign<Word>(quotient) < 0;
	BusIdle(ticks);
	Write<Long>(with, dividend | quotient);
	Prefetch();
}

void GenesisM68K::instructionDIVU(EffectiveAddress from, DataRegister with)
{
	uint32_t dividend = Read<Long>(with);
	uint32_t divisor = Read<Word>(from) << 16;
	if(divisor == 0) {
		BusIdle(4);
		Prefetched();
		return Exception(ExDivisionByZero, VDivisionByZero);
	}
	_r.c = 0;
	_r.v = dividend >= divisor;
	if(_r.v) {
		_r.z = 0;
		_r.n = 1;
		BusIdle(6);
		Prefetch();
		return;
	}
	uint32_t ticks = 6;
	uint16_t quotient = 0;
	bool carry = 0;
	bool force = dividend >> 31;
	dividend = dividend << 1;
	for(uint32_t i = 0; i < 15; i++) {
		carry = force || dividend >= divisor;
		if(carry) {
			dividend -= divisor;
		}
		ticks += !carry ? 8 : !force ? 6 :
												 4;
		force = dividend >> 31;
		dividend = dividend << 1;
		quotient = quotient << 1 | static_cast<uint16_t>(carry);
	}
	carry = force || dividend >= divisor;
	if(carry) {
		dividend -= divisor;
	}
	quotient = quotient << 1 | static_cast<uint16_t>(carry);
	_r.z = clip<Word>(quotient) == 0;
	_r.n = sign<Word>(quotient) < 0;
	BusIdle(ticks + 6);
	Write<Long>(with, dividend | quotient);
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionEOR(DataRegister from, EffectiveAddress with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size, Hold>(with);
	auto result = EOR<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(4);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionEORI(EffectiveAddress with)
{
	auto source = Extension<Size>();
	auto target = Read<Size, Hold>(with);
	auto result = EOR<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(4);
		}
	}
}

void GenesisM68K::instructionEORI_TO_CCR()
{
	auto data = Extension<Word>();
	WriteCCR(ReadCCR() ^ data);
	BusIdle(8);
	Read<Word>(_r.pc);
	Prefetch();
}

void GenesisM68K::instructionEORI_TO_SR()
{
	if(Supervisor()) {
		auto data = Extension<Word>();
		WriteSR(ReadSR() ^ data);
		BusIdle(8);
		Read<Word>(_r.pc);
		Prefetch();
	}
}

void GenesisM68K::instructionEXG(DataRegister x, DataRegister y)
{
	auto z = Read<Long>(x);
	Write<Long>(x, Read<Long>(y));
	Write<Long>(y, z);
	Prefetch();
	BusIdle(2);
}

void GenesisM68K::instructionEXG(AddressRegister x, AddressRegister y)
{
	auto z = Read<Long>(x);
	Prefetch();
	Write<Long>(x, Read<Long>(y));
	Write<Long>(y, z);
	BusIdle(2);
}

void GenesisM68K::instructionEXG(DataRegister x, AddressRegister y)
{
	auto z = Read<Long>(x);
	Prefetch();
	Write<Long>(x, Read<Long>(y));
	Write<Long>(y, z);
	BusIdle(2);
}

template<> void GenesisM68K::instructionEXT<GenesisM68K::Word>(DataRegister with)
{
	auto result = (int8_t)Read<Byte>(with);
	Prefetch();
	Write<Word>(with, result);
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Word>(result) == 0;
	_r.n = sign<Word>(result) < 0;
}

template<> void GenesisM68K::instructionEXT<GenesisM68K::Long>(DataRegister with)
{
	auto result = (int16_t)Read<Word>(with);
	Prefetch();
	Write<Long>(with, result);
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Long>(result) == 0;
	_r.n = sign<Long>(result) < 0;
}

void GenesisM68K::instructionILLEGAL(uint16_t code)
{
	if(bits(code, 12, 15) == 0xa) {
		return Exception(ExIllegal, VIllegalLineA);
	}
	if(bits(code, 12, 15) == 0xf) {
		return Exception(ExIllegal, VIllegalLineF);
	}
	return Exception(ExIllegal, VIllegalInstruction);
}

void GenesisM68K::instructionJMP(EffectiveAddress from)
{
	_r.pc = Prefetched(from);
	Prefetch();
	Prefetch();
}

void GenesisM68K::instructionJSR(EffectiveAddress from)
{
	auto ir = Prefetched(from);
	auto pc = _r.pc;
	_r.pc = ir;
	Prefetch();
	Push<Long>(pc - 2);
	Prefetch();
}

void GenesisM68K::instructionLEA(EffectiveAddress from, AddressRegister to)
{
	Write<Long>(to, Fetch<Long>(from));
	if(from.mode == AddressRegisterIndirectWithIndex || from.mode == ProgramCounterIndirectWithIndex) {
		BusIdle(2);
	}
	Prefetch();
}

void GenesisM68K::instructionLINK(AddressRegister with)
{
	auto displacement = (int16_t)Extension<Word>();
	auto sp = AddressRegister { 7 };
	Push<Long>(Read<Long>(with));
	Write<Long>(with, Read<Long>(sp));
	Write<Long>(sp, Read<Long>(sp) + displacement);
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionLSL(uint8_t count, DataRegister with)
{
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = LSL<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionLSL(DataRegister from, DataRegister with)
{
	auto count = Read<Long>(from) & 63;
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = LSL<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

void GenesisM68K::instructionLSL(EffectiveAddress with)
{
	auto result = LSL<Word>(Read<Word, Hold>(with), 1);
	Prefetch();
	Write<Word>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionLSR(uint8_t count, DataRegister with)
{
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = LSR<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionLSR(DataRegister from, DataRegister with)
{
	auto count = Read<Long>(from) & 63;
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = LSR<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

void GenesisM68K::instructionLSR(EffectiveAddress with)
{
	auto result = LSR<Word>(Read<Word, Hold>(with), 1);
	Prefetch();
	Write<Word>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionMOVE(EffectiveAddress from, EffectiveAddress to)
{
	auto data = Read<Size>(from);
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Size>(data) == 0;
	_r.n = sign<Size>(data) < 0;
	if(to.mode == AddressRegisterIndirectWithPreDecrement) {
		Prefetch();
		Write<Size>(to, data);
	} else {
		Write<Size>(to, data);
		Prefetch();
	}
}

template<uint32_t Size> void GenesisM68K::instructionMOVEA(EffectiveAddress from, AddressRegister to)
{
	auto data = sign<Size>(Read<Size>(from));
	Write<Long>(to, data);
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionMOVEM_TO_MEM(EffectiveAddress to)
{
	auto list = Extension<Word>();
	auto addr = Fetch<Long>(to);
	for(uint32_t n = 0; n < 16; n++) {
		if(!bit(list, n)) {
			continue;
		}
		uint32_t index = to.mode == AddressRegisterIndirectWithPreDecrement ? 15 - n : n;
		if(to.mode == AddressRegisterIndirectWithPreDecrement) {
			addr -= bytes<Size>();
		}
		auto data = index < 8 ? Read<Size>(DataRegister { index }) : Read<Size>(AddressRegister { index });
		if(to.mode == AddressRegisterIndirectWithPreDecrement) {
			Write<Size, Reverse>(addr, data);
		} else {
			Write<Size>(addr, data);
		}
		if(to.mode != AddressRegisterIndirectWithPreDecrement) {
			addr += bytes<Size>();
		}
	}
	AddressRegister with { to.reg };
	if(to.mode == AddressRegisterIndirectWithPreDecrement) {
		Write<Long>(with, addr);
	}
	if(to.mode == AddressRegisterIndirectWithPostIncrement) {
		Write<Long>(with, addr);
	}
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionMOVEM_TO_REG(EffectiveAddress from)
{
	auto list = Extension<Word>();
	auto addr = Fetch<Long>(from);
	for(uint32_t n = 0; n < 16; n++) {
		if(!bit(list, n)) {
			continue;
		}
		uint32_t index = from.mode == AddressRegisterIndirectWithPreDecrement ? 15 - n : n;
		if(from.mode == AddressRegisterIndirectWithPreDecrement) {
			addr -= bytes<Size>();
		}
		auto data = Read<Size>(addr);
		data = sign<Size>(data);
		index < 8 ? Write<Long>(DataRegister { index }, data) : Write<Long>(AddressRegister { index }, data);
		if(from.mode != AddressRegisterIndirectWithPreDecrement) {
			addr += bytes<Size>();
		}
	}
	if(from.mode == AddressRegisterIndirectWithPreDecrement) {
		addr -= 2;
	}
	Read<Word>(addr);
	AddressRegister with { from.reg };
	if(from.mode == AddressRegisterIndirectWithPreDecrement) {
		Write<Long>(with, addr);
	}
	if(from.mode == AddressRegisterIndirectWithPostIncrement) {
		Write<Long>(with, addr);
	}
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionMOVEP(DataRegister from, EffectiveAddress to)
{
	auto address = Fetch<Size>(to);
	auto data = Read<Long>(from);
	uint32_t shift = bits_<Size>();
	for(uint32_t i = 0; i < bytes<Size>(); i++) {
		shift -= 8;
		Write<Byte>(address, data >> shift);
		address += 2;
	}
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionMOVEP(EffectiveAddress from, DataRegister to)
{
	auto address = Fetch<Size>(from);
	auto data = Read<Long>(to);
	uint32_t shift = bits_<Size>();
	for(uint32_t i = 0; i < bytes<Size>(); i++) {
		shift -= 8;
		data &= ~(0xff << shift);
		data |= Read<Byte>(address) << shift;
		address += 2;
	}
	Write<Long>(to, data);
	Prefetch();
}

void GenesisM68K::instructionMOVEQ(uint8_t immediate, DataRegister to)
{
	Write<Long>(to, sign<Byte>(immediate));
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Byte>(immediate) == 0;
	_r.n = sign<Byte>(immediate) < 0;
	Prefetch();
}

void GenesisM68K::instructionMOVE_FROM_SR(EffectiveAddress to)
{
	auto data = ReadSR();
	Read<Word, Hold>(to);
	Prefetch();
	Write<Word>(to, data);
	if(to.mode == DataRegisterDirect) {
		BusIdle(2);
	}
}

void GenesisM68K::instructionMOVE_TO_CCR(EffectiveAddress from)
{
	auto data = Read<Word>(from);
	BusIdle(4);
	WriteCCR(data);
	BusIdle(4);
	Prefetch();
}

void GenesisM68K::instructionMOVE_TO_SR(EffectiveAddress from)
{
	if(Supervisor()) {
		auto data = Read<Word>(from);
		BusIdle(4);
		WriteSR(data);
		BusIdle(4);
		Prefetch();
	}
}

void GenesisM68K::instructionMOVE_FROM_USP(AddressRegister to)
{
	if(Supervisor()) {
		Write<Long>(to, _r.sp);
		Prefetch();
	}
}

void GenesisM68K::instructionMOVE_TO_USP(AddressRegister from)
{
	if(Supervisor()) {
		_r.sp = Read<Long>(from);
		Prefetch();
	}
}

void GenesisM68K::instructionMULS(EffectiveAddress from, DataRegister with)
{
	auto source = Read<Word>(from);
	auto target = Read<Word>(with);
	auto result = (int16_t)source * (int16_t)target;
	Prefetch();
	auto cycles = std::bitset<32>((uint16_t)(source << 1) ^ source).count();
	BusIdle(34 + static_cast<uint32_t>(cycles) * 2);
	Write<Long>(with, result);
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Long>(result) == 0;
	_r.n = sign<Long>(result) < 0;
}

void GenesisM68K::instructionMULU(EffectiveAddress from, DataRegister with)
{
	auto source = Read<Word>(from);
	auto target = Read<Word>(with);
	auto result = source * target;
	Prefetch();
	auto cycles = std::bitset<32>(source).count();
	BusIdle(34 + static_cast<uint32_t>(cycles) * 2);
	Write<Long>(with, result);
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Long>(result) == 0;
	_r.n = sign<Long>(result) < 0;
}

void GenesisM68K::instructionNBCD(EffectiveAddress with)
{
	auto source = Read<Byte, Hold>(with);
	auto target = 0u;
	auto result = target - source - _r.x;
	bool c = false, v = false;
	const bool adjustLo = (target ^ source ^ result) & 0x10;
	const bool adjustHi = result & 0x100;
	if(adjustLo) {
		auto previous = result;
		result -= 0x06;
		c = (~previous & 0x80) & (result & 0x80);
		v = v || ((previous & ~result) & 0x80) != 0;
	}
	if(adjustHi) {
		auto previous = result;
		result -= 0x60;
		c = true;
		v = v || ((previous & ~result) & 0x80) != 0;
	}
	Prefetch();
	Write<Byte>(with, result);
	if(with.mode == DataRegisterDirect || with.mode == AddressRegisterDirect) {
		BusIdle(2);
	}
	_r.c = c;
	_r.v = v;
	_r.z = clip<Byte>(result) ? 0 : _r.z;
	_r.n = sign<Byte>(result) < 0;
	_r.x = _r.c;
}

template<uint32_t Size> void GenesisM68K::instructionNEG(EffectiveAddress with)
{
	auto result = SUB<Size>(Read<Size, Hold>(with), 0);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect || with.mode == AddressRegisterDirect) {
			BusIdle(2);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionNEGX(EffectiveAddress with)
{
	auto result = SUB<Size, Extend>(Read<Size, Hold>(with), 0);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect || with.mode == AddressRegisterDirect) {
			BusIdle(2);
		}
	}
}

void GenesisM68K::instructionNOP()
{ Prefetch(); }

template<uint32_t Size> void GenesisM68K::instructionNOT(EffectiveAddress with)
{
	auto result = ~Read<Size, Hold>(with);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect || with.mode == AddressRegisterDirect) {
			BusIdle(2);
		}
	}
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Size>(result) == 0;
	_r.n = sign<Size>(result) < 0;
}

template<uint32_t Size> void GenesisM68K::instructionOR(EffectiveAddress from, DataRegister with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size>(with);
	auto result = OR<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(from.mode == DataRegisterDirect || from.mode == Immediate) {
			BusIdle(4);
		} else {
			BusIdle(2);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionOR(DataRegister from, EffectiveAddress with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size, Hold>(with);
	auto result = OR<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionORI(EffectiveAddress with)
{
	auto source = Extension<Size>();
	auto target = Read<Size, Hold>(with);
	auto result = OR<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(4);
		}
	}
}

void GenesisM68K::instructionORI_TO_CCR()
{
	auto data = Extension<Word>();
	WriteCCR(ReadCCR() | data);
	BusIdle(8);
	Read<Word>(_r.pc);
	Prefetch();
}

void GenesisM68K::instructionORI_TO_SR()
{
	if(Supervisor()) {
		auto data = Extension<Word>();
		WriteSR(ReadSR() | data);
		BusIdle(8);
		Read<Word>(_r.pc);
		Prefetch();
	}
}

void GenesisM68K::instructionPEA(EffectiveAddress from)
{
	auto data = Fetch<Long>(from);
	if(from.mode == AddressRegisterIndirectWithIndex || from.mode == ProgramCounterIndirectWithIndex) {
		BusIdle(2);
	}
	if(from.mode == AbsoluteShortIndirect || from.mode == AbsoluteLongIndirect) {
		Push<Long>(data);
		Prefetch();
	} else {
		Prefetch();
		Push<Long>(data);
	}
}

void GenesisM68K::instructionRESET()
{
	if(Supervisor()) {
		_r.reset = 1;
		BusIdle(128);
		_r.reset = 0;
		Prefetch();
	}
}

template<uint32_t Size> void GenesisM68K::instructionROL(uint8_t count, DataRegister with)
{
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ROL<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionROL(DataRegister from, DataRegister with)
{
	auto count = Read<Long>(from) & 63;
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ROL<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

void GenesisM68K::instructionROL(EffectiveAddress with)
{
	auto result = ROL<Word>(Read<Word, Hold>(with), 1);
	Prefetch();
	Write<Word>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionROR(uint8_t count, DataRegister with)
{
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ROR<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionROR(DataRegister from, DataRegister with)
{
	auto count = Read<Long>(from) & 63;
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ROR<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

void GenesisM68K::instructionROR(EffectiveAddress with)
{
	auto result = ROR<Word>(Read<Word, Hold>(with), 1);
	Prefetch();
	Write<Word>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionROXL(uint8_t count, DataRegister with)
{
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ROXL<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionROXL(DataRegister from, DataRegister with)
{
	auto count = Read<Long>(from) & 63;
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ROXL<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

void GenesisM68K::instructionROXL(EffectiveAddress with)
{
	auto result = ROXL<Word>(Read<Word, Hold>(with), 1);
	Prefetch();
	Write<Word>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionROXR(uint8_t count, DataRegister with)
{
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ROXR<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

template<uint32_t Size> void GenesisM68K::instructionROXR(DataRegister from, DataRegister with)
{
	auto count = Read<Long>(from) & 63;
	Prefetch();
	BusIdle((Size != Long ? 2 : 4) + count * 2);
	auto result = ROXR<Size>(Read<Size>(with), count);
	Write<Size>(with, result);
}

void GenesisM68K::instructionROXR(EffectiveAddress with)
{
	auto result = ROXR<Word>(Read<Word, Hold>(with), 1);
	Prefetch();
	Write<Word>(with, result);
}

void GenesisM68K::instructionRTE()
{
	if(Supervisor()) {
		auto sr = Pop<Word>();
		_r.pc = Pop<Long>();
		WriteSR(sr);
		Prefetch();
		Prefetch();
	}
}

void GenesisM68K::instructionRTR()
{
	WriteCCR(Pop<Word>());
	_r.pc = Pop<Long>();
	Prefetch();
	Prefetch();
}

void GenesisM68K::instructionRTS()
{
	_r.pc = Pop<Long>();
	Prefetch();
	Prefetch();
}

void GenesisM68K::instructionSBCD(EffectiveAddress from, EffectiveAddress with)
{
	auto source = Read<Byte>(from);
	auto target = Read<Byte, Hold, Fast>(with);
	auto result = target - source - _r.x;
	bool c = false, v = false;
	const bool adjustLo = (target ^ source ^ result) & 0x10;
	const bool adjustHi = result & 0x100;
	if(adjustLo) {
		auto previous = result;
		result -= 0x06;
		c = (~previous & 0x80) & (result & 0x80);
		v = v || ((previous & ~result) & 0x80) != 0;
	}
	if(adjustHi) {
		auto previous = result;
		result -= 0x60;
		c = true;
		v = v || ((previous & ~result) & 0x80) != 0;
	}
	Prefetch();
	Write<Byte>(with, result);
	if(with.mode == DataRegisterDirect) {
		BusIdle(2);
	}
	_r.c = c;
	_r.v = v;
	_r.z = clip<Byte>(result) ? 0 : _r.z;
	_r.n = sign<Byte>(result) < 0;
	_r.x = _r.c;
}

void GenesisM68K::instructionSCC(uint8_t test, EffectiveAddress to)
{
	Read<Byte, Hold>(to);
	Prefetch();
	if(!Condition(test)) {
		Write<Byte>(to, 0);
	} else {
		Write<Byte>(to, ~0);
		if(to.mode == DataRegisterDirect) {
			BusIdle(2);
		}
	}
}

void GenesisM68K::instructionSTOP()
{
	if(Supervisor()) {
		auto sr = Extension<Word>();
		WriteSR(sr);
		_r.stop = true;
		Prefetch();
	}
}

template<uint32_t Size> void GenesisM68K::instructionSUB(EffectiveAddress from, DataRegister with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size>(with);
	auto result = SUB<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(from.mode == DataRegisterDirect || from.mode == AddressRegisterDirect || from.mode == Immediate) {
			BusIdle(4);
		} else {
			BusIdle(2);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionSUB(DataRegister from, EffectiveAddress with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size, Hold>(with);
	auto result = SUB<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect || with.mode == AddressRegisterDirect) {
			BusIdle(4);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionSUBA(EffectiveAddress from, AddressRegister to)
{
	auto source = sign<Size>(Read<Size>(from));
	auto target = Read<Long>(to);
	Prefetch();
	Write<Long>(to, target - source);
	if constexpr(Size == Long) {
		const bool fastSource = from.mode == DataRegisterDirect || from.mode == AddressRegisterDirect || from.mode == Immediate;
		BusIdle(fastSource ? 4 : 2);
	} else {
		BusIdle(4);
	}
}

template<uint32_t Size> void GenesisM68K::instructionSUBI(EffectiveAddress with)
{
	auto source = Extension<Size>();
	auto target = Read<Size, Hold>(with);
	auto result = SUB<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(4);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionSUBQ(uint8_t immediate, EffectiveAddress with)
{
	auto source = (uint32_t)immediate;
	auto target = Read<Size, Hold>(with);
	auto result = SUB<Size>(source, target);
	Prefetch();
	Write<Size>(with, result);
	if constexpr(Size == Long) {
		if(with.mode == DataRegisterDirect) {
			BusIdle(4);
		}
	}
}

template<uint32_t Size> void GenesisM68K::instructionSUBQ(uint8_t immediate, AddressRegister with)
{
	auto result = Read<Long>(with) - immediate;
	Prefetch();
	Write<Long>(with, result);
	BusIdle(4);
}

template<uint32_t Size> void GenesisM68K::instructionSUBX(EffectiveAddress from, EffectiveAddress with)
{
	auto source = Read<Size>(from);
	auto target = Read<Size, Hold, Fast>(with);
	auto result = SUB<Size, Extend>(source, target);
	if constexpr(Size == Long) {
		if(with.mode == AddressRegisterIndirectWithPreDecrement) {
			Write<Word>(with, result >> 0);
			Prefetch();
			Write<Word>(with, result >> 16);
		} else {
			Prefetch();
			Write<Long>(with, result);
			BusIdle(4);
		}
	} else {
		Prefetch();
		Write<Size>(with, result);
	}
}

void GenesisM68K::instructionSWAP(DataRegister with)
{
	auto result = Read<Long>(with);
	result = result >> 16 | result << 16;
	Write<Long>(with, result);

	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Long>(result) == 0;
	_r.n = sign<Long>(result) < 0;
	Prefetch();
}

void GenesisM68K::instructionTAS(EffectiveAddress with)
{
	uint32_t data;

	if(BusLockable() || with.mode == DataRegisterDirect) {
		data = Read<Byte, Hold>(with);
		if(with.mode != DataRegisterDirect) {
			BusIdle(2);
		}
		Write<Byte>(with, data | 0x80);
		Prefetch();
	} else {
		//Mega Drive models 1&2 have a bug that prevents TAS write from taking effect
		data = Read<Byte>(with);
		BusIdle(2 + 4); // 2 idle + 4 write (skipped)
		Prefetch();
	}

	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Byte>(data) == 0;
	_r.n = sign<Byte>(data) < 0;
}

void GenesisM68K::instructionTRAP(uint8_t vector)
{
	Prefetched();
	return Exception(ExTrap, VTrap + vector, _r.i);
}

void GenesisM68K::instructionTRAPV()
{
	if(_r.v) {
		Prefetched();
		return Exception(ExOverflow, VOverflow);
	}
	Prefetch();
}

template<uint32_t Size> void GenesisM68K::instructionTST(EffectiveAddress from)
{
	auto data = Read<Size>(from);
	_r.c = 0;
	_r.v = 0;
	_r.z = clip<Size>(data) == 0;
	_r.n = sign<Size>(data) < 0;
	Prefetch();
}

void GenesisM68K::instructionUNLK(AddressRegister with)
{
	auto sp = AddressRegister { 7 };
	Write<Long>(sp, Read<Long>(with));
	Write<Long>(with, Pop<Long>());
	Prefetch();
}

// ============================================================================
// serialization.cpp - ISerializable
// ============================================================================

void GenesisM68K::Serialize(Serializer& s)
{
	for(int i = 0; i < 8; i++) {
		SVI(_r.d[i]);
	}
	for(int i = 0; i < 8; i++) {
		SVI(_r.a[i]);
	}
	SV(_r.sp);
	SV(_r.pc);
	SV(_r.c);
	SV(_r.v);
	SV(_r.z);
	SV(_r.n);
	SV(_r.x);
	SV(_r.i);
	SV(_r.s);
	SV(_r.t);
	SV(_r.irc);
	SV(_r.ir);
	SV(_r.ird);
	SV(_r.stop);
	SV(_r.reset);
}

// ============================================================================
// instruction.cpp - BuildInstructionTable (ported from ares)
// ============================================================================

GenesisM68K::GenesisM68K()
{
	BuildInstructionTable();
}

void GenesisM68K::BuildInstructionTable()
{
	//Initialize all entries to the ILLEGAL instruction first
	for(int i = 0; i < 65536; i++) {
		_instructionTable[i] = [this, i] { instructionILLEGAL(i); };
	}

//Helper macros (local to this function, matching ares idiom)
#define bind(id, name, ...) { \
		_instructionTable[id] = [this, ##__VA_ARGS__]() mutable { instruction##name(__VA_ARGS__); }; \
	}

	//ABCD
	for(uint32_t treg = 0; treg < 8; treg++) {
		for(uint32_t sreg = 0; sreg < 8; sreg++) {
			auto opcode = Pattern("1100 ---1 0000 ----") | treg << 9 | sreg << 0;

			EffectiveAddress dataWith { DataRegisterDirect, treg };
			EffectiveAddress dataFrom { DataRegisterDirect, sreg };
			bind(opcode | 0 << 3, ABCD, dataFrom, dataWith);

			EffectiveAddress addressWith { AddressRegisterIndirectWithPreDecrement, treg };
			EffectiveAddress addressFrom { AddressRegisterIndirectWithPreDecrement, sreg };
			bind(opcode | 1 << 3, ABCD, addressFrom, addressWith);
		}
	}

	//ADD (ea -> Dn)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1101 ---0 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 7 && reg >= 5) {
					continue;
				}

				EffectiveAddress from { mode, reg };
				DataRegister with { dreg };
				bind(opcode | 0 << 6, ADD<Byte>, from, with);
				bind(opcode | 1 << 6, ADD<Word>, from, with);
				bind(opcode | 2 << 6, ADD<Long>, from, with);

				if(mode == 1) {
					_instructionTable[opcode | 0 << 6] = [this, opcode] { instructionILLEGAL(opcode); };
				}
			}
		}
	}

	//ADD (Dn -> ea)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1101 ---1 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode <= 1 || (mode == 7 && reg >= 2)) {
					continue;
				}

				DataRegister from { dreg };
				EffectiveAddress with { mode, reg };
				bind(opcode | 0 << 6, ADD<Byte>, from, with);
				bind(opcode | 1 << 6, ADD<Word>, from, with);
				bind(opcode | 2 << 6, ADD<Long>, from, with);
			}
		}
	}

	//ADDA
	for(uint32_t areg = 0; areg < 8; areg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1101 ---+ 11-- ----") | areg << 9 | mode << 3 | reg << 0;
				if(mode == 7 && reg >= 5) {
					continue;
				}

				AddressRegister with { areg };
				EffectiveAddress from { mode, reg };
				bind(opcode | 0 << 8, ADDA<Word>, from, with);
				bind(opcode | 1 << 8, ADDA<Long>, from, with);
			}
		}
	}

	//ADDI
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 0110 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}

			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, ADDI<Byte>, with);
			bind(opcode | 1 << 6, ADDI<Word>, with);
			bind(opcode | 2 << 6, ADDI<Long>, with);
		}
	}

	//ADDQ
	for(uint32_t data = 0; data < 8; data++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("0101 ---0 ++-- ----") | data << 9 | mode << 3 | reg << 0;
				if(mode == 7 && reg >= 2) {
					continue;
				}

				uint8_t immediate = data ? data : 8;
				if(mode != 1) {
					EffectiveAddress with { mode, reg };
					bind(opcode | 0 << 6, ADDQ<Byte>, immediate, with);
					bind(opcode | 1 << 6, ADDQ<Word>, immediate, with);
					bind(opcode | 2 << 6, ADDQ<Long>, immediate, with);
				} else {
					AddressRegister with { reg };
					bind(opcode | 1 << 6, ADDQ<Word>, immediate, with);
					bind(opcode | 2 << 6, ADDQ<Long>, immediate, with);
				}
			}
		}
	}

	//ADDX
	for(uint32_t xreg = 0; xreg < 8; xreg++) {
		for(uint32_t yreg = 0; yreg < 8; yreg++) {
			auto opcode = Pattern("1101 ---1 ++00 ----") | xreg << 9 | yreg << 0;

			EffectiveAddress dataWith { DataRegisterDirect, xreg };
			EffectiveAddress dataFrom { DataRegisterDirect, yreg };
			bind(opcode | 0 << 6 | 0 << 3, ADDX<Byte>, dataFrom, dataWith);
			bind(opcode | 1 << 6 | 0 << 3, ADDX<Word>, dataFrom, dataWith);
			bind(opcode | 2 << 6 | 0 << 3, ADDX<Long>, dataFrom, dataWith);

			EffectiveAddress addressWith { AddressRegisterIndirectWithPreDecrement, xreg };
			EffectiveAddress addressFrom { AddressRegisterIndirectWithPreDecrement, yreg };
			bind(opcode | 0 << 6 | 1 << 3, ADDX<Byte>, addressFrom, addressWith);
			bind(opcode | 1 << 6 | 1 << 3, ADDX<Word>, addressFrom, addressWith);
			bind(opcode | 2 << 6 | 1 << 3, ADDX<Long>, addressFrom, addressWith);
		}
	}

	//AND (ea -> Dn)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1100 ---0 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 5)) {
					continue;
				}

				EffectiveAddress from { mode, reg };
				DataRegister with { dreg };
				bind(opcode | 0 << 6, AND<Byte>, from, with);
				bind(opcode | 1 << 6, AND<Word>, from, with);
				bind(opcode | 2 << 6, AND<Long>, from, with);
			}
		}
	}

	//AND (Dn -> ea)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1100 ---1 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode <= 1 || (mode == 7 && reg >= 2)) {
					continue;
				}

				DataRegister from { dreg };
				EffectiveAddress with { mode, reg };
				bind(opcode | 0 << 6, AND<Byte>, from, with);
				bind(opcode | 1 << 6, AND<Word>, from, with);
				bind(opcode | 2 << 6, AND<Long>, from, with);
			}
		}
	}

	//ANDI
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 0010 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}

			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, ANDI<Byte>, with);
			bind(opcode | 1 << 6, ANDI<Word>, with);
			bind(opcode | 2 << 6, ANDI<Long>, with);
		}
	}

	//ANDI_TO_CCR
	bind(Pattern("0000 0010 0011 1100"), ANDI_TO_CCR);

	//ANDI_TO_SR
	bind(Pattern("0000 0010 0111 1100"), ANDI_TO_SR);

	//ASL (immediate)
	for(uint32_t immediate = 0; immediate < 8; immediate++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---1 ++00 0---") | immediate << 9 | dreg << 0;
			uint8_t count = immediate ? immediate : 8;
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ASL<Byte>, count, with);
			bind(opcode | 1 << 6, ASL<Word>, count, with);
			bind(opcode | 2 << 6, ASL<Long>, count, with);
		}
	}

	//ASL (register)
	for(uint32_t sreg = 0; sreg < 8; sreg++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---1 ++10 0---") | sreg << 9 | dreg << 0;
			DataRegister from { sreg };
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ASL<Byte>, from, with);
			bind(opcode | 1 << 6, ASL<Word>, from, with);
			bind(opcode | 2 << 6, ASL<Long>, from, with);
		}
	}

	//ASL (effective address)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("1110 0001 11-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, ASL, with);
		}
	}

	//ASR (immediate)
	for(uint32_t immediate = 0; immediate < 8; immediate++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---0 ++00 0---") | immediate << 9 | dreg << 0;
			uint8_t count = immediate ? immediate : 8;
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ASR<Byte>, count, with);
			bind(opcode | 1 << 6, ASR<Word>, count, with);
			bind(opcode | 2 << 6, ASR<Long>, count, with);
		}
	}

	//ASR (register)
	for(uint32_t sreg = 0; sreg < 8; sreg++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---0 ++10 0---") | sreg << 9 | dreg << 0;
			DataRegister from { sreg };
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ASR<Byte>, from, with);
			bind(opcode | 1 << 6, ASR<Word>, from, with);
			bind(opcode | 2 << 6, ASR<Long>, from, with);
		}
	}

	//ASR (effective address)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("1110 0000 11-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, ASR, with);
		}
	}

	//BCC
	for(uint32_t test = 0; test < 16; test++) {
		for(uint32_t displacement = 0; displacement < 256; displacement++) {
			if(test <= 1) {
				continue;
			}
			auto opcode = Pattern("0110 ---- ---- ----") | test << 8 | displacement << 0;
			bind(opcode, BCC, test, displacement);
		}
	}

	//BCHG (register)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("0000 ---1 01-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 2)) {
					continue;
				}
				DataRegister bit { dreg };
				EffectiveAddress with { mode, reg };
				if(mode == 0) {
					bind(opcode, BCHG<Long>, bit, with);
				}
				if(mode != 0) {
					bind(opcode, BCHG<Byte>, bit, with);
				}
			}
		}
	}

	//BCHG (immediate)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 1000 01-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			if(mode == 0) {
				bind(opcode, BCHG<Long>, with);
			}
			if(mode != 0) {
				bind(opcode, BCHG<Byte>, with);
			}
		}
	}

	//BCLR (register)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("0000 ---1 10-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 2)) {
					continue;
				}
				DataRegister bit { dreg };
				EffectiveAddress with { mode, reg };
				if(mode == 0) {
					bind(opcode, BCLR<Long>, bit, with);
				}
				if(mode != 0) {
					bind(opcode, BCLR<Byte>, bit, with);
				}
			}
		}
	}

	//BCLR (immediate)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 1000 10-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			if(mode == 0) {
				bind(opcode, BCLR<Long>, with);
			}
			if(mode != 0) {
				bind(opcode, BCLR<Byte>, with);
			}
		}
	}

	//BRA
	for(uint32_t displacement = 0; displacement < 256; displacement++) {
		auto opcode = Pattern("0110 0000 ---- ----") | displacement << 0;
		bind(opcode, BRA, displacement);
	}

	//BSET (register)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("0000 ---1 11-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 2)) {
					continue;
				}
				DataRegister bit { dreg };
				EffectiveAddress with { mode, reg };
				if(mode == 0) {
					bind(opcode, BSET<Long>, bit, with);
				}
				if(mode != 0) {
					bind(opcode, BSET<Byte>, bit, with);
				}
			}
		}
	}

	//BSET (immediate)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 1000 11-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			if(mode == 0) {
				bind(opcode, BSET<Long>, with);
			}
			if(mode != 0) {
				bind(opcode, BSET<Byte>, with);
			}
		}
	}

	//BSR
	for(uint32_t displacement = 0; displacement < 256; displacement++) {
		auto opcode = Pattern("0110 0001 ---- ----") | displacement << 0;
		bind(opcode, BSR, displacement);
	}

	//BTST (register)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("0000 ---1 00-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 5)) {
					continue;
				}
				DataRegister bit { dreg };
				EffectiveAddress with { mode, reg };
				if(mode == 0) {
					bind(opcode, BTST<Long>, bit, with);
				}
				if(mode != 0) {
					bind(opcode, BTST<Byte>, bit, with);
				}
			}
		}
	}

	//BTST (immediate)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 1000 00-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 4)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			if(mode == 0) {
				bind(opcode, BTST<Long>, with);
			}
			if(mode != 0) {
				bind(opcode, BTST<Byte>, with);
			}
		}
	}

	//CHK
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("0100 ---1 10-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 5)) {
					continue;
				}
				DataRegister compare { dreg };
				EffectiveAddress maximum { mode, reg };
				bind(opcode, CHK, compare, maximum);
			}
		}
	}

	//CLR
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 0010 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, CLR<Byte>, with);
			bind(opcode | 1 << 6, CLR<Word>, with);
			bind(opcode | 2 << 6, CLR<Long>, with);
		}
	}

	//CMP
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1011 ---0 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 7 && reg >= 5) {
					continue;
				}
				DataRegister with { dreg };
				EffectiveAddress from { mode, reg };
				bind(opcode | 0 << 6, CMP<Byte>, from, with);
				bind(opcode | 1 << 6, CMP<Word>, from, with);
				bind(opcode | 2 << 6, CMP<Long>, from, with);
				if(mode == 1) {
					_instructionTable[opcode | 0 << 6] = [this, opcode] { instructionILLEGAL(opcode); };
				}
			}
		}
	}

	//CMPA
	for(uint32_t areg = 0; areg < 8; areg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1011 ---+ 11-- ----") | areg << 9 | mode << 3 | reg << 0;
				if(mode == 7 && reg >= 5) {
					continue;
				}
				AddressRegister with { areg };
				EffectiveAddress from { mode, reg };
				bind(opcode | 0 << 8, CMPA<Word>, from, with);
				bind(opcode | 1 << 8, CMPA<Long>, from, with);
			}
		}
	}

	//CMPI
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 1100 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, CMPI<Byte>, with);
			bind(opcode | 1 << 6, CMPI<Word>, with);
			bind(opcode | 2 << 6, CMPI<Long>, with);
		}
	}

	//CMPM
	for(uint32_t xreg = 0; xreg < 8; xreg++) {
		for(uint32_t yreg = 0; yreg < 8; yreg++) {
			auto opcode = Pattern("1011 ---1 ++00 1---") | xreg << 9 | yreg << 0;
			EffectiveAddress with { AddressRegisterIndirectWithPostIncrement, xreg };
			EffectiveAddress from { AddressRegisterIndirectWithPostIncrement, yreg };
			bind(opcode | 0 << 6, CMPM<Byte>, from, with);
			bind(opcode | 1 << 6, CMPM<Word>, from, with);
			bind(opcode | 2 << 6, CMPM<Long>, from, with);
		}
	}

	//DBCC
	for(uint32_t condition = 0; condition < 16; condition++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("0101 ---- 1100 1---") | condition << 8 | dreg << 0;
			DataRegister with { dreg };
			bind(opcode, DBCC, condition, with);
		}
	}

	//DIVS
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1000 ---1 11-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 5)) {
					continue;
				}
				DataRegister with { dreg };
				EffectiveAddress from { mode, reg };
				bind(opcode, DIVS, from, with);
			}
		}
	}

	//DIVU
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1000 ---0 11-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 5)) {
					continue;
				}
				DataRegister with { dreg };
				EffectiveAddress from { mode, reg };
				bind(opcode, DIVU, from, with);
			}
		}
	}

	//EOR
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1011 ---1 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 2)) {
					continue;
				}
				DataRegister from { dreg };
				EffectiveAddress with { mode, reg };
				bind(opcode | 0 << 6, EOR<Byte>, from, with);
				bind(opcode | 1 << 6, EOR<Word>, from, with);
				bind(opcode | 2 << 6, EOR<Long>, from, with);
			}
		}
	}

	//EORI
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 1010 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, EORI<Byte>, with);
			bind(opcode | 1 << 6, EORI<Word>, with);
			bind(opcode | 2 << 6, EORI<Long>, with);
		}
	}

	//EORI_TO_CCR
	bind(Pattern("0000 1010 0011 1100"), EORI_TO_CCR);

	//EORI_TO_SR
	bind(Pattern("0000 1010 0111 1100"), EORI_TO_SR);

	//EXG (Dx,Dy)
	for(uint32_t xreg = 0; xreg < 8; xreg++) {
		for(uint32_t yreg = 0; yreg < 8; yreg++) {
			auto opcode = Pattern("1100 ---1 0100 0---") | xreg << 9 | yreg << 0;
			DataRegister x { xreg };
			DataRegister y { yreg };
			bind(opcode, EXG, x, y);
		}
	}

	//EXG (Ax,Ay)
	for(uint32_t xreg = 0; xreg < 8; xreg++) {
		for(uint32_t yreg = 0; yreg < 8; yreg++) {
			auto opcode = Pattern("1100 ---1 0100 1---") | xreg << 9 | yreg << 0;
			AddressRegister x { xreg };
			AddressRegister y { yreg };
			bind(opcode, EXG, x, y);
		}
	}

	//EXG (Dx,Ay)
	for(uint32_t xreg = 0; xreg < 8; xreg++) {
		for(uint32_t yreg = 0; yreg < 8; yreg++) {
			auto opcode = Pattern("1100 ---1 1000 1---") | xreg << 9 | yreg << 0;
			DataRegister x { xreg };
			AddressRegister y { yreg };
			bind(opcode, EXG, x, y);
		}
	}

	//EXT
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		auto opcode = Pattern("0100 1000 1+00 0---") | dreg << 0;
		DataRegister with { dreg };
		bind(opcode | 0 << 6, EXT<Word>, with);
		bind(opcode | 1 << 6, EXT<Long>, with);
	}

	//ILLEGAL (specific opcode)
	{
		uint16_t illegalOpcode = Pattern("0100 1010 1111 1100");
		_instructionTable[illegalOpcode] = [this, illegalOpcode]() { instructionILLEGAL(illegalOpcode); };
	}

	//JMP
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 1110 11-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || mode == 3 || mode == 4 || (mode == 7 && reg >= 4)) {
				continue;
			}
			EffectiveAddress from { mode, reg };
			bind(opcode, JMP, from);
		}
	}

	//JSR
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 1110 10-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || mode == 3 || mode == 4 || (mode == 7 && reg >= 4)) {
				continue;
			}
			EffectiveAddress from { mode, reg };
			bind(opcode, JSR, from);
		}
	}

	//LEA
	for(uint32_t areg = 0; areg < 8; areg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("0100 ---1 11-- ----") | areg << 9 | mode << 3 | reg << 0;
				if(mode <= 1 || mode == 3 || mode == 4 || (mode == 7 && reg >= 4)) {
					continue;
				}
				AddressRegister to { areg };
				EffectiveAddress from { mode, reg };
				bind(opcode, LEA, from, to);
			}
		}
	}

	//LINK
	for(uint32_t areg = 0; areg < 8; areg++) {
		auto opcode = Pattern("0100 1110 0101 0---") | areg << 0;
		AddressRegister with { areg };
		bind(opcode, LINK, with);
	}

	//LSL (immediate)
	for(uint32_t immediate = 0; immediate < 8; immediate++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---1 ++00 1---") | immediate << 9 | dreg << 0;
			uint8_t count = immediate ? immediate : 8;
			DataRegister with { dreg };
			bind(opcode | 0 << 6, LSL<Byte>, count, with);
			bind(opcode | 1 << 6, LSL<Word>, count, with);
			bind(opcode | 2 << 6, LSL<Long>, count, with);
		}
	}

	//LSL (register)
	for(uint32_t sreg = 0; sreg < 8; sreg++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---1 ++10 1---") | sreg << 9 | dreg << 0;
			DataRegister from { sreg };
			DataRegister with { dreg };
			bind(opcode | 0 << 6, LSL<Byte>, from, with);
			bind(opcode | 1 << 6, LSL<Word>, from, with);
			bind(opcode | 2 << 6, LSL<Long>, from, with);
		}
	}

	//LSL (effective address)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("1110 0011 11-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, LSL, with);
		}
	}

	//LSR (immediate)
	for(uint32_t immediate = 0; immediate < 8; immediate++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---0 ++00 1---") | immediate << 9 | dreg << 0;
			uint8_t count = immediate ? immediate : 8;
			DataRegister with { dreg };
			bind(opcode | 0 << 6, LSR<Byte>, count, with);
			bind(opcode | 1 << 6, LSR<Word>, count, with);
			bind(opcode | 2 << 6, LSR<Long>, count, with);
		}
	}

	//LSR (register)
	for(uint32_t sreg = 0; sreg < 8; sreg++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---0 ++10 1---") | sreg << 9 | dreg << 0;
			DataRegister from { sreg };
			DataRegister with { dreg };
			bind(opcode | 0 << 6, LSR<Byte>, from, with);
			bind(opcode | 1 << 6, LSR<Word>, from, with);
			bind(opcode | 2 << 6, LSR<Long>, from, with);
		}
	}

	//LSR (effective address)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("1110 0010 11-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, LSR, with);
		}
	}

	//MOVE
	//MOVE.B from An and MOVE.B to An are both illegal on real 68000.
	//There is no MOVEA.B instruction — only MOVEA.W and MOVEA.L exist.
	for(uint32_t toReg = 0; toReg < 8; toReg++) {
		for(uint32_t toMode = 0; toMode < 8; toMode++) {
			for(uint32_t fromMode = 0; fromMode < 8; fromMode++) {
				for(uint32_t fromReg = 0; fromReg < 8; fromReg++) {
					auto opcode = Pattern("00++ ---- ---- ----") | toReg << 9 | toMode << 6 | fromMode << 3 | fromReg << 0;
					if(toMode == 7 && toReg >= 2) {
						continue;
					}
					if(fromMode == 7 && fromReg >= 5) {
						continue;
					}
					EffectiveAddress to { toMode, toReg };
					EffectiveAddress from { fromMode, fromReg };
					bind(opcode | 1 << 12, MOVE<Byte>, from, to);
					bind(opcode | 3 << 12, MOVE<Word>, from, to);
					bind(opcode | 2 << 12, MOVE<Long>, from, to);
					if(fromMode == 1) {
						_instructionTable[opcode | 1 << 12] = [this, opcode] { instructionILLEGAL(opcode); };
					}
					if(toMode == 1) {
						_instructionTable[opcode | 1 << 12] = [this, opcode] { instructionILLEGAL(opcode); };
					}
				}
			}
		}
	}

	//MOVEA
	for(uint32_t areg = 0; areg < 8; areg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("00++ ---0 01-- ----") | areg << 9 | mode << 3 | reg << 0;
				if(mode == 7 && reg >= 5) {
					continue;
				}
				AddressRegister to { areg };
				EffectiveAddress from { mode, reg };
				bind(opcode | 3 << 12, MOVEA<Word>, from, to);
				bind(opcode | 2 << 12, MOVEA<Long>, from, to);
			}
		}
	}

	//MOVEM to memory
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 1000 1+-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || mode == 3 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress to { mode, reg };
			bind(opcode | 0 << 6, MOVEM_TO_MEM<Word>, to);
			bind(opcode | 1 << 6, MOVEM_TO_MEM<Long>, to);
		}
	}

	//MOVEM to register
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 1100 1+-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || mode == 4 || (mode == 7 && reg >= 4)) {
				continue;
			}
			EffectiveAddress from { mode, reg };
			bind(opcode | 0 << 6, MOVEM_TO_REG<Word>, from);
			bind(opcode | 1 << 6, MOVEM_TO_REG<Long>, from);
		}
	}

	//MOVEP (Dn -> mem)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t areg = 0; areg < 8; areg++) {
			auto opcode = Pattern("0000 ---1 1+00 1---") | dreg << 9 | areg << 0;
			DataRegister from { dreg };
			EffectiveAddress to { AddressRegisterIndirectWithDisplacement, areg };
			bind(opcode | 0 << 6, MOVEP<Word>, from, to);
			bind(opcode | 1 << 6, MOVEP<Long>, from, to);
		}
	}

	//MOVEP (mem -> Dn)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t areg = 0; areg < 8; areg++) {
			auto opcode = Pattern("0000 ---1 0+00 1---") | dreg << 9 | areg << 0;
			DataRegister to { dreg };
			EffectiveAddress from { AddressRegisterIndirectWithDisplacement, areg };
			bind(opcode | 0 << 6, MOVEP<Word>, from, to);
			bind(opcode | 1 << 6, MOVEP<Long>, from, to);
		}
	}

	//MOVEQ
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t immediate = 0; immediate < 256; immediate++) {
			auto opcode = Pattern("0111 ---0 ---- ----") | dreg << 9 | immediate << 0;
			DataRegister to { dreg };
			bind(opcode, MOVEQ, immediate, to);
		}
	}

	//MOVE_FROM_SR
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 0000 11-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress to { mode, reg };
			bind(opcode, MOVE_FROM_SR, to);
		}
	}

	//MOVE_TO_CCR
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 0100 11-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 5)) {
				continue;
			}
			EffectiveAddress from { mode, reg };
			bind(opcode, MOVE_TO_CCR, from);
		}
	}

	//MOVE_TO_SR
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 0110 11-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 5)) {
				continue;
			}
			EffectiveAddress from { mode, reg };
			bind(opcode, MOVE_TO_SR, from);
		}
	}

	//MOVE_FROM_USP
	for(uint32_t areg = 0; areg < 8; areg++) {
		auto opcode = Pattern("0100 1110 0110 1---") | areg << 0;
		AddressRegister to { areg };
		bind(opcode, MOVE_FROM_USP, to);
	}

	//MOVE_TO_USP
	for(uint32_t areg = 0; areg < 8; areg++) {
		auto opcode = Pattern("0100 1110 0110 0---") | areg << 0;
		AddressRegister from { areg };
		bind(opcode, MOVE_TO_USP, from);
	}

	//MULS
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1100 ---1 11-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 5)) {
					continue;
				}
				DataRegister with { dreg };
				EffectiveAddress from { mode, reg };
				bind(opcode, MULS, from, with);
			}
		}
	}

	//MULU
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1100 ---0 11-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 5)) {
					continue;
				}
				DataRegister with { dreg };
				EffectiveAddress from { mode, reg };
				bind(opcode, MULU, from, with);
			}
		}
	}

	//NBCD
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 1000 00-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, NBCD, with);
		}
	}

	//NEG
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 0100 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, NEG<Byte>, with);
			bind(opcode | 1 << 6, NEG<Word>, with);
			bind(opcode | 2 << 6, NEG<Long>, with);
		}
	}

	//NEGX
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 0000 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, NEGX<Byte>, with);
			bind(opcode | 1 << 6, NEGX<Word>, with);
			bind(opcode | 2 << 6, NEGX<Long>, with);
		}
	}

	//NOP
	bind(Pattern("0100 1110 0111 0001"), NOP);

	//NOT
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 0110 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, NOT<Byte>, with);
			bind(opcode | 1 << 6, NOT<Word>, with);
			bind(opcode | 2 << 6, NOT<Long>, with);
		}
	}

	//OR (ea -> Dn)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1000 ---0 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 5)) {
					continue;
				}
				EffectiveAddress from { mode, reg };
				DataRegister with { dreg };
				bind(opcode | 0 << 6, OR<Byte>, from, with);
				bind(opcode | 1 << 6, OR<Word>, from, with);
				bind(opcode | 2 << 6, OR<Long>, from, with);
			}
		}
	}

	//OR (Dn -> ea)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1000 ---1 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode <= 1 || (mode == 7 && reg >= 2)) {
					continue;
				}
				DataRegister from { dreg };
				EffectiveAddress with { mode, reg };
				bind(opcode | 0 << 6, OR<Byte>, from, with);
				bind(opcode | 1 << 6, OR<Word>, from, with);
				bind(opcode | 2 << 6, OR<Long>, from, with);
			}
		}
	}

	//ORI
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 0000 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, ORI<Byte>, with);
			bind(opcode | 1 << 6, ORI<Word>, with);
			bind(opcode | 2 << 6, ORI<Long>, with);
		}
	}

	//ORI_TO_CCR
	bind(Pattern("0000 0000 0011 1100"), ORI_TO_CCR);

	//ORI_TO_SR
	bind(Pattern("0000 0000 0111 1100"), ORI_TO_SR);

	//PEA
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 1000 01-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || mode == 3 || mode == 4 || (mode == 7 && reg >= 4)) {
				continue;
			}
			EffectiveAddress from { mode, reg };
			bind(opcode, PEA, from);
		}
	}

	//RESET
	bind(Pattern("0100 1110 0111 0000"), RESET);

	//ROL (immediate)
	for(uint32_t immediate = 0; immediate < 8; immediate++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---1 ++01 1---") | immediate << 9 | dreg << 0;
			uint8_t count = immediate ? immediate : 8;
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ROL<Byte>, count, with);
			bind(opcode | 1 << 6, ROL<Word>, count, with);
			bind(opcode | 2 << 6, ROL<Long>, count, with);
		}
	}

	//ROL (register)
	for(uint32_t sreg = 0; sreg < 8; sreg++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---1 ++11 1---") | sreg << 9 | dreg << 0;
			DataRegister from { sreg };
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ROL<Byte>, from, with);
			bind(opcode | 1 << 6, ROL<Word>, from, with);
			bind(opcode | 2 << 6, ROL<Long>, from, with);
		}
	}

	//ROL (effective address)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("1110 0111 11-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, ROL, with);
		}
	}

	//ROR (immediate)
	for(uint32_t immediate = 0; immediate < 8; immediate++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---0 ++01 1---") | immediate << 9 | dreg << 0;
			uint8_t count = immediate ? immediate : 8;
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ROR<Byte>, count, with);
			bind(opcode | 1 << 6, ROR<Word>, count, with);
			bind(opcode | 2 << 6, ROR<Long>, count, with);
		}
	}

	//ROR (register)
	for(uint32_t sreg = 0; sreg < 8; sreg++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---0 ++11 1---") | sreg << 9 | dreg << 0;
			DataRegister from { sreg };
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ROR<Byte>, from, with);
			bind(opcode | 1 << 6, ROR<Word>, from, with);
			bind(opcode | 2 << 6, ROR<Long>, from, with);
		}
	}

	//ROR (effective address)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("1110 0110 11-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, ROR, with);
		}
	}

	//ROXL (immediate)
	for(uint32_t immediate = 0; immediate < 8; immediate++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---1 ++01 0---") | immediate << 9 | dreg << 0;
			uint8_t count = immediate ? immediate : 8;
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ROXL<Byte>, count, with);
			bind(opcode | 1 << 6, ROXL<Word>, count, with);
			bind(opcode | 2 << 6, ROXL<Long>, count, with);
		}
	}

	//ROXL (register)
	for(uint32_t sreg = 0; sreg < 8; sreg++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---1 ++11 0---") | sreg << 9 | dreg << 0;
			DataRegister from { sreg };
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ROXL<Byte>, from, with);
			bind(opcode | 1 << 6, ROXL<Word>, from, with);
			bind(opcode | 2 << 6, ROXL<Long>, from, with);
		}
	}

	//ROXL (effective address)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("1110 0101 11-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, ROXL, with);
		}
	}

	//ROXR (immediate)
	for(uint32_t immediate = 0; immediate < 8; immediate++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---0 ++01 0---") | immediate << 9 | dreg << 0;
			uint8_t count = immediate ? immediate : 8;
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ROXR<Byte>, count, with);
			bind(opcode | 1 << 6, ROXR<Word>, count, with);
			bind(opcode | 2 << 6, ROXR<Long>, count, with);
		}
	}

	//ROXR (register)
	for(uint32_t sreg = 0; sreg < 8; sreg++) {
		for(uint32_t dreg = 0; dreg < 8; dreg++) {
			auto opcode = Pattern("1110 ---0 ++11 0---") | sreg << 9 | dreg << 0;
			DataRegister from { sreg };
			DataRegister with { dreg };
			bind(opcode | 0 << 6, ROXR<Byte>, from, with);
			bind(opcode | 1 << 6, ROXR<Word>, from, with);
			bind(opcode | 2 << 6, ROXR<Long>, from, with);
		}
	}

	//ROXR (effective address)
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("1110 0100 11-- ----") | mode << 3 | reg << 0;
			if(mode <= 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, ROXR, with);
		}
	}

	//RTE
	bind(Pattern("0100 1110 0111 0011"), RTE);

	//RTR
	bind(Pattern("0100 1110 0111 0111"), RTR);

	//RTS
	bind(Pattern("0100 1110 0111 0101"), RTS);

	//SBCD
	for(uint32_t treg = 0; treg < 8; treg++) {
		for(uint32_t sreg = 0; sreg < 8; sreg++) {
			auto opcode = Pattern("1000 ---1 0000 ----") | treg << 9 | sreg << 0;
			EffectiveAddress dataWith { DataRegisterDirect, treg };
			EffectiveAddress dataFrom { DataRegisterDirect, sreg };
			bind(opcode | 0 << 3, SBCD, dataFrom, dataWith);
			EffectiveAddress addressWith { AddressRegisterIndirectWithPreDecrement, treg };
			EffectiveAddress addressFrom { AddressRegisterIndirectWithPreDecrement, sreg };
			bind(opcode | 1 << 3, SBCD, addressFrom, addressWith);
		}
	}

	//SCC
	for(uint32_t test = 0; test < 16; test++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("0101 ---- 11-- ----") | test << 8 | mode << 3 | reg << 0;
				if(mode == 1 || (mode == 7 && reg >= 2)) {
					continue;
				}
				EffectiveAddress to { mode, reg };
				bind(opcode, SCC, test, to);
			}
		}
	}

	//STOP
	bind(Pattern("0100 1110 0111 0010"), STOP);

	//SUB (ea -> Dn)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1001 ---0 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode == 7 && reg >= 5) {
					continue;
				}
				EffectiveAddress from { mode, reg };
				DataRegister to { dreg };
				bind(opcode | 0 << 6, SUB<Byte>, from, to);
				bind(opcode | 1 << 6, SUB<Word>, from, to);
				bind(opcode | 2 << 6, SUB<Long>, from, to);
				if(mode == 1) {
					_instructionTable[opcode | 0 << 6] = [this, opcode] { instructionILLEGAL(opcode); };
				}
			}
		}
	}

	//SUB (Dn -> ea)
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1001 ---1 ++-- ----") | dreg << 9 | mode << 3 | reg << 0;
				if(mode <= 1 || (mode == 7 && reg >= 2)) {
					continue;
				}
				DataRegister from { dreg };
				EffectiveAddress to { mode, reg };
				bind(opcode | 0 << 6, SUB<Byte>, from, to);
				bind(opcode | 1 << 6, SUB<Word>, from, to);
				bind(opcode | 2 << 6, SUB<Long>, from, to);
			}
		}
	}

	//SUBA
	for(uint32_t areg = 0; areg < 8; areg++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("1001 ---+ 11-- ----") | areg << 9 | mode << 3 | reg << 0;
				if(mode == 7 && reg >= 5) {
					continue;
				}
				AddressRegister to { areg };
				EffectiveAddress from { mode, reg };
				bind(opcode | 0 << 8, SUBA<Word>, from, to);
				bind(opcode | 1 << 8, SUBA<Long>, from, to);
			}
		}
	}

	//SUBI
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0000 0100 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode | 0 << 6, SUBI<Byte>, with);
			bind(opcode | 1 << 6, SUBI<Word>, with);
			bind(opcode | 2 << 6, SUBI<Long>, with);
		}
	}

	//SUBQ
	for(uint32_t data = 0; data < 8; data++) {
		for(uint32_t mode = 0; mode < 8; mode++) {
			for(uint32_t reg = 0; reg < 8; reg++) {
				auto opcode = Pattern("0101 ---1 ++-- ----") | data << 9 | mode << 3 | reg << 0;
				if(mode == 7 && reg >= 2) {
					continue;
				}
				uint8_t immediate = data ? data : 8;
				if(mode != 1) {
					EffectiveAddress with { mode, reg };
					bind(opcode | 0 << 6, SUBQ<Byte>, immediate, with);
					bind(opcode | 1 << 6, SUBQ<Word>, immediate, with);
					bind(opcode | 2 << 6, SUBQ<Long>, immediate, with);
				} else {
					AddressRegister with { reg };
					bind(opcode | 1 << 6, SUBQ<Word>, immediate, with);
					bind(opcode | 2 << 6, SUBQ<Long>, immediate, with);
				}
			}
		}
	}

	//SUBX
	for(uint32_t treg = 0; treg < 8; treg++) {
		for(uint32_t sreg = 0; sreg < 8; sreg++) {
			auto opcode = Pattern("1001 ---1 ++00 ----") | treg << 9 | sreg << 0;
			EffectiveAddress dataWith { DataRegisterDirect, treg };
			EffectiveAddress dataFrom { DataRegisterDirect, sreg };
			bind(opcode | 0 << 6 | 0 << 3, SUBX<Byte>, dataFrom, dataWith);
			bind(opcode | 1 << 6 | 0 << 3, SUBX<Word>, dataFrom, dataWith);
			bind(opcode | 2 << 6 | 0 << 3, SUBX<Long>, dataFrom, dataWith);
			EffectiveAddress addressWith { AddressRegisterIndirectWithPreDecrement, treg };
			EffectiveAddress addressFrom { AddressRegisterIndirectWithPreDecrement, sreg };
			bind(opcode | 0 << 6 | 1 << 3, SUBX<Byte>, addressFrom, addressWith);
			bind(opcode | 1 << 6 | 1 << 3, SUBX<Word>, addressFrom, addressWith);
			bind(opcode | 2 << 6 | 1 << 3, SUBX<Long>, addressFrom, addressWith);
		}
	}

	//SWAP
	for(uint32_t dreg = 0; dreg < 8; dreg++) {
		auto opcode = Pattern("0100 1000 0100 0---") | dreg << 0;
		DataRegister with { dreg };
		bind(opcode, SWAP, with);
	}

	//TAS
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 1010 11-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress with { mode, reg };
			bind(opcode, TAS, with);
		}
	}

	//TRAP
	for(uint32_t vector = 0; vector < 16; vector++) {
		auto opcode = Pattern("0100 1110 0100 ----") | vector << 0;
		bind(opcode, TRAP, vector);
	}

	//TRAPV
	bind(Pattern("0100 1110 0111 0110"), TRAPV);

	//TST
	for(uint32_t mode = 0; mode < 8; mode++) {
		for(uint32_t reg = 0; reg < 8; reg++) {
			auto opcode = Pattern("0100 1010 ++-- ----") | mode << 3 | reg << 0;
			if(mode == 1 || (mode == 7 && reg >= 2)) {
				continue;
			}
			EffectiveAddress from { mode, reg };
			bind(opcode | 0 << 6, TST<Byte>, from);
			bind(opcode | 1 << 6, TST<Word>, from);
			bind(opcode | 2 << 6, TST<Long>, from);
		}
	}

	//UNLK
	for(uint32_t areg = 0; areg < 8; areg++) {
		auto opcode = Pattern("0100 1110 0101 1---") | areg << 0;
		AddressRegister with { areg };
		bind(opcode, UNLK, with);
	}

#undef bind
}

// ============================================================================
// Explicit template instantiations
// ============================================================================

//registers.cpp
template uint32_t GenesisM68K::Read<GenesisM68K::Byte>(DataRegister);
template uint32_t GenesisM68K::Read<GenesisM68K::Word>(DataRegister);
template uint32_t GenesisM68K::Read<GenesisM68K::Long>(DataRegister);
template void GenesisM68K::Write<GenesisM68K::Byte>(DataRegister, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Word>(DataRegister, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Long>(DataRegister, uint32_t);
template uint32_t GenesisM68K::Read<GenesisM68K::Byte>(AddressRegister);
template uint32_t GenesisM68K::Read<GenesisM68K::Word>(AddressRegister);
template uint32_t GenesisM68K::Read<GenesisM68K::Long>(AddressRegister);
template void GenesisM68K::Write<GenesisM68K::Byte>(AddressRegister, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Word>(AddressRegister, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Long>(AddressRegister, uint32_t);

//memory.cpp
template uint32_t GenesisM68K::Read<GenesisM68K::Byte>(uint32_t);
template uint32_t GenesisM68K::Read<GenesisM68K::Word>(uint32_t);
template uint32_t GenesisM68K::Read<GenesisM68K::Long>(uint32_t);
template void GenesisM68K::Write<GenesisM68K::Byte, false>(uint32_t, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Word, false>(uint32_t, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Long, false>(uint32_t, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Byte, true>(uint32_t, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Word, true>(uint32_t, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Long, true>(uint32_t, uint32_t);
template uint32_t GenesisM68K::Extension<GenesisM68K::Byte>();
template uint32_t GenesisM68K::Extension<GenesisM68K::Word>();
template uint32_t GenesisM68K::Extension<GenesisM68K::Long>();
template uint32_t GenesisM68K::Pop<GenesisM68K::Byte>();
template uint32_t GenesisM68K::Pop<GenesisM68K::Word>();
template uint32_t GenesisM68K::Pop<GenesisM68K::Long>();
template void GenesisM68K::Push<GenesisM68K::Byte>(uint32_t);
template void GenesisM68K::Push<GenesisM68K::Word>(uint32_t);
template void GenesisM68K::Push<GenesisM68K::Long>(uint32_t);

//effective-address.cpp
template uint32_t GenesisM68K::Fetch<GenesisM68K::Byte>(EffectiveAddress&);
template uint32_t GenesisM68K::Fetch<GenesisM68K::Word>(EffectiveAddress&);
template uint32_t GenesisM68K::Fetch<GenesisM68K::Long>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Byte, 0, 0>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Word, 0, 0>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Long, 0, 0>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Byte, 1, 0>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Word, 1, 0>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Long, 1, 0>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Byte, 0, 1>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Word, 0, 1>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Long, 0, 1>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Byte, 1, 1>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Word, 1, 1>(EffectiveAddress&);
template uint32_t GenesisM68K::Read<GenesisM68K::Long, 1, 1>(EffectiveAddress&);
template void GenesisM68K::Write<GenesisM68K::Byte, 0>(EffectiveAddress&, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Word, 0>(EffectiveAddress&, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Long, 0>(EffectiveAddress&, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Byte, 1>(EffectiveAddress&, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Word, 1>(EffectiveAddress&, uint32_t);
template void GenesisM68K::Write<GenesisM68K::Long, 1>(EffectiveAddress&, uint32_t);

//algorithms.cpp
template uint32_t GenesisM68K::ADD<GenesisM68K::Byte, false>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ADD<GenesisM68K::Word, false>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ADD<GenesisM68K::Long, false>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ADD<GenesisM68K::Byte, true>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ADD<GenesisM68K::Word, true>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ADD<GenesisM68K::Long, true>(uint32_t, uint32_t);
template uint32_t GenesisM68K::AND<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::AND<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::AND<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ASL<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ASL<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ASL<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ASR<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ASR<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ASR<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::CMP<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::CMP<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::CMP<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::EOR<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::EOR<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::EOR<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::LSL<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::LSL<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::LSL<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::LSR<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::LSR<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::LSR<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::OR<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::OR<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::OR<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROL<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROL<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROL<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROR<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROR<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROR<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROXL<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROXL<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROXL<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROXR<GenesisM68K::Byte>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROXR<GenesisM68K::Word>(uint32_t, uint32_t);
template uint32_t GenesisM68K::ROXR<GenesisM68K::Long>(uint32_t, uint32_t);
template uint32_t GenesisM68K::SUB<GenesisM68K::Byte, false>(uint32_t, uint32_t);
template uint32_t GenesisM68K::SUB<GenesisM68K::Word, false>(uint32_t, uint32_t);
template uint32_t GenesisM68K::SUB<GenesisM68K::Long, false>(uint32_t, uint32_t);
template uint32_t GenesisM68K::SUB<GenesisM68K::Byte, true>(uint32_t, uint32_t);
template uint32_t GenesisM68K::SUB<GenesisM68K::Word, true>(uint32_t, uint32_t);
template uint32_t GenesisM68K::SUB<GenesisM68K::Long, true>(uint32_t, uint32_t);

//instructions.cpp (size-templated)
template void GenesisM68K::instructionADD<GenesisM68K::Byte>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionADD<GenesisM68K::Word>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionADD<GenesisM68K::Long>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionADD<GenesisM68K::Byte>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionADD<GenesisM68K::Word>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionADD<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionADDA<GenesisM68K::Word>(EffectiveAddress, AddressRegister);
template void GenesisM68K::instructionADDA<GenesisM68K::Long>(EffectiveAddress, AddressRegister);
template void GenesisM68K::instructionADDI<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionADDI<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionADDI<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionADDQ<GenesisM68K::Byte>(uint8_t, EffectiveAddress);
template void GenesisM68K::instructionADDQ<GenesisM68K::Word>(uint8_t, EffectiveAddress);
template void GenesisM68K::instructionADDQ<GenesisM68K::Long>(uint8_t, EffectiveAddress);
template void GenesisM68K::instructionADDQ<GenesisM68K::Word>(uint8_t, AddressRegister);
template void GenesisM68K::instructionADDQ<GenesisM68K::Long>(uint8_t, AddressRegister);
template void GenesisM68K::instructionADDX<GenesisM68K::Byte>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionADDX<GenesisM68K::Word>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionADDX<GenesisM68K::Long>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionAND<GenesisM68K::Byte>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionAND<GenesisM68K::Word>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionAND<GenesisM68K::Long>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionAND<GenesisM68K::Byte>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionAND<GenesisM68K::Word>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionAND<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionANDI<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionANDI<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionANDI<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionASL<GenesisM68K::Byte>(uint8_t, DataRegister);
template void GenesisM68K::instructionASL<GenesisM68K::Word>(uint8_t, DataRegister);
template void GenesisM68K::instructionASL<GenesisM68K::Long>(uint8_t, DataRegister);
template void GenesisM68K::instructionASL<GenesisM68K::Byte>(DataRegister, DataRegister);
template void GenesisM68K::instructionASL<GenesisM68K::Word>(DataRegister, DataRegister);
template void GenesisM68K::instructionASL<GenesisM68K::Long>(DataRegister, DataRegister);
template void GenesisM68K::instructionASR<GenesisM68K::Byte>(uint8_t, DataRegister);
template void GenesisM68K::instructionASR<GenesisM68K::Word>(uint8_t, DataRegister);
template void GenesisM68K::instructionASR<GenesisM68K::Long>(uint8_t, DataRegister);
template void GenesisM68K::instructionASR<GenesisM68K::Byte>(DataRegister, DataRegister);
template void GenesisM68K::instructionASR<GenesisM68K::Word>(DataRegister, DataRegister);
template void GenesisM68K::instructionASR<GenesisM68K::Long>(DataRegister, DataRegister);
template void GenesisM68K::instructionBCHG<GenesisM68K::Byte>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionBCHG<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionBCHG<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionBCHG<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionBCLR<GenesisM68K::Byte>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionBCLR<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionBCLR<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionBCLR<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionBSET<GenesisM68K::Byte>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionBSET<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionBSET<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionBSET<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionBTST<GenesisM68K::Byte>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionBTST<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionBTST<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionBTST<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionCLR<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionCLR<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionCLR<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionCMP<GenesisM68K::Byte>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionCMP<GenesisM68K::Word>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionCMP<GenesisM68K::Long>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionCMPA<GenesisM68K::Word>(EffectiveAddress, AddressRegister);
template void GenesisM68K::instructionCMPA<GenesisM68K::Long>(EffectiveAddress, AddressRegister);
template void GenesisM68K::instructionCMPI<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionCMPI<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionCMPI<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionCMPM<GenesisM68K::Byte>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionCMPM<GenesisM68K::Word>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionCMPM<GenesisM68K::Long>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionEOR<GenesisM68K::Byte>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionEOR<GenesisM68K::Word>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionEOR<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionEORI<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionEORI<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionEORI<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionLSL<GenesisM68K::Byte>(uint8_t, DataRegister);
template void GenesisM68K::instructionLSL<GenesisM68K::Word>(uint8_t, DataRegister);
template void GenesisM68K::instructionLSL<GenesisM68K::Long>(uint8_t, DataRegister);
template void GenesisM68K::instructionLSL<GenesisM68K::Byte>(DataRegister, DataRegister);
template void GenesisM68K::instructionLSL<GenesisM68K::Word>(DataRegister, DataRegister);
template void GenesisM68K::instructionLSL<GenesisM68K::Long>(DataRegister, DataRegister);
template void GenesisM68K::instructionLSR<GenesisM68K::Byte>(uint8_t, DataRegister);
template void GenesisM68K::instructionLSR<GenesisM68K::Word>(uint8_t, DataRegister);
template void GenesisM68K::instructionLSR<GenesisM68K::Long>(uint8_t, DataRegister);
template void GenesisM68K::instructionLSR<GenesisM68K::Byte>(DataRegister, DataRegister);
template void GenesisM68K::instructionLSR<GenesisM68K::Word>(DataRegister, DataRegister);
template void GenesisM68K::instructionLSR<GenesisM68K::Long>(DataRegister, DataRegister);
template void GenesisM68K::instructionMOVE<GenesisM68K::Byte>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionMOVE<GenesisM68K::Word>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionMOVE<GenesisM68K::Long>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionMOVEA<GenesisM68K::Word>(EffectiveAddress, AddressRegister);
template void GenesisM68K::instructionMOVEA<GenesisM68K::Long>(EffectiveAddress, AddressRegister);
template void GenesisM68K::instructionMOVEM_TO_MEM<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionMOVEM_TO_MEM<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionMOVEM_TO_REG<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionMOVEM_TO_REG<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionMOVEP<GenesisM68K::Word>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionMOVEP<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionMOVEP<GenesisM68K::Word>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionMOVEP<GenesisM68K::Long>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionNEG<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionNEG<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionNEG<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionNEGX<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionNEGX<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionNEGX<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionNOT<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionNOT<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionNOT<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionOR<GenesisM68K::Byte>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionOR<GenesisM68K::Word>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionOR<GenesisM68K::Long>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionOR<GenesisM68K::Byte>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionOR<GenesisM68K::Word>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionOR<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionORI<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionORI<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionORI<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionROL<GenesisM68K::Byte>(uint8_t, DataRegister);
template void GenesisM68K::instructionROL<GenesisM68K::Word>(uint8_t, DataRegister);
template void GenesisM68K::instructionROL<GenesisM68K::Long>(uint8_t, DataRegister);
template void GenesisM68K::instructionROL<GenesisM68K::Byte>(DataRegister, DataRegister);
template void GenesisM68K::instructionROL<GenesisM68K::Word>(DataRegister, DataRegister);
template void GenesisM68K::instructionROL<GenesisM68K::Long>(DataRegister, DataRegister);
template void GenesisM68K::instructionROR<GenesisM68K::Byte>(uint8_t, DataRegister);
template void GenesisM68K::instructionROR<GenesisM68K::Word>(uint8_t, DataRegister);
template void GenesisM68K::instructionROR<GenesisM68K::Long>(uint8_t, DataRegister);
template void GenesisM68K::instructionROR<GenesisM68K::Byte>(DataRegister, DataRegister);
template void GenesisM68K::instructionROR<GenesisM68K::Word>(DataRegister, DataRegister);
template void GenesisM68K::instructionROR<GenesisM68K::Long>(DataRegister, DataRegister);
template void GenesisM68K::instructionROXL<GenesisM68K::Byte>(uint8_t, DataRegister);
template void GenesisM68K::instructionROXL<GenesisM68K::Word>(uint8_t, DataRegister);
template void GenesisM68K::instructionROXL<GenesisM68K::Long>(uint8_t, DataRegister);
template void GenesisM68K::instructionROXL<GenesisM68K::Byte>(DataRegister, DataRegister);
template void GenesisM68K::instructionROXL<GenesisM68K::Word>(DataRegister, DataRegister);
template void GenesisM68K::instructionROXL<GenesisM68K::Long>(DataRegister, DataRegister);
template void GenesisM68K::instructionROXR<GenesisM68K::Byte>(uint8_t, DataRegister);
template void GenesisM68K::instructionROXR<GenesisM68K::Word>(uint8_t, DataRegister);
template void GenesisM68K::instructionROXR<GenesisM68K::Long>(uint8_t, DataRegister);
template void GenesisM68K::instructionROXR<GenesisM68K::Byte>(DataRegister, DataRegister);
template void GenesisM68K::instructionROXR<GenesisM68K::Word>(DataRegister, DataRegister);
template void GenesisM68K::instructionROXR<GenesisM68K::Long>(DataRegister, DataRegister);
template void GenesisM68K::instructionSUB<GenesisM68K::Byte>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionSUB<GenesisM68K::Word>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionSUB<GenesisM68K::Long>(EffectiveAddress, DataRegister);
template void GenesisM68K::instructionSUB<GenesisM68K::Byte>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionSUB<GenesisM68K::Word>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionSUB<GenesisM68K::Long>(DataRegister, EffectiveAddress);
template void GenesisM68K::instructionSUBA<GenesisM68K::Word>(EffectiveAddress, AddressRegister);
template void GenesisM68K::instructionSUBA<GenesisM68K::Long>(EffectiveAddress, AddressRegister);
template void GenesisM68K::instructionSUBI<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionSUBI<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionSUBI<GenesisM68K::Long>(EffectiveAddress);
template void GenesisM68K::instructionSUBQ<GenesisM68K::Byte>(uint8_t, EffectiveAddress);
template void GenesisM68K::instructionSUBQ<GenesisM68K::Word>(uint8_t, EffectiveAddress);
template void GenesisM68K::instructionSUBQ<GenesisM68K::Long>(uint8_t, EffectiveAddress);
template void GenesisM68K::instructionSUBQ<GenesisM68K::Word>(uint8_t, AddressRegister);
template void GenesisM68K::instructionSUBQ<GenesisM68K::Long>(uint8_t, AddressRegister);
template void GenesisM68K::instructionSUBX<GenesisM68K::Byte>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionSUBX<GenesisM68K::Word>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionSUBX<GenesisM68K::Long>(EffectiveAddress, EffectiveAddress);
template void GenesisM68K::instructionTST<GenesisM68K::Byte>(EffectiveAddress);
template void GenesisM68K::instructionTST<GenesisM68K::Word>(EffectiveAddress);
template void GenesisM68K::instructionTST<GenesisM68K::Long>(EffectiveAddress);