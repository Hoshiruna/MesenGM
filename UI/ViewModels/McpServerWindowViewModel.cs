using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Config;
using Mesen.Debugger.Utilities;
using Mesen.Localization;
using System;

namespace Mesen.ViewModels
{
	public partial class McpServerWindowViewModel : DisposableViewModel
	{
		[ObservableProperty] public partial int Port { get; set; }
		[ObservableProperty] public partial bool IsRunning { get; private set; }
		[ObservableProperty] public partial string ServerUrl { get; private set; } = "";
		[ObservableProperty] public partial string Status { get; private set; } = "";
		[ObservableProperty] public partial bool CanEditPort { get; private set; }
		[ObservableProperty] public partial bool CanStart { get; private set; }
		[ObservableProperty] public partial bool CanStop { get; private set; }

		public McpServerWindowViewModel()
		{
			Port = ConfigManager.Config.McpServer.Port;
			McpServerManager.StateChanged += McpServerManager_StateChanged;
			RefreshDerivedState();
		}

		public bool TryStart(out string error)
		{
			error = "";
			if(Port < 1 || Port > UInt16.MaxValue) {
				error = ResourceHelper.GetViewLabel("McpServerWindow", "errorInvalidPort");
				return false;
			}

			if(!McpServerManager.TryStart((ushort)Port, out error)) {
				RefreshDerivedState();
				return false;
			}

			ConfigManager.Config.McpServer.Port = (ushort)Port;
			ConfigManager.Config.Save();
			RefreshDerivedState();
			return true;
		}

		public void Stop()
		{
			McpServerManager.Stop();
			RefreshDerivedState();
		}

		partial void OnPortChanged(int value)
		{
			RefreshDerivedState();
		}

		protected override void DisposeView()
		{
			McpServerManager.StateChanged -= McpServerManager_StateChanged;
		}

		private void McpServerManager_StateChanged(object? sender, EventArgs e)
		{
			Dispatcher.UIThread.Post(RefreshDerivedState);
		}

		private void RefreshDerivedState()
		{
			IsRunning = McpServerManager.IsRunning;
			ServerUrl = $"http://127.0.0.1:{Port}/mcp/";
			Status = IsRunning
				? string.Format(ResourceHelper.GetViewLabel("McpServerWindow", "statusRunning"), McpServerManager.ServerUrl)
				: ResourceHelper.GetViewLabel("McpServerWindow", "statusStopped");
			CanEditPort = !IsRunning;
			CanStart = !IsRunning;
			CanStop = IsRunning;
		}
	}
}
