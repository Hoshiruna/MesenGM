using Mesen.Config;
using Mesen.Debugger.Utilities;
using Mesen.Interop;
using Mesen.Localization;
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace Mesen.Utilities;

internal static class TestRunner
{
	private const int Failure = -1;
	private const int StdErrorHandle = -12;
	private const uint AttachParentProcess = uint.MaxValue;

	[DllImport("kernel32.dll", ExactSpelling = true)]
	private static extern IntPtr GetStdHandle(int standardHandle);

	[DllImport("kernel32.dll", ExactSpelling = true)]
	[return: MarshalAs(UnmanagedType.Bool)]
	private static extern bool AttachConsole(uint processId);

	internal static int Run(string[] args)
	{
		if(OperatingSystem.IsWindows()) {
			IntPtr standardError = GetStdHandle(StdErrorHandle);
			if(standardError == IntPtr.Zero || standardError == new IntPtr(-1)) {
				AttachConsole(AttachParentProcess);
			}
		}

		try {
			string[] runnerArgs = CommandLineHelper.ExtractTestRunnerDataFolder(args, out string? dataFolder);
			ConfigManager.DisableSaveSettings = true;
			if(dataFolder != null) {
				ConfigManager.UseTestRunnerDataFolder(dataFolder);
			}
			string homeFolder = ConfigManager.HomeFolder;
			Environment.CurrentDirectory = homeFolder;
			ResourceHelper.LoadResources();
			CommandLineHelper commandLine = new(runnerArgs, forStartup: true, forTestRunner: true);
			if(commandLine.ErrorMessages.Count > 0) {
				foreach(string error in commandLine.ErrorMessages) {
					ReportFailure(error);
				}
				return Failure;
			}

			if(commandLine.FilesToLoad.Count != 1) {
				return ReportFailure("Specify exactly one ROM.");
			}

			DependencyHelper.ExtractNativeDependencies(homeFolder);
			return RunTest(commandLine, homeFolder);
		} catch(Exception ex) {
			return ReportFailure(ex.Message);
		}
	}

	private static int RunTest(CommandLineHelper commandLine, string homeFolder)
	{
		string? scriptError = null;
		DebugApi.ScriptErrorCallback onScriptError = (name, message) =>
			Interlocked.CompareExchange(ref scriptError, $"Lua script '{name}': {message}", null);

		EmuApi.InitDll();
		try {
			if(commandLine.EnableStdout) {
				ConfigApi.SetEmulationFlag(EmulationFlags.OutputToStdout, true);
			}

			ConfigManager.Config.ApplyConfig();
			EmuApi.InitializeEmu(homeFolder, IntPtr.Zero, IntPtr.Zero, true, true, true, true);
			EmuApi.Pause();
			ConfigApi.SetEmulationFlag(EmulationFlags.ConsoleMode, true);

			string rom = commandLine.FilesToLoad[0];
			if(!EmuApi.LoadRom(rom, string.Empty)) {
				return ReportFailure($"Could not load ROM '{rom}'.");
			}

			DebugWorkspaceManager.Load();
			DebugApi.SetScriptErrorCallback(onScriptError);
			foreach(string luaScript in commandLine.LuaScriptsToLoad) {
				string script;
				try {
					script = File.ReadAllText(luaScript);
				} catch(Exception ex) when(ex is IOException or UnauthorizedAccessException) {
					return ReportFailure($"Could not read Lua script '{luaScript}': {ex.Message}");
				}

				string path = (Path.GetDirectoryName(luaScript) ?? Program.OriginalFolder) + Path.DirectorySeparatorChar;
				int scriptId = DebugApi.LoadTestScript(luaScript, path, script, out bool loaded);
				string? error = Volatile.Read(ref scriptError);
				if(scriptId <= 0 || !loaded || error != null) {
					return ReportFailure(error ?? $"Could not load Lua script '{luaScript}'.");
				}
				if(EmuApi.IsStopRequested()) {
					return EmuApi.GetStopCode();
				}
			}

			ConfigApi.SetEmulationFlag(EmulationFlags.MaximumSpeed, true);
			EmuApi.Resume();

			TimeSpan timeout = TimeSpan.FromSeconds(commandLine.TestRunnerTimeout);
			Stopwatch stopwatch = Stopwatch.StartNew();
			while(true) {
				bool stopped = EmuApi.IsStopRequested() || !EmuApi.IsRunning();
				string? error = Volatile.Read(ref scriptError);
				if(error != null) {
					return ReportFailure(error);
				}
				if(stopped) {
					return EmuApi.GetStopCode();
				}

				TimeSpan remaining = timeout - stopwatch.Elapsed;
				if(remaining <= TimeSpan.Zero) {
					return ReportFailure($"Timed out after {commandLine.TestRunnerTimeout} seconds.");
				}
				Thread.Sleep((int)Math.Clamp(Math.Ceiling(remaining.TotalMilliseconds), 1, 100));
			}
		} finally {
			EmuApi.Release();
			GC.KeepAlive(onScriptError);
		}
	}

	private static int ReportFailure(string message)
	{
		Console.Error.WriteLine($"--testRunner: {message}");
		return Failure;
	}
}
