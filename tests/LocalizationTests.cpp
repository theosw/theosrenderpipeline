#include "Localization.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace TheosRenderPipeline::Localization;

namespace
{
void Require(bool a_condition, const char* a_message)
{
	if (!a_condition) { throw std::runtime_error(a_message); }
}

bool Warned(const std::vector<std::string>& a_warnings, std::string_view a_needle)
{
	return std::ranges::any_of(a_warnings, [&](const auto& warning) { return warning.find(a_needle) != std::string::npos; });
}

std::string Text(const std::shared_ptr<const Table>& a_table, const char* a_english, const char* a_context = "")
{
	const char* text = a_table->Find(a_english, a_context);
	return text ? text : "";
}

// The file format: [Language], plain and quoted entries, escapes, comments,
// untranslated lines, contexts and the lines it skips.
void Format()
{
	std::vector<std::string> warnings;
	const auto table = Parse(R"ini(; a comment
[Language]
Code = ja-JP
Name = 日本語
Fonts = C:/Windows/Fonts/meiryo.ttc; msgothic.ttc
[Strings]
Apply = 適用
" (full)" = "（完全）"
"line" = "一行目\n二行目"
"quoted \"word\" = sign" = "引用"
Unfinished =
Deliberate = ""
Normal = 正常
[Strings.Textures]
Normal = 法線
this line has no separator
[Cooking]
Apply = ignored
)ini", "ja.ini", &warnings);
	const auto& info = table->Description();
	Require(info.code == "ja-JP" && info.name == "日本語", "[Language] Code and Name are read");
	Require(info.fonts.size() == 2 && info.fonts[1] == "msgothic.ttc", "Fonts is split at ';'");
	Require(Text(table, "Apply") == "適用", "a plain entry is found by its English text");
	Require(!table->Find("apply"), "keys are case sensitive");
	Require(Text(table, " (full)") == "（完全）", "quotes keep outer spaces");
	Require(Text(table, "line") == "一行目\n二行目", "\\n decodes");
	Require(Text(table, "quoted \"word\" = sign") == "引用", "escaped quotes and a quoted '=' stay in the key");
	Require(!table->Find("Unfinished"), "an empty translation keeps the English");
	Require(table->Find("Deliberate") && Text(table, "Deliberate").empty(), "a quoted empty translation is deliberate");
	Require(Text(table, "Normal", "Textures") == "法線", "a context entry wins in its context");
	Require(Text(table, "Normal", "Status") == "正常", "other contexts fall back to the plain entry");
	Require(Warned(warnings, "ja.ini:16: expected key = value"), "a broken line is reported with its line");
	Require(Warned(warnings, "ja.ini:17: unknown section"), "an unknown section is reported");
	Require(!table->Find("ignored"), "an unknown section's lines are skipped");
	Require(std::ranges::find(table->Codepoints(), U'適') != table->Codepoints().end(), "the translations' glyphs are listed");
	Require(Codepoints("Auto · 简体中文").size() == 5, "Codepoints lists each non-ASCII character once");
}

// A translation used as a printf format must take the same arguments, or ImGui
// reads the wrong ones; such an entry keeps the English text.
void Conversions()
{
	std::vector<std::string> warnings;
	const auto table = Parse(R"ini([Strings]
"Pass %d: %.1f%%" = "第 %d 遍：%.1f%%"
"Limited to %.0f%% so the menu fits." = "限制为 %s，以便菜单适合。"
"NR Pass %d: %s" = "NR %s"
"Uses about 19% fewer model pixels." = "约少 19% 的模型像素。"
"{} passes" = "{} 遍 (100% 输入)"
"At Intel's default (0.70)." = "{%s%s%s}"
"{} frames" = "{} %s"
)ini", "formats.ini", &warnings);
	Require(table->Find("Pass %d: %.1f%%"), "matching conversions are kept");
	Require(!table->Find("Limited to %.0f%% so the menu fits."), "a different conversion keeps English");
	Require(!table->Find("NR Pass %d: %s"), "a missing conversion keeps English");
	Require(Warned(warnings, "formats.ini:3:") && Warned(warnings, "formats.ini:4:"), "each rejected entry is reported");
	Require(table->Find("Uses about 19% fewer model pixels."), "a percent sign in prose is not a conversion");
	Require(table->Find("{} passes"), "a percent sign in std::format text is not a conversion");
	Require(!table->Find("At Intel's default (0.70)."), "a brace does not let a translation add conversions to plain text");
	Require(!table->Find("{} frames"), "std::format text cannot gain printf conversions");
	Require(warnings.size() == 4, "only the mismatched entries warn");
}

// Activation from the Lang folder, Translate, TRF and the catalogue.
void Activation()
{
	const auto directory = Directory();
	const auto path = directory / "xx-TEST.ini";
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	{
		std::ofstream file(path, std::ios::binary);
		// A UTF-8 BOM and CRLF line endings, as Notepad saves them.
		file << "\xEF\xBB\xBF[Language]\r\nCode = xx-TEST\r\nName = Testish\r\n[Strings]\r\nApply = Applique\r\n"
		        "\"{} passes\" = \"{} passages\"\r\n\"Broken {}\" = \"Cassé {} {}\"\r\n";
	}
	Require(Activate("xx-TEST"), "a dictionary is activated by its file name");
	Require(ActiveName() == "Testish" && std::string(TR("Apply")) == "Applique", "translations apply after activation");
	Require(TRF("{} passes", 3) == "3 passages", "TRF formats the translation");
	Require(TRF("Broken {}", 3) == "Broken 3", "a translation that does not fit its arguments formats the English");
	Require(std::string(TR("Not in the file")) == "Not in the file", "missing text stays English");
	Require(TRID("Apply") == "Applique###Apply", "TRID keeps the English as the ImGui ID");
	const char* applied = TR("Apply");
	Require(std::ranges::any_of(Catalog(), [](const Info& a) { return a.code == "xx-TEST" && a.name == "Testish"; }),
		"the catalogue lists the dictionary");

	Require(!Activate("de-DE"), "a missing dictionary reports failure");
	Require(!LastError().empty() && std::string(TR("Apply")) == "Applique", "a failed activation keeps the current language");
	Require(Activate("en") && LastError().empty() && std::string(TR("Apply")) == "Apply", "'en' selects English");
	Require(std::string(applied) == "Applique", "a translation handed out earlier stays valid");
	Require(Activate("auto"), "'auto' always succeeds");
	std::filesystem::remove(path, error);
}

// The dictionary that ships in the package parses cleanly.
void Packaged(const std::filesystem::path& a_path)
{
	std::vector<std::string> warnings;
	const auto table = Load(a_path, &warnings);
	Require(table && warnings.empty(), "the packaged dictionary parses without warnings");
	Require(table->Description().code == "zh-CN" && table->Count() > 500, "the packaged dictionary is zh-CN with its entries");
}
}

int main(int argc, char** argv)
{
	try {
		Format();
		Conversions();
		Activation();
		if (argc > 1) { Packaged(argv[1]); }
		std::puts("PASS dictionary format, conversions, activation and package");
		return 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "FAIL %s\n", error.what());
		return 1;
	}
}
