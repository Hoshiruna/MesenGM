using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using System;
using System.Collections.Generic;

namespace Mesen.Debugger.StatusViews
{
	public partial class GenesisStatusViewModel(CpuType cpuType) : BaseConsoleStatusViewModel
	{
		[ObservableProperty] public partial IReadOnlyList<string> RegisterRows { get; private set; } = Array.Empty<string>();

		protected override void InternalUpdateUiState()
		{
			if(cpuType == CpuType.GenesisM68K) {
				var state = DebugApi.GetCpuState<GenesisM68KState>(cpuType);
				var rows = new List<string> { $"PC  ${state.PC:X6}    SR  ${state.SR:X4}", $"SSP ${state.SSP:X8}" };
				for(int i = 0; i < 8; i++) {
					rows.Add($"D{i}  ${state.D[i]:X8}  A{i}  ${state.A[i]:X8}");
				}
				RegisterRows = rows;
				UpdateCycleCount(state.CycleCount);
			} else {
				var state = DebugApi.GetCpuState<GenesisZ80State>(cpuType);
				RegisterRows = new[] {
					$"PC  ${state.PC:X4}  SP  ${state.SP:X4}",
					$"AF  ${state.AF:X4}  BC  ${state.BC:X4}",
					$"DE  ${state.DE:X4}  HL  ${state.HL:X4}",
					$"IX  ${state.IX:X4}  IY  ${state.IY:X4}",
					$"I   ${state.I:X2}    R   ${state.R:X2}"
				};
				UpdateCycleCount(state.CycleCount);
			}
		}

		protected override void InternalUpdateConsoleState()
		{
		}
	}
}
