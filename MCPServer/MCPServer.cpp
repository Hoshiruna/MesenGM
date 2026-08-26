// Standalone MCP transport bridge for Mesen.
//
// Architecture:
//   Mesen.exe <-> named pipe "MesenDebug" <-> MCPServer.exe <-> MCP client
//
// MCP protocol handling lives in the C# process. This executable only adapts
// the named-pipe protocol to standard MCP stdio or Streamable HTTP transports.

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
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
	constexpr const char* MutexName = "Local\\MesenMcpServerSingleton";
	constexpr size_t MaxHeaderSize = 64 * 1024;
	constexpr size_t MaxBodySize = 1024 * 1024;

	int g_port = 51234;
	bool g_stdioMode = false;
	DWORD g_parentPid = 0;
	HANDLE g_singletonMutex = nullptr;
	HANDLE g_parentProcess = nullptr;
	HANDLE g_parentWatcherThread = nullptr;
	HANDLE g_pipe = INVALID_HANDLE_VALUE;
	CRITICAL_SECTION g_pipeLock;

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

	bool AcquireSingletonMutex()
	{
		g_singletonMutex = CreateMutexA(nullptr, FALSE, MutexName);
		if(g_singletonMutex == nullptr) {
			fprintf(stderr, "[MCPServer] Failed to create the singleton mutex.\n");
			return false;
		}

		if(GetLastError() == ERROR_ALREADY_EXISTS) {
			fprintf(stderr, "[MCPServer] Another MCP bridge process is already running.\n");
			CloseHandle(g_singletonMutex);
			g_singletonMutex = nullptr;
			return false;
		}
		return true;
	}

	void ReleaseSingletonMutex()
	{
		if(g_singletonMutex != nullptr) {
			CloseHandle(g_singletonMutex);
			g_singletonMutex = nullptr;
		}
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
			fprintf(stderr, "[MCPServer] Failed to open parent process %lu.\n", static_cast<unsigned long>(g_parentPid));
			return false;
		}

		g_parentWatcherThread = CreateThread(nullptr, 0, ParentWatcherThreadProc, nullptr, 0, nullptr);
		if(g_parentWatcherThread == nullptr) {
			fprintf(stderr, "[MCPServer] Failed to start the parent watcher thread.\n");
			StopParentWatcher();
			return false;
		}
		return true;
	}

	bool PipeIsOpen()
	{
		return g_pipe != nullptr && g_pipe != INVALID_HANDLE_VALUE;
	}

	void PipeClose()
	{
		if(PipeIsOpen()) {
			CloseHandle(g_pipe);
		}
		g_pipe = INVALID_HANDLE_VALUE;
	}

	void PipeConnect()
	{
		PipeClose();
		while(true) {
			g_pipe = CreateFileA(PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if(g_pipe != INVALID_HANDLE_VALUE) {
				return;
			}

			DWORD error = GetLastError();
			if(error == ERROR_PIPE_BUSY) {
				WaitNamedPipeA(PipeName, 2000);
			} else {
				fprintf(stderr, "[MCPServer] Waiting for Mesen...\n");
				Sleep(1000);
			}
		}
	}

	bool PipeReadLine(std::string& output)
	{
		output.clear();
		char value = 0;
		DWORD bytesRead = 0;
		while(PipeIsOpen() && ReadFile(g_pipe, &value, 1, &bytesRead, nullptr) && bytesRead == 1) {
			if(value == '\n') {
				return true;
			}
			if(value != '\r') {
				output += value;
			}
		}
		return false;
	}

	std::string PipeRequest(const std::string& json)
	{
		EnterCriticalSection(&g_pipeLock);

		for(int attempt = 0; attempt < 2; attempt++) {
			if(!PipeIsOpen()) {
				PipeConnect();
			}

			std::string message;
			message.reserve(json.size() + 1);
			for(char value : json) {
				if(value != '\n' && value != '\r') {
					message += value;
				}
			}
			message += '\n';

			DWORD bytesWritten = 0;
			if(!WriteFile(g_pipe, message.data(), static_cast<DWORD>(message.size()), &bytesWritten, nullptr) ||
				bytesWritten != static_cast<DWORD>(message.size())) {
				PipeClose();
				continue;
			}

			std::string response;
			if(PipeReadLine(response)) {
				LeaveCriticalSection(&g_pipeLock);
				return response;
			}
			PipeClose();
		}

		LeaveCriticalSection(&g_pipeLock);
		return R"({"jsonrpc":"2.0","id":0,"error":{"code":-32603,"message":"Mesen is not available"}})";
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

		fprintf(stderr, "[MCPServer] Starting stdio transport.\n");
		InitializeCriticalSection(&g_pipeLock);
		PipeConnect();
		fprintf(stderr, "[MCPServer] Connected to Mesen.\n");

		std::string request;
		while(ReadStdioMessage(request)) {
			std::string response = PipeRequest(request);
			if(response != "{}") {
				WriteStdioMessage(response);
			}
		}

		PipeClose();
		DeleteCriticalSection(&g_pipeLock);
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
}

int main(int argc, char* argv[])
{
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
			fprintf(stderr, "[MCPServer] Invalid port: %s\n", argv[index]);
			return 1;
		}
	}

	if(!AcquireSingletonMutex()) {
		return 1;
	}
	if(!StartParentWatcher()) {
		ReleaseSingletonMutex();
		return 1;
	}

	if(g_stdioMode) {
		int result = RunStdio();
		StopParentWatcher();
		ReleaseSingletonMutex();
		return result;
	}

	printf("[MCPServer] Mesen debugger MCP bridge\n");
	printf("[MCPServer] Connecting to Mesen on pipe %s...\n", PipeName);

	WSADATA winsockData {};
	int startupResult = WSAStartup(MAKEWORD(2, 2), &winsockData);
	if(startupResult != 0) {
		fprintf(stderr, "[MCPServer] WSAStartup failed: %d\n", startupResult);
		StopParentWatcher();
		ReleaseSingletonMutex();
		return 1;
	}
	InitializeCriticalSection(&g_pipeLock);
	PipeConnect();
	printf("[MCPServer] Connected to Mesen.\n");

	SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(server == INVALID_SOCKET) {
		fprintf(stderr, "[MCPServer] socket failed: %d\n", WSAGetLastError());
		PipeClose();
		DeleteCriticalSection(&g_pipeLock);
		WSACleanup();
		StopParentWatcher();
		ReleaseSingletonMutex();
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
		fprintf(stderr, "[MCPServer] Could not listen on port %d: %d\n", g_port, WSAGetLastError());
		closesocket(server);
		PipeClose();
		DeleteCriticalSection(&g_pipeLock);
		WSACleanup();
		StopParentWatcher();
		ReleaseSingletonMutex();
		return 1;
	}

	printf("[MCPServer] Listening on http://127.0.0.1:%d/mcp/\n", g_port);
	printf("[MCPServer] HTTP: codex mcp add mesen-debugger --url http://127.0.0.1:%d/mcp/\n", g_port);
	printf("[MCPServer] stdio: codex mcp add mesen-debugger -- \"%s\" --stdio\n", GetExecutablePath().c_str());

	while(true) {
		SOCKET client = accept(server, nullptr, nullptr);
		if(client == INVALID_SOCKET) {
			break;
		}
		std::thread([client]() { HandleClient(client); }).detach();
	}

	closesocket(server);
	PipeClose();
	DeleteCriticalSection(&g_pipeLock);
	WSACleanup();
	StopParentWatcher();
	ReleaseSingletonMutex();
	return 0;
}
