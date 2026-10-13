using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using Mesen.Utilities;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;

namespace Mesen.Windows
{
	public partial class SelectBinFileConsoleTypeWindow : MesenWindow
	{
		public SelectBinFileConsoleTypeWindow()
		{
			AvaloniaXamlLoader.Load(this);
		}

		protected override void OnKeyDown(KeyEventArgs e)
		{
			if(e.Key == Key.Enter) {
				AcceptSelection();
			} else if(e.Key == Key.Escape) {
				Close();
			}
			base.OnKeyDown(e);
		}

		public static async Task<BinFileConsoleTypeSelection?> Show(ResourcePath romPath)
		{
			Window? parent = ApplicationHelper.GetMainWindow();
			if(parent == null) {
				return null;
			}

			SelectBinFileConsoleTypeViewModel model = new(romPath);
			SelectBinFileConsoleTypeWindow window = new() {
				DataContext = model,
				WindowStartupLocation = WindowStartupLocation.CenterOwner
			};
			await window.ShowDialog(parent);

			return model.Cancelled || model.SelectedConsoleType == null
				? null
				: new(model.SelectedConsoleType.ConsoleType, model.RememberSelection);
		}

		private void AcceptSelection()
		{
			if(DataContext is SelectBinFileConsoleTypeViewModel { SelectedConsoleType: not null } model) {
				model.Cancelled = false;
				Close();
			}
		}

		private void OnOkClick(object? sender, RoutedEventArgs e) => AcceptSelection();
		private void OnCancelClick(object? sender, RoutedEventArgs e) => Close();
	}

	public partial class SelectBinFileConsoleTypeViewModel : ObservableObject
	{
		public List<BinFileConsoleTypeOption> ConsoleTypes { get; } = new() {
			new(ConsoleType.Snes, "Super Nintendo"),
			new(ConsoleType.Nes, "NES / Famicom"),
			new(ConsoleType.Gameboy, "Game Boy / Game Boy Color"),
			new(ConsoleType.PcEngine, "PC Engine / TurboGrafx-16"),
			new(ConsoleType.Sms, "Master System / Game Gear / SG-1000 / ColecoVision"),
			new(ConsoleType.Gba, "Game Boy Advance"),
			new(ConsoleType.Ws, "WonderSwan")
		};

		[ObservableProperty] public partial BinFileConsoleTypeOption? SelectedConsoleType { get; set; }
		[ObservableProperty] public partial bool RememberSelection { get; set; }
		public bool Cancelled { get; set; } = true;
		public string FileName { get; }

		public SelectBinFileConsoleTypeViewModel(ResourcePath romPath)
		{
			FileName = Path.GetFileName(romPath.FileName);
			SelectedConsoleType = ConsoleTypes[0];
		}
	}

	public record BinFileConsoleTypeOption(ConsoleType ConsoleType, string DisplayName);
	public record BinFileConsoleTypeSelection(ConsoleType ConsoleType, bool RememberSelection);
}
