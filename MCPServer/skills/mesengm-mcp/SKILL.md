---
name: mesengm-mcp
description: Use MesenGM's MCP debugger tools to inspect running games, search and edit memory, examine disassembly and traces, control execution, and export ROM edits or IPS patches. Use for game debugging through the mesen-debugger MCP connection, not for developing the MCP server or changing emulator source code.
---

# MesenGM MCP debugger

Use the connected MesenGM debugger to answer the user's question about a loaded game. Keep observations tied to the ROM, CPU, memory region and execution state that produced them.

## Establish the session

1. Discover the available Mesen debugger tools and read their current schemas. Tool prefixes depend on the client's connection name; the names below are the underlying tool names. The running server's schemas take precedence over this skill.
2. Call `get_rom_info` to identify the loaded ROM and discover `cpu_types` and `memory_types`, including each region's size. Never guess their numeric IDs. Refresh this information after changing ROMs or reconnecting to a different emulator session.
3. Call `debugger_status` before controlling execution. Distinguish `execution_stopped` from `emulation_paused`; a successful handshake or tool listing does not establish that a ROM is running.

If tools are unavailable or calls cannot reach Mesen, read [connection.md](references/connection.md). Do not rebuild the project to establish a connection unless the user requests a build.

## Inspect the game

- Start with a small CPU-state, disassembly or memory read that addresses the question. Use `search_memory` for byte-pattern searches instead of dumping entire regions.
- Distinguish CPU-relative addresses from offsets in physical ROM or RAM regions. On a banked cartridge, the bytes mapped at a CPU address can change. Record the region name and address alongside relevant bytes; a CPU address is not automatically a ROM-file offset.
- Send addresses and identifiers as JSON integers, not strings such as `"$8000"`. Use hexadecimal notation when explaining addresses to the user.
- Memory transfers are limited to 4096 bytes per call. Check the discovered region size and split larger transfers into valid ranges. Read the returned encoding once; structured content and the JSON text block describe the same result.
- CPU registers and PPU state are structured for NES, SNES and Game Boy. Other supported CPUs may return only a program counter; do not invent missing registers or PPU support.
- Reads while the game runs do not form an atomic snapshot across calls. When a consistent snapshot is needed and execution control is within scope, pause once, confirm `execution_stopped`, then collect the related state.
- Use bounded trace and event queries. An empty trace does not prove that code never executed. Check the trace configuration in Mesen if needed; this tool set has no trace-configuration tool.

Read [tool-details.md](references/tool-details.md) for argument names, limits and stepping modes.

## Control execution and breakpoints

Use execution controls when the requested investigation calls for them. Do not pause or resume an unrelated session just to check connectivity.

`pause` requests a stop after an instruction; `step` returns after requesting execution, not after proving completion. Check `debugger_status` before inspecting the result. Prefer small steps and bounded status checks; stop polling when the requested condition is reached or a useful timeout/error needs attention.

`set_breakpoints` replaces the entire active list, including breakpoints from other debugger clients. An empty array clears it. The current tool set has no breakpoint-list reader. Reuse the complete list when it is already known. If the user asks to add a breakpoint while preserving an unknown list, obtain that list through the UI or ask for the missing information before replacing it. Do not claim to have preserved breakpoints that were never read.

When a task is observational, restore the original running/stopped state if the task changed it. Leave execution stopped when that is the requested result, such as investigating a breakpoint hit, and report that state.

## Edit memory and export patches

For an authorized memory edit, read and retain the original bytes for the affected range, write only the requested bytes, then read the range back. Use either `hex` or `data` in `set_memory`, not both.

`set_memory` uses the debugger's bulk memory-edit path with CPU bus side effects disabled. Do not use it as a substitute for a CPU write to a mapper, APU or PPU register. To investigate register behavior, observe or step the game's actual writes.

Editing a ROM region changes the emulator's loaded copy. `save_rom` exports those edits to a ROM or IPS file; it is not a save-state tool. Use an absolute destination path with an existing parent directory. Keep `overwrite` false and `cdl_strip_option` at 0 unless the user's request calls for different behavior. A request to inspect or temporarily edit memory does not imply a request to export or overwrite a file.

Follow existing authorization for edits and exports without asking again. If a mutation times out, check its resulting state before retrying: the server's worker can continue after the timeout response.

## Report the result

State the finding, the relevant addresses and bytes, and the observations supporting it. Distinguish a suspected cause from one reproduced by execution. For edits, include the changed range and verification result; for exports, include the output path. Mention any remaining breakpoint or execution-state changes.

These tools do not provide ROM loading, controller input, audio capture or save-state management. Use another available interface when the task needs those capabilities; do not fabricate tool calls or claim a listening test from memory inspection alone.

## Source of project-specific behavior

When this repository is available, resolve these paths from its root:

- `UI/Debugger/Utilities/McpDebugService.cs`: authoritative tool schemas and handlers.
- `Core/Debugger/MemoryDumper.cpp`: memory-read and edit semantics.
- `MCP_SERVER.md`: connection overview; it may lag the tool implementation.
- `MCPServer/MCPServer.cpp`: transport behavior and reconnection errors.

This skill operates the debugger. It does not authorize edits to the MCP server, mapper implementations or shared audio code.
