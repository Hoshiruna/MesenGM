using System;
using System.Text;
using System.Text.Json.Nodes;

namespace Mesen.Debugger.Utilities
{
	// Tracks one MCP connection at the named-pipe boundary shared by HTTP and
	// stdio clients. Request arguments and response bodies are never logged.
	internal sealed class McpClientSession
	{
		private const int MaxLogValueLength = 80;

		private readonly McpDebugService _service;
		private readonly Action<string> _writeLog;
		private string _clientName = "unknown";

		public McpClientSession(McpDebugService service, Action<string> writeLog)
		{
			_service = service;
			_writeLog = writeLog;
		}

		public void LogConnected()
		{
			WriteConnectionLog("client-connected");
		}

		public void LogDisconnected()
		{
			WriteConnectionLog("client-disconnected");
		}

		public string HandleRequest(string body)
		{
			JsonObject? request = ParseObject(body);
			string method = GetString(request?["method"]);
			if(method == "initialize") {
				string clientName = GetString(request?["params"]?["clientInfo"]?["name"]);
				if(!string.IsNullOrWhiteSpace(clientName)) {
					_clientName = SanitizeLogValue(clientName);
				}
			}

			string response = _service.HandleRequest(body);
			string status = IsErrorResponse(response) ? "request-failed" : "request-complete";
			string toolName = method == "tools/call" ? GetString(request?["params"]?["name"]) : "";
			WriteRequestLog(status, method, toolName);
			return response;
		}

		private void WriteConnectionLog(string status)
		{
			_writeLog($"[MCP] status={status} client={_clientName}");
		}

		private void WriteRequestLog(string status, string method, string toolName)
		{
			StringBuilder line = new();
			line.Append("[MCP] status=");
			line.Append(status);
			line.Append(" client=");
			line.Append(_clientName);
			line.Append(" method=");
			line.Append(SanitizeLogValue(method));
			if(!string.IsNullOrWhiteSpace(toolName)) {
				line.Append(" tool=");
				line.Append(SanitizeLogValue(toolName));
			}
			_writeLog(line.ToString());
		}

		private static JsonObject? ParseObject(string json)
		{
			try {
				return JsonNode.Parse(json) as JsonObject;
			} catch {
				return null;
			}
		}

		private static string GetString(JsonNode? value)
		{
			return value is JsonValue jsonValue && jsonValue.TryGetValue(out string? text) ? text ?? "" : "";
		}

		private static bool IsErrorResponse(string response)
		{
			JsonObject? value = ParseObject(response);
			if(value == null || value["error"] != null) {
				return true;
			}

			JsonNode? isError = value["result"]?["isError"];
			return isError is JsonValue jsonValue && jsonValue.TryGetValue(out bool result) && result;
		}

		private static string SanitizeLogValue(string value)
		{
			StringBuilder sanitized = new(Math.Min(value.Length, MaxLogValueLength));
			foreach(char character in value) {
				if(sanitized.Length >= MaxLogValueLength) {
					break;
				}
				if(char.IsLetterOrDigit(character) || character is '/' or '_' or '-' or '.') {
					sanitized.Append(character);
				} else {
					sanitized.Append('_');
				}
			}
			return sanitized.Length > 0 ? sanitized.ToString() : "unknown";
		}
	}
}
