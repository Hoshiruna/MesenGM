using Mesen.Config;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;

namespace Mesen.Debugger.Utilities
{
	public static class McpServerManager
	{
		private const int MaxLogLines = 200;
		private const int StartupTimeoutMs = 5000;

		private static readonly object _lock = new();
		private static readonly string _serverExe = Path.Combine(AppContext.BaseDirectory, "MCPServer.exe");
		private static readonly int _parentProcessId = Process.GetCurrentProcess().Id;
		private static readonly Queue<string> _log = new();
		private static DebugPipeServer? _debugPipeServer;
		private static Process? _serverProcess;
		private static ManualResetEventSlim? _readyEvent;
		private static ushort _port = 51234;
		private static bool _isListening;
		private static bool _isPipeConnected;

		public static event EventHandler? StateChanged;
		public static event EventHandler<string>? LogReceived;

		public static ushort Port {
			get {
				lock(_lock) {
					return _port;
				}
			}
		}

		public static string ServerUrl => $"http://127.0.0.1:{Port}/mcp/";

		public static bool IsRunning {
			get {
				lock(_lock) {
					CleanupExitedProcess_NoLock();
					return IsRunning_NoLock();
				}
			}
		}

		/// <summary>True once the bridge reported that it is accepting HTTP requests.</summary>
		public static bool IsListening {
			get {
				lock(_lock) {
					CleanupExitedProcess_NoLock();
					return IsRunning_NoLock() && _isListening;
				}
			}
		}

		/// <summary>True while the bridge holds an open connection to Mesen's debugger pipe.</summary>
		public static bool IsPipeConnected {
			get {
				lock(_lock) {
					CleanupExitedProcess_NoLock();
					return IsRunning_NoLock() && _isPipeConnected;
				}
			}
		}

		public static string GetLogText()
		{
			lock(_lock) {
				return string.Join(Environment.NewLine, _log);
			}
		}

		public static void Initialize()
		{
			if(!OperatingSystem.IsWindows()) {
				return;
			}

			lock(_lock) {
				_debugPipeServer ??= new DebugPipeServer();
				_debugPipeServer.Start();
			}
		}

		public static bool TryStart(ushort port, out string error)
		{
			error = "";
			if(!OperatingSystem.IsWindows()) {
				error = "The MCP bridge is currently available only on Windows.";
				return false;
			}

			Initialize();

			bool showConsole = ConfigManager.Config.McpServer.ShowConsoleWindow;
			Process process;
			ManualResetEventSlim readyEvent;

			lock(_lock) {
				CleanupExitedProcess_NoLock();
				if(IsRunning_NoLock()) {
					if(_port == port) {
						return true;
					}
					error = "The HTTP MCP bridge is already running. Stop it before changing the port.";
					return false;
				}

				if(!File.Exists(_serverExe)) {
					error = $"MCPServer.exe was not found at:\n{_serverExe}";
					return false;
				}
				if(!IsPortAvailable(port)) {
					error = $"Port {port} is already in use on localhost.";
					return false;
				}

				_log.Clear();
				_isListening = false;
				_isPipeConnected = false;
				_readyEvent = readyEvent = new ManualResetEventSlim(false);

				ProcessStartInfo startInfo = new() {
					FileName = _serverExe,
					Arguments = $"{port} --parent-pid {_parentProcessId}",
					WorkingDirectory = AppContext.BaseDirectory,
					UseShellExecute = false,
					// The bridge writes its whole log to stderr. Capturing it is what
					// makes the MCP Server window able to show why a start failed.
					// A visible console and redirected output are mutually exclusive,
					// so the debug option gives up the in-app log.
					RedirectStandardOutput = !showConsole,
					RedirectStandardError = !showConsole,
					CreateNoWindow = !showConsole,
					WindowStyle = showConsole ? ProcessWindowStyle.Normal : ProcessWindowStyle.Hidden
				};

				Process? started;
				try {
					started = Process.Start(startInfo);
				} catch(Exception ex) {
					error = $"Could not start MCPServer.exe: {ex.Message}";
					return false;
				}
				if(started == null) {
					error = "Could not start MCPServer.exe.";
					return false;
				}

				process = started;
				process.EnableRaisingEvents = true;
				process.Exited += ServerProcess_Exited;
				if(!showConsole) {
					process.OutputDataReceived += ServerProcess_OutputDataReceived;
					process.ErrorDataReceived += ServerProcess_OutputDataReceived;
					process.BeginOutputReadLine();
					process.BeginErrorReadLine();
				}

				_serverProcess = process;
				_port = port;
			}

			// Waiting happens outside the lock so the output handlers can append to
			// the log while the bridge starts up.
			bool isListening = showConsole ? !process.WaitForExit(250) : WaitForListening(process, readyEvent);
			if(!isListening) {
				string log = GetLogText();
				Stop();
				error = "MCPServer.exe did not start listening.";
				if(log.Length > 0) {
					error += $"\n\n{log}";
				} else {
					error += "\nThe selected port may be unavailable.";
				}
				return false;
			}

			if(showConsole) {
				// Without captured output there is no readiness line to wait for, so
				// "still alive after 250 ms" is the only signal available.
				lock(_lock) {
					_isListening = true;
				}
			}

			NotifyStateChanged();
			return true;
		}

		public static void Stop()
		{
			Process? process;
			lock(_lock) {
				CleanupExitedProcess_NoLock();
				process = _serverProcess;
				_serverProcess = null;
				_isListening = false;
				_isPipeConnected = false;
				_readyEvent?.Set();
				_readyEvent = null;
				if(process != null) {
					process.Exited -= ServerProcess_Exited;
					process.OutputDataReceived -= ServerProcess_OutputDataReceived;
					process.ErrorDataReceived -= ServerProcess_OutputDataReceived;
				}
			}

			if(process != null) {
				try {
					if(!process.HasExited) {
						process.Kill(true);
						process.WaitForExit(1000);
					}
				} catch(Exception) {
				} finally {
					process.Dispose();
				}
			}
			NotifyStateChanged();
		}

		public static void Shutdown()
		{
			Stop();
			lock(_lock) {
				_debugPipeServer?.Dispose();
				_debugPipeServer = null;
			}
		}

		private static bool WaitForListening(Process process, ManualResetEventSlim readyEvent)
		{
			for(int waited = 0; waited < StartupTimeoutMs; waited += 50) {
				if(readyEvent.Wait(50)) {
					lock(_lock) {
						return _isListening;
					}
				}
				if(process.HasExited) {
					return false;
				}
			}
			lock(_lock) {
				return _isListening;
			}
		}

		private static void ServerProcess_OutputDataReceived(object? sender, DataReceivedEventArgs e)
		{
			AppendLog(e.Data);
		}

		private static void AppendLog(string? line)
		{
			if(string.IsNullOrWhiteSpace(line)) {
				return;
			}

			bool stateChanged = false;
			lock(_lock) {
				_log.Enqueue(line);
				while(_log.Count > MaxLogLines) {
					_log.Dequeue();
				}

				if(line.Contains("status=listening", StringComparison.Ordinal)) {
					_isListening = true;
					stateChanged = true;
					_readyEvent?.Set();
				} else if(line.Contains("status=pipe-connected", StringComparison.Ordinal)) {
					_isPipeConnected = true;
					stateChanged = true;
				} else if(line.Contains("status=mesen-unavailable", StringComparison.Ordinal)) {
					_isPipeConnected = false;
					stateChanged = true;
				}
			}

			LogReceived?.Invoke(null, line);
			if(stateChanged) {
				NotifyStateChanged();
			}
		}

		private static void ServerProcess_Exited(object? sender, EventArgs e)
		{
			lock(_lock) {
				if(ReferenceEquals(_serverProcess, sender)) {
					_isListening = false;
					_isPipeConnected = false;
					_readyEvent?.Set();
					CleanupExitedProcess_NoLock();
				}
			}
			AppendLog("[MCPServer] status=exited");
			NotifyStateChanged();
		}

		private static bool IsPortAvailable(ushort port)
		{
			TcpListener? listener = null;
			try {
				listener = new TcpListener(IPAddress.Loopback, port);
				listener.Start();
				return true;
			} catch(Exception) {
				return false;
			} finally {
				listener?.Stop();
			}
		}

		private static bool IsRunning_NoLock()
		{
			return _serverProcess != null && !_serverProcess.HasExited;
		}

		private static void CleanupExitedProcess_NoLock()
		{
			if(_serverProcess != null && _serverProcess.HasExited) {
				_serverProcess.Exited -= ServerProcess_Exited;
				_serverProcess.OutputDataReceived -= ServerProcess_OutputDataReceived;
				_serverProcess.ErrorDataReceived -= ServerProcess_OutputDataReceived;
				_serverProcess.Dispose();
				_serverProcess = null;
				_isListening = false;
				_isPipeConnected = false;
			}
		}

		private static void NotifyStateChanged()
		{
			StateChanged?.Invoke(null, EventArgs.Empty);
		}
	}
}
