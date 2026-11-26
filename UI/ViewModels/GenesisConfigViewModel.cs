using Avalonia.Controls;
using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Config;
using Mesen.Utilities;
using System;

namespace Mesen.ViewModels
{
	public partial class GenesisConfigViewModel : DisposableViewModel
	{
		public GenesisConfig Config { get; }
		public GenesisConfig OriginalConfig { get; }
		[ObservableProperty] public partial GenesisConfigTab SelectedTab { get; set; }

		public Enum[] AvailableRegions => new Enum[] {
			ConsoleRegion.Auto,
			ConsoleRegion.Ntsc,
			ConsoleRegion.NtscJapan,
			ConsoleRegion.Pal
		};

		public Enum[] AvailableControllerTypes => new Enum[] {
			ControllerType.None,
			ControllerType.GenesisController,
			ControllerType.GenesisController6Buttons
		};

		public GenesisConfigViewModel()
		{
			Config = ConfigManager.Config.Genesis;
			OriginalConfig = Config.Clone();

			if(Design.IsDesignMode) {
				return;
			}

			AddDisposable(ReactiveHelper.RegisterRecursiveObserver(Config, (_, _) => Config.ApplyConfig()));
		}
	}

	public enum GenesisConfigTab
	{
		General,
		Audio,
		Emulation,
		Input,
		Video
	}
}
