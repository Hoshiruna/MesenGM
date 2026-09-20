using CommunityToolkit.Mvvm.ComponentModel;

namespace Mesen.Config
{
	public partial class McpServerConfig : BaseConfig<McpServerConfig>
	{
		[ObservableProperty] public partial ushort Port { get; set; } = 51234;

		/// <summary>
		/// Shows MCPServer.exe's own console window instead of capturing its log.
		/// The two are mutually exclusive, so the MCP Server window's log stays
		/// empty while this is enabled.
		/// </summary>
		[ObservableProperty] public partial bool ShowConsoleWindow { get; set; } = false;
	}
}
