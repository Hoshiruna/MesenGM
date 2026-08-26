# MesenGM

Mesen is a multi-system emulator for Windows, Linux, and macOS. It supports NES, SNES, Game Boy (GB/SGB/GBC), Game Boy Advance, PC Engine, SMS/Game Gear, and WonderSwan (WS/WSC).

This branch is a fork of MesenCE, created to explore the possibility of running Genesis/Megadrive on Mesen.

## Releases

The latest stable version is available from the [releases page on GitHub]().

## Development Builds

[![Mesen]()]()

* [Windows]()
  * Windows 7 or higher is required. Windows 7 users must use SP1 and have all updates installed.
* [Linux x64]()  (requires **SDL2**)  
* [Linux ARM64]()  (requires **SDL2**)  
* [macOS - Intel]()  (requires **SDL2**)  
* [macOS - Apple Silicon]()  (requires **SDL2**)  

#### <ins>Notes</ins> ####

* Other builds are also available in the [Actions]() tab.
* **macOS**: Builds are self-signed and will require approval via Gatekeeper before they are able to be run.  
* **SteamOS**: See [SteamOS.md](SteamOS.md)  

## Compiling

See [COMPILING.md](COMPILING.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md)

## MCP debugger server

Windows builds include a local MCP bridge for debugger clients. See [MCP_SERVER.md](MCP_SERVER.md) for setup, available tools, and security notes.

## License

Mesen is available under the GPL V3 license.  Full text here: <http://www.gnu.org/licenses/gpl-3.0.en.html>

Copyright (C) 2014-2026 Sour, 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
