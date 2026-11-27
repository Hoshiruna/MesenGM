using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Mesen.Config;
using Mesen.Debugger.Controls;
using Mesen.Debugger.Utilities;
using Mesen.Debugger.ViewModels;
using Mesen.Interop;
using Mesen.Localization;
using System;
using System.ComponentModel;

namespace Mesen.Debugger.Windows
{
	public class TilemapViewerWindow : MesenWindow, INotificationHandler
	{
		private TilemapViewerViewModel _model;
		private PictureViewer _picViewer;
		internal bool IsGenesisViewer => _model.IsGenesis;

		[Obsolete("For designer only")]
		public TilemapViewerWindow() : this(CpuType.Snes) { }

		public TilemapViewerWindow(CpuType cpuType)
		{
			InitializeComponent();

			ScrollPictureViewer scrollViewer = this.GetControl<ScrollPictureViewer>("picViewer");
			_picViewer = scrollViewer.InnerViewer;
			_model = new TilemapViewerViewModel(cpuType, _picViewer, scrollViewer, this);
			DataContext = _model;

			_model.Config.LoadWindowSettings(this);

			if(Design.IsDesignMode) {
				return;
			}

			MouseViewerModelEvents.InitEvents(_model, this, _picViewer);
			_picViewer.Source = _model.ViewerBitmap;
			if(IsGenesisViewer) {
				Title = ResourceHelper.GetViewLabel(nameof(TilemapViewerWindow), "wndGenesisTitle");
				_picViewer.PositionClicked += (_, e) => {
					if(e.Properties.IsLeftButtonPressed && _model.SelectGenesisSprite(e.Position)) {
						e.Handled = true;
					}
				};
			}
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}

		protected override void OnOpened(EventArgs e)
		{
			base.OnOpened(e);

			if(Design.IsDesignMode) {
				return;
			}

			_model.RefreshData();
			if(IsGenesisViewer) {
				GenesisViewer.StartRefreshTimer(this, _model.Config.RefreshTiming, _model.RefreshData);
			}
		}

		protected override void OnClosing(WindowClosingEventArgs e)
		{
			base.OnClosing(e);
			_model.Config.SaveWindowSettings(this);
			ConfigManager.Config.Debug.TilemapViewer = _model.Config;
		}

		private void OnSettingsClick(object sender, RoutedEventArgs e)
		{
			_model.Config.ShowSettingsPanel = !_model.Config.ShowSettingsPanel;
		}

		public void ProcessNotification(NotificationEventArgs e)
		{
			if(_model.RefreshTiming is { } timing) {
				ToolRefreshHelper.ProcessNotification(this, e, timing, _model, _model.RefreshData);
			}
		}
	}
}
