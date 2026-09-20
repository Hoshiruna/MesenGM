using System;
using System.Collections.Generic;
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
		private readonly List<NamedPipeServerStream> _connections = new();
		private Thread? _thread;
		private NamedPipeServerStream? _pendingPipe;
		private volatile bool _running;

		public event EventHandler<string>? LogReceived;

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
			NamedPipeServerStream[] connections;
			lock(_lock) {
				if(!_running) {
					return;
				}

				_running = false;
				_pendingPipe?.Dispose();
				_pendingPipe = null;
				connections = _connections.ToArray();
				_connections.Clear();
				thread = _thread;
				_thread = null;
			}

			foreach(NamedPipeServerStream connection in connections) {
				try {
					connection.Dispose();
				} catch(Exception) {
				}
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
					// Several bridges can be connected at once (one HTTP bridge started
					// from Mesen plus one stdio bridge per MCP client), so each client
					// gets its own pipe instance and handler thread.
					pipe = new NamedPipeServerStream(
						PipeName,
						PipeDirection.InOut,
						NamedPipeServerStream.MaxAllowedServerInstances,
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
						_pendingPipe = pipe;
					}

					pipe.WaitForConnection();

					NamedPipeServerStream connection = pipe;
					pipe = null;
					lock(_lock) {
						_pendingPipe = null;
						if(!_running) {
							connection.Dispose();
							return;
						}
						_connections.Add(connection);
					}

					new Thread(() => HandleConnection(connection)) {
						IsBackground = true,
						Name = nameof(DebugPipeServer) + "Client"
					}.Start();
				} catch(IOException) when(!_running) {
				} catch(ObjectDisposedException) when(!_running) {
				} catch(Exception ex) {
					Console.Error.WriteLine($"[MCP] Named-pipe server error: {ex.Message}");
					Thread.Sleep(100);
				} finally {
					if(pipe != null) {
						lock(_lock) {
							if(ReferenceEquals(_pendingPipe, pipe)) {
								_pendingPipe = null;
							}
						}
						pipe.Dispose();
					}
				}
			}
		}

		private void HandleConnection(NamedPipeServerStream pipe)
		{
			McpClientSession session = new(_service, WriteLog);
			session.LogConnected();
			try {
				using StreamReader reader = new(pipe, new UTF8Encoding(false), false, 65536, true);
				using StreamWriter writer = new(pipe, new UTF8Encoding(false), 65536, true) { AutoFlush = true };

				string? line;
				while(_running && (line = reader.ReadLine()) != null) {
					writer.WriteLine(session.HandleRequest(line));
				}
			} catch(IOException) {
			} catch(ObjectDisposedException) {
			} catch(Exception ex) {
				Console.Error.WriteLine($"[MCP] Named-pipe client error: {ex.Message}");
			} finally {
				session.LogDisconnected();
				lock(_lock) {
					_connections.Remove(pipe);
				}
				try {
					pipe.Dispose();
				} catch(Exception) {
				}
			}
		}

		private void WriteLog(string line)
		{
			LogReceived?.Invoke(this, line);
		}
	}
}
