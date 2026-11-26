using Avalonia;
using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Debugger.Controls;
using Mesen.Debugger.Utilities;
using Mesen.Debugger.Windows;
using Mesen.Interop;
using Mesen.Localization;
using Mesen.Utilities;
using System;
using System.Collections.Generic;
using static Mesen.Interop.GenesisVdpSpriteFields;

namespace Mesen.Debugger.ViewModels
{
	public partial class TilemapViewerViewModel
	{
		[ObservableProperty] public partial string GenesisStatus { get; private set; } = "";

		private GenesisVdpPreview? _genesisPreview;
		private int _genesisLayer = -1;

		public bool IsGenesisSpriteTab => IsGenesis && SelectedTab?.Layer == GenesisVdpPreview.SpriteLayer;
		public string InfoTitle => GenesisLabel(IsGenesisSpriteTab ? "lblGenesisSpriteTable" : "lblTilemap");
		public string SelectionTitle => GenesisLabel(IsGenesisSpriteTab ? "lblGenesisSelectedSprite" : "lblSelectedTile");

		private static string GenesisLabel(string id) => ResourceHelper.GetViewLabel(nameof(TilemapViewerWindow), id);

		private static List<TilemapViewerTab> CreateGenesisTabs() => new() {
			new() { Title = "BG A", Layer = 0, VramMemoryType = MemoryType.GenesisVdpVram },
			new() { Title = "BG B", Layer = 1, VramMemoryType = MemoryType.GenesisVdpVram },
			new() { Title = GenesisLabel("tabGenesisSprites"), Layer = GenesisVdpPreview.SpriteLayer, VramMemoryType = MemoryType.GenesisVdpVram }
		};

		private void RefreshGenesisData()
		{
			var snapshot = GenesisVdpDebugApi.ReadGraphics();
			_genesisPreview = snapshot == null ? null : new GenesisVdpPreview(snapshot);
			RefreshTab();
		}

		private unsafe void DrawGenesisTab()
		{
			int layer = SelectedTab.Layer;
			if(_genesisLayer != layer) {
				_genesisLayer = layer;
				SelectionRect = default;
				ClearGenesisHover();
			}

			if(_genesisPreview is not { } preview) {
				using var bitmap = ViewerBitmap.Lock();
				var framebuffer = bitmap.FrameBuffer;
				new Span<byte>((void*)framebuffer.Address, framebuffer.RowBytes * framebuffer.Size.Height).Clear();
				IsTilemapInfoVisible = false;
				PreviewPanel = null;
				ClearGenesisHover();
				ScrollOverlayRect = default;
				GenesisStatus = GenesisLabel("lblGenesisWaiting");
				return;
			}

			PixelSize size = preview.GetSize(layer);
			InitBitmap(size.Width, size.Height);
			using(var bitmap = ViewerBitmap.Lock()) {
				preview.Draw(layer, bitmap.FrameBuffer, Config.DisplayMode, Config.Background);
			}

			bool sprites = IsGenesisSpriteTab;
			var snapshot = preview.Snapshot;
			GridSizeX = 8;
			GridSizeY = preview.TileHeight;
			ScrollOverlayRect = !sprites && Config.ShowScrollOverlay ? new Rect(
				(layer == 0 ? snapshot.Graphics.ScrollAX : snapshot.Graphics.ScrollBX) % (uint)size.Width,
				(layer == 0 ? snapshot.Graphics.ScrollAY : snapshot.Graphics.ScrollBY) % (uint)size.Height,
				snapshot.State.Width, snapshot.State.Height * preview.TileHeight / 8
			) : default;
			GenesisStatus = string.Format(GenesisLabel(sprites ? "lblGenesisSpriteFrame" : "lblGenesisFrame"), snapshot.State.FrameCount);

			TooltipEntries entries = TilemapInfoPanel.Items ?? new();
			entries.StartUpdate();
			entries.AddEntry("Size (px)", $"{size.Width}x{size.Height}");
			if(sprites) {
				entries.AddEntry("Sprite table", $"${snapshot.State.SatAddress:X4}");
			} else {
				entries.AddEntry("Tilemap address", $"${preview.GetPlaneAddress(layer) * 2:X4}");
				entries.AddEntry("Tileset address", $"${preview.GetPatternAddress(layer) * 2:X4}");
			}
			entries.EndUpdate();
			TilemapInfoPanel.Items = entries;
			IsTilemapInfoVisible = true;
			UpdatePreviewPanel();
		}

		private void ClearGenesisHover()
		{
			if(ViewerTooltip != null) {
				TooltipHelper.HideTooltip(_picViewer);
			}
			ViewerTooltip = null;
			ViewerMousePos = null;
		}

		internal bool SelectGenesisSprite(PixelPoint p)
		{
			if(!IsGenesisSpriteTab) {
				return false;
			}
			SelectionRect = new Rect(p.X, p.Y, 1, 1);
			return true;
		}

		private DynamicTooltip? GetGenesisPreview(PixelPoint p, DynamicTooltip? tooltip)
		{
			if(_genesisPreview is not { } preview || _genesisLayer != SelectedTab.Layer) {
				return null;
			}

			int layer = SelectedTab.Layer;
			PixelSize size = preview.GetSize(layer);
			if((uint)p.X >= (uint)size.Width || (uint)p.Y >= (uint)size.Height) {
				return null;
			}

			TooltipEntries entries = tooltip?.Items ?? new();
			ushort attributes;
			if(layer == GenesisVdpPreview.SpriteLayer) {
				if(preview.GetSpriteAt(p) is not { } sprite) {
					return null;
				}
				var slot = sprite.Slot;
				var bounds = sprite.Bounds;
				int left = Math.Max(0, bounds.X);
				int top = Math.Max(0, bounds.Y);
				_previewCropRect = new PixelRect(left, top, Math.Min(size.Width, bounds.Right) - left, Math.Min(size.Height, bounds.Bottom) - top);
				attributes = slot.Attributes;

				entries.StartUpdate();
				entries.AddPicture("Sprite", ViewerBitmap, 3, _previewCropRect);
				entries.AddEntry("Sprite index", sprite.Index);
				entries.AddEntry("X, Y", $"{bounds.X}, {bounds.Y}");
				entries.AddEntry("Size (tiles)", $"{WidthTiles(slot.SizeLink)}x{HeightTiles(slot.SizeLink)}");
				entries.AddEntry("Link", Link(slot.SizeLink));
				entries.AddEntry("Cached Y (raw)", $"${slot.CachedY:X4}");
				entries.AddEntry("Cached size (tiles)", $"{WidthTiles(slot.CachedSizeLink)}x{HeightTiles(slot.CachedSizeLink)}");
				entries.AddEntry("Cached link", Link(slot.CachedSizeLink));
			} else {
				int column = p.X / 8;
				int row = p.Y / preview.TileHeight;
				attributes = preview.GetTileAttributes(layer, column, row);
				int address = preview.GetPlaneAddress(layer) + row * (int)preview.Snapshot.Graphics.PlaneColumns + column;
				_previewCropRect = new PixelRect(column * 8, row * preview.TileHeight, 8, preview.TileHeight);

				entries.StartUpdate();
				entries.AddPicture("Tile", ViewerBitmap, 6, _previewCropRect);
				entries.AddEntry("Column, row", $"{column}, {row}");
				entries.AddEntry("Tilemap address", $"${(address & 0xFFFF) * 2:X4}");
				entries.AddEntry("Tile address", $"${preview.GetTileAddress(layer, attributes) * 2:X4}");
			}

			entries.AddEntry("Tile index", $"${Tile(attributes):X3}");
			entries.AddEntry("Palette index", Palette(attributes));
			entries.AddEntry("Horizontal mirror", (attributes & 0x0800) != 0);
			entries.AddEntry("Vertical mirror", (attributes & 0x1000) != 0);
			entries.AddEntry("High priority", Priority(attributes));
			entries.EndUpdate();
			if(tooltip != null) {
				tooltip.Items = entries;
				return tooltip;
			}
			return new DynamicTooltip { Items = entries };
		}
	}
}
