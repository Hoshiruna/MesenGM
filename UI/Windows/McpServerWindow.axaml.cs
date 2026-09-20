using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Input.Platform;
using Avalonia.Markup.Xaml;
using Mesen.Localization;
using Mesen.Utilities;
using Mesen.ViewModels;
using System;
using System.ComponentModel;

namespace Mesen.Windows
{
	public class McpServerWindow : MesenWindow
	{
		private McpServerWindowViewModel Model => (McpServerWindowViewModel)DataContext!;

		public McpServerWindow()
		{
			DataContext = new McpServerWindowViewModel();
			AvaloniaXamlLoader.Load(this);
			Model.PropertyChanged += Model_PropertyChanged;
		}

		private void Model_PropertyChanged(object? sender, PropertyChangedEventArgs e)
		{
			if(e.PropertyName == nameof(McpServerWindowViewModel.Log)) {
				this.FindControl<ScrollViewer>("_logScroll")?.ScrollToEnd();
			}
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

		private async void CopyLog_OnClick(object? sender, RoutedEventArgs e)
		{
			if(Clipboard != null) {
				await Clipboard.SetTextAsync(Model.Log);
			}
		}

		private void Close_OnClick(object? sender, RoutedEventArgs e)
		{
			Close();
		}

		protected override void OnClosed(EventArgs e)
		{
			Model.PropertyChanged -= Model_PropertyChanged;
			Model.Dispose();
			base.OnClosed(e);
		}
	}
}
