# Tool arguments and behavior

Check the live tool schema first. These details reflect `McpDebugService.cs` in this project and are not a substitute for discovery on an older or newer binary.

## Reading state

| Tool | Arguments and limits |
| --- | --- |
| `get_rom_info` | No arguments. Returns ROM identity, CPU IDs, memory region IDs/names/sizes. |
| `debugger_status` | No arguments. Includes `debugger_running`, `emulation_running`, `execution_stopped`, `emulation_paused`. |
| `get_cpu_state` | Optional `cpu_type`; defaults to the loaded system's main CPU. |
| `get_ppu_state` | Optional `cpu_type`; structured PPU state for NES, SNES and Game Boy. |
| `get_screen` | Optional `apply_video_filter` (default false) and `include_base64` (default false). Returns the last rendered frame as an `image/png` content block plus `width`, `height`, `byte_length`, `frame_count`, `emulation_paused`. Fails until at least one frame has been rendered. |
| `get_disassembly` | Optional `cpu_type`, `address`, `line_count` (1-100, default 20). Omitting `address` centers on the current PC. |
| `get_trace_tail` | `count` (1-1000, default 100), `offset` (default 0, measured from the newest entry). No `cpu_type` argument. |
| `get_debug_events` | Optional `cpu_type`, `max_count` (1-1000, default 100). Returns event type, PC, scanline, cycle and breakpoint ID. |

## Memory

- `get_memory_range`: required `memory_type`, `start_address`, `length` (1-4096). Optional `format`: `hex` (default, unspaced uppercase), `base64`, or `bytes` (integer array).
- `search_memory`: required `memory_type`, `pattern`. Pattern is 1-256 hex bytes; `??` matches one arbitrary byte. Spaces are optional. Optional `start_address` (default 0), inclusive `end_address` (default end of region), `max_results` (1-1000, default 100). If `truncated` is true, the result hit the cap and is not an exhaustive list. To continue, search from one byte after the last returned address, retaining the original end bound so overlapping matches are not skipped.
- `set_memory`: required `memory_type`, `address`, and either `hex` or `data`. Between 1 and 4096 bytes; integer-array values must be 0-255. The entire range must fit the region. Reads use `start_address`; writes use `address`.

## Breakpoints

`set_breakpoints` takes a required `breakpoints` array with at most 1000 entries. Each entry requires `address`. Optional fields:

- `end_address`: inclusive; defaults to the same address.
- `cpu_type`: defaults to the main CPU.
- `memory_type`: defaults to that CPU's relative memory.
- `type`: flags Read=1, Write=2, Execute=4, Forbid=8. Combine needed flags; default 4.
- `enabled`: defaults to true.
- `condition`: debugger expression; must fit within 999 UTF-8 bytes. Use this project's debugger expression syntax rather than guessing a programming-language expression.

The call replaces all breakpoints. IDs are assigned from array indices. It does not return the previous list.

## Execution

`step` accepts optional `cpu_type`, `count` (0-1000000, default 1), and `step_type` (default 0). The meaning of `count` depends on the selected mode, so use a positive count for ordinary instruction stepping. CPU support is checked by the handler.

| ID | Mode |
| --- | --- |
| 0 | Step |
| 1 | StepOut |
| 2 | StepOver |
| 3 | CpuCycleStep |
| 4 | PpuStep |
| 5 | PpuScanline |
| 6 | PpuFrame |
| 7 | SpecificScanline |
| 8 | RunToNmi |
| 9 | RunToIrq |
| 10 | StepBack |

`pause` and `resume` take no arguments. Read `debugger_status` after execution requests when a stopped state matters to the next observation.

## Export

`save_rom` requires an absolute `path`. Optional `as_ips` defaults to false, `overwrite` to false, and `cdl_strip_option` to 0 (StripNone; 1=StripUnused, 2=StripUsed). Use `as_ips: true` for an IPS patch containing ROM edits. Export support depends on the loaded format; report the server's format error instead of treating every loaded game as exportable.

Check tool results for `isError` and JSON-RPC errors. A transport response alone does not mean an edit or export succeeded.
