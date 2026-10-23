# Language tooling

`audit.py` compares a menu dictionary with the menu sources (`src/Overlay*`):

```powershell
python tools/lang/audit.py                         # the shipped zh-CN.ini
python tools/lang/audit.py path\to\de-DE.ini        # another dictionary
python tools/lang/audit.py --missing out\missing.ini  # untranslated text as ready-to-fill lines
python tools/lang/audit.py --prune                  # delete entries nothing uses any more
python tools/lang/audit.py --unwrapped              # menu text drawn without TR(); should be 0
```

It reports the menu text the dictionary does not translate yet (missing) and the
entries whose English no longer appears in the sources (unused). Text that the
menu translates through a variable (lookup arrays, the status line's messages)
counts as used when its English appears anywhere in `src/`.

`--unwrapped` lists prose in the menu sources that no dictionary can reach: text
drawn without `TR()`, or dropped untranslated into a translated format. Product
names and text translated where it is shown (a lookup array, a status from the
backend) are marked on their line with `// not translated: <reason>` or
`// translated where shown`.

Only the menu sources use `TR`. Settings keys, weather override names and log
lines stay English: a translated key would point at a section that no longer
exists once a dictionary is active. New menu text gets `TR()` by hand when it
is written; see [LOCALIZATION.md](../../LOCALIZATION.md).
