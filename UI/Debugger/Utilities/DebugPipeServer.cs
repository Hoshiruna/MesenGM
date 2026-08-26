using System;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading;

namespace Mesen.Debugger.Utilities
{
	public sealed class DebugPipeServer : IDisposable
	{
		private const string PipeName = "MesenDebug";
		private readonly object _lock = new();
		private readonly McpDebugService _service = new();
		private Thread? _thread;
		private NamedPipeServerStream? _currentPipe;
		private volatile bool _running;

		public void Start()
		{
			lock(_lock) {
				if(_running) {
					return;
				}

				_running = true;
				_thread = new Thread(Run) {
					IsBackground = true,
					Name = nameof(DebugPipeServer)
				};
				_thread.Start();
			}
		}

		public void Stop()
		{
			Thread? thread;
			lock(_lock) {
				if(!_running) {
					return;
				}

				_running = false;
				_currentPipe?.Dispose();
				_currentPipe = null;
				thread = _thread;
				_thread = null;
			}

			try {
				thread?.Join(1000);
			} catch(ThreadStateException) {
			}
		}

		public void Dispose()
		{
			Stop();
		}

		private void Run()
		{
			while(_running) {
				NamedPipeServerStream? pipe = null;
				try {
					pipe = new NamedPipeServerStream(
						PipeName,
						PipeDirection.InOut,
						1,
						PipeTransmissionMode.Byte,
						PipeOptions.CurrentUserOnly,
						65536,
						65536
					);
					lock(_lock) {
						if(!_running) {
							pipe.Dispose();
							return;
						}
						_currentPipe = pipe;
					}

					pipe.WaitForConnection();
					using StreamReader reader = new(pipe, new UTF8Encoding(false), false, 65536, true);
					using StreamWriter writer = new(pipe, new UTF8Encoding(false), 65536, true) { AutoFlush = true };

					string? line;
					while(_running && (line = reader.ReadLine()) != null) {
						writer.WriteLine(_service.HandleRequest(line));
					}
				} catch(IOException) when(!_running) {
				} catch(ObjectDisposedException) when(!_running) {
				} catch(Exception ex) {
					Console.Error.WriteLine($"[MCP] Named-pipe server error: {ex.Message}");
					Thread.Sleep(100);
				} finally {
					lock(_lock) {
						if(ReferenceEquals(_currentPipe, pipe)) {
							_currentPipe = null;
						}
					}
					pipe?.Dispose();
				}
			}
		}
	}
}
