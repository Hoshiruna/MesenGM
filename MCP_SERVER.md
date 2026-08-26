# MCP debugger server

MesenGM can expose its debugger to local Model Context Protocol clients. The feature has two parts:

- Mesen owns the debugger service and a local named pipe.
- `MCPServer.exe` connects that pipe to either stdio or Streamable HTTP.

The bridge is available on Windows builds. Mesen must stay open while a client is connected.

## Connect through stdio

Use stdio when the MCP client launches its own server process. The bridge waits for Mesen if the emulator has not opened yet.

For Codex CLI:

```powershell
codex mcp add mesen-debugger -- "C:\path\to\MCPServer.exe" --stdio
```

For Claude Code in PowerShell, use `add-json` so `--stdio` is passed to the bridge instead of being parsed as a Claude option:

```powershell
$claudeConfig = @{
    type = "stdio"
    command = "C:\path\to\MCPServer.exe"
    args = @("--stdio")
}
claude mcp add-json mesen-debugger ($claudeConfig | ConvertTo-Json -Compress)
```

Replace `C:\path\to\MCPServer.exe` with the full path to the `MCPServer.exe` next to `Mesen.exe`. Only one bridge process can connect at a time, so stop the HTTP bridge in Mesen before starting a stdio client.

## Connect through HTTP

Open **Debug > MCP Server**, choose a port, and select **Start**. The default endpoint is:

```text
http://127.0.0.1:51234/mcp/
```

For Codex CLI:

```powershell
codex mcp add mesen-debugger --url http://127.0.0.1:51234/mcp/
```

For Claude Code:

```powershell
claude mcp add --transport http mesen-debugger http://127.0.0.1:51234/mcp/
```

The endpoint listens only on the IPv4 loopback address. Stopping the HTTP bridge does not stop Mesen's debugger pipe, so a stdio client can connect afterward.

## Tools

| Tool | What it does | Changes emulator state |
| --- | --- | --- |
| `debugger_status` | Reports emulator and debugger status | No |
| `get_rom_info` | Reports the loaded ROM and available CPU identifiers | No |
| `get_cpu_state` | Reads CPU registers or the current program counter | No |
| `get_ppu_state` | Reads scanline, cycle, and frame state | No |
| `get_memory_range` | Reads up to 4096 bytes from a memory region | No |
| `set_memory` | Writes up to 4096 bytes to a memory region | Yes |
| `get_disassembly` | Disassembles code around an address | No |
| `get_trace_tail` | Reads recent execution trace rows | No |
| `get_debug_events` | Reads recent debugger events | No |
| `set_breakpoints` | Replaces the current breakpoint list | Yes |
| `step` | Advances debugger execution | Yes |
| `resume` | Resumes execution | Yes |
| `pause` | Stops after the next instruction | Yes |

`get_cpu_state` returns structured SNES, NES, and Game Boy registers. Other debugger-supported CPUs return their program counter. `get_ppu_state` currently supports SNES, NES, and Game Boy.

Genesis-specific state tools are not included yet because this branch does not contain the Genesis core and interop types from mesen2-expanded. They can be added without changing the transport layer once those types arrive.

## Finding identifiers

Call `get_rom_info` first. Its response includes the CPU identifiers and memory regions available for the loaded ROM. Each memory region has the numeric ID used by the memory tools, its API name, and its size. Invalid or unavailable regions are rejected with an error.

Tool results contain both structured content and an equivalent JSON text block. This keeps them usable with clients that have not added structured tool-result support.

## Local security

The HTTP bridge binds to `127.0.0.1` and accepts browser origins only from localhost, `127.0.0.1`, or `[::1]`. It is not intended for network exposure.

An authorized client can read ROM state, change memory, replace breakpoints, and control execution. Review a client's tool requests before granting it access to a session you care about.

## Troubleshooting

- If `MCPServer.exe` waits for Mesen, open Mesen and try the request again.
- If a bridge exits immediately, close the other stdio or HTTP bridge process first.
- If HTTP startup fails, choose an unused port in **Debug > MCP Server**.
- If debugger tools report that no ROM is loaded, load a game before calling them.
