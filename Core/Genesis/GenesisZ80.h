#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"

// Genesis Z80 (APU) — native Mesen2 implementation.
//
// Algorithm ported from ares/component/processor/z80 with Genesis-specific
// bus mapping from ares/md/apu. Uses bus callbacks (like GenesisM68K) so the
// memory manager can wire reads/writes to RAM, YM2612, bank register, and the
// M68K shared bus without this class knowing about those details.
//
// Key differences from ares Z80:
//   - No ares:: namespace or nall types; uses standard C++ types.
//   - ISerializable / SV() for save states instead of ares serializer.
//   - Bus callbacks (std::function) instead of virtual Bus base class.
//   - NMOS mode only (Genesis uses NMOS Z80).

class Emulator;
enum class MemoryOperationType;

class GenesisZ80 final : public ISerializable
{
public:
	GenesisZ80();
	~GenesisZ80() = default;

	void SetEmulator(Emulator* emu) { _emu = emu; }

	void Power();
	void Reset();

	//Execute one instruction (or spin if stopped/halted/bus-not-granted).
	//Returns cycles consumed.
	uint32_t ExecuteInstruction();

	void AddCycles(uint32_t cycles) { _cycleAccum += cycles; }
	uint32_t GetInstructionCycles() const { return _cycleAccum; }

	//Interrupt lines
	void SetIrq(bool line); //level-sensitive maskable interrupt
	void SetNmi(bool line); //edge-sensitive non-maskable interrupt
	void SetBusreq(bool line); //bus request from M68K
	void SetReset(bool line); //reset line from M68K

	//Bus interface callbacks — set by the memory manager / console.
	//Read/Write operate on the Z80 64K address space.
	std::function<uint8_t(uint16_t addr)> BusRead;
	std::function<void(uint16_t addr, uint8_t data)> BusWrite;

	//I/O port access (unused on Genesis, but needed for completeness)
	std::function<uint8_t(uint16_t port)> PortRead = [](uint16_t) { return 0xFF; };
	std::function<void(uint16_t port, uint8_t data)> PortWrite = [](uint16_t, uint8_t) {};

	//Clock callback — consume Z80 clock cycles
	std::function<void(uint32_t cycles)> BusWait;

	//Register accessors (for debugger)
	uint16_t GetPC() const { return _r.pc; }
	void SetPC(uint16_t pc) { _r.pc = pc; }
	uint8_t GetA() const { return _r.a; }
	uint8_t GetB() const { return _r.b; }
	uint8_t GetC() const { return _r.c; }
	uint8_t GetD() const { return _r.d; }
	uint8_t GetE() const { return _r.e; }
	uint8_t GetH() const { return _r.h; }
	uint8_t GetL() const { return _r.l; }
	uint16_t GetAF() const { return (_r.a << 8) | _r.flags; }
	uint16_t GetBC() const { return (_r.b << 8) | _r.c; }
	uint16_t GetDE() const { return (_r.d << 8) | _r.e; }
	uint16_t GetHL() const { return (_r.h << 8) | _r.l; }
	uint16_t GetSP() const { return _r.sp; }
	uint16_t GetIX() const { return _r.ix; }
	uint16_t GetIY() const { return _r.iy; }
	uint8_t GetI() const { return _r.i; }
	uint8_t GetR() const { return _r.r; }
	uint8_t GetFlags() const { return _r.flags; }
	bool IsHalted() const { return _r.halt; }

	//ISerializable
	void Serialize(Serializer& s) override;

	//Flag bit positions
	static constexpr uint8_t FlagC = 0x01;
	static constexpr uint8_t FlagN = 0x02;
	static constexpr uint8_t FlagP = 0x04; //also V (overflow/parity)
	static constexpr uint8_t FlagX = 0x08; //undocumented bit 3
	static constexpr uint8_t FlagH = 0x10;
	static constexpr uint8_t FlagY = 0x20; //undocumented bit 5
	static constexpr uint8_t FlagZ = 0x40;
	static constexpr uint8_t FlagS = 0x80;

private:
	//Register set — using separate bytes for clean flag access.
	//Shadow registers stored as raw 16-bit words (only used for EX/EXX).
	struct Registers
	{
		//Main registers
		uint8_t a = 0xFF;
		uint8_t flags = 0xFF;
		uint8_t b = 0;
		uint8_t c = 0;
		uint8_t d = 0;
		uint8_t e = 0;
		uint8_t h = 0;
		uint8_t l = 0;

		//Index registers
		uint16_t ix = 0;
		uint16_t iy = 0;

		//Stack pointer & program counter
		uint16_t sp = 0xFFFF;
		uint16_t pc = 0;

		//Interrupt vector / refresh
		uint8_t i = 0;
		uint8_t r = 0;

		//Shadow registers (AF', BC', DE', HL')
		uint8_t a_ = 0;
		uint8_t flags_ = 0;
		uint8_t b_ = 0;
		uint8_t c_ = 0;
		uint8_t d_ = 0;
		uint8_t e_ = 0;
		uint8_t h_ = 0;
		uint8_t l_ = 0;

		//Internal temporary register (WZ)
		uint16_t wz = 0;

		//Control flags
		bool iff1 = false; //interrupt flip-flop 1
		bool iff2 = false; //interrupt flip-flop 2
		uint8_t im = 0; //interrupt mode (0-2)
		bool halt = false;
		bool ei = false; //EI executed last (suppress IRQ for 1 instruction)
		bool p = false; //LD a,i / LD a,r executed last
		bool q = false; //flag-updating opcode executed last

		//Prefix state for IX/IY displacement
		enum Prefix : uint8_t
		{
			HL,
			IX,
			IY
		} prefix = HL;
	} _r;

	//Bus control state
	bool _nmiLine = false;
	bool _intLine = false;
	bool _busreqLine = false;
	bool _busreqLatch = false;
	bool _resetLine = false;
	bool _nmiEdge = false; //edge detect for NMI
	uint32_t _cycleAccum = 0;
	Emulator* _emu = nullptr;

	//--- Memory helpers ---
	void Wait(uint32_t clocks = 1);
	uint8_t ReadBus(uint16_t addr, MemoryOperationType type);
	uint8_t Opcode(bool firstByte = false);
	uint8_t Operand();
	uint16_t Operands();
	void Push(uint16_t value);
	uint16_t Pop();
	uint16_t Displace(uint16_t& reg);
	uint8_t Read(uint16_t addr);
	void Write(uint16_t addr, uint8_t data);
	uint8_t In(uint16_t port);
	void Out(uint16_t port, uint8_t data);

	//--- Flag helpers ---
	bool Parity(uint8_t value) const;
	void IncrementR();

	//--- Register access helpers ---
	//Get HL/IX/IY based on current prefix
	uint16_t& HL();
	//Set HL/IX/IY based on current prefix — propagates to _r.h/_r.l
	void setHL(uint16_t v);
	//Read/write H or L respecting DD/FD prefix (IXH/IXL, IYH/IYL)
	uint8_t readH();
	uint8_t readL();
	void writeH(uint8_t v);
	void writeL(uint8_t v);
	uint8_t& H();
	uint8_t& L();
	//True HL (ignoring prefix)
	uint16_t TrueHL() { return (_r.h << 8) | _r.l; }

	//--- Algorithms (ported from ares z80/algorithms.cpp) ---
	uint8_t ADD(uint8_t x, uint8_t y, bool carry = false);
	uint8_t AND(uint8_t x, uint8_t y);
	uint8_t BIT(uint8_t bit, uint8_t x);
	void CP(uint8_t x, uint8_t y);
	uint8_t DEC(uint8_t x);
	uint8_t IN(uint8_t x);
	uint8_t INC(uint8_t x);
	uint8_t OR(uint8_t x, uint8_t y);
	uint8_t RES(uint8_t bit, uint8_t x);
	uint8_t RL(uint8_t x);
	uint8_t RLC(uint8_t x);
	uint8_t RR(uint8_t x);
	uint8_t RRC(uint8_t x);
	uint8_t SET(uint8_t bit, uint8_t x);
	uint8_t SLA(uint8_t x);
	uint8_t SLL(uint8_t x);
	uint8_t SRA(uint8_t x);
	uint8_t SRL(uint8_t x);
	uint8_t SUB(uint8_t x, uint8_t y, bool carry = false);
	uint8_t XOR(uint8_t x, uint8_t y);

	//--- Instruction dispatch ---
	void Instruction();
	void Instruction(uint8_t code);
	void InstructionCB(uint8_t code);
	void InstructionCBd(uint16_t address, uint8_t code);
	void InstructionED(uint8_t code);

	//--- Individual instructions ---
	void instADC_a_irr(uint16_t& x);
	void instADC_a_n();
	void instADC_a_r(uint8_t& x);
	void instADC_hl_rr(uint16_t x);
	void instADD_a_irr(uint16_t& x);
	void instADD_a_n();
	void instADD_a_r(uint8_t& x);
	void instADD_hl_rr(uint16_t x);
	void instAND_a_irr(uint16_t& x);
	void instAND_a_n();
	void instAND_a_r(uint8_t& x);
	void instBIT_o_irr(uint8_t bit, uint16_t addr, uint8_t& x);
	void instBIT_o_r(uint8_t bit, uint8_t& x);
	void instCALL_c_nn(bool c);
	void instCALL_nn();
	void instCCF();
	void instCP_a_irr(uint16_t& x);
	void instCP_a_n();
	void instCP_a_r(uint8_t& x);
	void instCPD();
	void instCPDR();
	void instCPI();
	void instCPIR();
	void instCPL();
	void instDAA();
	void instDEC_irr(uint16_t& x);
	void instDEC_r(uint8_t& x);
	void instDEC_rr(uint16_t& x);
	void instDI();
	void instDJNZ_e();
	void instEI();
	void instEX_irr_rr(uint16_t addr, uint16_t& y);
	void instEX_rr_rr(uint16_t& x, uint16_t& y);
	void instEXX();
	void instHALT();
	void instIM_o(uint8_t mode);
	void instIN_a_in();
	void instIN_r_ic(uint8_t& x);
	void instIN_ic();
	void instINC_irr(uint16_t& x);
	void instINC_r(uint8_t& x);
	void instINC_rr(uint16_t& x);
	void instIND();
	void instINDR();
	void instINI();
	void instINIR();
	void instJP_c_nn(bool c);
	void instJP_rr(uint16_t& x);
	void instJR_c_e(bool c);
	void instLD_a_inn();
	void instLD_a_irr(uint16_t& x);
	void instLD_inn_a();
	void instLD_inn_rr(uint16_t x);
	void instLD_irr_a(uint16_t& x);
	void instLD_irr_n(uint16_t& x);
	void instLD_irr_r(uint16_t& x, uint8_t& y);
	void instLD_r_n(uint8_t& x);
	void instLD_r_irr(uint8_t& x, uint16_t& y);
	void instLD_r_r(uint8_t& x, uint8_t& y);
	void instLD_r_r1(uint8_t& x, uint8_t& y); //LD to I/R — extra T-cycle
	void instLD_r_r2(uint8_t& x, uint8_t& y); //LD from I/R — sets flags
	void instLD_rr_inn(uint16_t& x);
	void instLD_rr_nn(uint16_t& x);
	void instLD_sp_rr(uint16_t x);
	void instLDD();
	void instLDDR();
	void instLDI();
	void instLDIR();
	void instNEG();
	void instNOP();
	void instOR_a_irr(uint16_t& x);
	void instOR_a_n();
	void instOR_a_r(uint8_t& x);
	void instOTDR();
	void instOTIR();
	void instOUT_ic_r(uint8_t& x);
	void instOUT_ic();
	void instOUT_in_a();
	void instOUTD();
	void instOUTI();
	void instPOP_rr(uint16_t& x);
	void instPUSH_rr(uint16_t x);
	void instRES_o_irr(uint8_t bit, uint16_t addr, uint8_t& x);
	void instRES_o_r(uint8_t bit, uint8_t& x);
	void instRET();
	void instRET_c(bool c);
	void instRETI();
	void instRETN();
	void instRL_irr(uint16_t addr, uint8_t& x);
	void instRL_r(uint8_t& x);
	void instRLA();
	void instRLC_irr(uint16_t addr, uint8_t& x);
	void instRLC_r(uint8_t& x);
	void instRLCA();
	void instRLD();
	void instRR_irr(uint16_t addr, uint8_t& x);
	void instRR_r(uint8_t& x);
	void instRRA();
	void instRRC_irr(uint16_t addr, uint8_t& x);
	void instRRC_r(uint8_t& x);
	void instRRCA();
	void instRRD();
	void instRST_o(uint8_t vector);
	void instSBC_a_irr(uint16_t& x);
	void instSBC_a_n();
	void instSBC_a_r(uint8_t& x);
	void instSBC_hl_rr(uint16_t x);
	void instSCF();
	void instSET_o_irr(uint8_t bit, uint16_t addr, uint8_t& x);
	void instSET_o_r(uint8_t bit, uint8_t& x);
	void instSLA_irr(uint16_t addr, uint8_t& x);
	void instSLA_r(uint8_t& x);
	void instSLL_irr(uint16_t addr, uint8_t& x);
	void instSLL_r(uint8_t& x);
	void instSRA_irr(uint16_t addr, uint8_t& x);
	void instSRA_r(uint8_t& x);
	void instSRL_irr(uint16_t addr, uint8_t& x);
	void instSRL_r(uint8_t& x);
	void instSUB_a_irr(uint16_t& x);
	void instSUB_a_n();
	void instSUB_a_r(uint8_t& x);
	void instXOR_a_irr(uint16_t& x);
	void instXOR_a_n();
	void instXOR_a_r(uint8_t& x);

	//Helper to get 16-bit register reference for IX/IY-aware HL
	uint16_t& _getHL();
	uint16_t _trueHL;
};
