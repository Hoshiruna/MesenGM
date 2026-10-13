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
		[ObservableProperty] public partial bool ShowConsoleWindow { get; set; }
		[ObservableProperty] public partial bool IsRunning { get; private set; }
		[ObservableProperty] public partial string ServerUrl { get; private set; } = "";
		[ObservableProperty] public partial string Status { get; private set; } = "";
		[ObservableProperty] public partial string Log { get; private set; } = "";
		[ObservableProperty] public partial bool CanEditPort { get; private set; }
		[ObservableProperty] public partial bool CanStart { get; private set; }
		[ObservableProperty] public partial bool CanStop { get; private set; }

		public McpServerWindowViewModel()
		{
			Port = ConfigManager.Config.McpServer.Port;
			ShowConsoleWindow = ConfigManager.Config.McpServer.ShowConsoleWindow;
			McpServerManager.StateChanged += McpServerManager_StateChanged;
			McpServerManager.LogReceived += McpServerManager_LogReceived;
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

		partial void OnShowConsoleWindowChanged(bool value)
		{
			ConfigManager.Config.McpServer.ShowConsoleWindow = value;
			ConfigManager.Config.Save();
		}

		protected override void DisposeView()
		{
			McpServerManager.StateChanged -= McpServerManager_StateChanged;
			McpServerManager.LogReceived -= McpServerManager_LogReceived;
		}

		private void McpServerManager_StateChanged(object? sender, EventArgs e)
		{
			Dispatcher.UIThread.Post(RefreshDerivedState);
		}

		private void McpServerManager_LogReceived(object? sender, string line)
		{
			Dispatcher.UIThread.Post(RefreshLog);
		}

		private void RefreshDerivedState()
		{
			IsRunning = McpServerManager.IsRunning;
			ServerUrl = $"http://127.0.0.1:{Port}/mcp/";
			Status = GetStatusText();
			CanEditPort = !IsRunning;
			CanStart = !IsRunning;
			CanStop = IsRunning;
			RefreshLog();
		}

		private string GetStatusText()
		{
			if(!IsRunning) {
				return ResourceHelper.GetViewLabel("McpServerWindow", "statusStopped");
			}
			if(!McpServerManager.IsListening) {
				return ResourceHelper.GetViewLabel("McpServerWindow", "statusStarting");
			}

			string label = McpServerManager.IsPipeConnected ? "statusRunning" : "statusRunningNoClient";
			return string.Format(ResourceHelper.GetViewLabel("McpServerWindow", label), McpServerManager.ServerUrl);
		}

		private void RefreshLog()
		{
			Log = McpServerManager.GetLogText();
		}
	}
}
