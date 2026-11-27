#pragma once
#include "pch.h"
#include "Debugger/IDebugger.h"
#include "Genesis/GenesisTypes.h"

class GenesisConsole;
class Debugger;

class GenesisDebugger final : public IDebugger
{
	Debugger* _debugger;
	GenesisConsole* _console;
	CpuType _cpuType;
	MemoryType _memoryType;
	unique_ptr<BreakpointManager> _breakpointManager;
	unique_ptr<CallstackManager> _callstackManager;
	unique_ptr<ITraceLogger> _traceLogger;
	GenesisM68KState _m68kState {};
	GenesisZ80State _z80State {};
	uint32_t _instructionPc = 0;
	uint32_t _instructionSp = 0;
	DisassemblyInfo _instruction;

	uint32_t GetStackPointer();
	void UpdateCallstack(uint32_t pc, uint32_t sp, AddressInfo& address);

public:
	GenesisDebugger(Debugger* debugger, CpuType cpuType);
	~GenesisDebugger();

	void ProcessInstruction();
	void ProcessMemory(uint32_t address, uint8_t value, MemoryOperationType type);
	void ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi) override;
	void ProcessVdpCycle(uint16_t hCounter, uint16_t vCounter);
	void ProcessFrame();

	void Reset() override;
	void ResetPrevOpCode() override;
	void Run() override;
	void Step(int32_t stepCount, StepType type) override;
	DebuggerFeatures GetSupportedFeatures() override;
	uint64_t GetCpuCycleCount(bool forProfiler) override;
	uint32_t GetProgramCounter(bool getInstPc) override;
	void SetProgramCounter(uint32_t address, bool updateDebuggerOnly) override;

	BreakpointManager* GetBreakpointManager() override { return _breakpointManager.get(); }
	CallstackManager* GetCallstackManager() override { return _callstackManager.get(); }
	ITraceLogger* GetTraceLogger() override { return _traceLogger.get(); }
	IAssembler* GetAssembler() override { return nullptr; }
	BaseEventManager* GetEventManager() override { return nullptr; }
	BaseState& GetState() override;
	ISerializable* GetSerializableCpu() override;
};
