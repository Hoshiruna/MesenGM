using CommunityToolkit.Mvvm.ComponentModel;

namespace Mesen.Config
{
	public partial class McpServerConfig : BaseConfig<McpServerConfig>
	{
		[ObservableProperty] public partial ushort Port { get; set; } = 51234;
	}
}
