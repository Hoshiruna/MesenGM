#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"

//Motorola MC68000 - native Mesen2 port of ares M68000 interpreter.
//Algorithm ported verbatim from ares/component/processor/m68000; types and
//bus interface adapted to Mesen2 infrastructure.

class GenesisMemoryManager;
class Emulator;

class GenesisM68K final : public ISerializable
{
public:
	GenesisM68K();
	~GenesisM68K() = default;

	void SetEmulator(Emulator* emu) { _emu = emu; }

	void Power();
	uint32_t ExecuteInstruction(); //run one instruction; returns cycles consumed
	void Interrupt(uint32_t vector, uint32_t priority = 0);

	//Cycle accounting — BusIdle/BusWait call this to accumulate cycles.
	void AddCycles(uint32_t cycles) { _cycleAccum += cycles; }

	//Bus interface callbacks - set by GenesisMemoryManager.
	//idle/wait: consume master-clock cycles.
	//read/write: upper=UDS, lower=LDS (1 = selected); address is word-aligned.
	std::function<void(uint32_t)> BusIdle;
	std::function<void(uint32_t)> BusWait;
	std::function<uint16_t(uint8_t upper, uint8_t lower, uint32_t address)> BusRead;
	std::function<void(uint8_t upper, uint8_t lower, uint32_t address, uint16_t data)> BusWrite;
	std::function<bool()> BusLockable = [] { return true; };

	//Interrupt check callback - called before each instruction to poll for
	//pending VDP interrupts, matching ares's per-instruction interrupt delivery.
	//Returns true if an interrupt was delivered (which clears _r.stop).
	std::function<bool()> CheckInterrupts;

	//Register accessors (for debugger / VDP DMA)
	uint32_t GetPC() const { return _r.pc; }
	void SetPC(uint32_t pc) { _r.pc = pc; }
	uint32_t GetDataReg(int i) const { return _r.d[i]; }
	void SetDataReg(int i, uint32_t v) { _r.d[i] = v; }
	uint32_t GetAddrReg(int i) const { return _r.a[i]; }
	void SetAddrReg(int i, uint32_t v) { _r.a[i] = v; }
	uint16_t GetSR() const;
	void SetSR(uint16_t sr);
	uint8_t GetInterruptMask() const { return _r.i; }
	bool IsStopped() const { return _r.stop; }
	uint16_t GetIrc() const { return _r.irc; }

	//ISerializable
	void Serialize(Serializer& s) override;

	//Size constants (match ares enum order)
	static constexpr uint32_t Byte = 0;
	static constexpr uint32_t Word = 1;
	static constexpr uint32_t Long = 2;

	//Mode constants
	static constexpr uint32_t DataRegisterDirect = 0;
	static constexpr uint32_t AddressRegisterDirect = 1;
	static constexpr uint32_t AddressRegisterIndirect = 2;
	static constexpr uint32_t AddressRegisterIndirectWithPostIncrement = 3;
	static constexpr uint32_t AddressRegisterIndirectWithPreDecrement = 4;
	static constexpr uint32_t AddressRegisterIndirectWithDisplacement = 5;
	static constexpr uint32_t AddressRegisterIndirectWithIndex = 6;
	static constexpr uint32_t AbsoluteShortIndirect = 7;
	static constexpr uint32_t AbsoluteLongIndirect = 8;
	static constexpr uint32_t ProgramCounterIndirectWithDisplacement = 9;
	static constexpr uint32_t ProgramCounterIndirectWithIndex = 10;
	static constexpr uint32_t Immediate = 11;

	//Exception / Vector enums
	enum Exception : uint32_t
	{
		ExIllegal,
		ExDivisionByZero,
		ExBoundsCheck,
		ExOverflow,
		ExUnprivileged,
		ExTrap,
		ExInterrupt
	};
	enum Vector : uint32_t
	{
		VResetSP = 0,
		VResetPC = 1,
		VBusError = 2,
		VAddressError = 3,
		VIllegalInstruction = 4,
		VDivisionByZero = 5,
		VBoundsCheck = 6,
		VOverflow = 7,
		VUnprivileged = 8,
		VTrace = 9,
		VIllegalLineA = 10,
		VIllegalLineF = 11,
		VSpurious = 24,
		VLevel1 = 25,
		VLevel2 = 26,
		VLevel3 = 27,
		VLevel4 = 28,
		VLevel5 = 29,
		VLevel6 = 30,
		VLevel7 = 31,
		VTrap = 32
	};

	//Boolean flag constants
	static constexpr bool Reverse = 1, Extend = 1, Hold = 1, Fast = 1;

private:
	struct DataRegister
	{
		uint8_t number;
		explicit DataRegister(uint32_t n) : number(n & 7) {}
	};
	struct AddressRegister
	{
		uint8_t number;
		explicit AddressRegister(uint32_t n) : number(n & 7) {}
	};

	struct EffectiveAddress
	{
		uint8_t mode;
		uint8_t reg;
		bool valid = false;
		uint32_t address = 0;
		EffectiveAddress(uint32_t mode_, uint32_t reg_) : mode(mode_ & 0xF), reg(reg_ & 7)
		{
			if(mode == 7) {
				mode += reg; //optimization: convert modes {7;0-4} to {7-11}
			}
		}
		EffectiveAddress() : mode(0), reg(0) {}
	};

	struct Registers
	{
		uint32_t d[8] = {};
		uint32_t a[8] = {};
		uint32_t sp = 0;
		uint32_t pc = 0;
		bool c = 0, v = 0, z = 0, n = 0, x = 0;
		uint8_t i = 7;
		bool s = 1, t = 0;
		uint16_t irc = 0x4e71, ir = 0x4e71, ird = 0x4e71;
		bool stop = false, reset = false;
	} _r;

	std::function<void()> _instructionTable[65536];
	uint32_t _cycleAccum = 0; //cycles consumed by current instruction
	Emulator* _emu = nullptr;

	//--- Helper: bit manipulation ---
	static inline bool bit(uint32_t v, uint32_t b) { return (v >> b) & 1; }
	static inline uint32_t bits(uint32_t v, uint32_t lo, uint32_t hi)
	{
		return (v >> lo) & ((1ULL << (hi - lo + 1)) - 1);
	}

	//--- m68000.cpp ---
	bool Supervisor();
	void Exception(uint32_t exception, uint32_t vector, uint32_t priority = 0);

	//--- registers.cpp ---
	template<uint32_t Size> uint32_t Read(DataRegister reg);
	template<uint32_t Size> void Write(DataRegister reg, uint32_t data);
	template<uint32_t Size> uint32_t Read(AddressRegister reg);
	template<uint32_t Size> void Write(AddressRegister reg, uint32_t data);
	uint8_t ReadCCR();
	uint16_t ReadSR() const;
	void WriteCCR(uint8_t ccr);
	void WriteSR(uint16_t sr);

	//--- memory.cpp ---
	template<uint32_t Size> uint32_t Read(uint32_t addr);
	template<uint32_t Size, bool Order = 0> void Write(uint32_t addr, uint32_t data);
	template<uint32_t Size> uint32_t Extension();
	uint16_t Prefetch();
	uint16_t Prefetched();
	template<uint32_t Size> uint32_t Pop();
	template<uint32_t Size> void Push(uint32_t data);

	//--- effective-address.cpp ---
	uint32_t Prefetched(EffectiveAddress& ea);
	template<uint32_t Size> uint32_t Fetch(EffectiveAddress& ea);
	template<uint32_t Size, bool Hold = 0, bool Fast = 0> uint32_t Read(EffectiveAddress& ea);
	template<uint32_t Size, bool Hold = 0> void Write(EffectiveAddress& ea, uint32_t data);

	//--- traits.cpp ---
	template<uint32_t Size> static constexpr uint32_t bytes()
	{
		if constexpr(Size == Byte) {
			return 1;
		}
		if constexpr(Size == Word) {
			return 2;
		}
		if constexpr(Size == Long) {
			return 4;
		}
	}
	template<uint32_t Size> static constexpr uint32_t bits_()
	{
		if constexpr(Size == Byte) {
			return 8;
		}
		if constexpr(Size == Word) {
			return 16;
		}
		if constexpr(Size == Long) {
			return 32;
		}
	}
	template<uint32_t Size> static constexpr uint32_t lsb() { return 1; }
	template<uint32_t Size> static constexpr uint32_t msb()
	{
		if constexpr(Size == Byte) {
			return 0x80;
		}
		if constexpr(Size == Word) {
			return 0x8000;
		}
		if constexpr(Size == Long) {
			return 0x80000000;
		}
	}
	template<uint32_t Size> static constexpr uint32_t mask()
	{
		if constexpr(Size == Byte) {
			return 0xff;
		}
		if constexpr(Size == Word) {
			return 0xffff;
		}
		if constexpr(Size == Long) {
			return 0xffffffff;
		}
	}
	template<uint32_t Size> static uint32_t clip(uint32_t data)
	{
		if constexpr(Size == Byte) {
			return (uint8_t)data;
		}
		if constexpr(Size == Word) {
			return (uint16_t)data;
		}
		if constexpr(Size == Long) {
			return (uint32_t)data;
		}
	}
	template<uint32_t Size> static int32_t sign(uint32_t data)
	{
		if constexpr(Size == Byte) {
			return (int8_t)data;
		}
		if constexpr(Size == Word) {
			return (int16_t)data;
		}
		if constexpr(Size == Long) {
			return (int32_t)data;
		}
	}

	//--- conditions.cpp ---
	bool Condition(uint32_t condition);

	//--- algorithms.cpp ---
	template<uint32_t Size, bool ExtendFlag = false> uint32_t ADD(uint32_t source, uint32_t target);
	template<uint32_t Size> uint32_t AND(uint32_t source, uint32_t target);
	template<uint32_t Size> uint32_t ASL(uint32_t result, uint32_t shift);
	template<uint32_t Size> uint32_t ASR(uint32_t result, uint32_t shift);
	template<uint32_t Size> uint32_t CMP(uint32_t source, uint32_t target);
	template<uint32_t Size> uint32_t EOR(uint32_t source, uint32_t target);
	template<uint32_t Size> uint32_t LSL(uint32_t result, uint32_t shift);
	template<uint32_t Size> uint32_t LSR(uint32_t result, uint32_t shift);
	template<uint32_t Size> uint32_t OR(uint32_t source, uint32_t target);
	template<uint32_t Size> uint32_t ROL(uint32_t result, uint32_t shift);
	template<uint32_t Size> uint32_t ROR(uint32_t result, uint32_t shift);
	template<uint32_t Size> uint32_t ROXL(uint32_t result, uint32_t shift);
	template<uint32_t Size> uint32_t ROXR(uint32_t result, uint32_t shift);
	template<uint32_t Size, bool ExtendFlag = false> uint32_t SUB(uint32_t source, uint32_t target);

	//--- instructions.cpp ---
	void instructionABCD(EffectiveAddress from, EffectiveAddress with);
	template<uint32_t Size> void instructionADD(EffectiveAddress from, DataRegister with);
	template<uint32_t Size> void instructionADD(DataRegister from, EffectiveAddress with);
	template<uint32_t Size> void instructionADDA(EffectiveAddress from, AddressRegister with);
	template<uint32_t Size> void instructionADDI(EffectiveAddress with);
	template<uint32_t Size> void instructionADDQ(uint8_t immediate, EffectiveAddress with);
	template<uint32_t Size> void instructionADDQ(uint8_t immediate, AddressRegister with);
	template<uint32_t Size> void instructionADDX(EffectiveAddress from, EffectiveAddress with);
	template<uint32_t Size> void instructionAND(EffectiveAddress from, DataRegister with);
	template<uint32_t Size> void instructionAND(DataRegister from, EffectiveAddress with);
	template<uint32_t Size> void instructionANDI(EffectiveAddress with);
	void instructionANDI_TO_CCR();
	void instructionANDI_TO_SR();
	template<uint32_t Size> void instructionASL(uint8_t count, DataRegister with);
	template<uint32_t Size> void instructionASL(DataRegister from, DataRegister with);
	void instructionASL(EffectiveAddress with);
	template<uint32_t Size> void instructionASR(uint8_t count, DataRegister with);
	template<uint32_t Size> void instructionASR(DataRegister from, DataRegister with);
	void instructionASR(EffectiveAddress with);
	void instructionBCC(uint8_t test, uint8_t displacement);
	template<uint32_t Size> void instructionBCHG(DataRegister bit, EffectiveAddress with);
	template<uint32_t Size> void instructionBCHG(EffectiveAddress with);
	template<uint32_t Size> void instructionBCLR(DataRegister bit, EffectiveAddress with);
	template<uint32_t Size> void instructionBCLR(EffectiveAddress with);
	void instructionBRA(uint8_t displacement);
	template<uint32_t Size> void instructionBSET(DataRegister bit, EffectiveAddress with);
	template<uint32_t Size> void instructionBSET(EffectiveAddress with);
	void instructionBSR(uint8_t displacement);
	template<uint32_t Size> void instructionBTST(DataRegister bit, EffectiveAddress with);
	template<uint32_t Size> void instructionBTST(EffectiveAddress with);
	void instructionCHK(DataRegister compare, EffectiveAddress maximum);
	template<uint32_t Size> void instructionCLR(EffectiveAddress with);
	template<uint32_t Size> void instructionCMP(EffectiveAddress from, DataRegister with);
	template<uint32_t Size> void instructionCMPA(EffectiveAddress from, AddressRegister with);
	template<uint32_t Size> void instructionCMPI(EffectiveAddress with);
	template<uint32_t Size> void instructionCMPM(EffectiveAddress from, EffectiveAddress with);
	void instructionDBCC(uint8_t condition, DataRegister with);
	void instructionDIVS(EffectiveAddress from, DataRegister with);
	void instructionDIVU(EffectiveAddress from, DataRegister with);
	template<uint32_t Size> void instructionEOR(DataRegister from, EffectiveAddress with);
	template<uint32_t Size> void instructionEORI(EffectiveAddress with);
	void instructionEORI_TO_CCR();
	void instructionEORI_TO_SR();
	void instructionEXG(DataRegister x, DataRegister y);
	void instructionEXG(AddressRegister x, AddressRegister y);
	void instructionEXG(DataRegister x, AddressRegister y);
	template<uint32_t Size> void instructionEXT(DataRegister with);
	void instructionILLEGAL(uint16_t code);
	void instructionJMP(EffectiveAddress from);
	void instructionJSR(EffectiveAddress from);
	void instructionLEA(EffectiveAddress from, AddressRegister to);
	void instructionLINK(AddressRegister with);
	template<uint32_t Size> void instructionLSL(uint8_t count, DataRegister with);
	template<uint32_t Size> void instructionLSL(DataRegister from, DataRegister with);
	void instructionLSL(EffectiveAddress with);
	template<uint32_t Size> void instructionLSR(uint8_t count, DataRegister with);
	template<uint32_t Size> void instructionLSR(DataRegister from, DataRegister with);
	void instructionLSR(EffectiveAddress with);
	template<uint32_t Size> void instructionMOVE(EffectiveAddress from, EffectiveAddress to);
	template<uint32_t Size> void instructionMOVEA(EffectiveAddress from, AddressRegister to);
	template<uint32_t Size> void instructionMOVEM_TO_MEM(EffectiveAddress to);
	template<uint32_t Size> void instructionMOVEM_TO_REG(EffectiveAddress from);
	template<uint32_t Size> void instructionMOVEP(DataRegister from, EffectiveAddress to);
	template<uint32_t Size> void instructionMOVEP(EffectiveAddress from, DataRegister to);
	void instructionMOVEQ(uint8_t immediate, DataRegister to);
	void instructionMOVE_FROM_SR(EffectiveAddress to);
	void instructionMOVE_TO_CCR(EffectiveAddress from);
	void instructionMOVE_TO_SR(EffectiveAddress from);
	void instructionMOVE_FROM_USP(AddressRegister to);
	void instructionMOVE_TO_USP(AddressRegister from);
	void instructionMULS(EffectiveAddress from, DataRegister with);
	void instructionMULU(EffectiveAddress from, DataRegister with);
	void instructionNBCD(EffectiveAddress with);
	template<uint32_t Size> void instructionNEG(EffectiveAddress with);
	template<uint32_t Size> void instructionNEGX(EffectiveAddress with);
	void instructionNOP();
	template<uint32_t Size> void instructionNOT(EffectiveAddress with);
	template<uint32_t Size> void instructionOR(EffectiveAddress from, DataRegister with);
	template<uint32_t Size> void instructionOR(DataRegister from, EffectiveAddress with);
	template<uint32_t Size> void instructionORI(EffectiveAddress with);
	void instructionORI_TO_CCR();
	void instructionORI_TO_SR();
	void instructionPEA(EffectiveAddress from);
	void instructionRESET();
	template<uint32_t Size> void instructionROL(uint8_t count, DataRegister with);
	template<uint32_t Size> void instructionROL(DataRegister from, DataRegister with);
	void instructionROL(EffectiveAddress with);
	template<uint32_t Size> void instructionROR(uint8_t count, DataRegister with);
	template<uint32_t Size> void instructionROR(DataRegister from, DataRegister with);
	void instructionROR(EffectiveAddress with);
	template<uint32_t Size> void instructionROXL(uint8_t count, DataRegister with);
	template<uint32_t Size> void instructionROXL(DataRegister from, DataRegister with);
	void instructionROXL(EffectiveAddress with);
	template<uint32_t Size> void instructionROXR(uint8_t count, DataRegister with);
	template<uint32_t Size> void instructionROXR(DataRegister from, DataRegister with);
	void instructionROXR(EffectiveAddress with);
	void instructionRTE();
	void instructionRTR();
	void instructionRTS();
	void instructionSBCD(EffectiveAddress from, EffectiveAddress with);
	void instructionSCC(uint8_t test, EffectiveAddress to);
	void instructionSTOP();
	template<uint32_t Size> void instructionSUB(EffectiveAddress from, DataRegister with);
	template<uint32_t Size> void instructionSUB(DataRegister from, EffectiveAddress with);
	template<uint32_t Size> void instructionSUBA(EffectiveAddress from, AddressRegister to);
	template<uint32_t Size> void instructionSUBI(EffectiveAddress with);
	template<uint32_t Size> void instructionSUBQ(uint8_t immediate, EffectiveAddress with);
	template<uint32_t Size> void instructionSUBQ(uint8_t immediate, AddressRegister with);
	template<uint32_t Size> void instructionSUBX(EffectiveAddress from, EffectiveAddress with);
	void instructionSWAP(DataRegister with);
	void instructionTAS(EffectiveAddress with);
	void instructionTRAP(uint8_t vector);
	void instructionTRAPV();
	template<uint32_t Size> void instructionTST(EffectiveAddress from);
	void instructionUNLK(AddressRegister with);

	//--- instruction.cpp ---
	void BuildInstructionTable();

	//Pattern matcher: converts binary string like "1100 ---1 0000 ----" to uint16.
	static constexpr uint16_t Pattern(const char* s)
	{
		uint16_t value = 0;
		int b = 15;
		for(const char* p = s; *p; p++) {
			if(*p == ' ') {
				continue;
			}
			if(*p == '1') {
				value |= (1 << b);
			}
			b--;
		}
		return value;
	}
};
