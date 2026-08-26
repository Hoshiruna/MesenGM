using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Mesen.Localization;
using Mesen.Utilities;
using Mesen.ViewModels;
using System;

namespace Mesen.Windows
{
	public class McpServerWindow : MesenWindow
	{
		private McpServerWindowViewModel Model => (McpServerWindowViewModel)DataContext!;

		public McpServerWindow()
		{
			DataContext = new McpServerWindowViewModel();
			AvaloniaXamlLoader.Load(this);
		}

		private async void Start_OnClick(object? sender, RoutedEventArgs e)
		{
			if(!Model.TryStart(out string error)) {
				await MessageBox.Show(this, error, ResourceHelper.GetViewLabel(nameof(McpServerWindow), "wndTitle"), MessageBoxButtons.OK, MessageBoxIcon.Error);
			}
		}

		private void Stop_OnClick(object? sender, RoutedEventArgs e)
		{
			Model.Stop();
		}

		private async void CopyUrl_OnClick(object? sender, RoutedEventArgs e)
		{
			if(Clipboard != null) {
				await Clipboard.SetTextAsync(Model.ServerUrl);
			}
		}

		private void Close_OnClick(object? sender, RoutedEventArgs e)
		{
			Close();
		}

		protected override void OnClosed(EventArgs e)
		{
			Model.Dispose();
			base.OnClosed(e);
		}
	}
}
