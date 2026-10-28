#include "Localization.h"
#include "PluginPaths.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <mutex>

namespace TheosRenderPipeline::Localization
{
	namespace
	{
		void (*g_sink)(std::string_view){};
		std::atomic<const Table*> g_active{ nullptr };
		std::mutex g_mutex;                                  // guards the state below, not lookups
		std::vector<std::shared_ptr<const Table>> g_tables;  // every table installed, kept alive
		std::shared_ptr<const Table> g_installed;
		std::string g_error;
		std::filesystem::path g_directory;                   // SetDirectory's folder, else empty
		std::vector<Info> g_catalog;
		bool g_catalogLoaded{};

		std::string Lower(std::string_view a_text)
		{
			std::string text(a_text);
			for (auto& c : text) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
			return text;
		}

		std::string_view Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r");
			if (first == std::string_view::npos) { return {}; }
			return a_text.substr(first, a_text.find_last_not_of(" \t\r") - first + 1);
		}

		// Quoted text keeps its outer spaces and reads \n, \t, \" and \\.
		bool Decode(std::string_view a_text, std::string& a_out, bool& a_quoted)
		{
			a_quoted = a_text.size() >= 2 && a_text.front() == '"' && a_text.back() == '"';
			if (!a_quoted) {
				a_out.assign(a_text);
				return a_text.empty() || a_text.front() != '"';
			}
			a_out.clear();
			for (std::size_t i = 1; i + 1 < a_text.size(); ++i) {
				char c = a_text[i];
				if (c == '\\' && i + 2 < a_text.size()) {
					c = a_text[++i];
					c = c == 'n' ? '\n' : c == 't' ? '\t' : c;
				}
				a_out.push_back(c);
			}
			return true;
		}

		// The '=' that splits "key = value": the first one outside quotes.
		std::size_t Separator(std::string_view a_line)
		{
			bool quoted = false;
			for (std::size_t i = 0; i < a_line.size(); ++i) {
				if (a_line[i] == '\\' && quoted) { ++i; }
				else if (a_line[i] == '"') { quoted = !quoted; }
				else if (a_line[i] == '=' && !quoted) { return i; }
			}
			return std::string_view::npos;
		}

		// The printf conversions in a_text, in order, without %%. Labels reach
		// ImGui::Text, tooltips, slider formats and snprintf as printf formats, where
		// a missing or changed %s reads the wrong argument. Text with std::format
		// fields goes through Format instead, which falls back on its own, but is
		// checked too: a translation must not add conversions behind a brace.
		std::string Conversions(std::string_view a_text)
		{
			std::string result;
			for (std::size_t i = 0; i < a_text.size(); ++i) {
				if (a_text[i] != '%') { continue; }
				std::size_t j = i + 1;
				if (j < a_text.size() && a_text[j] == '%') { i = j; continue; }
				while (j < a_text.size() && std::string_view("-+#0123456789.*hlLqjzt").find(a_text[j]) != std::string_view::npos) { ++j; }
				if (j < a_text.size() && std::string_view("diouxXeEfFgGaAcspn").find(a_text[j]) != std::string_view::npos) {
					result.append(a_text.substr(i, j - i + 1)).push_back('|');
					i = j;
				}
			}
			return result;
		}

		std::vector<std::filesystem::path> DictionaryFiles()
		{
			std::vector<std::filesystem::path> files;
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(Directory(), error)) {
				if (entry.is_regular_file(error) && Lower(entry.path().extension().string()) == ".ini") {
					files.push_back(entry.path());
				}
			}
			std::sort(files.begin(), files.end());
			return files;
		}

		void Install(std::shared_ptr<const Table> a_table)
		{
			std::lock_guard lock(g_mutex);
			if (a_table) { g_tables.push_back(a_table); }
			g_installed = std::move(a_table);
			g_active.store(g_installed.get(), std::memory_order_release);
			g_error.clear();
			g_catalogLoaded = false;
		}
	}

	const char* Table::Find(std::string_view a_english, std::string_view a_context) const
	{
		if (!a_context.empty()) {
			if (const auto entry = strings.find(std::string(a_context) + '\x1F' + std::string(a_english)); entry != strings.end()) {
				return entry->second.c_str();
			}
		}
		const auto entry = strings.find(std::string(a_english));
		return entry != strings.end() ? entry->second.c_str() : nullptr;
	}

	std::filesystem::path Directory()
	{
		{
			std::lock_guard lock(g_mutex);
			if (!g_directory.empty()) { return g_directory; }
		}
		// The game's virtual Data tree, wherever Mod Organizer starts the game from.
		const auto plugins = PluginPaths::Directory();
		return (plugins.empty() ? std::filesystem::path(L"Data/SKSE/Plugins") : plugins) / L"TheosRenderPipeline" / L"Lang";
	}

	std::shared_ptr<const Table> Parse(std::string_view a_text, std::string_view a_origin, std::vector<std::string>* a_warnings)
	{
		auto table = std::make_shared<Table>();
		const auto warn = [&](std::size_t a_line, std::string_view a_message) {
			const auto text = std::format("{}:{}: {}", a_origin, a_line, a_message);
			if (a_warnings) { a_warnings->push_back(text); }
			if (g_sink) { g_sink(text); }
		};
		if (a_text.starts_with("\xEF\xBB\xBF")) { a_text.remove_prefix(3); }  // UTF-8 BOM
		std::string section;
		std::size_t number = 0;
		for (std::size_t position = 0; position <= a_text.size();) {
			const auto end = (std::min)(a_text.find('\n', position), a_text.size());
			const auto line = Trim(a_text.substr(position, end - position));
			position = end + 1;
			++number;
			if (line.empty() || line.front() == ';' || line.front() == '#') { continue; }
			if (line.front() == '[') {
				section = line.back() == ']' ? std::string(Trim(line.substr(1, line.size() - 2))) : std::string();
				if (section != "Language" && section != "Strings" && !section.starts_with("Strings.")) {
					warn(number, "unknown section; its lines are skipped");
				}
				continue;
			}
			const auto split = Separator(line);
			std::string key, value;
			bool keyQuoted{}, valueQuoted{};
			if (split == std::string_view::npos || !Decode(Trim(line.substr(0, split)), key, keyQuoted) ||
				!Decode(Trim(line.substr(split + 1)), value, valueQuoted) || key.empty()) {
				warn(number, "expected key = value");
				continue;
			}
			if (section == "Language") {
				const auto name = Lower(key);
				if (name == "code") { table->info.code = value; }
				else if (name == "name") { table->info.name = value; }
				else if (name == "fonts") {
					for (std::size_t start = 0; start <= value.size();) {
						const auto stop = (std::min)(value.find(';', start), value.size());
						if (const auto font = Trim(std::string_view(value).substr(start, stop - start)); !font.empty()) {
							table->info.fonts.emplace_back(font);
						}
						start = stop + 1;
					}
				}
				continue;
			}
			if (section != "Strings" && !section.starts_with("Strings.")) { continue; }
			if (value.empty() && !valueQuoted) { continue; }  // not translated yet: stays English
			if (Conversions(key) != Conversions(value)) {
				warn(number, "the translation's % conversions differ from the English; English is kept");
				continue;
			}
			const auto context = section.size() > 8 ? section.substr(8) : std::string();
			const auto glyphs = Codepoints(value);
			table->codepoints.insert(table->codepoints.end(), glyphs.begin(), glyphs.end());
			table->strings[context.empty() ? key : context + '\x1F' + key] = std::move(value);
		}
		std::sort(table->codepoints.begin(), table->codepoints.end());
		table->codepoints.erase(std::unique(table->codepoints.begin(), table->codepoints.end()), table->codepoints.end());
		return table;
	}

	std::shared_ptr<const Table> Load(const std::filesystem::path& a_file, std::vector<std::string>* a_warnings)
	{
		std::ifstream file(a_file, std::ios::binary);
		if (!file) { return nullptr; }
		const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		auto parsed = Parse(text, a_file.filename().string(), a_warnings);
		auto table = std::const_pointer_cast<Table>(parsed);
		table->info.path = a_file;
		if (table->info.code.empty()) { table->info.code = a_file.stem().string(); }
		if (table->info.name.empty()) { table->info.name = table->info.code; }
		return table;
	}

	void SetDirectory(std::filesystem::path a_directory)
	{
		std::lock_guard lock(g_mutex);
		g_directory = std::move(a_directory);
		g_catalogLoaded = false;
	}

	std::vector<Info> Catalog()
	{
		{
			std::lock_guard lock(g_mutex);
			if (g_catalogLoaded) { return g_catalog; }
		}
		std::vector<Info> catalog;
		for (const auto& file : DictionaryFiles()) {
			if (const auto table = Load(file); table && table->Count()) { catalog.push_back(table->info); }
		}
		std::sort(catalog.begin(), catalog.end(), [](const Info& a, const Info& b) { return a.code < b.code; });
		std::lock_guard lock(g_mutex);
		g_catalog = catalog;
		g_catalogLoaded = true;
		return catalog;
	}

	bool Activate(std::string_view a_codeOrAuto)
	{
		const std::string request(Trim(a_codeOrAuto));
		const bool automatic = Lower(request) == "auto";
		const auto code = Lower(automatic ? AutoCode() : request);
		if (code.empty() || code == "en" || code.starts_with("en-")) {
			Install(nullptr);
			return true;
		}
		// The file name or its [Language] Code (Load falls back to the file name).
		for (const auto& file : DictionaryFiles()) {
			if (const auto table = Load(file); table && table->Count() &&
				(Lower(file.stem().string()) == code || Lower(table->info.code) == code)) {
				Install(table);
				return true;
			}
		}
		if (automatic) {
			// Windows asks for a language nobody has translated yet: English.
			Install(nullptr);
			return true;
		}
		const auto directory = Directory().string();  // takes the lock itself
		std::lock_guard lock(g_mutex);
		g_error = std::format("no dictionary for '{}' in {}", request, directory);
		if (g_sink) { g_sink(g_error); }
		return false;
	}

	std::shared_ptr<const Table> Active()
	{
		std::lock_guard lock(g_mutex);
		return g_installed;
	}

	std::string ActiveName()
	{
		const auto table = Active();
		return table ? table->info.name : std::string("English");
	}

	std::string LastError()
	{
		std::lock_guard lock(g_mutex);
		return g_error;
	}

	std::string AutoCode()
	{
		wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
		if (::LCIDToLocaleName(MAKELCID(::GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) <= 0) {
			return {};
		}
		std::string code;
		for (const wchar_t* c = name; *c; ++c) { code.push_back(static_cast<char>(*c)); }  // locale names are ASCII
		return code;
	}

	void SetLogSink(void (*a_sink)(std::string_view)) { g_sink = a_sink; }

	std::vector<char32_t> Codepoints(std::string_view a_text)
	{
		std::vector<char32_t> result;
		for (std::size_t i = 0; i < a_text.size();) {
			const auto lead = static_cast<unsigned char>(a_text[i]);
			const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 0;
			if (!length || i + length > a_text.size()) { ++i; continue; }  // a malformed byte is skipped
			char32_t value = length == 1 ? lead : lead & (0x7F >> length);
			for (std::size_t k = 1; k < length; ++k) { value = (value << 6) | (static_cast<unsigned char>(a_text[i + k]) & 0x3F); }
			if (value > 0x7F) { result.push_back(value); }
			i += length;
		}
		std::sort(result.begin(), result.end());
		result.erase(std::unique(result.begin(), result.end()), result.end());
		return result;
	}

	const char* Translate(const char* a_english, const char* a_context) noexcept
	{
		if (!a_english) { return ""; }
		const auto* table = g_active.load(std::memory_order_acquire);
		if (!table) { return a_english; }
		try {
			const auto* text = table->Find(a_english, a_context ? a_context : "");
			return text ? text : a_english;
		} catch (...) {
			return a_english;
		}
	}

	std::string TranslateText(std::string_view a_english)
	{
		return Translate(std::string(a_english).c_str());
	}
}
