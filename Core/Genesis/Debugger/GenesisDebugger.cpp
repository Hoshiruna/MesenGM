#include "pch.h"
#include "Genesis/Debugger/GenesisDebugger.h"
#include "Debugger/BaseTraceLogger.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/CallstackManager.h"
#include "Debugger/Debugger.h"
#include "Debugger/Disassembler.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Debugger/MemoryDumper.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisM68K.h"
#include "Genesis/GenesisMemoryManager.h"
#include "Genesis/GenesisVdp.h"
#include "Genesis/GenesisZ80.h"

namespace
{
	template<typename State>
	class GenesisTraceLogger final : public BaseTraceLogger<GenesisTraceLogger<State>, State>
	{
		using Base = BaseTraceLogger<GenesisTraceLogger<State>, State>;
		GenesisVdp* _vdp;

		RowDataType GetFormatTagType(string& tag) override
		{
			if(tag == "SP") {
				return RowDataType::SP;
			}
			return RowDataType::Text;
		}

	public:
		GenesisTraceLogger(Debugger* debugger, IDebugger* cpuDebugger, CpuType cpuType, GenesisVdp* vdp)
			: Base(debugger, cpuDebugger, cpuType), _vdp(vdp)
		{
		}

		void GetTraceRow(string& output, State& state, TraceLogPpuState& vdpState, DisassemblyInfo& disassembly)
		{
			for(RowPart& part : this->_rowParts) {
				if(part.DataType == RowDataType::SP) {
					this->WriteIntValue(output, GetStackPointer(state), part);
				} else {
					this->ProcessSharedTag(part, output, state, vdpState, disassembly);
				}
			}
		}

		void LogPpuState()
		{
			this->_ppuState[this->_currentPos] = { _vdp->GetHCounter(), _vdp->GetHCounter(), _vdp->GetVCounter(), _vdp->GetFrameCount() };
		}

		uint32_t GetProgramCounter(State& state) { return state.PC; }
		uint64_t GetCycleCount(State& state) { return state.CycleCount; }
		uint32_t GetStackPointer(State& state)
		{
			if constexpr(std::is_same_v<State, GenesisM68KState>) {
				return state.A[7];
			} else {
				return state.SP;
			}
		}
	};
}

GenesisDebugger::GenesisDebugger(Debugger* debugger, CpuType cpuType)
	: IDebugger(debugger->GetEmulator()), _debugger(debugger), _console(static_cast<GenesisConsole*>(debugger->GetConsole())), _cpuType(cpuType), _memoryType(DebugUtilities::GetCpuMemoryType(cpuType))
{
	_breakpointManager.reset(new BreakpointManager(debugger, this, cpuType, nullptr));
	_callstackManager.reset(new CallstackManager(debugger, this));
	if(cpuType == CpuType::GenesisM68K) {
		_traceLogger.reset(new GenesisTraceLogger<GenesisM68KState>(debugger, this, cpuType, _console->GetVdp()));
	} else {
		_traceLogger.reset(new GenesisTraceLogger<GenesisZ80State>(debugger, this, cpuType, _console->GetVdp()));
	}
	Run();
	Reset();
}

GenesisDebugger::~GenesisDebugger() = default;

void GenesisDebugger::ProcessInstruction()
{
	uint32_t pc = GetProgramCounter(false);
	uint32_t sp = GetStackPointer();
	AddressInfo address = _debugger->GetAbsoluteAddress({ (int32_t)pc, _memoryType });
	UpdateCallstack(pc, sp, address);

	_instructionPc = pc;
	_instructionSp = sp;
	uint32_t prefetch = _cpuType == CpuType::GenesisM68K ? _console->GetM68K()->GetPrefetch() : 0;
	_instruction = _debugger->GetDisassembler()->CacheGenesisInstruction(address, pc, _cpuType, prefetch);
	uint32_t opcode = _cpuType == CpuType::GenesisM68K ? prefetch >> 16 : _instruction.GetOpCode();
	MemoryOperationInfo operation(pc, opcode, MemoryOperationType::ExecOpCode, _memoryType);
	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.StartCycle = GetCpuCycleCount(false);
	InstructionProgress.CurrentCycle = 0;
	InstructionProgress.LastOpCode = opcode;

	auto* counter = _debugger->GetMemoryAccessCounter();
	uint64_t clock = _console->GetCpuMasterClock(_cpuType);
	counter->ProcessMemoryExec(address, clock);
	if(_cpuType == CpuType::GenesisM68K) {
		AddressInfo lowByte = _debugger->GetAbsoluteAddress({ (int32_t)(pc + 1), _memoryType });
		counter->ProcessMemoryExec(lowByte, clock);
	}
	_step->ProcessCpuExec();
	_debugger->ProcessBreakConditions(_cpuType, *_step, _breakpointManager.get(), operation, address);
	if(_traceLogger->IsEnabled() && _instruction.IsInitialized()) {
		if(_cpuType == CpuType::GenesisM68K) {
			auto* trace = static_cast<GenesisTraceLogger<GenesisM68KState>*>(_traceLogger.get());
			trace->Log(static_cast<GenesisM68KState&>(GetState()), _instruction, operation, address);
		} else {
			auto* trace = static_cast<GenesisTraceLogger<GenesisZ80State>*>(_traceLogger.get());
			trace->Log(static_cast<GenesisZ80State&>(GetState()), _instruction, operation, address);
		}
	}
}

void GenesisDebugger::ProcessMemory(uint32_t address, uint8_t value, MemoryOperationType type)
{
	AddressInfo absolute = _console->GetMemoryManager()->GetAbsoluteAddress(address, _cpuType, type == MemoryOperationType::Write);
	MemoryOperationInfo operation(address, value, type, _memoryType);
	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.CurrentCycle = GetCpuCycleCount(false) - InstructionProgress.StartCycle;

	uint64_t clock = _console->GetCpuMasterClock(_cpuType);
	auto* counter = _debugger->GetMemoryAccessCounter();
	if(type == MemoryOperationType::Write) {
		counter->ProcessMemoryWrite(absolute, clock);
		if(absolute.Type != MemoryType::GenesisCartridgeRom) {
			_debugger->GetDisassembler()->InvalidateCache(absolute, _cpuType);
		}
	} else if(type == MemoryOperationType::ExecOperand) {
		counter->ProcessMemoryExec(absolute, clock);
	} else {
		counter->ProcessMemoryRead(absolute, clock);
	}
	if(_traceLogger->IsEnabled()) {
		if(_cpuType == CpuType::GenesisM68K) {
			static_cast<GenesisTraceLogger<GenesisM68KState>*>(_traceLogger.get())->LogNonExec(operation, absolute);
		} else {
			static_cast<GenesisTraceLogger<GenesisZ80State>*>(_traceLogger.get())->LogNonExec(operation, absolute);
		}
	}
	_debugger->ProcessBreakConditions(_cpuType, *_step, _breakpointManager.get(), operation, absolute);
}

void GenesisDebugger::UpdateCallstack(uint32_t pc, uint32_t sp, AddressInfo& address)
{
	if(_instruction.IsInitialized()) {
		if(_instruction.IsJumpToSub() && sp == (_cpuType == CpuType::GenesisM68K ? _instructionSp - 4 : (uint16_t)(_instructionSp - 2))) {
			uint32_t returnPc = (_instructionPc + _instruction.GetOpSize()) & (_cpuType == CpuType::GenesisM68K ? 0xFFFFFF : 0xFFFF);
			AddressInfo source = _debugger->GetAbsoluteAddress({ (int32_t)_instructionPc, _memoryType });
			AddressInfo ret = _debugger->GetAbsoluteAddress({ (int32_t)returnPc, _memoryType });
			_callstackManager->Push(source, _instructionPc, address, pc, ret, returnPc, _instructionSp, StackFrameFlags::None);
		} else if(_instruction.IsReturnInstruction() && sp != _instructionSp) {
			_callstackManager->Pop(address, pc, sp);
		}
	}

	if(_step->BreakAddress == pc && _step->BreakStackPointer == sp) {
		_step->Break(BreakSource::CpuStep);
	}
}

void GenesisDebugger::ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi)
{
	uint32_t sp = _cpuType == CpuType::GenesisM68K ? _console->GetM68K()->GetInterruptReturnSP() : (uint16_t)(_console->GetZ80()->GetSP() + 2);
	AddressInfo source = _debugger->GetAbsoluteAddress({ (int32_t)originalPc, _memoryType });
	AddressInfo dest = _debugger->GetAbsoluteAddress({ (int32_t)currentPc, _memoryType });
	UpdateCallstack(originalPc, sp, source);
	ResetPrevOpCode();
	_instructionPc = currentPc;
	_debugger->InternalProcessInterrupt(_cpuType, *this, *_step, source, originalPc, dest, currentPc, source, originalPc, sp, forNmi);
}

void GenesisDebugger::ProcessVdpCycle(uint16_t hCounter, uint16_t vCounter)
{
	bool countStep = _step->Type == StepType::PpuStep || (_step->Type == StepType::PpuScanline && hCounter == 0);
	if((countStep && _step->PpuStepCount > 0 && --_step->PpuStepCount == 0) || (hCounter == 0 && _step->HasScanlineBreakRequest() && _step->BreakScanline == vCounter)) {
		_debugger->SleepUntilResume(_cpuType, _step->GetBreakSource());
	}
}

void GenesisDebugger::ProcessFrame()
{
	if(_step->Type == StepType::PpuFrame && _step->PpuStepCount > 0 && --_step->PpuStepCount == 0) {
		_debugger->SleepUntilResume(_cpuType, _step->GetBreakSource());
	}
}

void GenesisDebugger::Reset()
{
	ResetPrevOpCode();
	_instructionPc = GetProgramCounter(false);
	_callstackManager->Clear();
	if(_cpuType == CpuType::GenesisM68K) {
		_debugger->GetDisassembler()->ResetGenesisRamCache();
	}
}

void GenesisDebugger::ResetPrevOpCode()
{
	_instruction.Reset();
}

void GenesisDebugger::Run()
{
	_step.reset(new StepRequest());
}

void GenesisDebugger::Step(int32_t stepCount, StepType type)
{
	StepRequest step(type);
	switch(type) {
		case StepType::Step: step.StepCount = stepCount; break;
		case StepType::StepOver:
			if(_instruction.IsInitialized() && _instruction.IsJumpToSub() && _instructionPc == GetProgramCounter(false) && AllowChangeProgramCounter) {
				step.BreakAddress = (_instructionPc + _instruction.GetOpSize()) & (_cpuType == CpuType::GenesisM68K ? 0xFFFFFF : 0xFFFF);
				step.BreakStackPointer = _instructionSp;
			} else {
				step.StepCount = 1;
			}
			break;
		case StepType::StepOut:
			step.BreakAddress = _callstackManager->GetReturnAddress();
			step.BreakStackPointer = _callstackManager->GetReturnStackPointer();
			if(step.BreakAddress < 0) {
				step.StepCount = 1;
			}
			break;
		case StepType::PpuStep:
		case StepType::PpuScanline:
		case StepType::PpuFrame: step.PpuStepCount = stepCount; break;
		case StepType::SpecificScanline: step.BreakScanline = stepCount; break;
		default: break;
	}
	_step.reset(new StepRequest(step));
}

DebuggerFeatures GenesisDebugger::GetSupportedFeatures()
{
	DebuggerFeatures features {};
	features.RunToIrq = true;
	features.StepOver = true;
	features.StepOut = true;
	features.CallStack = true;
	features.ChangeProgramCounter = AllowChangeProgramCounter;
	if(_cpuType == CpuType::GenesisM68K) {
		features.CpuVectors[0] = { "Reset", 4, VectorType::Indirect };
		features.CpuVectors[1] = { "HBlank", 0x70, VectorType::Indirect };
		features.CpuVectors[2] = { "VBlank", 0x78, VectorType::Indirect };
		features.CpuVectorCount = 3;
	} else {
		features.CpuVectors[0] = { "IRQ", 0x38, VectorType::Direct };
		features.CpuVectorCount = 1;
	}
	return features;
}

uint64_t GenesisDebugger::GetCpuCycleCount(bool forProfiler)
{
	uint32_t divider = _cpuType == CpuType::GenesisM68K ? GenesisM68kClockDivider : GenesisZ80ClockDivider;
	return _console->GetCpuMasterClock(_cpuType) / divider;
}

uint32_t GenesisDebugger::GetStackPointer()
{
	return _cpuType == CpuType::GenesisM68K ? _console->GetM68K()->GetAddrReg(7) : _console->GetZ80()->GetSP();
}

uint32_t GenesisDebugger::GetProgramCounter(bool getInstPc)
{
	if(getInstPc) {
		return _instructionPc;
	}
	return _cpuType == CpuType::GenesisM68K ? _console->GetM68K()->GetInstructionPC() : _console->GetZ80()->GetPC();
}

void GenesisDebugger::SetProgramCounter(uint32_t address, bool updateDebuggerOnly)
{
	address &= _cpuType == CpuType::GenesisM68K ? 0xFFFFFE : 0xFFFF;
	if(!updateDebuggerOnly) {
		if(_cpuType == CpuType::GenesisM68K) {
			auto* memory = _debugger->GetMemoryDumper();
			_console->GetM68K()->SetInstructionPC(address, memory->GetMemoryValue16(_memoryType, address), memory->GetMemoryValue16(_memoryType, (address + 2) & 0xFFFFFF));
		} else {
			_console->GetZ80()->SetPC((uint16_t)address);
		}
	}
	_instructionPc = address;
	ResetPrevOpCode();
	if(updateDebuggerOnly && _cpuType == CpuType::GenesisM68K) {
		_debugger->GetDisassembler()->ResetGenesisRamCache();
	}
}

BaseState& GenesisDebugger::GetState()
{
	if(_cpuType == CpuType::GenesisM68K) {
		_console->GetCpuState(_m68kState);
		_m68kState.PC = _instructionPc;
		return _m68kState;
	}
	_console->GetCpuState(_z80State);
	_z80State.PC = (uint16_t)_instructionPc;
	return _z80State;
}

ISerializable* GenesisDebugger::GetSerializableCpu()
{
	if(_cpuType == CpuType::GenesisM68K) {
		return _console->GetM68K();
	}
	return _console->GetZ80();
}
