using Avalonia.Controls;
using Avalonia.Threading;
using Mesen.Config;
using Mesen.Interop;
using Mesen.Utilities;
using System;
using System.Linq;
using System.Threading;

namespace Mesen.Debugger.Utilities
{
	internal static class GenesisViewer
	{
		public static void Open<T>(Window parent, Func<T> create, Func<T, bool> isGenesisViewer) where T : Window
		{
			var viewer = ApplicationHelper.GetOpenedWindows().OfType<T>().FirstOrDefault(isGenesisViewer);
			if(viewer == null) {
				create().ShowCentered(parent);
			} else {
				viewer.BringToFront();
			}
		}

		public static void StartRefreshTimer(Window window, RefreshTimingConfig config, Action refresh, Func<bool>? hasSnapshot = null)
		{
			bool wasPaused = EmuApi.IsPaused();
			int completedFrame = 0;
			if(hasSnapshot != null) {
				WatchSnapshotChanges(window, config, refresh, () => Interlocked.Exchange(ref completedFrame, 1));
			}

			var timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(100) };
			timer.Tick += (_, _) => {
				if(!EmuApi.IsRunning() || EmuApi.GetRomInfo().ConsoleType != ConsoleType.Genesis) {
					window.Close();
					return;
				}

				bool paused = EmuApi.IsPaused();
				bool shouldRefresh = paused
					? hasSnapshot == null && !wasPaused && config.RefreshOnBreakPause
					: config.AutoRefresh;
				bool frameCompleted = Interlocked.Exchange(ref completedFrame, 0) != 0;
				if(shouldRefresh || (frameCompleted && hasSnapshot?.Invoke() == false)) {
					refresh();
				}
				wasPaused = paused;
			};
			window.Closed += (_, _) => timer.Stop();
			timer.Start();
		}

		private static void WatchSnapshotChanges(Window window, RefreshTimingConfig config, Action refresh, Action notifyFrameCompleted)
		{
			bool closed = false;
			var listener = new NotificationListener();
			listener.OnNotification += e => {
				var type = e.NotificationType;
				if(type == ConsoleNotificationType.PpuFrameDone) {
					notifyFrameCompleted();
				} else if(type is ConsoleNotificationType.GameLoaded or ConsoleNotificationType.GameReset or ConsoleNotificationType.StateLoaded or ConsoleNotificationType.GamePaused) {
					Dispatcher.UIThread.Post(() => {
						if(closed || (type == ConsoleNotificationType.GamePaused && !config.RefreshOnBreakPause)) {
							return;
						}
						if(EmuApi.IsRunning() && EmuApi.GetRomInfo().ConsoleType == ConsoleType.Genesis) {
							refresh();
						}
					});
				}
			};
			window.Closed += (_, _) => {
				closed = true;
				listener.Dispose();
			};
		}
	}
}
