// Standalone MCP transport bridge for Mesen.
//
// Architecture:
//   Mesen.exe <-> named pipe "MesenDebug" <-> MCPServer.exe <-> MCP client
//
// MCP protocol handling lives in the C# process. This executable only adapts
// the named-pipe protocol to standard MCP stdio or Streamable HTTP transports.
//
// The bridge never blocks a client forever: it connects to the pipe lazily,
// bounds every pipe operation with a timeout, and answers "initialize" (plus a
// cached "tools/list") on its own when Mesen is closed. That keeps a client's
// session alive so later calls reconnect instead of failing for good.
//
// All diagnostics go to stderr. In stdio mode stdout carries the protocol, and
// Mesen captures stderr to show the bridge log in its MCP Server window.

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
	#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

#include <fcntl.h>
#include <io.h>

#pragma comment(lib, "ws2_32.lib")

namespace
{
	constexpr const char* PipeName = R"(\\.\pipe\MesenDebug)";
	constexpr size_t MaxHeaderSize = 64 * 1024;
	constexpr size_t MaxBodySize = 1024 * 1024;
	constexpr DWORD PipeConnectTimeoutMs = 3000;
	constexpr DWORD PipeRequestTimeoutMs = 20000;
	constexpr DWORD PipeCancelTimeoutMs = 2000;

	int g_port = 51234;
	bool g_stdioMode = false;
	DWORD g_parentPid = 0;
	HANDLE g_parentProcess = nullptr;
	HANDLE g_parentWatcherThread = nullptr;
	HANDLE g_pipe = INVALID_HANDLE_VALUE;
	HANDLE g_pipeEvent = nullptr;
	CRITICAL_SECTION g_pipeLock;
	std::string g_readBuffer;

	std::string Trim(const std::string& value)
	{
		size_t start = 0;
		while(start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
			start++;
		}

		size_t end = value.size();
		while(end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
			end--;
		}

		return value.substr(start, end - start);
	}

	std::string ToLower(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		return value;
	}

	std::string GetExecutablePath()
	{
		char path[MAX_PATH] = {};
		DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
		if(length == 0 || length >= MAX_PATH) {
			return "path\\to\\MCPServer.exe";
		}
		return std::string(path, length);
	}

	DWORD WINAPI ParentWatcherThreadProc(LPVOID)
	{
		if(g_parentProcess != nullptr) {
			WaitForSingleObject(g_parentProcess, INFINITE);
			ExitProcess(0);
		}
		return 0;
	}

	void StopParentWatcher()
	{
		if(g_parentWatcherThread != nullptr) {
			CloseHandle(g_parentWatcherThread);
			g_parentWatcherThread = nullptr;
		}
		if(g_parentProcess != nullptr) {
			CloseHandle(g_parentProcess);
			g_parentProcess = nullptr;
		}
	}

	bool StartParentWatcher()
	{
		if(g_parentPid == 0) {
			return true;
		}

		g_parentProcess = OpenProcess(SYNCHRONIZE, FALSE, g_parentPid);
		if(g_parentProcess == nullptr) {
			fprintf(stderr, "[MCPServer] status=failed reason=parent-process-%lu-not-found\n", static_cast<unsigned long>(g_parentPid));
			return false;
		}

		g_parentWatcherThread = CreateThread(nullptr, 0, ParentWatcherThreadProc, nullptr, 0, nullptr);
		if(g_parentWatcherThread == nullptr) {
			fprintf(stderr, "[MCPServer] status=failed reason=parent-watcher-thread\n");
			StopParentWatcher();
			return false;
		}
		return true;
	}

	//
	// Minimal JSON scanning. The bridge does not interpret MCP payloads. It only
	// needs the request id (to echo it in transport errors), the method name (to
	// pick a fallback), and the result object of a tools/list response (to cache).
	//

	size_t SkipWhitespace(const std::string& body, size_t position)
	{
		while(position < body.size() && std::isspace(static_cast<unsigned char>(body[position]))) {
			position++;
		}
		return position;
	}

	// Returns the exact source text of the JSON value that starts at position.
	std::string ReadJsonValue(const std::string& body, size_t position)
	{
		position = SkipWhitespace(body, position);
		if(position >= body.size()) {
			return "";
		}

		size_t start = position;
		char first = body[position];
		if(first == '"' || first == '{' || first == '[') {
			int depth = 0;
			bool inString = false;
			bool escaped = false;
			for(; position < body.size(); position++) {
				char value = body[position];
				if(inString) {
					if(escaped) {
						escaped = false;
					} else if(value == '\\') {
						escaped = true;
					} else if(value == '"') {
						inString = false;
						if(depth == 0) {
							return body.substr(start, position - start + 1);
						}
					}
					continue;
				}

				if(value == '"') {
					inString = true;
				} else if(value == '{' || value == '[') {
					depth++;
				} else if(value == '}' || value == ']') {
					depth--;
					if(depth == 0) {
						return body.substr(start, position - start + 1);
					}
				}
			}
			return "";
		}

		while(position < body.size() && body[position] != ',' && body[position] != '}' && body[position] != ']' &&
			!std::isspace(static_cast<unsigned char>(body[position]))) {
			position++;
		}
		return body.substr(start, position - start);
	}

	// Finds key at the requested object depth (1 is the top-level object) and
	// returns its raw value text. A negative depth matches at any depth.
	std::string ExtractValue(const std::string& body, const std::string& key, int requiredDepth)
	{
		std::string quoted = "\"" + key + "\"";
		int depth = 0;
		bool inString = false;
		bool escaped = false;

		for(size_t index = 0; index < body.size(); index++) {
			char value = body[index];
			if(inString) {
				if(escaped) {
					escaped = false;
				} else if(value == '\\') {
					escaped = true;
				} else if(value == '"') {
					inString = false;
				}
				continue;
			}

			if(value == '{' || value == '[') {
				depth++;
			} else if(value == '}' || value == ']') {
				depth--;
			} else if(value == '"') {
				if((requiredDepth < 0 || depth == requiredDepth) && body.compare(index, quoted.size(), quoted) == 0) {
					size_t position = SkipWhitespace(body, index + quoted.size());
					if(position < body.size() && body[position] == ':') {
						return ReadJsonValue(body, position + 1);
					}
				}
				inString = true;
			}
		}
		return "";
	}

	std::string Unquote(const std::string& value)
	{
		if(value.size() >= 2 && value.front() == '"' && value.back() == '"') {
			return value.substr(1, value.size() - 2);
		}
		return value;
	}

	std::string GetRequestId(const std::string& body)
	{
		std::string id = ExtractValue(body, "id", 1);
		return id.empty() ? "null" : id;
	}

	std::string MakeTransportError(const std::string& body, const char* message)
	{
		return R"({"jsonrpc":"2.0","id":)" + GetRequestId(body) +
			R"(,"error":{"code":-32603,"message":")" + std::string(message) + R"("}})";
	}

	// Answered without Mesen so a client that starts before the emulator still
	// completes its handshake and can retry tool calls later in the same session.
	std::string MakeLocalInitializeResponse(const std::string& body)
	{
		std::string requested = ExtractValue(body, "protocolVersion", -1);
		if(requested != R"("2024-11-05")" && requested != R"("2025-03-26")") {
			requested = R"("2025-06-18")";
		}

		return R"({"jsonrpc":"2.0","id":)" + GetRequestId(body) + R"(,"result":{"protocolVersion":)" + requested +
			R"(,"capabilities":{"tools":{"listChanged":false}})"
			R"(,"serverInfo":{"name":"Mesen2-MCP","version":"1.2.0"})"
			R"(,"instructions":"Mesen is not running yet. Ask the user to open Mesen and load a ROM, then call get_rom_info to confirm the connection."}})";
	}

	//
	// tools/list cache. Mesen owns the tool definitions, so the bridge only stores
	// the last result object it saw and replays it when the emulator is closed.
	//

	std::string GetToolsCachePath()
	{
		char folder[MAX_PATH] = {};
		DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", folder, MAX_PATH);
		if(length == 0 || length >= MAX_PATH) {
			return "";
		}

		std::string path = std::string(folder, length) + "\\Mesen2";
		CreateDirectoryA(path.c_str(), nullptr);
		return path + "\\mcp-tools.cache.json";
	}

	void SaveToolsCache(const std::string& result)
	{
		std::string path = GetToolsCachePath();
		if(path.empty()) {
			return;
		}

		FILE* file = nullptr;
		if(fopen_s(&file, path.c_str(), "wb") == 0 && file != nullptr) {
			fwrite(result.data(), 1, result.size(), file);
			fclose(file);
		}
	}

	std::string LoadToolsCache()
	{
		std::string path = GetToolsCachePath();
		if(path.empty()) {
			return "";
		}

		FILE* file = nullptr;
		if(fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) {
			return "";
		}

		std::string content;
		char buffer[8192];
		size_t read = 0;
		while((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
			content.append(buffer, read);
			if(content.size() > MaxBodySize) {
				content.clear();
				break;
			}
		}
		fclose(file);
		return Trim(content);
	}

	//
	// Named pipe. Opened overlapped so a wedged emulator cannot hold a client
	// request open forever. Every entry point runs under g_pipeLock.
	//

	bool PipeIsOpen()
	{
		return g_pipe != nullptr && g_pipe != INVALID_HANDLE_VALUE;
	}

	void PipeClose()
	{
		if(PipeIsOpen()) {
			CancelIoEx(g_pipe, nullptr);
			CloseHandle(g_pipe);
		}
		g_pipe = INVALID_HANDLE_VALUE;
		g_readBuffer.clear();
	}

	bool PipeTryConnect(DWORD timeoutMs)
	{
		PipeClose();

		ULONGLONG deadline = GetTickCount64() + timeoutMs;
		while(true) {
			g_pipe = CreateFileA(PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
			if(PipeIsOpen()) {
				g_readBuffer.clear();
				fprintf(stderr, "[MCPServer] status=pipe-connected\n");
				return true;
			}

			DWORD error = GetLastError();
			ULONGLONG now = GetTickCount64();
			if(now >= deadline) {
				g_pipe = INVALID_HANDLE_VALUE;
				fprintf(stderr, "[MCPServer] status=mesen-unavailable error=%lu\n", static_cast<unsigned long>(error));
				return false;
			}

			DWORD remaining = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 500));
			if(error == ERROR_PIPE_BUSY) {
				WaitNamedPipeA(PipeName, remaining);
			} else {
				Sleep(std::min<DWORD>(remaining, 200));
			}
		}
	}

	bool WaitForOverlapped(OVERLAPPED& overlapped, DWORD timeoutMs, DWORD& transferred)
	{
		if(WaitForSingleObject(g_pipeEvent, timeoutMs) != WAIT_OBJECT_0) {
			CancelIoEx(g_pipe, &overlapped);
			// Wait for the cancelled operation to finish so the OVERLAPPED
			// structure is not written after it leaves scope.
			WaitForSingleObject(g_pipeEvent, PipeCancelTimeoutMs);
			GetOverlappedResult(g_pipe, &overlapped, &transferred, TRUE);
			return false;
		}
		return GetOverlappedResult(g_pipe, &overlapped, &transferred, FALSE) != FALSE;
	}

	bool PipeWriteAll(const char* data, size_t length, DWORD timeoutMs)
	{
		while(length > 0) {
			OVERLAPPED overlapped {};
			overlapped.hEvent = g_pipeEvent;
			ResetEvent(g_pipeEvent);

			DWORD chunk = static_cast<DWORD>(std::min(length, static_cast<size_t>(64 * 1024)));
			DWORD written = 0;
			if(!WriteFile(g_pipe, data, chunk, &written, &overlapped)) {
				if(GetLastError() != ERROR_IO_PENDING || !WaitForOverlapped(overlapped, timeoutMs, written)) {
					return false;
				}
			}
			if(written == 0) {
				return false;
			}

			data += written;
			length -= written;
		}
		return true;
	}

	bool PipeReadLine(std::string& output, DWORD timeoutMs)
	{
		output.clear();
		while(true) {
			size_t newline = g_readBuffer.find('\n');
			if(newline != std::string::npos) {
				output = g_readBuffer.substr(0, newline);
				g_readBuffer.erase(0, newline + 1);
				if(!output.empty() && output.back() == '\r') {
					output.pop_back();
				}
				return true;
			}
			if(g_readBuffer.size() > MaxBodySize) {
				return false;
			}

			OVERLAPPED overlapped {};
			overlapped.hEvent = g_pipeEvent;
			ResetEvent(g_pipeEvent);

			char buffer[8192];
			DWORD read = 0;
			if(!ReadFile(g_pipe, buffer, static_cast<DWORD>(sizeof(buffer)), &read, &overlapped)) {
				if(GetLastError() != ERROR_IO_PENDING || !WaitForOverlapped(overlapped, timeoutMs, read)) {
					return false;
				}
			}
			if(read == 0) {
				return false;
			}
			g_readBuffer.append(buffer, read);
		}
	}

	// Forwards one JSON-RPC message to Mesen. Never returns an empty string:
	// "{}" means "do not answer the client".
	std::string PipeRequest(const std::string& body)
	{
		std::string method = Unquote(ExtractValue(body, "method", 1));
		bool notification = ExtractValue(body, "id", 1).empty();
		std::string response;
		bool connected = false;

		EnterCriticalSection(&g_pipeLock);

		for(int attempt = 0; attempt < 2 && response.empty(); attempt++) {
			if(!PipeIsOpen() && !PipeTryConnect(PipeConnectTimeoutMs)) {
				break;
			}
			connected = true;

			std::string message;
			message.reserve(body.size() + 1);
			for(char value : body) {
				if(value != '\n' && value != '\r') {
					message += value;
				}
			}
			message += '\n';

			if(!PipeWriteAll(message.data(), message.size(), PipeRequestTimeoutMs)) {
				PipeClose();
				continue;
			}

			// Mesen answers every message, including notifications ("{}"), so the
			// reply is always consumed to keep the pipe in sync.
			if(!PipeReadLine(response, PipeRequestTimeoutMs)) {
				PipeClose();
				response.clear();
				continue;
			}
		}

		if(!response.empty() && method == "tools/list") {
			std::string result = ExtractValue(response, "result", 1);
			if(!result.empty()) {
				SaveToolsCache(result);
			}
		}

		LeaveCriticalSection(&g_pipeLock);

		if(!response.empty()) {
			return response;
		}
		if(notification) {
			return "{}";
		}
		if(method == "initialize") {
			fprintf(stderr, "[MCPServer] status=initialize-answered-locally\n");
			return MakeLocalInitializeResponse(body);
		}
		if(method == "tools/list") {
			std::string cached = LoadToolsCache();
			if(!cached.empty()) {
				fprintf(stderr, "[MCPServer] status=tools-list-from-cache\n");
				return R"({"jsonrpc":"2.0","id":)" + GetRequestId(body) + R"(,"result":)" + cached + "}";
			}
		}
		return MakeTransportError(body, connected
			? "Mesen stopped responding. Check the MCP Server window in Mesen, then try again."
			: "Mesen is not running. Open Mesen, load a ROM, then try again.");
	}

	bool ReadStdioMessage(std::string& output)
	{
		while(std::getline(std::cin, output)) {
			if(!output.empty() && output.back() == '\r') {
				output.pop_back();
			}
			if(!output.empty()) {
				return true;
			}
		}
		return false;
	}

	void WriteStdioMessage(const std::string& json)
	{
		std::cout << json << '\n';
		std::cout.flush();
	}

	int RunStdio()
	{
		_setmode(_fileno(stdin), _O_BINARY);
		_setmode(_fileno(stdout), _O_BINARY);

		// The pipe is opened on the first forwarded message instead of here, so a
		// closed emulator cannot stall the MCP handshake.
		fprintf(stderr, "[MCPServer] status=listening transport=stdio\n");

		std::string request;
		while(ReadStdioMessage(request)) {
			std::string response = PipeRequest(request);
			if(response != "{}") {
				WriteStdioMessage(response);
			}
		}

		PipeClose();
		return 0;
	}

	struct HttpRequest
	{
		std::string method;
		std::string path;
		std::string headers;
		std::string body;
		bool valid = false;
		bool tooLarge = false;
	};

	std::string GetHeaderValue(const std::string& headers, const std::string& requestedName)
	{
		std::string lowerName = ToLower(requestedName);
		size_t position = 0;
		while(position < headers.size()) {
			size_t lineEnd = headers.find("\r\n", position);
			if(lineEnd == std::string::npos) {
				lineEnd = headers.size();
			}

			std::string line = headers.substr(position, lineEnd - position);
			size_t separator = line.find(':');
			if(separator != std::string::npos && ToLower(Trim(line.substr(0, separator))) == lowerName) {
				return Trim(line.substr(separator + 1));
			}

			if(lineEnd == headers.size()) {
				break;
			}
			position = lineEnd + 2;
		}
		return "";
	}

	bool ParseSize(const std::string& text, size_t& value)
	{
		if(text.empty()) {
			value = 0;
			return true;
		}

		char* end = nullptr;
		unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
		if(end == text.c_str() || *end != '\0' || parsed > std::numeric_limits<size_t>::max()) {
			return false;
		}
		value = static_cast<size_t>(parsed);
		return true;
	}

	HttpRequest ReceiveHttpRequest(SOCKET socket)
	{
		HttpRequest request;
		std::string data;
		char buffer[8192];
		size_t headerEnd = std::string::npos;

		while(headerEnd == std::string::npos) {
			int received = recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
			if(received <= 0) {
				return request;
			}
			data.append(buffer, received);
			if(data.size() > MaxHeaderSize) {
				request.tooLarge = true;
				return request;
			}
			headerEnd = data.find("\r\n\r\n");
		}

		size_t requestLineEnd = data.find("\r\n");
		if(requestLineEnd == std::string::npos) {
			return request;
		}
		std::string requestLine = data.substr(0, requestLineEnd);
		size_t firstSpace = requestLine.find(' ');
		size_t secondSpace = requestLine.find(' ', firstSpace == std::string::npos ? 0 : firstSpace + 1);
		if(firstSpace == std::string::npos || secondSpace == std::string::npos) {
			return request;
		}

		request.method = requestLine.substr(0, firstSpace);
		request.path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
		request.headers = data.substr(requestLineEnd + 2, headerEnd - requestLineEnd - 2);

		size_t contentLength = 0;
		if(!ParseSize(GetHeaderValue(request.headers, "Content-Length"), contentLength)) {
			return request;
		}
		if(contentLength > MaxBodySize) {
			request.tooLarge = true;
			return request;
		}

		size_t bodyStart = headerEnd + 4;
		while(data.size() - bodyStart < contentLength) {
			int received = recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
			if(received <= 0) {
				return request;
			}
			data.append(buffer, received);
		}
		request.body = data.substr(bodyStart, contentLength);
		request.valid = true;
		return request;
	}

	bool SendAll(SOCKET socket, const char* data, size_t length)
	{
		while(length > 0) {
			int sent = send(socket, data, static_cast<int>(std::min(length, static_cast<size_t>(INT_MAX))), 0);
			if(sent <= 0) {
				return false;
			}
			data += sent;
			length -= static_cast<size_t>(sent);
		}
		return true;
	}

	const char* GetStatusText(int status)
	{
		switch(status) {
			case 200: return "200 OK";
			case 202: return "202 Accepted";
			case 204: return "204 No Content";
			case 400: return "400 Bad Request";
			case 403: return "403 Forbidden";
			case 404: return "404 Not Found";
			case 405: return "405 Method Not Allowed";
			case 413: return "413 Content Too Large";
			case 415: return "415 Unsupported Media Type";
			default: return "500 Internal Server Error";
		}
	}

	bool IsAllowedOrigin(const std::string& origin)
	{
		if(origin.empty()) {
			return true;
		}

		std::string value = ToLower(Trim(origin));
		constexpr const char* allowedPrefixes[] = {
			"http://127.0.0.1", "https://127.0.0.1", "http://localhost", "https://localhost", "http://[::1]", "https://[::1]"
		};
		for(const char* prefix : allowedPrefixes) {
			size_t length = std::char_traits<char>::length(prefix);
			if(value.compare(0, length, prefix) == 0 &&
				(value.size() == length || value[length] == ':')) {
				return true;
			}
		}
		return false;
	}

	void SendHttp(SOCKET socket, int status, const std::string& body, const std::string& origin = "")
	{
		std::string headers = "HTTP/1.1 " + std::string(GetStatusText(status)) + "\r\n";
		if(!body.empty()) {
			headers += "Content-Type: application/json\r\n";
		}
		if(!origin.empty()) {
			headers += "Access-Control-Allow-Origin: " + origin + "\r\n";
			headers += "Vary: Origin\r\n";
		}
		headers += "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n";
		headers += "Access-Control-Allow-Headers: Content-Type, Accept, MCP-Protocol-Version, Mcp-Session-Id, Last-Event-ID\r\n";
		headers += "Allow: POST, GET, OPTIONS\r\n";
		headers += "Content-Length: " + std::to_string(body.size()) + "\r\n";
		headers += "Connection: close\r\n\r\n";

		SendAll(socket, headers.data(), headers.size());
		if(!body.empty()) {
			SendAll(socket, body.data(), body.size());
		}
	}

	bool IsMcpPath(const std::string& path)
	{
		size_t query = path.find('?');
		std::string cleanPath = query == std::string::npos ? path : path.substr(0, query);
		return cleanPath == "/mcp" || cleanPath == "/mcp/";
	}

	bool IsJsonContentType(const std::string& contentType)
	{
		std::string value = ToLower(contentType);
		size_t parameterStart = value.find(';');
		if(parameterStart != std::string::npos) {
			value = value.substr(0, parameterStart);
		}
		return Trim(value) == "application/json";
	}

	void HandleClient(SOCKET client)
	{
		HttpRequest request = ReceiveHttpRequest(client);
		if(request.tooLarge) {
			SendHttp(client, 413, R"({"error":"Request is too large"})");
			closesocket(client);
			return;
		}
		if(!request.valid) {
			SendHttp(client, 400, R"({"error":"Invalid HTTP request"})");
			closesocket(client);
			return;
		}

		std::string origin = GetHeaderValue(request.headers, "Origin");
		if(!IsAllowedOrigin(origin)) {
			SendHttp(client, 403, R"({"error":"Origin is not allowed"})");
			closesocket(client);
			return;
		}

		if(!IsMcpPath(request.path)) {
			SendHttp(client, 404, R"({"error":"MCP endpoint not found"})", origin);
		} else if(request.method == "OPTIONS") {
			SendHttp(client, 204, "", origin);
		} else if(request.method == "GET") {
			SendHttp(client, 405, R"({"error":"This server does not provide an SSE stream"})", origin);
		} else if(request.method != "POST") {
			SendHttp(client, 405, R"({"error":"Method is not supported"})", origin);
		} else if(!IsJsonContentType(GetHeaderValue(request.headers, "Content-Type"))) {
			SendHttp(client, 415, R"({"error":"Content-Type must be application/json"})", origin);
		} else if(request.body.empty()) {
			SendHttp(client, 400, R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Empty request body"}})", origin);
		} else {
			std::string response = PipeRequest(request.body);
			if(response == "{}") {
				SendHttp(client, 202, "", origin);
			} else {
				SendHttp(client, 200, response, origin);
			}
		}

		closesocket(client);
	}

	bool ParsePort(const char* value, int& port)
	{
		char* end = nullptr;
		long parsed = std::strtol(value, &end, 10);
		if(end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
			return false;
		}
		port = static_cast<int>(parsed);
		return true;
	}

	int RunHttp()
	{
		WSADATA winsockData {};
		int startupResult = WSAStartup(MAKEWORD(2, 2), &winsockData);
		if(startupResult != 0) {
			fprintf(stderr, "[MCPServer] status=failed reason=wsastartup-%d\n", startupResult);
			return 1;
		}

		SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if(server == INVALID_SOCKET) {
			fprintf(stderr, "[MCPServer] status=failed reason=socket-%d\n", WSAGetLastError());
			WSACleanup();
			return 1;
		}

		int reuseAddress = 1;
		setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuseAddress), static_cast<int>(sizeof(reuseAddress)));

		sockaddr_in address {};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<u_short>(g_port));
		inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

		if(bind(server, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) == SOCKET_ERROR ||
			listen(server, SOMAXCONN) == SOCKET_ERROR) {
			fprintf(stderr, "[MCPServer] status=failed reason=listen-port-%d-error-%d\n", g_port, WSAGetLastError());
			closesocket(server);
			WSACleanup();
			return 1;
		}

		// Mesen waits for this line before it reports the bridge as running. The
		// pipe is connected lazily, so listening does not depend on the emulator.
		fprintf(stderr, "[MCPServer] status=listening url=http://127.0.0.1:%d/mcp/\n", g_port);
		fprintf(stderr, "[MCPServer] http: codex mcp add mesen-debugger --url http://127.0.0.1:%d/mcp/\n", g_port);
		fprintf(stderr, "[MCPServer] stdio: codex mcp add mesen-debugger -- \"%s\" --stdio\n", GetExecutablePath().c_str());

		while(true) {
			SOCKET client = accept(server, nullptr, nullptr);
			if(client == INVALID_SOCKET) {
				break;
			}
			std::thread([client]() { HandleClient(client); }).detach();
		}

		closesocket(server);
		PipeClose();
		WSACleanup();
		return 0;
	}
}

int main(int argc, char* argv[])
{
	// stderr carries the bridge log. Keep it unbuffered so Mesen sees each line
	// as it happens instead of only when the process exits.
	setvbuf(stderr, nullptr, _IONBF, 0);

	for(int index = 1; index < argc; index++) {
		std::string argument = argv[index];
		if(argument == "--stdio") {
			g_stdioMode = true;
		} else if(argument == "--parent-pid" && index + 1 < argc) {
			g_parentPid = static_cast<DWORD>(std::strtoul(argv[++index], nullptr, 10));
		} else if(argument == "--help") {
			printf("Usage: MCPServer.exe [port] [--parent-pid PID]\n       MCPServer.exe --stdio\n");
			return 0;
		} else if(!ParsePort(argv[index], g_port)) {
			fprintf(stderr, "[MCPServer] status=failed reason=invalid-port-%s\n", argv[index]);
			return 1;
		}
	}

	if(!StartParentWatcher()) {
		return 1;
	}

	g_pipeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if(g_pipeEvent == nullptr) {
		fprintf(stderr, "[MCPServer] status=failed reason=pipe-event\n");
		StopParentWatcher();
		return 1;
	}
	InitializeCriticalSection(&g_pipeLock);

	fprintf(stderr, "[MCPServer] status=starting transport=%s\n", g_stdioMode ? "stdio" : "http");
	int result = g_stdioMode ? RunStdio() : RunHttp();

	DeleteCriticalSection(&g_pipeLock);
	CloseHandle(g_pipeEvent);
	g_pipeEvent = nullptr;
	StopParentWatcher();
	return result;
}
