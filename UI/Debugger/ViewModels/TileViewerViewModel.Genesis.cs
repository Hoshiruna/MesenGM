using Avalonia;
using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using Mesen.Localization;
using Mesen.Utilities;
using System;

namespace Mesen.Debugger.ViewModels
{
	public partial class TileViewerViewModel
	{
		private GenesisVdpGraphicsSnapshot? _genesisGraphics;
		internal bool HasGenesisSnapshot => _genesisGraphics != null;
		[ObservableProperty] public partial string GenesisStatus { get; private set; } = "";

		private void InitGenesis()
		{
			AvailableMemoryTypes = [MemoryType.GenesisVdpVram];
			AvailableFormats = [TileFormat.Bpp4];
			Config.Format = TileFormat.Bpp4;
			if(Config.Source != MemoryType.GenesisVdpVram) {
				Config.Source = MemoryType.GenesisVdpVram;
				Config.StartAddress = 0;
				Config.ColumnCount = 32;
				Config.RowCount = 64;
				Config.Layout = TileLayout.Normal;
			}
			Config.SelectedPreset = "";
			MaximumAddress = 0x1FFFF;
			RawFormat = RawPaletteFormat.GenesisRgb333;
			PaletteSelectionMode = Mesen.Debugger.Controls.PaletteSelectionMode.SixteenColors;
			ApplyColumnRowCountRestrictions();
			UpdateAddressIncrement();
			InitBitmap();
			GenesisStatus = ResourceHelper.GetMessage("GenesisViewerWaitingForFrame");
		}

		private void RefreshGenesisData()
		{
			var snapshot = GenesisVdpDebugApi.ReadTiles();
			if(snapshot == null && _genesisGraphics == null) {
				return;
			}

			_genesisGraphics = snapshot;
			RawPalette = snapshot?.Cram ?? Array.Empty<uint>();
			PaletteColors = snapshot?.Palette ?? Array.Empty<uint>();
			if(snapshot != null) {
				MaximumAddress = snapshot.VramByteSize - 1;
				Config.StartAddress = Math.Clamp(Config.StartAddress, 0, MaximumAddress);
				if(GridSizeY != snapshot.TileHeight) {
					GridSizeY = snapshot.TileHeight;
					UpdateAddressIncrement();
					SelectionRect = default;
					ClearGenesisHover();
				}
				GenesisStatus = ResourceHelper.GetMessage("GenesisTileViewerFrame", snapshot.State.FrameCount, snapshot.VramByteSize / 1024, snapshot.TileHeight);
			} else {
				SelectionRect = default;
				ClearGenesisHover();
				GenesisStatus = ResourceHelper.GetMessage("GenesisViewerWaitingForFrame");
			}
			RefreshTab();
		}

		private void ClearGenesisHover()
		{
			if(ViewerTooltip != null) {
				TooltipHelper.HideTooltip(_pictureViewer);
			}
			ViewerTooltip = null;
			ViewerMousePos = null;
		}

		private bool IsGenesisTileVisible(PixelPoint point)
		{
			return HasGenesisSnapshot && (uint)point.X < (uint)ViewerBitmap.PixelSize.Width && (uint)point.Y < (uint)ViewerBitmap.PixelSize.Height;
		}

		private PixelRect? GetGenesisCopyBounds()
		{
			if(ViewerMousePos is { } hovered && IsGenesisTileVisible(hovered)) {
				return GetTileBounds(hovered);
			}
			if(SelectionRect != default) {
				PixelPoint selected = PixelPoint.FromPoint(SelectionRect.TopLeft, 1);
				if(IsGenesisTileVisible(selected)) {
					return GetTileBounds(selected);
				}
			}
			return null;
		}

		private int GetGenesisTileAddress(int column, int row)
		{
			PixelPoint tile = FromLayoutCoordinates(Config.Layout, new PixelPoint(column, row));
			int offset = (tile.Y * ColumnCount + tile.X) * GridSizeY * 4;
			return (Config.StartAddress + offset) & MaximumAddress;
		}

		private unsafe void DrawGenesisTiles()
		{
			InitBitmap();
			using var bitmap = ViewerBitmap.Lock();
			var framebuffer = bitmap.FrameBuffer;
			int stride = framebuffer.RowBytes / sizeof(uint);
			Span<uint> pixels = new((void*)framebuffer.Address, stride * framebuffer.Size.Height);
			if(_genesisGraphics is not { } snapshot) {
				pixels.Clear();
				return;
			}

			ReadOnlySpan<uint> palette = snapshot.Palette.AsSpan(Math.Clamp(SelectedPalette, 0, 3) * 16, 16);
			bool grayscale = Config.UseGrayscalePalette;
			uint background = Config.Background switch {
				TileBackground.Transparent => 0,
				TileBackground.Black => 0xFF000000u,
				TileBackground.White => 0xFFFFFFFFu,
				TileBackground.Magenta => 0xFFFF00FFu,
				_ when grayscale => 0xFF000000u,
				TileBackground.PaletteColor => palette[0],
				_ => snapshot.Palette[snapshot.Graphics.Backdrop]
			};

			int rows = RowCount;
			int columns = ColumnCount;
			int tileHeight = snapshot.TileHeight;
			for(int row = 0; row < rows; row++) {
				for(int column = 0; column < columns; column++) {
					int address = GetGenesisTileAddress(column, row);
					int tileStart = row * tileHeight * stride + column * 8;
					for(int y = 0; y < tileHeight; y++) {
						Span<uint> line = pixels.Slice(tileStart + y * stride, 8);
						for(int x = 0; x < line.Length; x++) {
							int index = snapshot.ReadPatternPixel(address, x, y);
							uint color = grayscale ? 0xFF000000u | ((uint)index * 0x111111u) : palette[index];
							line[x] = index == 0 ? background : color;
						}
					}
				}
			}
		}
	}
}
