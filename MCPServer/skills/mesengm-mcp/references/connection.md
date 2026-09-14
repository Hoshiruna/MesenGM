# Connecting to MesenGM

Prefer an existing connected MCP server. A skill supplies instructions; it does not create an MCP connection by itself.

## HTTP

In Mesen, open **Debug > MCP Server** and select **Start**. The default Streamable HTTP endpoint is `http://127.0.0.1:51234/mcp/`. Use the configured port if it differs. The bridge listens on IPv4 loopback and is intended for local clients.

The window's status and logs identify bridge startup failures. An occupied port can be changed there, with the client's URL updated to match. Starting or stopping this bridge does not load a ROM.

## Stdio

A client can launch the existing `MCPServer.exe` next to `Mesen.exe` with the argument `--stdio`. Resolve the actual installed path; do not assume a build-output directory contains the user's current emulator.

The client owns this bridge process. The bridge can answer initialization without Mesen running, and can reconnect after Mesen restarts. Tool calls still require a running Mesen and, for game inspection, a loaded ROM. HTTP and stdio clients can coexist, but debugger state is shared between them.

## Errors

- **Mesen is not running:** Open the intended emulator session and load the relevant ROM. Retry the observation after it becomes available; restarting the bridge is normally unnecessary.
- **No ROM is loaded:** Load the user's selected game through Mesen or another available interface. There is no MCP ROM-loading tool in the current implementation.
- **Tool not found:** Refresh the connected server's tool list. `search_memory` and `save_rom` exist in this checkout but may be absent from an older installed binary. Do not assume source changes are already running.
- **Mesen did not answer / another call is still running:** Inspect Mesen and its MCP log. The service times out a tool after 10 seconds but lets its worker finish while retaining the request gate. Avoid concurrent retries. Recheck state before repeating a timed-out mutation.
- **Unknown CPU or memory region:** Refresh `get_rom_info` and use the returned IDs and sizes.

Report the specific missing connection, running emulator or loaded ROM when it prevents progress. Do not install dependencies, modify unrelated client settings or run a build merely to use this skill.
