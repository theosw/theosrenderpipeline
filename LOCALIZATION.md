# Menu languages

The menu's text is written in English in the source and looked up in a language
file before it is drawn, so a language is added or corrected without rebuilding
the plugin. Text a file does not translate stays English, so a partial file works.

One file per language, inside the mod (or Skyrim's `Data` folder):

```
SKSE/Plugins/TheosRenderPipeline/Lang/<code>.ini
```

TRP ships `zh-CN.ini`. To replace it, put your own `zh-CN.ini` at the same path in
a mod placed below TRP in MO2's left pane. Keep it updated with new releases:
text a release adds shows in English until your file translates it.

## Choosing the language

The button next to Zoom at the top of the menu lists Automatic (Windows), English
and every language file, each under its own name. A choice applies at once;
**Save as default** keeps it. `[Overlay] Language` in `TheosRenderPipeline.ini`
does the same: `auto` (the default) follows the Windows display language, `en`
keeps English, and any other value names a file by its name or `Code`.

## File format

UTF-8, with or without a BOM:

```ini
[Language]
Code = zh-CN
Name = 简体中文
; Fonts with the characters the translations need: a path, or a file in
; %WINDIR%\Fonts. Windows' Chinese, Japanese and Korean fonts are tried after them.
Fonts = msyh.ttc; simhei.ttf

[Strings]
Apply = 应用
"Every frame" = "每一帧"
" (full)" = "（完整）"
"Line one\nline two" = "第一行\n第二行"
Unfinished =

[Strings.OverlayImagePanel]
Normal = 法线
```

- The left side is the English text exactly as the menu shows it; the right side
  is the translation. Matching is case sensitive.
- Quotes are needed when the text starts or ends with a space or contains `=`.
  Inside quotes, `\n`, `\t`, `\"` and `\\` are escapes.
- An empty right side means "not translated yet" and shows the English. `""` is
  a deliberately empty translation.
- Keep every `%d`, `%s`, `%.1f` (and `%%`) of the English, in the same order. A
  line whose `%` placeholders differ is skipped with a warning in
  `TheosRenderPipeline.log`, because a wrong placeholder could crash the game.
  `{}` placeholders must stay too; if they don't fit, the English is shown.
- `[Strings.<context>]` translates a word differently in one place, where the
  menu asks for that context. Elsewhere the `[Strings]` entry applies.
- Lines starting with `;` or `#` are comments.

## Checking a file

`python tools/lang/audit.py path\to\<code>.ini` lists the menu text the file
does not translate yet and the entries nothing uses any more. Problems the game
finds while loading a file are written to `TheosRenderPipeline.log`.

## For developers

Wrap menu text in `TR("...")`, formatted text in `TRF("...", args)`, and the
labels of tabs and collapsing headers in `TRID("...")`, which keeps the English as
the ImGui ID so switching language keeps their state. Only the menu (`src/Overlay*`)
translates: settings keys, weather override names and log lines stay English.

Run `python tools/lang/audit.py --check` before committing menu changes. It fails
when menu prose cannot be reached through translation wrappers; missing dictionary
entries remain allowed English fallbacks. The localization workflow runs this
check and its CLI regression tests without requiring renderer dependencies.
