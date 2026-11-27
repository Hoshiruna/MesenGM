#include "pch.h"
#include "Genesis/GenesisZ80.h"
#include "Utilities/Serializer.h"
#include "Shared/Emulator.h"

// Genesis Z80 (APU) — native Mesen2 port.
// Algorithm ported from ares/component/processor/z80, Genesis bus mapping from
// ares/md/apu. NMOS mode only. Uses bus callbacks for memory access.

GenesisZ80::GenesisZ80()
{
	BusRead = [](uint16_t) -> uint8_t { return 0xFF; };
	BusWrite = [](uint16_t, uint8_t) {};
	BusWait = [](uint32_t) {};
}

void GenesisZ80::Power()
{
	_r = {};
	_r.a = 0xFF;
	_r.flags = 0xFF;
	_r.sp = 0xFFFF;
	_nmiLine = false;
	_intLine = false;
	_busreqLine = false;
	_busreqLatch = false;
	_resetLine = false;
	_nmiEdge = false;
}

void GenesisZ80::Reset()
{
	_r.prefix = Registers::HL;
	_r.i = 0;
	_r.r = 0;
	_r.wz = 0;
	_r.pc = 0;
	_r.ei = false;
	_r.halt = false;
	_r.iff1 = false;
	_r.iff2 = false;
	_r.im = 0;
	_nmiLine = false;
	_intLine = false;
	_nmiEdge = false;
}

uint32_t GenesisZ80::ExecuteInstruction()
{
	_cycleAccum = 0;
	if(!_resetLine || _busreqLatch) {
		if(_emu) {
			_emu->ProcessHaltedCpu<CpuType::GenesisZ80>();
		}
		Wait(1);
		return _cycleAccum;
	}

	if(_nmiEdge) {
		uint16_t oldPc = _r.pc;
		_nmiEdge = false;
		_r.halt = false;
		IncrementR();
		Push(_r.pc);
		_r.wz = 0x0066;
		Wait(5);
		_r.pc = _r.wz;
		_r.iff1 = false;
		_r.p = false;
		_r.q = false;
		if(_emu) {
			_emu->ProcessInterrupt<CpuType::GenesisZ80>(oldPc, _r.pc, true);
		}
		return _cycleAccum;
	}

	if(_intLine && _r.iff1 && !_r.ei) {
		uint16_t oldPc = _r.pc;
		IncrementR();
		_r.halt = false;
		switch(_r.im) {
			case 0:
				Wait(6);
				_r.wz = 0xFF;
				Instruction(0xFF);
				break; //IM 0: 0xFF → RST 38
			case 1:
				Wait(6);
				Push(_r.pc);
				_r.wz = 0x0038;
				_r.pc = _r.wz;
				break;
			case 2: {
				uint16_t addr = (_r.i << 8) | 0xFF;
				uint8_t lo = Read(addr);
				uint8_t hi = Read(addr + 1);
				Wait(7);
				Push(_r.pc);
				_r.wz = (hi << 8) | lo;
				_r.pc = _r.wz;
				break;
			}
		}
		_r.iff1 = false;
		_r.iff2 = false;
		if(_r.p) {
			_r.flags &= ~FlagP;
		}
		_r.p = false;
		_r.q = false;
		if(_emu) {
			_emu->ProcessInterrupt<CpuType::GenesisZ80>(oldPc, _r.pc, false);
		}
		return _cycleAccum;
	}

	if(_r.halt) {
		if(_emu) {
			_emu->ProcessHaltedCpu<CpuType::GenesisZ80>();
		}
		Wait(1);
		return _cycleAccum;
	}

	if(_emu) {
		_emu->ProcessInstruction<CpuType::GenesisZ80>();
	}
	Instruction();
	return _cycleAccum;
}

void GenesisZ80::SetIrq(bool line)
{ _intLine = line; }
void GenesisZ80::SetNmi(bool line)
{
	if(!_nmiLine && line) {
		_nmiEdge = true;
	}
	_nmiLine = line;
}
void GenesisZ80::SetBusreq(bool line)
{
	_busreqLine = line;
	_busreqLatch = _busreqLine;
}
void GenesisZ80::SetReset(bool line)
{
	if(!_resetLine && line) {
		Reset();
	}
	_resetLine = line;
}

//--- Memory helpers ---

void GenesisZ80::Wait(uint32_t clocks)
{ BusWait(clocks); }
uint8_t GenesisZ80::ReadBus(uint16_t addr, MemoryOperationType type)
{
	uint8_t value = BusRead(addr);
	if(_emu) {
		_emu->ProcessMemoryRead<CpuType::GenesisZ80>(addr, value, type);
	}
	return value;
}

uint8_t GenesisZ80::Opcode(bool firstByte)
{
	Wait(4);
	uint16_t address = _r.pc++;
	return firstByte ? BusRead(address) : ReadBus(address, MemoryOperationType::ExecOperand);
}
uint8_t GenesisZ80::Operand()
{
	Wait(3);
	return ReadBus(_r.pc++, MemoryOperationType::ExecOperand);
}
uint16_t GenesisZ80::Operands()
{
	uint16_t lo = Operand();
	return lo | (Operand() << 8);
}
void GenesisZ80::Push(uint16_t v)
{
	Write(--_r.sp, v >> 8);
	Write(--_r.sp, v & 0xFF);
}
uint16_t GenesisZ80::Pop()
{
	uint16_t lo = Read(_r.sp++);
	return lo | (Read(_r.sp++) << 8);
}

uint16_t GenesisZ80::Displace(uint16_t& reg)
{
	if(&reg != &_r.ix && &reg != &_r.iy) {
		return reg;
	}
	int8_t d = (int8_t)Operand();
	Wait(5);
	_r.wz = reg + d;
	return _r.wz;
}

uint8_t GenesisZ80::Read(uint16_t addr)
{
	Wait(3);
	return ReadBus(addr, MemoryOperationType::Read);
}
void GenesisZ80::Write(uint16_t addr, uint8_t data)
{
	Wait(3);
	if(!_emu || _emu->ProcessMemoryWrite<CpuType::GenesisZ80>(addr, data, MemoryOperationType::Write)) {
		BusWrite(addr, data);
	}
}
uint8_t GenesisZ80::In(uint16_t port)
{
	Wait(4);
	return PortRead(port);
}
void GenesisZ80::Out(uint16_t port, uint8_t data)
{
	Wait(4);
	PortWrite(port, data);
}

bool GenesisZ80::Parity(uint8_t v) const
{
	v ^= v >> 4;
	v ^= v >> 2;
	v ^= v >> 1;
	return !(v & 1);
}
void GenesisZ80::IncrementR()
{ _r.r = (_r.r & 0x80) | ((_r.r + 1) & 0x7F); }

uint16_t& GenesisZ80::HL()
{
	switch(_r.prefix) {
		case Registers::IX: return _r.ix;
		case Registers::IY: return _r.iy;
		default: _trueHL = (_r.h << 8) | _r.l; return _trueHL;
	}
}

void GenesisZ80::setHL(uint16_t v)
{
	switch(_r.prefix) {
		case Registers::IX: _r.ix = v; break;
		case Registers::IY: _r.iy = v; break;
		default:
			_r.h = v >> 8;
			_r.l = v & 0xFF;
			_trueHL = v;
			break;
	}
}

uint8_t GenesisZ80::readH()
{
	switch(_r.prefix) {
		case Registers::IX: return _r.ix >> 8;
		case Registers::IY: return _r.iy >> 8;
		default: return _r.h;
	}
}

uint8_t GenesisZ80::readL()
{
	switch(_r.prefix) {
		case Registers::IX: return _r.ix & 0xFF;
		case Registers::IY: return _r.iy & 0xFF;
		default: return _r.l;
	}
}

void GenesisZ80::writeH(uint8_t v)
{
	switch(_r.prefix) {
		case Registers::IX: _r.ix = (_r.ix & 0x00FF) | ((uint16_t)v << 8); break;
		case Registers::IY: _r.iy = (_r.iy & 0x00FF) | ((uint16_t)v << 8); break;
		default: _r.h = v; break;
	}
}

void GenesisZ80::writeL(uint8_t v)
{
	switch(_r.prefix) {
		case Registers::IX: _r.ix = (_r.ix & 0xFF00) | v; break;
		case Registers::IY: _r.iy = (_r.iy & 0xFF00) | v; break;
		default: _r.l = v; break;
	}
}

uint8_t& GenesisZ80::H()
{
	static uint8_t dummy;
	switch(_r.prefix) {
		case Registers::IX: dummy = _r.ix >> 8; return dummy;
		case Registers::IY: dummy = _r.iy >> 8; return dummy;
		default: return _r.h;
	}
}

uint8_t& GenesisZ80::L()
{
	static uint8_t dummy;
	switch(_r.prefix) {
		case Registers::IX: dummy = _r.ix & 0xFF; return dummy;
		case Registers::IY: dummy = _r.iy & 0xFF; return dummy;
		default: return _r.l;
	}
}

//--- Algorithms (from ares z80/algorithms.cpp) ---

uint8_t GenesisZ80::ADD(uint8_t x, uint8_t y, bool carry)
{
	uint16_t z = x + y + (carry ? 1 : 0);
	_r.flags = 0;
	if(z & 0x100) {
		_r.flags |= FlagC;
	}
	if((~(x ^ y) & (x ^ z)) & 0x80) {
		_r.flags |= FlagP;
	}
	if(z & 0x08) {
		_r.flags |= FlagX;
	}
	if((x ^ y ^ z) & 0x10) {
		_r.flags |= FlagH;
	}
	if(z & 0x20) {
		_r.flags |= FlagY;
	}
	if((z & 0xFF) == 0) {
		_r.flags |= FlagZ;
	}
	if(z & 0x80) {
		_r.flags |= FlagS;
	}
	return (uint8_t)z;
}

uint8_t GenesisZ80::AND(uint8_t x, uint8_t y)
{
	uint8_t z = x & y;
	_r.flags = 0;
	if(Parity(z)) {
		_r.flags |= FlagP;
	}
	if(z & 0x08) {
		_r.flags |= FlagX;
	}
	_r.flags |= FlagH;
	if(z & 0x20) {
		_r.flags |= FlagY;
	}
	if(z == 0) {
		_r.flags |= FlagZ;
	}
	if(z & 0x80) {
		_r.flags |= FlagS;
	}
	return z;
}

uint8_t GenesisZ80::BIT(uint8_t bit, uint8_t x)
{
	uint8_t z = x & (1 << bit);
	_r.flags &= ~(FlagN | FlagZ | FlagP | FlagS);
	_r.flags |= FlagH;
	if(z == 0) {
		_r.flags |= FlagZ;
	}
	if(Parity(z)) {
		_r.flags |= FlagP;
	}
	if(z & 0x80) {
		_r.flags |= FlagS;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	return x;
}

void GenesisZ80::CP(uint8_t x, uint8_t y)
{
	uint16_t z = x - y;
	_r.flags = 0;
	if(z & 0x100) {
		_r.flags |= FlagC;
	}
	_r.flags |= FlagN;
	if(((x ^ y) & (x ^ z)) & 0x80) {
		_r.flags |= FlagP;
	}
	if(y & 0x08) {
		_r.flags |= FlagX;
	}
	if((x ^ y ^ z) & 0x10) {
		_r.flags |= FlagH;
	}
	if(y & 0x20) {
		_r.flags |= FlagY;
	}
	if((z & 0xFF) == 0) {
		_r.flags |= FlagZ;
	}
	if(z & 0x80) {
		_r.flags |= FlagS;
	}
}

uint8_t GenesisZ80::DEC(uint8_t x)
{
	uint8_t z = x - 1;
	_r.flags = (_r.flags & (FlagC | FlagX | FlagY)) | FlagN;
	if(z == 0x7F) {
		_r.flags |= FlagP;
	}
	if((z & 0x0F) == 0x0F) {
		_r.flags |= FlagH;
	}
	if(z == 0) {
		_r.flags |= FlagZ;
	}
	if(z & 0x80) {
		_r.flags |= FlagS;
	}
	if(z & 0x08) {
		_r.flags |= FlagX;
	}
	if(z & 0x20) {
		_r.flags |= FlagY;
	}
	return z;
}

uint8_t GenesisZ80::IN(uint8_t x)
{
	_r.flags = _r.flags & (FlagC);
	if(Parity(x)) {
		_r.flags |= FlagP;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	if(x == 0) {
		_r.flags |= FlagZ;
	}
	if(x & 0x80) {
		_r.flags |= FlagS;
	}
	return x;
}

uint8_t GenesisZ80::INC(uint8_t x)
{
	uint8_t z = x + 1;
	_r.flags = _r.flags & (FlagC | FlagX | FlagY);
	if(z == 0x80) {
		_r.flags |= FlagP;
	}
	if((z & 0x0F) == 0x00) {
		_r.flags |= FlagH;
	}
	if(z == 0) {
		_r.flags |= FlagZ;
	}
	if(z & 0x80) {
		_r.flags |= FlagS;
	}
	if(z & 0x08) {
		_r.flags |= FlagX;
	}
	if(z & 0x20) {
		_r.flags |= FlagY;
	}
	return z;
}

uint8_t GenesisZ80::OR(uint8_t x, uint8_t y)
{
	uint8_t z = x | y;
	_r.flags = 0;
	if(Parity(z)) {
		_r.flags |= FlagP;
	}
	if(z & 0x08) {
		_r.flags |= FlagX;
	}
	if(z & 0x20) {
		_r.flags |= FlagY;
	}
	if(z == 0) {
		_r.flags |= FlagZ;
	}
	if(z & 0x80) {
		_r.flags |= FlagS;
	}
	return z;
}

uint8_t GenesisZ80::RES(uint8_t bit, uint8_t x)
{ return x & ~(1 << bit); }

uint8_t GenesisZ80::RL(uint8_t x)
{
	bool c = x & 0x80;
	x = (x << 1) | ((_r.flags & FlagC) ? 1 : 0);
	_r.flags = 0;
	if(c) {
		_r.flags |= FlagC;
	}
	if(Parity(x)) {
		_r.flags |= FlagP;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	if(x == 0) {
		_r.flags |= FlagZ;
	}
	if(x & 0x80) {
		_r.flags |= FlagS;
	}
	return x;
}

uint8_t GenesisZ80::RLC(uint8_t x)
{
	x = (x << 1) | (x >> 7);
	_r.flags = 0;
	if(x & 1) {
		_r.flags |= FlagC;
	}
	if(Parity(x)) {
		_r.flags |= FlagP;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	if(x == 0) {
		_r.flags |= FlagZ;
	}
	if(x & 0x80) {
		_r.flags |= FlagS;
	}
	return x;
}

uint8_t GenesisZ80::RR(uint8_t x)
{
	bool c = x & 1;
	x = (x >> 1) | ((_r.flags & FlagC) ? 0x80 : 0);
	_r.flags = 0;
	if(c) {
		_r.flags |= FlagC;
	}
	if(Parity(x)) {
		_r.flags |= FlagP;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	if(x == 0) {
		_r.flags |= FlagZ;
	}
	if(x & 0x80) {
		_r.flags |= FlagS;
	}
	return x;
}

uint8_t GenesisZ80::RRC(uint8_t x)
{
	uint8_t c = x & 1;
	x = (x >> 1) | (c << 7);
	_r.flags = 0;
	if(c) {
		_r.flags |= FlagC;
	}
	if(Parity(x)) {
		_r.flags |= FlagP;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	if(x == 0) {
		_r.flags |= FlagZ;
	}
	if(x & 0x80) {
		_r.flags |= FlagS;
	}
	return x;
}

uint8_t GenesisZ80::SET(uint8_t bit, uint8_t x)
{ return x | (1 << bit); }

uint8_t GenesisZ80::SLA(uint8_t x)
{
	bool c = x & 0x80;
	x <<= 1;
	_r.flags = 0;
	if(c) {
		_r.flags |= FlagC;
	}
	if(Parity(x)) {
		_r.flags |= FlagP;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	if(x == 0) {
		_r.flags |= FlagZ;
	}
	if(x & 0x80) {
		_r.flags |= FlagS;
	}
	return x;
}

uint8_t GenesisZ80::SLL(uint8_t x)
{
	bool c = x & 0x80;
	x = (x << 1) | 1;
	_r.flags = 0;
	if(c) {
		_r.flags |= FlagC;
	}
	if(Parity(x)) {
		_r.flags |= FlagP;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	if(x == 0) {
		_r.flags |= FlagZ;
	}
	if(x & 0x80) {
		_r.flags |= FlagS;
	}
	return x;
}

uint8_t GenesisZ80::SRA(uint8_t x)
{
	bool c = x & 1;
	x = (uint8_t)((int8_t)x >> 1);
	_r.flags = 0;
	if(c) {
		_r.flags |= FlagC;
	}
	if(Parity(x)) {
		_r.flags |= FlagP;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	if(x == 0) {
		_r.flags |= FlagZ;
	}
	if(x & 0x80) {
		_r.flags |= FlagS;
	}
	return x;
}

uint8_t GenesisZ80::SRL(uint8_t x)
{
	bool c = x & 1;
	x >>= 1;
	_r.flags = 0;
	if(c) {
		_r.flags |= FlagC;
	}
	if(Parity(x)) {
		_r.flags |= FlagP;
	}
	if(x & 0x08) {
		_r.flags |= FlagX;
	}
	if(x & 0x20) {
		_r.flags |= FlagY;
	}
	if(x == 0) {
		_r.flags |= FlagZ;
	}
	if(x & 0x80) {
		_r.flags |= FlagS;
	}
	return x;
}

uint8_t GenesisZ80::SUB(uint8_t x, uint8_t y, bool carry)
{
	uint16_t z = x - y - (carry ? 1 : 0);
	_r.flags = 0;
	if(z & 0x100) {
		_r.flags |= FlagC;
	}
	_r.flags |= FlagN;
	if(((x ^ y) & (x ^ z)) & 0x80) {
		_r.flags |= FlagP;
	}
	if(z & 0x08) {
		_r.flags |= FlagX;
	}
	if((x ^ y ^ z) & 0x10) {
		_r.flags |= FlagH;
	}
	if(z & 0x20) {
		_r.flags |= FlagY;
	}
	if((z & 0xFF) == 0) {
		_r.flags |= FlagZ;
	}
	if(z & 0x80) {
		_r.flags |= FlagS;
	}
	return (uint8_t)z;
}

uint8_t GenesisZ80::XOR(uint8_t x, uint8_t y)
{
	uint8_t z = x ^ y;
	_r.flags = 0;
	if(Parity(z)) {
		_r.flags |= FlagP;
	}
	if(z & 0x08) {
		_r.flags |= FlagX;
	}
	if(z & 0x20) {
		_r.flags |= FlagY;
	}
	if(z == 0) {
		_r.flags |= FlagZ;
	}
	if(z & 0x80) {
		_r.flags |= FlagS;
	}
	return z;
}

//--- Instruction dispatch ---

void GenesisZ80::Instruction()
{
	_r.ei = false;
	_r.p = false;
	if(_r.halt) {
		Wait(1);
		return;
	}

	uint8_t code;
	bool firstByte = true;
	while(true) {
		IncrementR();
		code = Opcode(firstByte);
		firstByte = false;
		if(code == 0xDD) {
			_r.prefix = Registers::IX;
			continue;
		}
		if(code == 0xFD) {
			_r.prefix = Registers::IY;
			continue;
		}
		break;
	}

	if(code == 0xCB && _r.prefix != Registers::HL) {
		uint16_t base = (_r.prefix == Registers::IX) ? _r.ix : _r.iy;
		_r.wz = base + (int8_t)Operand();
		uint8_t opcd = Operand();
		Wait(2);
		InstructionCBd(_r.wz, opcd);
	} else if(code == 0xCB) {
		IncrementR();
		InstructionCB(Opcode());
	} else if(code == 0xED) {
		IncrementR();
		InstructionED(Opcode());
	} else {
		Instruction(code);
	}
	_r.prefix = Registers::HL;
}

//--- Unprefixed instructions ---

//Convenience register macros (local to instruction methods)
#define A _r.a
#define F _r.flags
#define B _r.b
#define C _r.c
#define D _r.d
#define E _r.e
#define HH _r.h
#define LL _r.l
#define BC ((_r.b<<8)|_r.c)
#define DE ((_r.d<<8)|_r.e)
#define SP _r.sp
#define AF ((_r.a<<8)|_r.flags)
#define rIX _r.ix
#define rIY _r.iy

//Helpers for 16-bit register pairs with IX/IY prefix
static inline void setPair(uint8_t& hi, uint8_t& lo, uint16_t v)
{
	hi = v >> 8;
	lo = v & 0xFF;
}

void GenesisZ80::Instruction(uint8_t code)
{
	//Get HL reference (respects IX/IY prefix)
	auto& hlRef = HL();
	uint16_t hlVal = hlRef;

#define op(id, name, ...) case id: inst##name(__VA_ARGS__); break;

	switch(code) {
	op(0x00, NOP) case 0x01: {
		uint16_t v = Operands();
		setPair(B, C, v);
		break;
	} //LD BC,nn
	case 0x02:
		Write(BC, A);
		_r.wz = (BC & 0xFF) | (A << 8);
		break; //LD (BC),A
		case 0x03:
			Wait(2);
			setPair(B, C, BC + 1);
			break; //INC BC
		op(0x04, INC_r, B) op(0x05, DEC_r, B) case 0x06:
			B = Operand();
			break; //LD B,n
		case 0x07: {
			_r.q = 1;
			bool c = A & 0x80;
			A = (A << 1) | (c ? 1 : 0);
			F &= ~(FlagC | FlagN | FlagH);
			if(c) {
				F |= FlagC;
			}
			if(A & 8) {
				F |= FlagX;
			}
			if(A & 0x20) {
				F |= FlagY;
			}
		} break; //RLCA
		case 0x08: {
			uint8_t ta = _r.a, tf = _r.flags;
			_r.a = _r.a_;
			_r.flags = _r.flags_;
			_r.a_ = ta;
			_r.flags_ = tf;
		} break; //EX AF,AF'
		case 0x09: instADD_hl_rr(BC); break;
		case 0x0A:
			A = Read(BC);
			_r.wz = BC + 1;
			break; //LD A,(BC)
		case 0x0B:
			Wait(2);
			setPair(B, C, BC - 1);
			break; //DEC BC
		op(0x0C, INC_r, C) op(0x0D, DEC_r, C) case 0x0E:
			C = Operand();
			break;
		case 0x0F: {
			_r.q = 1;
			bool c = A & 1;
			A = (c << 7) | (A >> 1);
			F &= ~(FlagC | FlagN | FlagH);
			if(c) {
				F |= FlagC;
			}
			if(A & 8) {
				F |= FlagX;
			}
			if(A & 0x20) {
				F |= FlagY;
			}
		} break; //RRCA
		case 0x10: instDJNZ_e(); break;
		case 0x11: {
			uint16_t v = Operands();
			setPair(D, E, v);
			break;
		}
		case 0x12:
			Write(DE, A);
			_r.wz = (DE & 0xFF) | (A << 8);
			break;
		case 0x13:
			Wait(2);
			setPair(D, E, DE + 1);
			break;
		op(0x14, INC_r, D) op(0x15, DEC_r, D) case 0x16:
			D = Operand();
			break;
		case 0x17: {
			_r.q = 1;
			bool c = A & 0x80;
			A = (A << 1) | ((F & FlagC) ? 1 : 0);
			F &= ~(FlagC | FlagN | FlagH);
			if(c) {
				F |= FlagC;
			}
			if(A & 8) {
				F |= FlagX;
			}
			if(A & 0x20) {
				F |= FlagY;
			}
		} break; //RLA
		case 0x18: instJR_c_e(true); break; //JR e (always)
		case 0x19: instADD_hl_rr(DE); break;
		case 0x1A:
			A = Read(DE);
			_r.wz = DE + 1;
			break;
		case 0x1B:
			Wait(2);
			setPair(D, E, DE - 1);
			break;
		op(0x1C, INC_r, E) op(0x1D, DEC_r, E) case 0x1E:
			E = Operand();
			break;
		case 0x1F: {
			_r.q = 1;
			bool c = A & 1;
			A = ((F & FlagC) ? 0x80 : 0) | (A >> 1);
			F &= ~(FlagC | FlagN | FlagH);
			if(c) {
				F |= FlagC;
			}
			if(A & 8) {
				F |= FlagX;
			}
			if(A & 0x20) {
				F |= FlagY;
			}
		} break; //RRA
		case 0x20: instJR_c_e(!(F & FlagZ)); break; //JR NZ
		case 0x21: {
			uint16_t v = Operands();
			if(_r.prefix == Registers::IX) {
				_r.ix = v;
			} else if(_r.prefix == Registers::IY) {
				_r.iy = v;
			} else {
				setPair(HH, LL, v);
			}
			break;
		} //LD HL/IX/IY,nn
		case 0x22: {
			uint16_t a = Operands();
			Write(a, hlVal & 0xFF);
			Write(a + 1, hlVal >> 8);
			_r.wz = a + 1;
		} break; //LD (nn),HL
		case 0x23:
			Wait(2);
			setHL(hlVal + 1);
			break; //INC HL
		case 0x24: {
			_r.q = 1;
			writeH(INC(readH()));
		} break; //INC H/IXH/IYH
		case 0x25: {
			_r.q = 1;
			writeH(DEC(readH()));
		} break; //DEC H/IXH/IYH
		case 0x26: writeH(Operand()); break; //LD H/IXH/IYH,n
		case 0x27: instDAA(); break;
		case 0x28: instJR_c_e(F & FlagZ); break; //JR Z
		case 0x29: instADD_hl_rr(hlVal); break;
		case 0x2A: {
			uint16_t a = Operands();
			uint8_t lo = Read(a);
			uint8_t hi = Read(a + 1);
			setHL((hi << 8) | lo);
			_r.wz = a + 1;
		} break; //LD HL,(nn)
		case 0x2B:
			Wait(2);
			setHL(hlVal - 1);
			break;
		case 0x2C: {
			_r.q = 1;
			writeL(INC(readL()));
		} break; //INC L/IXL/IYL
		case 0x2D: {
			_r.q = 1;
			writeL(DEC(readL()));
		} break; //DEC L/IXL/IYL
		case 0x2E: writeL(Operand()); break; //LD L/IXL/IYL,n
		case 0x2F: instCPL(); break;
		case 0x30: instJR_c_e(!(F & FlagC)); break; //JR NC
		case 0x31: SP = Operands(); break;
		case 0x32: {
			uint16_t a = Operands();
			Write(a, A);
			_r.wz = (a & 0xFF) + 1 | (A << 8);
		} break; //LD (nn),A
		case 0x33:
			Wait(2);
			SP++;
			break;
		case 0x34: instINC_irr(hlRef); break;
		case 0x35: instDEC_irr(hlRef); break;
		case 0x36: {
			uint16_t a = Displace(hlRef);
			uint8_t d = Operand();
			if(&hlRef == &_r.ix || &hlRef == &_r.iy) {
				Wait(2);
			}
			Write(a, d);
		} break;
		case 0x37: instSCF(); break;
		case 0x38: instJR_c_e(F & FlagC); break; //JR C
		case 0x39: instADD_hl_rr(SP); break;
		case 0x3A: {
			uint16_t a = Operands();
			A = Read(a);
			_r.wz = a + 1;
		} break;
		case 0x3B:
			Wait(2);
			SP--;
			break;
		op(0x3C, INC_r, A) op(0x3D, DEC_r, A) case 0x3E:
			A = Operand();
			break;
		case 0x3F: instCCF(); break;

		//LD r,r' block (0x40-0x7F)
		case 0x76: instHALT(); break;
		default: {
			if(code >= 0x40 && code <= 0x7F) {
				//LD r,r'
				uint8_t dst = (code >> 3) & 7, src = code & 7;
				//When one operand is (HL) [src==6 or dst==6], the DD/FD prefix only
				//affects the memory address: (HL)→(IX+d)/(IY+d). The H/L register
				//operand refers to the regular H/L, NOT IXH/IXL.
				//For register-to-register ops (neither is 6), H/L→IXH/IXL is correct.
				bool memOp = (src == 6 || dst == 6);
				auto readReg = [&](uint8_t r) -> uint8_t {
					switch(r) {
						case 0: return B;
						case 1: return C;
						case 2: return D;
						case 3: return E;
						case 4: return memOp ? HH : readH();
						case 5: return memOp ? LL : readL();
						case 7: return A;
						default: return 0;
					}
				};
				auto writeReg = [&](uint8_t r, uint8_t v) {
					switch(r) {
						case 0: B = v; break;
						case 1: C = v; break;
						case 2: D = v; break;
						case 3: E = v; break;
						case 4:
							if(memOp) {
								HH = v;
							} else {
								writeH(v);
							}
							break;
						case 5:
							if(memOp) {
								LL = v;
							} else {
								writeL(v);
							}
							break;
						case 7: A = v; break;
						default: break;
					}
				};
				if(src == 6) {
					uint8_t v = Read(Displace(hlRef));
					if(dst != 6) {
						writeReg(dst, v);
					}
				} else if(dst == 6) {
					Write(Displace(hlRef), readReg(src));
				} else {
					writeReg(dst, readReg(src));
				}
			} else if(code >= 0x80 && code <= 0xBF) {
				//ALU A,r
				uint8_t op = (code >> 3) & 7, src = code & 7;
				auto readReg = [&](uint8_t r) -> uint8_t {
					switch(r) {
						case 0: return B;
						case 1: return C;
						case 2: return D;
						case 3: return E;
						case 4: return readH();
						case 5: return readL();
						case 7: return A;
						default: return 0;
					}
				};
				uint8_t val = (src == 6) ? Read(Displace(hlRef)) : readReg(src);
				_r.q = 1;
				switch(op) {
					case 0: A = ADD(A, val); break;
					case 1: A = ADD(A, val, F & FlagC); break;
					case 2: A = SUB(A, val); break;
					case 3: A = SUB(A, val, F & FlagC); break;
					case 4: A = AND(A, val); break;
					case 5: A = XOR(A, val); break;
					case 6: A = OR(A, val); break;
					case 7: CP(A, val); break;
				}
			} else if(code >= 0xC0) {
				//Control flow block
				switch(code) {
					case 0xC0: instRET_c(!(F & FlagZ)); break;
					case 0xC1: {
						uint16_t v = Pop();
						setPair(B, C, v);
					} break; //POP BC
					case 0xC2: {
						uint16_t a = Operands();
						_r.wz = a;
						if(!(F & FlagZ)) {
							_r.pc = a;
						}
					} break; //JP NZ
					case 0xC3: {
						uint16_t a = Operands();
						_r.wz = a;
						_r.pc = a;
					} break; //JP nn
					case 0xC4: {
						uint16_t a = Operands();
						_r.wz = a;
						if(!(F & FlagZ)) {
							Wait(1);
							Push(_r.pc);
							_r.pc = a;
						}
					} break; //CALL NZ
					case 0xC5:
						Wait(1);
						Push(BC);
						break; //PUSH BC
					case 0xC6:
						_r.q = 1;
						A = ADD(A, Operand());
						break;
					case 0xC7:
						Wait(1);
						Push(_r.pc);
						_r.wz = 0x00;
						_r.pc = _r.wz;
						break; //RST 00
					case 0xC8: instRET_c(F & FlagZ); break;
					case 0xC9:
						_r.wz = Pop();
						_r.pc = _r.wz;
						break; //RET
					case 0xCA: {
						uint16_t a = Operands();
						_r.wz = a;
						if(F & FlagZ) {
							_r.pc = a;
						}
					} break;
					case 0xCC: {
						uint16_t a = Operands();
						_r.wz = a;
						if(F & FlagZ) {
							Wait(1);
							Push(_r.pc);
							_r.pc = a;
						}
					} break;
					case 0xCD: {
						uint16_t a = Operands();
						_r.wz = a;
						Wait(1);
						Push(_r.pc);
						_r.pc = a;
					} break; //CALL nn
					case 0xCE:
						_r.q = 1;
						A = ADD(A, Operand(), F & FlagC);
						break;
					case 0xCF:
						Wait(1);
						Push(_r.pc);
						_r.wz = 0x08;
						_r.pc = _r.wz;
						break;
					case 0xD0: instRET_c(!(F & FlagC)); break;
					case 0xD1: {
						uint16_t v = Pop();
						setPair(D, E, v);
					} break;
					case 0xD2: {
						uint16_t a = Operands();
						_r.wz = a;
						if(!(F & FlagC)) {
							_r.pc = a;
						}
					} break;
					case 0xD3: {
						uint8_t p = Operand();
						_r.wz = (A << 8) | (p + 1);
						Out((A << 8) | p, A);
					} break; //OUT (n),A
					case 0xD4: {
						uint16_t a = Operands();
						_r.wz = a;
						if(!(F & FlagC)) {
							Wait(1);
							Push(_r.pc);
							_r.pc = a;
						}
					} break;
					case 0xD5:
						Wait(1);
						Push(DE);
						break;
					case 0xD6:
						_r.q = 1;
						A = SUB(A, Operand());
						break;
					case 0xD7:
						Wait(1);
						Push(_r.pc);
						_r.wz = 0x10;
						_r.pc = _r.wz;
						break;
					case 0xD8: instRET_c(F & FlagC); break;
					case 0xD9: {
						uint8_t tb = _r.b, tc = _r.c, td = _r.d, te = _r.e, th = _r.h, tl = _r.l;
						_r.b = _r.b_;
						_r.c = _r.c_;
						_r.d = _r.d_;
						_r.e = _r.e_;
						_r.h = _r.h_;
						_r.l = _r.l_;
						_r.b_ = tb;
						_r.c_ = tc;
						_r.d_ = td;
						_r.e_ = te;
						_r.h_ = th;
						_r.l_ = tl;
					} break; //EXX
					case 0xDA: {
						uint16_t a = Operands();
						_r.wz = a;
						if(F & FlagC) {
							_r.pc = a;
						}
					} break;
					case 0xDB: {
						uint8_t p = Operand();
						_r.wz = (A << 8) | (p + 1);
						A = In((A << 8) | p);
					} break; //IN A,(n)
					case 0xDC: {
						uint16_t a = Operands();
						_r.wz = a;
						if(F & FlagC) {
							Wait(1);
							Push(_r.pc);
							_r.pc = a;
						}
					} break;
					case 0xDE:
						_r.q = 1;
						A = SUB(A, Operand(), F & FlagC);
						break;
					case 0xDF:
						Wait(1);
						Push(_r.pc);
						_r.wz = 0x18;
						_r.pc = _r.wz;
						break;
					case 0xE0: instRET_c(!(F & FlagP)); break;
					case 0xE1: {
						uint16_t v = Pop();
						if(_r.prefix == Registers::IX) {
							_r.ix = v;
						} else if(_r.prefix == Registers::IY) {
							_r.iy = v;
						} else {
							setPair(HH, LL, v);
						}
					} break; //POP HL/IX/IY
					case 0xE2: {
						uint16_t a = Operands();
						_r.wz = a;
						if(!(F & FlagP)) {
							_r.pc = a;
						}
					} break;
					case 0xE3: { //EX (SP),HL
						uint8_t lo = Read(SP), hi = Read(SP + 1);
						Wait(1);
						uint16_t hv = hlRef;
						Write(SP, hv & 0xFF);
						Write(SP + 1, hv >> 8);
						Wait(2);
						uint16_t newHL = (hi << 8) | lo;
						setHL(newHL);
						_r.wz = newHL;
					} break;
					case 0xE4: {
						uint16_t a = Operands();
						_r.wz = a;
						if(!(F & FlagP)) {
							Wait(1);
							Push(_r.pc);
							_r.pc = a;
						}
					} break;
					case 0xE5:
						Wait(1);
						Push(hlRef);
						break; //PUSH HL/IX/IY
					case 0xE6:
						_r.q = 1;
						A = AND(A, Operand());
						break;
					case 0xE7:
						Wait(1);
						Push(_r.pc);
						_r.wz = 0x20;
						_r.pc = _r.wz;
						break;
					case 0xE8: instRET_c(F & FlagP); break;
					case 0xE9: _r.pc = hlRef; break; //JP (HL)
					case 0xEA: {
						uint16_t a = Operands();
						_r.wz = a;
						if(F & FlagP) {
							_r.pc = a;
						}
					} break;
					case 0xEB: {
						uint16_t de = DE, hl = (_r.h << 8) | _r.l;
						setPair(D, E, hl);
						setPair(HH, LL, de);
					} break; //EX DE,HL (true HL)
					case 0xEC: {
						uint16_t a = Operands();
						_r.wz = a;
						if(F & FlagP) {
							Wait(1);
							Push(_r.pc);
							_r.pc = a;
						}
					} break;
					case 0xEE:
						_r.q = 1;
						A = XOR(A, Operand());
						break;
					case 0xEF:
						Wait(1);
						Push(_r.pc);
						_r.wz = 0x28;
						_r.pc = _r.wz;
						break;
					case 0xF0: instRET_c(!(F & FlagS)); break;
					case 0xF1: {
						uint16_t v = Pop();
						setPair(A, F, v);
					} break; //POP AF
					case 0xF2: {
						uint16_t a = Operands();
						_r.wz = a;
						if(!(F & FlagS)) {
							_r.pc = a;
						}
					} break;
					case 0xF3:
						_r.iff1 = false;
						_r.iff2 = false;
						break; //DI
					case 0xF4: {
						uint16_t a = Operands();
						_r.wz = a;
						if(!(F & FlagS)) {
							Wait(1);
							Push(_r.pc);
							_r.pc = a;
						}
					} break;
					case 0xF5:
						Wait(1);
						Push(AF);
						break;
					case 0xF6:
						_r.q = 1;
						A = OR(A, Operand());
						break;
					case 0xF7:
						Wait(1);
						Push(_r.pc);
						_r.wz = 0x30;
						_r.pc = _r.wz;
						break;
					case 0xF8: instRET_c(F & FlagS); break;
					case 0xF9:
						Wait(2);
						SP = hlRef;
						break; //LD SP,HL
					case 0xFA: {
						uint16_t a = Operands();
						_r.wz = a;
						if(F & FlagS) {
							_r.pc = a;
						}
					} break;
					case 0xFB:
						_r.iff1 = true;
						_r.iff2 = true;
						_r.ei = true;
						break; //EI
					case 0xFC: {
						uint16_t a = Operands();
						_r.wz = a;
						if(F & FlagS) {
							Wait(1);
							Push(_r.pc);
							_r.pc = a;
						}
					} break;
					case 0xFE:
						_r.q = 1;
						CP(A, Operand());
						break;
					case 0xFF:
						Wait(1);
						Push(_r.pc);
						_r.wz = 0x38;
						_r.pc = _r.wz;
						break;
					default: break;
				}
			}
			break;
		}
	}
#undef op
}

//--- Named instruction helpers ---

void GenesisZ80::instNOP()
{
}
void GenesisZ80::instHALT()
{
	_r.halt = true;
	_r.q = false;
}

void GenesisZ80::instADD_hl_rr(uint16_t rr)
{
	_r.q = 1;
	uint16_t hl = HL();
	_r.wz = hl + 1;
	bool saveVF = _r.flags & FlagP, saveZF = _r.flags & FlagZ, saveSF = _r.flags & FlagS;
	Wait(3);
	auto lo = ADD((uint8_t)(hl & 0xFF), (uint8_t)(rr & 0xFF));
	Wait(4);
	auto hi = ADD((uint8_t)(hl >> 8), (uint8_t)(rr >> 8), _r.flags & FlagC);
	uint16_t result = (hi << 8) | lo;
	setHL(result);
	if(saveVF) {
		_r.flags |= FlagP;
	} else {
		_r.flags &= ~FlagP;
	}
	if(saveZF) {
		_r.flags |= FlagZ;
	} else {
		_r.flags &= ~FlagZ;
	}
	if(saveSF) {
		_r.flags |= FlagS;
	} else {
		_r.flags &= ~FlagS;
	}
}

void GenesisZ80::instINC_r(uint8_t& r)
{
	_r.q = 1;
	r = INC(r);
}
void GenesisZ80::instDEC_r(uint8_t& r)
{
	_r.q = 1;
	r = DEC(r);
}

void GenesisZ80::instINC_irr(uint16_t& ref)
{
	_r.q = 1;
	uint16_t addr = Displace(ref);
	uint8_t data = Read(addr);
	Wait(1);
	Write(addr, INC(data));
}

void GenesisZ80::instDEC_irr(uint16_t& ref)
{
	_r.q = 1;
	uint16_t addr = Displace(ref);
	uint8_t data = Read(addr);
	Wait(1);
	Write(addr, DEC(data));
}

void GenesisZ80::instJR_c_e(bool c)
{
	int8_t disp = (int8_t)Operand();
	if(!c) {
		return;
	}
	Wait(5);
	_r.wz = _r.pc + disp;
	_r.pc = _r.wz;
}

void GenesisZ80::instRET_c(bool c)
{
	Wait(1);
	if(!c) {
		return;
	}
	_r.wz = Pop();
	_r.pc = _r.wz;
}

void GenesisZ80::instDAA()
{
	_r.q = 1;
	uint8_t a = A;
	if((_r.flags & FlagC) || (A & 0xFF) > 0x99) {
		A += (_r.flags & FlagN) ? 0x60 : 0x60;
		_r.flags |= FlagC;
	}
	if((_r.flags & FlagH) || (A & 0x0F) > 0x09) {
		A += (_r.flags & FlagN) ? 0x06 : 0x06;
	}
	if(Parity(A)) {
		_r.flags |= FlagP;
	} else {
		_r.flags &= ~FlagP;
	}
	if(A & 0x08) {
		_r.flags |= FlagX;
	} else {
		_r.flags &= ~FlagX;
	}
	if((A ^ a) & 0x10) {
		_r.flags |= FlagH;
	} else {
		_r.flags &= ~FlagH;
	}
	if(A & 0x20) {
		_r.flags |= FlagY;
	} else {
		_r.flags &= ~FlagY;
	}
	if(A == 0) {
		_r.flags |= FlagZ;
	} else {
		_r.flags &= ~FlagZ;
	}
	if(A & 0x80) {
		_r.flags |= FlagS;
	} else {
		_r.flags &= ~FlagS;
	}
}

void GenesisZ80::instCPL()
{
	_r.q = 1;
	A = ~A;
	_r.flags |= FlagN | FlagH;
	if(A & 0x08) {
		_r.flags |= FlagX;
	} else {
		_r.flags &= ~FlagX;
	}
	if(A & 0x20) {
		_r.flags |= FlagY;
	} else {
		_r.flags &= ~FlagY;
	}
}

void GenesisZ80::instSCF()
{
	if(_r.q) {
		_r.flags &= ~(FlagX | FlagY);
	}
	_r.flags |= FlagC;
	_r.flags &= ~(FlagN | FlagH);
	if(A & 0x08) {
		_r.flags |= FlagX;
	}
	if(A & 0x20) {
		_r.flags |= FlagY;
	}
	_r.q = 1;
}

void GenesisZ80::instCCF()
{
	if(_r.q) {
		_r.flags &= ~(FlagX | FlagY);
	}
	bool oldC = _r.flags & FlagC;
	if(oldC) {
		_r.flags |= FlagH;
	} else {
		_r.flags &= ~FlagH;
	}
	_r.flags &= ~FlagN;
	_r.flags &= ~FlagC;
	if(!oldC) {
		_r.flags |= FlagC;
	}
	if(A & 0x08) {
		_r.flags |= FlagX;
	}
	if(A & 0x20) {
		_r.flags |= FlagY;
	}
	_r.q = 1;
}

void GenesisZ80::instDJNZ_e()
{
	Wait(1);
	int8_t disp = (int8_t)Operand();
	if(!--B) {
		return;
	}
	Wait(5);
	_r.wz = _r.pc + disp;
	_r.pc = _r.wz;
}

//--- CB prefix ---

void GenesisZ80::InstructionCB(uint8_t code)
{
	auto getReg = [&](uint8_t r) -> uint8_t& {
		static uint8_t dummy;
		switch(r) {
			case 0: return _r.b;
			case 1: return _r.c;
			case 2: return _r.d;
			case 3: return _r.e;
			case 4: return _r.h;
			case 5: return _r.l;
			case 6: return dummy;
			case 7: return _r.a;
			default: return _r.b;
		}
	};

	uint8_t rotType = (code >> 3) & 7;
	uint8_t bit = (code >> 3) & 7;
	uint8_t reg = code & 7;
	uint8_t op = (code >> 6) & 3;

	auto doRot = [&](uint8_t x) -> uint8_t {
		switch(rotType) {
			case 0: return RLC(x);
			case 1: return RRC(x);
			case 2: return RL(x);
			case 3: return RR(x);
			case 4: return SLA(x);
			case 5: return SRA(x);
			case 6: return SLL(x);
			case 7: return SRL(x);
			default: return x;
		}
	};

	if(op == 0) { //Rotate/shift
		if(reg == 6) {
			uint16_t addr = TrueHL();
			uint8_t data = Read(addr);
			Wait(1);
			Write(addr, doRot(data));
		} else {
			_r.q = 1;
			getReg(reg) = doRot(getReg(reg));
		}
	} else if(op == 1) { //BIT
		if(reg == 6) {
			BIT(bit, Read(TrueHL()));
			Wait(1);
		} else {
			BIT(bit, getReg(reg));
		}
		_r.q = 1;
	} else if(op == 2) { //RES
		if(reg == 6) {
			uint16_t addr = TrueHL();
			uint8_t data = Read(addr);
			Wait(1);
			Write(addr, RES(bit, data));
		} else {
			getReg(reg) = RES(bit, getReg(reg));
		}
	} else { //SET
		if(reg == 6) {
			uint16_t addr = TrueHL();
			uint8_t data = Read(addr);
			Wait(1);
			Write(addr, SET(bit, data));
		} else {
			getReg(reg) = SET(bit, getReg(reg));
		}
	}
}

//--- CB+d prefix (IX/IY + displacement + CB) ---

void GenesisZ80::InstructionCBd(uint16_t addr, uint8_t code)
{
	auto getReg = [&](uint8_t r) -> uint8_t& {
		static uint8_t dummy;
		switch(r) {
			case 0: return _r.b;
			case 1: return _r.c;
			case 2: return _r.d;
			case 3: return _r.e;
			case 4: return _r.h;
			case 5: return _r.l;
			case 6: return dummy;
			case 7: return _r.a;
			default: return _r.b;
		}
	};

	uint8_t rotType = (code >> 3) & 7;
	uint8_t bit = (code >> 3) & 7;
	uint8_t reg = code & 7;
	uint8_t op = (code >> 6) & 3;

	auto doRot = [&](uint8_t x) -> uint8_t {
		switch(rotType) {
			case 0: return RLC(x);
			case 1: return RRC(x);
			case 2: return RL(x);
			case 3: return RR(x);
			case 4: return SLA(x);
			case 5: return SRA(x);
			case 6: return SLL(x);
			case 7: return SRL(x);
			default: return x;
		}
	};

	if(op == 0) {
		uint8_t data = Read(addr);
		Wait(1);
		uint8_t result = doRot(data);
		Write(addr, result);
		if(reg != 6) {
			getReg(reg) = result;
		}
		_r.q = 1;
	} else if(op == 1) {
		BIT(bit, Read(addr));
		Wait(1);
		_r.q = 1;
	} else if(op == 2) {
		uint8_t data = Read(addr);
		Wait(1);
		uint8_t result = RES(bit, data);
		Write(addr, result);
		if(reg != 6) {
			getReg(reg) = result;
		}
	} else {
		uint8_t data = Read(addr);
		Wait(1);
		uint8_t result = SET(bit, data);
		Write(addr, result);
		if(reg != 6) {
			getReg(reg) = result;
		}
	}
}

//--- ED prefix ---

void GenesisZ80::InstructionED(uint8_t code)
{
	uint16_t bcVal = (_r.b << 8) | _r.c;
	uint16_t deVal = (_r.d << 8) | _r.e;
	uint16_t hlVal = HL(); //respects DD/FD prefix (IX/IY)

	auto setBC = [&](uint16_t v) { _r.b = v >> 8; _r.c = v & 0xFF; };
	auto setDE = [&](uint16_t v) { _r.d = v >> 8; _r.e = v & 0xFF; };
	//setHL respects DD/FD prefix for consistency with hlVal
	auto setHL = [&](uint16_t v) {
		switch(_r.prefix) {
			case Registers::IX: _r.ix = v; break;
			case Registers::IY: _r.iy = v; break;
			default:
				_r.h = v >> 8;
				_r.l = v & 0xFF;
				break;
		}
	};

	//Most ED opcodes are NOP; only specific ranges are defined.
	if(code >= 0x40 && code <= 0x7F) {
		//Z80 ED opcode layout for 0x40-0x7F:
		//  bits [2:0] = opType (operation): 0=IN, 1=OUT, 2=SBC/ADC, 3=LD, 4=NEG, 5=RETN, 6=IM, 7=misc
		//  bits [5:3] = regSel (register selector)
		uint8_t opType = code & 7;
		uint8_t regSel = (code >> 3) & 7;

		auto getReg = [&](uint8_t r) -> uint8_t& {
			static uint8_t dummy;
			switch(r) {
				case 0: return _r.b;
				case 1: return _r.c;
				case 2: return _r.d;
				case 3: return _r.e;
				case 4: return _r.h;
				case 5: return _r.l;
				case 6: return dummy;
				case 7: return _r.a;
				default: return _r.b;
			}
		};

		//opType 4-7: NEG, RETN/RETI, IM, and LD I,A / LD R,A / LD A,I / LD A,R / RRD / RLD
		if(opType >= 4) {
			switch(opType) {
				case 4: { //NEG
					_r.q = 1;
					A = SUB(0, A);
					break;
				}
				case 5: { //RETN / RETI (0x4D = RETI, all others RETN; functionally identical)
					_r.wz = Pop();
					_r.pc = _r.wz;
					_r.iff1 = _r.iff2;
					break;
				}
				case 6: { //IM mode — mode depends on regSel
					static const uint8_t modes[] = { 0, 0, 1, 2, 0, 0, 1, 2 };
					_r.im = modes[regSel];
					break;
				}
				case 7: { //LD I,A / LD R,A / LD A,I / LD A,R / RRD / RLD
					switch(regSel) {
						case 0: //LD I,A
							_r.i = A;
							break;
						case 1: //LD R,A
							_r.r = A;
							break;
						case 2: { //LD A,I
							Wait(1);
							_r.q = 1;
							A = _r.i;
							_r.flags &= ~(FlagN | FlagP | FlagH | FlagZ | FlagS);
							if(_r.iff2) {
								_r.flags |= FlagP;
							}
							if(A & 0x08) {
								_r.flags |= FlagX;
							}
							if(A & 0x20) {
								_r.flags |= FlagY;
							}
							if(A == 0) {
								_r.flags |= FlagZ;
							}
							if(A & 0x80) {
								_r.flags |= FlagS;
							}
							_r.p = true; //NMOS: clear PF on interrupt during LD A,I
							break;
						}
						case 3: { //LD A,R
							Wait(1);
							_r.q = 1;
							uint8_t r = (_r.r & 0x7F) | (_r.r & 0x80);
							_r.flags &= ~(FlagN | FlagP | FlagH | FlagZ | FlagS);
							if(_r.iff2) {
								_r.flags |= FlagP;
							}
							if(r & 0x08) {
								_r.flags |= FlagX;
							}
							if(r & 0x20) {
								_r.flags |= FlagY;
							}
							if(r == 0) {
								_r.flags |= FlagZ;
							}
							if(r & 0x80) {
								_r.flags |= FlagS;
							}
							A = r;
							_r.p = true;
							break;
						}
						case 4: { //RRD
							_r.q = 1;
							uint8_t data = Read(hlVal);
							Wait(4);
							Write(hlVal, (data >> 4) | (A << 4));
							A = (A & 0xF0) | (data & 0x0F);
							_r.flags &= ~(FlagN | FlagP | FlagH | FlagZ | FlagS);
							if(Parity(A)) {
								_r.flags |= FlagP;
							}
							if(A & 0x08) {
								_r.flags |= FlagX;
							}
							if(A & 0x20) {
								_r.flags |= FlagY;
							}
							if(A == 0) {
								_r.flags |= FlagZ;
							}
							if(A & 0x80) {
								_r.flags |= FlagS;
							}
							_r.wz = hlVal + 1;
							break;
						}
						case 5: { //RLD
							_r.q = 1;
							uint8_t data = Read(hlVal);
							Wait(4);
							Write(hlVal, (data << 4) | (A & 0x0F));
							A = (A & 0xF0) | (data >> 4);
							_r.flags &= ~(FlagN | FlagP | FlagH | FlagZ | FlagS);
							if(Parity(A)) {
								_r.flags |= FlagP;
							}
							if(A & 0x08) {
								_r.flags |= FlagX;
							}
							if(A & 0x20) {
								_r.flags |= FlagY;
							}
							if(A == 0) {
								_r.flags |= FlagZ;
							}
							if(A & 0x80) {
								_r.flags |= FlagS;
							}
							_r.wz = hlVal + 1;
							break;
						}
						default: break; //undocumented NOPs for regSel=6,7
					}
					break;
				}
			}
			return;
		}

		//opType 0-3: IN, OUT, SBC/ADC, LD — determined by opType
		switch(opType) {
			case 0: { //IN r,(C) / IN (C)
				uint8_t data = In((_r.b << 8) | _r.c);
				_r.wz = ((_r.b << 8) | _r.c) + 1;
				_r.q = 1;
				data = IN(data);
				if(regSel != 6) {
					getReg(regSel) = data;
				}
				break;
			}
			case 1: { //OUT (C),r / OUT (C),0
				if(regSel == 6) {
					Out((_r.b << 8) | _r.c, 0x00); //NMOS: 0x00
				} else {
					Out((_r.b << 8) | _r.c, getReg(regSel));
				}
				_r.wz = ((_r.b << 8) | _r.c) + 1;
				break;
			}
			case 2: { //SBC HL,rr / ADC HL,rr
				//Bit 3 (0x08) distinguishes SBC (0) from ADC (1).
				//Register pair is selected by bits [5:4] of the opcode.
				bool isSbc = !(code & 0x08);
				_r.q = 1;
				uint16_t rr;
				switch(regSel >> 1) {
					case 0: rr = bcVal; break;
					case 1: rr = deVal; break;
					case 2: rr = hlVal; break;
					case 3: rr = _r.sp; break;
					default: rr = 0; break;
				}
				_r.wz = hlVal + 1;
				Wait(3);
				if(isSbc) {
					auto lo = SUB((uint8_t)(hlVal & 0xFF), (uint8_t)(rr & 0xFF), _r.flags & FlagC);
					Wait(4);
					auto hi = SUB((uint8_t)(hlVal >> 8), (uint8_t)(rr >> 8), _r.flags & FlagC);
					uint16_t result = (hi << 8) | lo;
					setHL(result);
					if(result == 0) {
						_r.flags |= FlagZ;
					} else {
						_r.flags &= ~FlagZ;
					}
				} else {
					auto lo = ADD((uint8_t)(hlVal & 0xFF), (uint8_t)(rr & 0xFF), _r.flags & FlagC);
					Wait(4);
					auto hi = ADD((uint8_t)(hlVal >> 8), (uint8_t)(rr >> 8), _r.flags & FlagC);
					uint16_t result = (hi << 8) | lo;
					setHL(result);
					if(result == 0) {
						_r.flags |= FlagZ;
					} else {
						_r.flags &= ~FlagZ;
					}
				}
				break;
			}
			case 3: { //LD (nn),rr / LD rr,(nn)
				uint16_t addr = Operands();
				//Bit 3 (0x08) distinguishes store (LD (nn),rr, bit3=0)
				//from load (LD rr,(nn), bit3=1). Register pair is selected
				//by bits [5:4] of the opcode.
				bool isStore = !(code & 0x08);
				uint16_t rr;
				switch(regSel >> 1) {
					case 0: rr = bcVal; break;
					case 1: rr = deVal; break;
					case 2: rr = hlVal; break;
					case 3: rr = _r.sp; break;
					default: rr = 0; break;
				}
				if(isStore) {
					Write(addr, rr & 0xFF);
					Write(addr + 1, rr >> 8);
				} else {
					uint8_t lo = Read(addr);
					uint8_t hi = Read(addr + 1);
					rr = (hi << 8) | lo;
					switch(regSel >> 1) {
						case 0: setBC(rr); break;
						case 1: setDE(rr); break;
						case 2: setHL(rr); break;
						case 3: _r.sp = rr; break;
					}
				}
				_r.wz = addr + 1;
				break;
			}
		}
	} else if(code >= 0xA0) {
		//Block instructions — Z80 opcode map:
		//  bits [2:0] = operation: 000=LD, 001=CP, 010=IN, 011=OUT
		//  bit 3      = direction: 0=increment, 1=decrement
		//  bit 4      = repeat: 0=no, 1=yes
		switch(code) {
			case 0xA0: { //LDI
				uint8_t data = Read(hlVal++);
				Write(deVal++, data);
				Wait(2);
				_r.flags &= ~(FlagN | FlagP | FlagH | FlagX | FlagY);
				bcVal--;
				if(bcVal) {
					_r.flags |= FlagP;
				}
				if((uint8_t)(_r.a + data) & 0x08) {
					_r.flags |= FlagX;
				}
				if((uint8_t)(_r.a + data) & 0x02) {
					_r.flags |= FlagY;
				}
				setBC(bcVal);
				setDE(deVal);
				setHL(hlVal);
				_r.q = 1;
				break;
			}
			case 0xA1: { //CPI
				_r.wz++;
				uint8_t data = Read(hlVal++);
				Wait(5);
				uint8_t n = _r.a - data;
				_r.flags &= ~(FlagN | FlagZ | FlagP | FlagH | FlagX | FlagY | FlagS);
				_r.flags |= FlagN;
				bcVal--;
				if(bcVal) {
					_r.flags |= FlagP;
				}
				if((_r.a ^ data ^ n) & 0x10) {
					_r.flags |= FlagH;
				}
				uint8_t n2 = n - ((_r.flags & FlagH) ? 1 : 0);
				if(n2 & 0x08) {
					_r.flags |= FlagX;
				}
				if(n2 & 0x02) {
					_r.flags |= FlagY;
				}
				if(n == 0) {
					_r.flags |= FlagZ;
				}
				if(n & 0x80) {
					_r.flags |= FlagS;
				}
				setBC(bcVal);
				setHL(hlVal);
				_r.q = 1;
				break;
			}
			case 0xA2: { //INI
				_r.wz = bcVal + 1;
				Wait(1);
				uint8_t data = In(bcVal);
				_r.b--;
				Write(hlVal++, data);
				uint16_t cf = (uint16_t)((uint8_t)(_r.c + 1) + data);
				_r.flags = FlagN;
				if(cf & 0x100) {
					_r.flags |= FlagC | FlagH;
				}
				if(Parity(((_r.c + 1 + data) & 7) ^ _r.b)) {
					_r.flags |= FlagP;
				}
				if(_r.b & 0x08) {
					_r.flags |= FlagX;
				}
				if(_r.b & 0x20) {
					_r.flags |= FlagY;
				}
				if(_r.b == 0) {
					_r.flags |= FlagZ;
				}
				if(_r.b & 0x80) {
					_r.flags |= FlagS;
				}
				setBC((_r.b << 8) | _r.c);
				setHL(hlVal);
				_r.q = 1;
				break;
			}
			case 0xA3: { //OUTI
				_r.wz = bcVal + 1;
				Wait(1);
				uint8_t data = Read(hlVal++);
				_r.b--;
				Out(bcVal, data);
				uint16_t cf = (uint16_t)(_r.l + data);
				_r.flags = FlagN;
				if(data & 0x80) {
					_r.flags |= FlagN;
				}
				if(cf & 0x100) {
					_r.flags |= FlagC | FlagH;
				}
				if(Parity(((_r.l + data) & 7) ^ _r.b)) {
					_r.flags |= FlagP;
				}
				if(_r.b & 0x08) {
					_r.flags |= FlagX;
				}
				if(_r.b & 0x20) {
					_r.flags |= FlagY;
				}
				if(_r.b == 0) {
					_r.flags |= FlagZ;
				}
				if(_r.b & 0x80) {
					_r.flags |= FlagS;
				}
				setBC((_r.b << 8) | _r.c);
				setHL(hlVal);
				_r.q = 1;
				break;
			}
			case 0xA8: { //LDD
				uint8_t data = Read(hlVal--);
				Write(deVal--, data);
				Wait(2);
				_r.flags &= ~(FlagN | FlagP | FlagH | FlagX | FlagY);
				bcVal--;
				if(bcVal) {
					_r.flags |= FlagP;
				}
				if((uint8_t)(_r.a + data) & 0x08) {
					_r.flags |= FlagX;
				}
				if((uint8_t)(_r.a + data) & 0x02) {
					_r.flags |= FlagY;
				}
				setBC(bcVal);
				setDE(deVal);
				setHL(hlVal);
				_r.q = 1;
				break;
			}
			case 0xA9: { //CPD
				_r.wz--;
				uint8_t data = Read(hlVal--);
				Wait(5);
				uint8_t n = _r.a - data;
				_r.flags &= ~(FlagN | FlagZ | FlagP | FlagH | FlagX | FlagY | FlagS);
				_r.flags |= FlagN;
				bcVal--;
				if(bcVal) {
					_r.flags |= FlagP;
				}
				if((_r.a ^ data ^ n) & 0x10) {
					_r.flags |= FlagH;
				}
				uint8_t n2 = n - ((_r.flags & FlagH) ? 1 : 0);
				if(n2 & 0x08) {
					_r.flags |= FlagX;
				}
				if(n2 & 0x02) {
					_r.flags |= FlagY;
				}
				if(n == 0) {
					_r.flags |= FlagZ;
				}
				if(n & 0x80) {
					_r.flags |= FlagS;
				}
				setBC(bcVal);
				setHL(hlVal);
				_r.q = 1;
				break;
			}
			case 0xAA: { //IND
				_r.wz = bcVal - 1;
				Wait(1);
				uint8_t data = In(bcVal);
				_r.b--;
				Write(hlVal--, data);
				uint16_t cf = (uint16_t)((uint8_t)(_r.c - 1) + data);
				_r.flags = FlagN;
				if(cf & 0x100) {
					_r.flags |= FlagC | FlagH;
				}
				if(Parity(((_r.c - 1 + data) & 7) ^ _r.b)) {
					_r.flags |= FlagP;
				}
				if(_r.b & 0x08) {
					_r.flags |= FlagX;
				}
				if(_r.b & 0x20) {
					_r.flags |= FlagY;
				}
				if(_r.b == 0) {
					_r.flags |= FlagZ;
				}
				if(_r.b & 0x80) {
					_r.flags |= FlagS;
				}
				setBC((_r.b << 8) | _r.c);
				setHL(hlVal);
				_r.q = 1;
				break;
			}
			case 0xAB: { //OUTD
				_r.wz = bcVal - 1;
				Wait(1);
				uint8_t data = Read(hlVal--);
				_r.b--;
				Out(bcVal, data);
				uint16_t cf = (uint16_t)(_r.l + data);
				_r.flags = FlagN;
				if(data & 0x80) {
					_r.flags |= FlagN;
				}
				if(cf & 0x100) {
					_r.flags |= FlagC | FlagH;
				}
				if(Parity(((_r.l + data) & 7) ^ _r.b)) {
					_r.flags |= FlagP;
				}
				if(_r.b & 0x08) {
					_r.flags |= FlagX;
				}
				if(_r.b & 0x20) {
					_r.flags |= FlagY;
				}
				if(_r.b == 0) {
					_r.flags |= FlagZ;
				}
				if(_r.b & 0x80) {
					_r.flags |= FlagS;
				}
				setBC((_r.b << 8) | _r.c);
				setHL(hlVal);
				_r.q = 1;
				break;
			}
			case 0xB0: { //LDIR
				uint8_t data = Read(hlVal++);
				Write(deVal++, data);
				Wait(2);
				_r.flags &= ~(FlagN | FlagP | FlagH | FlagX | FlagY);
				bcVal--;
				if(bcVal) {
					_r.flags |= FlagP;
				}
				if((uint8_t)(_r.a + data) & 0x08) {
					_r.flags |= FlagX;
				}
				if((uint8_t)(_r.a + data) & 0x02) {
					_r.flags |= FlagY;
				}
				setBC(bcVal);
				setDE(deVal);
				setHL(hlVal);
				_r.q = 1;
				if(bcVal) {
					Wait(5);
					_r.pc -= 2;
					_r.wz = _r.pc + 1;
				}
				break;
			}
			case 0xB1: { //CPIR
				_r.wz++;
				uint8_t data = Read(hlVal++);
				Wait(5);
				uint8_t n = _r.a - data;
				_r.flags &= ~(FlagN | FlagZ | FlagP | FlagH | FlagX | FlagY | FlagS);
				_r.flags |= FlagN;
				bcVal--;
				if(bcVal) {
					_r.flags |= FlagP;
				}
				if((_r.a ^ data ^ n) & 0x10) {
					_r.flags |= FlagH;
				}
				uint8_t n2 = n - ((_r.flags & FlagH) ? 1 : 0);
				if(n2 & 0x08) {
					_r.flags |= FlagX;
				}
				if(n2 & 0x02) {
					_r.flags |= FlagY;
				}
				if(n == 0) {
					_r.flags |= FlagZ;
				}
				if(n & 0x80) {
					_r.flags |= FlagS;
				}
				setBC(bcVal);
				setHL(hlVal);
				_r.q = 1;
				if(bcVal && n) {
					Wait(5);
					_r.pc -= 2;
					_r.wz = _r.pc + 1;
				}
				break;
			}
			case 0xB2: { //INIR
				_r.wz = bcVal + 1;
				Wait(1);
				uint8_t data = In(bcVal);
				_r.b--;
				Write(hlVal++, data);
				uint16_t cf = (uint16_t)((uint8_t)(_r.c + 1) + data);
				_r.flags = FlagN;
				if(cf & 0x100) {
					_r.flags |= FlagC | FlagH;
				}
				if(Parity(((_r.c + 1 + data) & 7) ^ _r.b)) {
					_r.flags |= FlagP;
				}
				if(_r.b & 0x08) {
					_r.flags |= FlagX;
				}
				if(_r.b & 0x20) {
					_r.flags |= FlagY;
				}
				if(_r.b == 0) {
					_r.flags |= FlagZ;
				}
				if(_r.b & 0x80) {
					_r.flags |= FlagS;
				}
				setBC((_r.b << 8) | _r.c);
				setHL(hlVal);
				_r.q = 1;
				if(_r.b) {
					Wait(5);
					_r.pc -= 2;
					_r.wz = _r.pc + 1;
				}
				break;
			}
			case 0xB3: { //OTIR
				_r.wz = bcVal + 1;
				Wait(1);
				uint8_t data = Read(hlVal++);
				_r.b--;
				Out(bcVal, data);
				uint16_t cf = (uint16_t)(_r.l + data);
				_r.flags = FlagN;
				if(data & 0x80) {
					_r.flags |= FlagN;
				}
				if(cf & 0x100) {
					_r.flags |= FlagC | FlagH;
				}
				if(Parity(((_r.l + data) & 7) ^ _r.b)) {
					_r.flags |= FlagP;
				}
				if(_r.b & 0x08) {
					_r.flags |= FlagX;
				}
				if(_r.b & 0x20) {
					_r.flags |= FlagY;
				}
				if(_r.b == 0) {
					_r.flags |= FlagZ;
				}
				if(_r.b & 0x80) {
					_r.flags |= FlagS;
				}
				setBC((_r.b << 8) | _r.c);
				setHL(hlVal);
				_r.q = 1;
				if(_r.b) {
					Wait(5);
					_r.pc -= 2;
					_r.wz = _r.pc + 1;
				}
				break;
			}
			case 0xB8: { //LDDR
				uint8_t data = Read(hlVal--);
				Write(deVal--, data);
				Wait(2);
				_r.flags &= ~(FlagN | FlagP | FlagH | FlagX | FlagY);
				bcVal--;
				if(bcVal) {
					_r.flags |= FlagP;
				}
				if((uint8_t)(_r.a + data) & 0x08) {
					_r.flags |= FlagX;
				}
				if((uint8_t)(_r.a + data) & 0x02) {
					_r.flags |= FlagY;
				}
				setBC(bcVal);
				setDE(deVal);
				setHL(hlVal);
				_r.q = 1;
				if(bcVal) {
					Wait(5);
					_r.pc -= 2;
					_r.wz = _r.pc + 1;
				}
				break;
			}
			case 0xB9: { //CPDR
				_r.wz--;
				uint8_t data = Read(hlVal--);
				Wait(5);
				uint8_t n = _r.a - data;
				_r.flags &= ~(FlagN | FlagZ | FlagP | FlagH | FlagX | FlagY | FlagS);
				_r.flags |= FlagN;
				bcVal--;
				if(bcVal) {
					_r.flags |= FlagP;
				}
				if((_r.a ^ data ^ n) & 0x10) {
					_r.flags |= FlagH;
				}
				uint8_t n2 = n - ((_r.flags & FlagH) ? 1 : 0);
				if(n2 & 0x08) {
					_r.flags |= FlagX;
				}
				if(n2 & 0x02) {
					_r.flags |= FlagY;
				}
				if(n == 0) {
					_r.flags |= FlagZ;
				}
				if(n & 0x80) {
					_r.flags |= FlagS;
				}
				setBC(bcVal);
				setHL(hlVal);
				_r.q = 1;
				if(bcVal && n) {
					Wait(5);
					_r.pc -= 2;
					_r.wz = _r.pc + 1;
				}
				break;
			}
			case 0xBA: { //INDR
				_r.wz = bcVal - 1;
				Wait(1);
				uint8_t data = In(bcVal);
				_r.b--;
				Write(hlVal--, data);
				uint16_t cf = (uint16_t)((uint8_t)(_r.c - 1) + data);
				_r.flags = FlagN;
				if(cf & 0x100) {
					_r.flags |= FlagC | FlagH;
				}
				if(Parity(((_r.c - 1 + data) & 7) ^ _r.b)) {
					_r.flags |= FlagP;
				}
				if(_r.b & 0x08) {
					_r.flags |= FlagX;
				}
				if(_r.b & 0x20) {
					_r.flags |= FlagY;
				}
				if(_r.b == 0) {
					_r.flags |= FlagZ;
				}
				if(_r.b & 0x80) {
					_r.flags |= FlagS;
				}
				setBC((_r.b << 8) | _r.c);
				setHL(hlVal);
				_r.q = 1;
				if(_r.b) {
					Wait(5);
					_r.pc -= 2;
					_r.wz = _r.pc + 1;
				}
				break;
			}
			case 0xBB: { //OTDR
				_r.wz = bcVal - 1;
				Wait(1);
				uint8_t data = Read(hlVal--);
				_r.b--;
				Out(bcVal, data);
				uint16_t cf = (uint16_t)(_r.l + data);
				_r.flags = FlagN;
				if(data & 0x80) {
					_r.flags |= FlagN;
				}
				if(cf & 0x100) {
					_r.flags |= FlagC | FlagH;
				}
				if(Parity(((_r.l + data) & 7) ^ _r.b)) {
					_r.flags |= FlagP;
				}
				if(_r.b & 0x08) {
					_r.flags |= FlagX;
				}
				if(_r.b & 0x20) {
					_r.flags |= FlagY;
				}
				if(_r.b == 0) {
					_r.flags |= FlagZ;
				}
				if(_r.b & 0x80) {
					_r.flags |= FlagS;
				}
				setBC((_r.b << 8) | _r.c);
				setHL(hlVal);
				_r.q = 1;
				if(_r.b) {
					Wait(5);
					_r.pc -= 2;
					_r.wz = _r.pc + 1;
				}
				break;
			}
			default: break; //undocumented NOPs for all other 0xA0-0xBF opcodes
		}
	}
	//All other ED opcodes are NOP
}

//--- Serialization ---

void GenesisZ80::Serialize(Serializer& s)
{
	SV(_r.a);
	SV(_r.flags);
	SV(_r.b);
	SV(_r.c);
	SV(_r.d);
	SV(_r.e);
	SV(_r.h);
	SV(_r.l);
	SV(_r.ix);
	SV(_r.iy);
	SV(_r.sp);
	SV(_r.pc);
	SV(_r.i);
	SV(_r.r);
	SV(_r.wz);
	SV(_r.a_);
	SV(_r.flags_);
	SV(_r.b_);
	SV(_r.c_);
	SV(_r.d_);
	SV(_r.e_);
	SV(_r.h_);
	SV(_r.l_);
	SV(_r.iff1);
	SV(_r.iff2);
	SV(_r.im);
	SV(_r.halt);
	SV(_r.ei);
	SV(_r.p);
	SV(_r.q);
	SV(_r.prefix);
	SV(_nmiLine);
	SV(_intLine);
	SV(_busreqLine);
	SV(_busreqLatch);
	SV(_resetLine);
	SV(_nmiEdge);
}
