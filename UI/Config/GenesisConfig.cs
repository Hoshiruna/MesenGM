using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using System;
using System.Runtime.InteropServices;

namespace Mesen.Config;

public partial class GenesisConfig : BaseConfig<GenesisConfig>
{
	[ObservableProperty] public partial ControllerConfig Port1 { get; set; } = new();
	[ObservableProperty] public partial ControllerConfig Port2 { get; set; } = new();

	[ValidValues(ConsoleRegion.Auto, ConsoleRegion.Ntsc, ConsoleRegion.NtscJapan, ConsoleRegion.Pal)]
	[ObservableProperty] public partial ConsoleRegion Region { get; set; } = ConsoleRegion.Auto;

	[ObservableProperty] public partial RamState RamPowerOnState { get; set; } = RamState.Random;

	[ObservableProperty] public partial bool DisableSprites { get; set; } = false;
	[ObservableProperty] public partial bool DisableBackground { get; set; } = false;
	[ObservableProperty] public partial bool RemoveSpriteLimit { get; set; } = false;

	[ObservableProperty][MinMax(0, 100)] public partial UInt32 PsgVolume { get; set; } = 100;
	[ObservableProperty][MinMax(0, 100)] public partial UInt32 Ym2612Volume { get; set; } = 100;

	[ObservableProperty] public partial OverscanConfig NtscOverscan { get; set; } = new();
	[ObservableProperty] public partial OverscanConfig PalOverscan { get; set; } = new();

	public void ApplyConfig()
	{
		ConfigApi.SetGenesisConfig(new InteropGenesisConfig() {
			Port1 = Port1.ToInterop(),
			Port2 = Port2.ToInterop(),

			Region = Region,
			Model = 0,
			RamPowerOnState = RamPowerOnState,

			DisableSprites = DisableSprites,
			DisableBackground = DisableBackground,
			RemoveSpriteLimit = RemoveSpriteLimit,

			PsgVolume = PsgVolume,
			Ym2612Volume = Ym2612Volume,

			NtscOverscan = NtscOverscan.ToInterop(),
			PalOverscan = PalOverscan.ToInterop(),
		});
	}

	internal void InitializeDefaults(DefaultKeyMappingType defaultMappings)
	{
		DefaultKeyMappingType p1Mappings = defaultMappings;
		DefaultKeyMappingType p2Mappings = defaultMappings;

		if(defaultMappings.HasFlag(DefaultKeyMappingType.WasdKeys) && defaultMappings.HasFlag(DefaultKeyMappingType.ArrowKeys)) {
			p1Mappings &= ~DefaultKeyMappingType.WasdKeys;
			p2Mappings &= ~DefaultKeyMappingType.ArrowKeys;
		}

		Port1.InitDefaults(p1Mappings, ControllerType.GenesisController, 0);
		Port2.InitDefaults(p2Mappings, ControllerType.GenesisController, 1);
	}
}

[StructLayout(LayoutKind.Sequential)]
public struct InteropGenesisConfig
{
	public InteropControllerConfig Port1;
	public InteropControllerConfig Port2;

	public ConsoleRegion Region;
	public UInt32 Model;
	public RamState RamPowerOnState;

	[MarshalAs(UnmanagedType.I1)] public bool DisableSprites;
	[MarshalAs(UnmanagedType.I1)] public bool DisableBackground;
	[MarshalAs(UnmanagedType.I1)] public bool RemoveSpriteLimit;

	public UInt32 PsgVolume;
	public UInt32 Ym2612Volume;

	public InteropOverscanDimensions NtscOverscan;
	public InteropOverscanDimensions PalOverscan;
}
