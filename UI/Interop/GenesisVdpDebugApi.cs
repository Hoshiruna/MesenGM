using System;
using System.Runtime.InteropServices;

namespace Mesen.Interop
{
	internal static class GenesisVdpDebugApi
	{
		public const int MaxSprites = 80;
		public const int MaxScanline = 239;
		private const int MaxMappings = 20;
		private const int MaxHscrollWrites = 64;
		private const int VramWords = 32768;
		private const int PaletteColors = 64;

		[DllImport(EmuApi.DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		private static extern unsafe bool GetGenesisVdpDiagnosticSnapshot(
			int scanline, out GenesisVdpDebugState state, GenesisVdpDebugSprite* sprites, GenesisVdpDebugMapping* mappings,
			out GenesisVdpDebugHscroll hscroll, GenesisVdpDebugHscrollWrites* writes
		);

		[DllImport(EmuApi.DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		private static extern unsafe bool GetGenesisVdpGraphics(
			out GenesisVdpDebugState state, out GenesisVdpDebugGraphics graphics, GenesisVdpDebugSprite* sprites, ushort* vram, uint* palette
		);

		[DllImport(EmuApi.DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		private static extern unsafe bool GetGenesisVdpTiles(
			out GenesisVdpDebugState state, out GenesisVdpDebugGraphics graphics, ushort* vram, uint* palette, uint* cram
		);

		[DllImport(EmuApi.DllName)]
		[return: MarshalAs(UnmanagedType.I1)]
		private static extern unsafe bool GetGenesisVdpPaletteSnapshot(out uint frameCount, uint* palette, uint* cram);

		public static unsafe (uint FrameCount, uint[] Colors, uint[] Cram)? ReadPalette()
		{
			var colors = new uint[PaletteColors];
			var cram = new uint[PaletteColors];
			fixed(uint* colorPtr = colors)
			fixed(uint* cramPtr = cram) {
				return GetGenesisVdpPaletteSnapshot(out uint frameCount, colorPtr, cramPtr) ? (frameCount, colors, cram) : null;
			}
		}

		public static unsafe GenesisVdpGraphicsSnapshot? ReadGraphics()
		{
			var sprites = new GenesisVdpDebugSprite[MaxSprites];
			var vram = new ushort[VramWords];
			var palette = new uint[PaletteColors];
			fixed(GenesisVdpDebugSprite* spritePtr = sprites)
			fixed(ushort* vramPtr = vram)
			fixed(uint* palettePtr = palette) {
				return GetGenesisVdpGraphics(out var state, out var graphics, spritePtr, vramPtr, palettePtr)
					? new GenesisVdpGraphicsSnapshot(state, graphics, sprites.AsMemory(0, (int)state.SpriteCount), vram, palette)
					: null;
			}
		}

		public static unsafe GenesisVdpGraphicsSnapshot? ReadTiles()
		{
			var vram = new ushort[VramWords];
			var palette = new uint[PaletteColors];
			var cram = new uint[PaletteColors];
			fixed(ushort* vramPtr = vram)
			fixed(uint* palettePtr = palette)
			fixed(uint* cramPtr = cram) {
				return GetGenesisVdpTiles(out var state, out var graphics, vramPtr, palettePtr, cramPtr)
					? new GenesisVdpGraphicsSnapshot(state, graphics, ReadOnlyMemory<GenesisVdpDebugSprite>.Empty, vram, palette, cram)
					: null;
			}
		}

		public static unsafe GenesisVdpSnapshot? ReadSnapshot(int scanline, bool includeHscrollWrites)
		{
			var sprites = new GenesisVdpDebugSprite[MaxSprites];
			var mappings = new GenesisVdpDebugMapping[MaxMappings];
			var writes = includeHscrollWrites ? new GenesisVdpDebugHscrollWrite[MaxHscrollWrites] : Array.Empty<GenesisVdpDebugHscrollWrite>();
			fixed(GenesisVdpDebugSprite* spritePtr = sprites)
			fixed(GenesisVdpDebugMapping* mappingPtr = mappings)
			fixed(GenesisVdpDebugHscrollWrite* writePtr = writes) {
				GenesisVdpDebugHscrollWrites trace = new() { Changes = writePtr };
				if(!GetGenesisVdpDiagnosticSnapshot(scanline, out var state, spritePtr, mappingPtr, out var hscroll, includeHscrollWrites ? &trace : null)) {
					return null;
				}
				return new GenesisVdpSnapshot(
					state, sprites.AsMemory(0, (int)state.SpriteCount), mappings.AsMemory(0, (int)state.MappingCount), hscroll,
					writes.AsMemory(0, (int)trace.Count), trace.Truncated != 0
				);
			}
		}
	}

	internal sealed record GenesisVdpSnapshot(
		GenesisVdpDebugState State, ReadOnlyMemory<GenesisVdpDebugSprite> Sprites, ReadOnlyMemory<GenesisVdpDebugMapping> Mappings,
		GenesisVdpDebugHscroll Hscroll, ReadOnlyMemory<GenesisVdpDebugHscrollWrite> HscrollWrites, bool HscrollWritesTruncated
	);

	internal sealed record GenesisVdpGraphicsSnapshot(
		GenesisVdpDebugState State, GenesisVdpDebugGraphics Graphics, ReadOnlyMemory<GenesisVdpDebugSprite> Sprites, ushort[] Vram, uint[] Palette, uint[]? Cram = null
	)
	{
		public int TileHeight => State.InterlaceMode == 3 ? 16 : 8;
		public int VramByteSize => Graphics.VramMode == 0 ? 0x10000 : 0x20000;

		public int ReadPatternPixel(int byteAddress, int x, int y)
		{
			int address = byteAddress + y * 4 + (x >> 1);
			ushort word = ReadVramWord(address >> 1);
			int nibble = ((address & 1) << 1) | (x & 1);
			return (word >> ((3 - nibble) * 4)) & 15;
		}

		public ushort ReadVramWord(int address)
		{
			if(Graphics.VramMode == 0) {
				return Vram[address & 0x7FFF];
			}
			// In 128 KiB mode, a logical word repeats one byte from physical VRAM.
			int offset = ((address >> 1) & 0x7E00) | (address & 0x01FE) | ((address >> 9) & 1);
			int value = (Vram[offset] >> ((address & 1) != 0 ? 0 : 8)) & 0xFF;
			return (ushort)(value | (value << 8));
		}
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct GenesisVdpDebugGraphics
	{
		public uint VramMode, PlaneColumns, PlaneRows, Backdrop;
		public uint PlaneA, PlaneB, PatternA, PatternB, SpritePattern;
		public uint ScrollAX, ScrollAY, ScrollBX, ScrollBY;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct GenesisVdpTimingState : BaseState
	{
		public uint FrameCount;
		public ushort VCounter;
		public ushort HCounter;
		public ushort FirstScanline;
		public ushort LastScanline;
		[MarshalAs(UnmanagedType.I1)] public bool VBlank;
		[MarshalAs(UnmanagedType.I1)] public bool HBlank;
		[MarshalAs(UnmanagedType.I1)] public bool DisplayEnable;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct GenesisVdpDebugState
	{
		public uint FrameCount, Width, Height, VCounter, HCounter;
		public uint SatAddress, InterlaceMode, SpriteCount, MappingCount;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct GenesisVdpDebugSprite
	{
		public ushort Y, SizeLink, Attributes, X, CachedY, CachedSizeLink;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct GenesisVdpDebugMapping
	{
		public ushort Index, Y, SizeLink, Attributes, X, SatAddress;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct GenesisVdpDebugHscroll
	{
		public uint Captured, Mode, VramMode, HCounter, TableAddress, EntryAddress;
		public ushort RawA, RawB, FrameEndRawA, FrameEndRawB;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct GenesisVdpDebugHscrollWrite
	{
		public uint Address, SourceAddress;
		public ushort Before, After, VCounter, HCounter;
		public GenesisVdpDebugWriteSource Source;
		public byte VramMode, InScanline, BeforeFetch;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal unsafe struct GenesisVdpDebugHscrollWrites
	{
		public uint Count, Truncated;
		public GenesisVdpDebugHscrollWrite* Changes;
	}

	internal enum GenesisVdpDebugWriteSource : byte
	{
		UnknownFifo,
		CpuDataPort,
		DmaLoad,
		DmaFill,
		DmaCopy
	}

	internal static class GenesisVdpSpriteFields
	{
		public static int XOnScreen(ushort x) => (x & 0x1FF) - 128;
		public static int WidthTiles(ushort sizeLink) => ((sizeLink >> 10) & 3) + 1;
		public static int HeightTiles(ushort sizeLink) => ((sizeLink >> 8) & 3) + 1;
		public static int Link(ushort sizeLink) => sizeLink & 0x7F;
		public static int Tile(ushort attributes) => attributes & 0x7FF;
		public static int Palette(ushort attributes) => (attributes >> 13) & 3;
		public static bool Priority(ushort attributes) => (attributes & 0x8000) != 0;
	}
}
