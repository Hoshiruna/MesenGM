using Mesen.Config;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Xml;

namespace Mesen.Localization
{
	public static class ResourceHelper
	{
		private const string DefaultLanguageCode = "en";
		private const string ResourcePrefix = "Mesen.Localization.resources.";
		private const string ResourceSuffix = ".xml";

		private static XmlDocument _resources = new XmlDocument();
		private static readonly Dictionary<Enum, string> EnumLabelCache = new();
		private static readonly Dictionary<string, string> ViewLabelCache = new();
		private static readonly Dictionary<string, string> MessageCache = new();
		private static readonly Dictionary<string, Type> EnumTypes = GetEnumTypes();

		private static Dictionary<string, Type> GetEnumTypes()
		{
#pragma warning disable IL2026
			return Assembly.GetExecutingAssembly().GetTypes().Where(type => type.IsEnum).ToDictionary(type => type.Name);
#pragma warning restore IL2026
		}

		public static void LoadResources()
		{
			EnumLabelCache.Clear();
			ViewLabelCache.Clear();
			MessageCache.Clear();

			//English is always loaded first and acts as a complete fallback.
			LoadEmbeddedResource(DefaultLanguageCode, true);
			LoadExternalResource(DefaultLanguageCode);

			string language = NormalizeLanguageCode(ConfigManager.Config.Preferences.Language);
			if(language != DefaultLanguageCode) {
				LoadEmbeddedResource(language, false);
				LoadExternalResource(language);
			}
		}

		private static void LoadEmbeddedResource(string language, bool updateResourceDocument)
		{
			Assembly assembly = Assembly.GetExecutingAssembly();
			using Stream? stream = assembly.GetManifestResourceStream(ResourcePrefix + language + ResourceSuffix);
			if(stream != null) {
				LoadResourceDocument(stream, updateResourceDocument);
			}
		}

		private static void LoadExternalResource(string language)
		{
			string path = Path.Combine(ConfigManager.HomeFolder, "Localization", "resources." + language + ".xml");
			if(File.Exists(path)) {
				try {
					using FileStream stream = File.OpenRead(path);
					LoadResourceDocument(stream, false);
				} catch {
					//An invalid optional translation must not prevent the UI from starting.
				}
			}
		}

		private static void LoadResourceDocument(Stream stream, bool updateResourceDocument)
		{
			try {
				XmlReaderSettings settings = new() { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null };
				using XmlReader reader = XmlReader.Create(stream, settings);
				XmlDocument resources = new() { XmlResolver = null };
				resources.Load(reader);

				if(updateResourceDocument) {
					_resources = resources;
				}

				foreach(XmlNode node in resources.SelectNodes("/Resources/Messages/Message")!) {
					string? id = node.Attributes?["ID"]?.Value;
					if(id != null) {
						MessageCache[id] = node.InnerText;
					}
				}

				foreach(XmlNode node in resources.SelectNodes("/Resources/Enums/Enum")!) {
					string? enumName = node.Attributes?["ID"]?.Value;
					if(enumName == null || !EnumTypes.TryGetValue(enumName, out Type? enumType)) {
						//Partial/external translations can outlive an enum that was removed.
						continue;
					}

					foreach(XmlNode enumNode in node.ChildNodes) {
						string? id = enumNode.Attributes?["ID"]?.Value;
						if(id != null && Enum.TryParse(enumType, id, out object? value) && value is Enum enumValue) {
							EnumLabelCache[enumValue] = enumNode.InnerText;
						}
					}
				}

				foreach(XmlNode node in resources.SelectNodes("/Resources/Forms/Form")!) {
					string? viewName = node.Attributes?["ID"]?.Value;
					if(viewName == null) {
						continue;
					}

					foreach(XmlNode formNode in node.ChildNodes) {
						if(formNode is XmlElement element && element.Attributes["ID"]?.Value is string id) {
							ViewLabelCache[viewName + "_" + id] = element.InnerText;
						}
					}
				}
			} catch {
				//Keep the resources already loaded from the fallback language.
			}
		}

		private static string NormalizeLanguageCode(string? language)
		{
			return string.IsNullOrWhiteSpace(language) ? DefaultLanguageCode : language.Trim().Replace('_', '-').ToLowerInvariant();
		}

		public static List<LanguageOption> GetAvailableLanguages()
		{
			HashSet<string> languages = new(StringComparer.OrdinalIgnoreCase) { DefaultLanguageCode };
			foreach(string resourceName in Assembly.GetExecutingAssembly().GetManifestResourceNames()) {
				if(resourceName.StartsWith(ResourcePrefix, StringComparison.Ordinal) && resourceName.EndsWith(ResourceSuffix, StringComparison.Ordinal)) {
					languages.Add(resourceName[ResourcePrefix.Length..^ResourceSuffix.Length]);
				}
			}

			string externalFolder = Path.Combine(ConfigManager.HomeFolder, "Localization");
			if(Directory.Exists(externalFolder)) {
				foreach(string file in Directory.EnumerateFiles(externalFolder, "resources.*.xml")) {
					string filename = Path.GetFileName(file);
					languages.Add(filename["resources.".Length..^ResourceSuffix.Length]);
				}
			}

			return languages
				.Select(NormalizeLanguageCode)
				.Distinct(StringComparer.OrdinalIgnoreCase)
				.Select(code => new LanguageOption(code, GetLanguageDisplayName(code)))
				.OrderBy(language => language.DisplayName, StringComparer.CurrentCulture)
				.ToList();
		}

		private static string GetLanguageDisplayName(string language)
		{
			return language switch {
				"en" => "English",
				"zh-cn" => "中文（简体）",
				"ja" => "日本語",
				_ => GetCultureDisplayName(language)
			};
		}

		private static string GetCultureDisplayName(string language)
		{
			try {
				return CultureInfo.GetCultureInfo(language).NativeName;
			} catch(CultureNotFoundException) {
				return language;
			}
		}

		public static string GetMessage(string id, params object[] args)
		{
			return MessageCache.TryGetValue(id, out string? text) ? string.Format(text, args) : "[[" + id + "]]";
		}

		public static string GetEnumText(Enum value)
		{
			return EnumLabelCache.TryGetValue(value, out string? text) ? text : "[[" + value + "]]";
		}

		public static Enum[] GetEnumValues(Type type)
		{
			List<Enum> values = new();
			XmlNode? node = _resources.SelectSingleNode("/Resources/Enums/Enum[@ID='" + type.Name + "']");
			if(node?.Attributes?["ID"]?.Value == type.Name) {
				foreach(XmlNode enumNode in node.ChildNodes) {
					string? id = enumNode.Attributes?["ID"]?.Value;
					if(id != null && Enum.TryParse(type, id, out object? value) && value is Enum enumValue) {
						values.Add(enumValue);
					}
				}
			}
			return values.ToArray();
		}

		public static string GetViewLabel(string view, string control)
		{
			return ViewLabelCache.TryGetValue(view + "_" + control, out string? text) ? text : $"[{view}:{control}]";
		}
	}

	public record LanguageOption(string Code, string DisplayName);
}
