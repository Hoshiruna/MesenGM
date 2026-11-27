using System.Runtime.InteropServices;

namespace Mesen.Interop
{
	[StructLayout(LayoutKind.Sequential)]
	public struct GenesisM68KState : BaseState
	{
		[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
		public uint[] D;
		[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
		public uint[] A;
		public uint PC;
		public ushort SR;
		public uint SSP;
		[MarshalAs(UnmanagedType.I1)]
		public bool Stopped;
		public ulong CycleCount;
	}

	[StructLayout(LayoutKind.Sequential)]
	public struct GenesisZ80State : BaseState
	{
		public byte A, Flags;
		public byte B, C, D, E, H, L;
		public ushort IX, IY;
		public ushort SP, PC;
		public byte I, R;
		[MarshalAs(UnmanagedType.I1)]
		public bool Halted;
		public ulong CycleCount;

		public ushort AF => (ushort)((A << 8) | Flags);
		public ushort BC => (ushort)((B << 8) | C);
		public ushort DE => (ushort)((D << 8) | E);
		public ushort HL => (ushort)((H << 8) | L);
	}
}
