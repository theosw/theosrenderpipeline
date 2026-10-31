#pragma once

#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Menu languages. Menu text is written in English in the source and passed
// through TR(); a dictionary maps that English text to a translation:
//
//   Data\SKSE\Plugins\TheosRenderPipeline\Lang\<code>.ini
//   [Language]
//   Code = zh-CN
//   Name = 简体中文
//   Fonts = msyh.ttc
//   [Strings]
//   "Every frame" = "每一帧"
//
// Text a dictionary does not translate stays English. [Strings.<context>] holds
// the few words that need a different translation in one place (TRC). Settings
// keys, weather override names and log lines are never translated. The menu
// uses TR; CS startup recovery uses the same dictionary before UI initialization.
// LOCALIZATION.md describes the format.
namespace TheosRenderPipeline::Localization
{
	struct Info
	{
		std::string code;                // [Language] Code, else the file name: "zh-CN"
		std::string name;                // [Language] Name, in its own language: "简体中文"
		std::vector<std::string> fonts;  // [Language] Fonts: a path, or a file in %WINDIR%\Fonts
		std::filesystem::path path;
	};

	// A parsed dictionary. Never changed after parsing, and kept alive once
	// installed, so a translation pointer stays valid for the whole session.
	class Table
	{
	public:
		const Info& Description() const noexcept { return info; }
		std::size_t Count() const noexcept { return strings.size(); }
		// The code points beyond ASCII the translations need, sorted and unique.
		const std::vector<char32_t>& Codepoints() const noexcept { return codepoints; }
		// The translation, or nullptr. A context entry wins over the plain one.
		const char* Find(std::string_view a_english, std::string_view a_context = {}) const;

		Info info;
		// Keys are the English text, or "<context>\x1F<English text>".
		std::unordered_map<std::string, std::string> strings;
		std::vector<char32_t> codepoints;
	};

	// Data\SKSE\Plugins\TheosRenderPipeline\Lang, or the folder SetDirectory chose.
	std::filesystem::path Directory();
	// Read language files from another folder. The menu preview uses it to read the
	// repository's package/ copy, so an edited file shows on the next language pick.
	void SetDirectory(std::filesystem::path a_directory);
	// Every dictionary in Directory() with at least one translation, by code.
	std::vector<Info> Catalog();
	// a_origin names the file in warnings; each skipped line is reported there.
	std::shared_ptr<const Table> Parse(std::string_view a_text, std::string_view a_origin,
		std::vector<std::string>* a_warnings = nullptr);
	std::shared_ptr<const Table> Load(const std::filesystem::path& a_file, std::vector<std::string>* a_warnings = nullptr);

	// "en" or "" selects English, "auto" the Windows display language (English
	// when no dictionary matches it), anything else a dictionary by file name or
	// [Language] Code. A missing dictionary keeps the current language, sets
	// LastError and returns false.
	bool Activate(std::string_view a_codeOrAuto);
	std::shared_ptr<const Table> Active();
	std::string ActiveName();      // "English", or the dictionary's Name
	std::string LastError();       // why the last Activate failed; empty on success
	std::string AutoCode();        // the Windows display language, "zh-CN"; "" when unknown
	// Where warnings go (the plugin log). Without a sink they are dropped.
	void SetLogSink(void (*a_sink)(std::string_view a_message));

	// The code points beyond ASCII in UTF-8 text, sorted and unique.
	std::vector<char32_t> Codepoints(std::string_view a_text);

	const char* Translate(const char* a_english, const char* a_context = nullptr) noexcept;
	std::string TranslateText(std::string_view a_english);

	// std::vformat cannot take a format string checked at compile time, so a
	// translation that does not fit its arguments throws; the English is used then.
	template <class... Args>
	std::string Format(const char* a_english, Args&&... a_args)
	{
		const char* translated = Translate(a_english);
		try {
			return std::vformat(translated, std::make_format_args(a_args...));
		} catch (const std::exception&) {
			try {
				return std::vformat(a_english, std::make_format_args(a_args...));
			} catch (const std::exception&) {
				return a_english;
			}
		}
	}
}

// TR marks menu text for translation; TRC adds a context; TRF formats like
// std::format. TRID is for widgets that keep state by ID (tabs, collapsing
// headers): "<translation>###<English>", so a language switch keeps the open tab.
#define TR(text) ::TheosRenderPipeline::Localization::Translate(text)
#define TRC(text, context) ::TheosRenderPipeline::Localization::Translate(text, context)
#define TRF(format, ...) ::TheosRenderPipeline::Localization::Format(format, __VA_ARGS__)
#define TRID(text) (std::string(::TheosRenderPipeline::Localization::Translate(text)) + "###" text)
