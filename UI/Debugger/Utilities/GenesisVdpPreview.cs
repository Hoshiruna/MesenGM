using Avalonia;
using Avalonia.Platform;
using Mesen.Interop;
using System;
using System.Collections.Generic;
using static Mesen.Interop.GenesisVdpSpriteFields;

namespace Mesen.Debugger.Utilities
{
	// Uses the tilemap and linked SAT layout from Mesen2-Expanded with the Ares VDP snapshot.
	internal sealed class GenesisVdpPreview
	{
		public const int SpriteLayer = 2;
		public GenesisVdpGraphicsSnapshot Snapshot { get; }
		public int TileHeight => Snapshot.TileHeight;

		private readonly List<Sprite> _sprites = new();

		public GenesisVdpPreview(GenesisVdpGraphicsSnapshot snapshot)
		{
			Snapshot = snapshot;

			var slots = snapshot.Sprites.Span;
			Span<bool> visited = stackalloc bool[GenesisVdpDebugApi.MaxSprites];
			visited.Clear();
			int index = 0;
			while(index < slots.Length && !visited[index]) {
				visited[index] = true;
				var slot = slots[index];
				int y = (slot.Y & (TileHeight == 16 ? 0x3FF : 0x1FF)) - TileHeight * 16;
				PixelRect bounds = new(XOnScreen(slot.X), y, WidthTiles(slot.SizeLink) * 8, HeightTiles(slot.SizeLink) * TileHeight);
				_sprites.Add(new Sprite(index, slot, bounds));
				index = Link(slot.SizeLink);
				if(index == 0) {
					break;
				}
			}
		}

		public PixelSize GetSize(int layer)
		{
			return layer == SpriteLayer
				? new PixelSize((int)Snapshot.State.Width, (int)Snapshot.State.Height * TileHeight / 8)
				: new PixelSize((int)Snapshot.Graphics.PlaneColumns * 8, (int)Snapshot.Graphics.PlaneRows * TileHeight);
		}

		public int GetPlaneAddress(int layer) => (int)(layer == 0 ? Snapshot.Graphics.PlaneA : Snapshot.Graphics.PlaneB);
		public int GetPatternAddress(int layer) => (int)(layer == 0 ? Snapshot.Graphics.PatternA : Snapshot.Graphics.PatternB);

		public ushort GetTileAttributes(int layer, int column, int row)
		{
			return Snapshot.ReadVramWord(GetPlaneAddress(layer) + row * (int)Snapshot.Graphics.PlaneColumns + column);
		}

		public int GetTileAddress(int layer, ushort attributes)
		{
			return GetPatternAddress(layer) | (Tile(attributes) * TileHeight * 2);
		}

		public Sprite? GetSpriteAt(PixelPoint point)
		{
			foreach(var sprite in _sprites) {
				int x = point.X - sprite.Bounds.X;
				int y = point.Y - sprite.Bounds.Y;
				if((uint)x < (uint)sprite.Bounds.Width && (uint)y < (uint)sprite.Bounds.Height && GetSpritePixel(sprite, x, y) != 0) {
					return sprite;
				}
			}
			return null;
		}

		public unsafe void Draw(int layer, ILockedFramebuffer target, TilemapDisplayMode mode, TilemapBackground background)
		{
			Span<uint> pixels = new((void*)target.Address, target.RowBytes / sizeof(uint) * target.Size.Height);
			int stride = target.RowBytes / sizeof(uint);
			uint clear = background switch {
				TilemapBackground.Transparent => 0,
				TilemapBackground.Black => 0xFF000000u,
				TilemapBackground.White => 0xFFFFFFFFu,
				TilemapBackground.Magenta => 0xFFFF00FFu,
				_ => layer == SpriteLayer ? 0 : Snapshot.Palette[Snapshot.Graphics.Backdrop]
			};
			if(layer == SpriteLayer) {
				pixels.Fill(clear);
				for(int i = _sprites.Count - 1; i >= 0; i--) {
					DrawSprite(_sprites[i], target.Size, pixels, stride, mode);
				}
			} else {
				DrawPlane(layer, target.Size, pixels, stride, mode, clear);
			}
		}

		private void DrawPlane(int layer, PixelSize size, Span<uint> pixels, int stride, TilemapDisplayMode mode, uint clear)
		{
			for(int y = 0; y < size.Height; y += TileHeight) {
				for(int x = 0; x < size.Width; x += 8) {
					ushort entry = GetTileAttributes(layer, x / 8, y / TileHeight);
					int byteAddress = GetTileAddress(layer, entry) * 2;
					for(int row = 0; row < TileHeight; row++) {
						int sourceY = (entry & 0x1000) != 0 ? TileHeight - 1 - row : row;
						for(int col = 0; col < 8; col++) {
							int sourceX = (entry & 0x0800) != 0 ? 7 - col : col;
							int color = Snapshot.ReadPatternPixel(byteAddress, sourceX, sourceY);
							pixels[(y + row) * stride + x + col] = color == 0 ? clear : GetColor(mode, entry, color);
						}
					}
				}
			}
		}

		private void DrawSprite(Sprite sprite, PixelSize size, Span<uint> pixels, int stride, TilemapDisplayMode mode)
		{
			PixelRect bounds = sprite.Bounds;
			for(int y = Math.Max(0, bounds.Y); y < Math.Min(size.Height, bounds.Bottom); y++) {
				for(int x = Math.Max(0, bounds.X); x < Math.Min(size.Width, bounds.Right); x++) {
					int color = GetSpritePixel(sprite, x - bounds.X, y - bounds.Y);
					if(color != 0) {
						pixels[y * stride + x] = GetColor(mode, sprite.Slot.Attributes, color);
					}
				}
			}
		}

		private int GetSpritePixel(Sprite sprite, int x, int y)
		{
			ushort attributes = sprite.Slot.Attributes;
			if((attributes & 0x0800) != 0) {
				x = sprite.Bounds.Width - 1 - x;
			}
			if((attributes & 0x1000) != 0) {
				y = sprite.Bounds.Height - 1 - y;
			}
			int tile = Tile(attributes) + (x / 8) * HeightTiles(sprite.Slot.SizeLink) + y / TileHeight;
			int address = (int)Snapshot.Graphics.SpritePattern | (tile * TileHeight * 2);
			return Snapshot.ReadPatternPixel(address * 2, x & 7, y % TileHeight);
		}

		private uint GetColor(TilemapDisplayMode mode, ushort attributes, int color)
		{
			return mode == TilemapDisplayMode.Grayscale
				? 0xFF000000u | ((uint)color * 0x111111u)
				: Snapshot.Palette[Palette(attributes) * 16 + color];
		}

		public readonly record struct Sprite(int Index, GenesisVdpDebugSprite Slot, PixelRect Bounds);
	}
}
