"""Compare a menu dictionary with the menu sources.

    python tools/lang/audit.py [dictionary.ini] [--prune] [--missing FILE] [--unwrapped]

Lists the menu text the dictionary does not translate yet (missing) and the
entries no menu text uses any more (unused). --prune deletes the unused
entries; --missing writes the missing text as ready-to-fill dictionary lines;
--unwrapped lists menu text drawn without TR(), which no dictionary can reach.
--check exits nonzero for such text (read-only CI check). Missing translations
remain allowed English fallbacks and do not fail the check.
Run from the repository root. The default dictionary is the shipped zh-CN.ini.
"""
import re
import sys
from pathlib import Path

LIT = r'"(?:[^"\\\n]|\\.)*"'
RUN = rf'{LIT}(?:\s*{LIT})*'
# TR("text"), TRF("text", ...), TRID("text") and TRC("text", "context").
CALL = re.compile(rf'\bTR(F|ID|C)?\(\s*({RUN})(?:\s*,\s*({LIT}))?')
PIECE = re.compile(r'"((?:[^"\\]|\\.)*)"')
ESCAPES = {'n': '\n', 't': '\t', '"': '"', '\\': '\\'}


def unescape(text):
    # \xNN escapes are UTF-8 bytes ("\xC2\xB7" is a middle dot).
    raw = re.sub(rb'\\x([0-9A-Fa-f]{2})', lambda m: bytes([int(m.group(1), 16)]), text.encode('utf-8'))
    return re.sub(r'\\(.)', lambda m: ESCAPES.get(m.group(1), m.group(1)), raw.decode('utf-8', 'replace'))


def source_keys():
    """(context, English) for every TR call in the menu sources."""
    keys = set()
    for path in sorted(Path('src').glob('Overlay*')):
        for m in CALL.finditer(path.read_text(encoding='utf-8')):
            english = unescape(''.join(PIECE.findall(m.group(2))))
            context = unescape(m.group(3)[1:-1]) if m.group(1) == 'C' and m.group(3) else ''
            keys.add((context, english))
    # Capability reasons are translated dynamically by the menu and shared with Apply/logs.
    admission_path = Path('src/FrameGen/NeuralAdmission.h')
    admission = admission_path.read_text(encoding='utf-8') if admission_path.is_file() else ''
    for text in re.findall(r'(?:return|=)\s*("(?:[^"\\]|\\.)*")\s*;', admission):
        value = unescape(text[1:-1])
        if ' ' in value and not value.endswith('.trpk'):
            keys.add(('', value))
    return keys


def source_texts():
    """Every string literal run in src/. Some menu text is translated through a
    variable (TR(names[i]), the status line's controller messages), so an entry
    whose English appears anywhere in the sources is not unused."""
    texts = set()
    for path in sorted(Path('src').rglob('*')):
        if path.suffix in ('.cpp', '.h'):
            for m in re.finditer(RUN, path.read_text(encoding='utf-8', errors='replace')):
                texts.add(unescape(''.join(PIECE.findall(m.group(0)))))
    return texts


COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
CHAR = re.compile(r"'(?:[^'\\\n]|\\.)'")
WRAPPED = re.compile(r'\bTR(?:F|ID|C)?\(\s*$')
CONTEXT_ARG = re.compile(r'\bTRC\(\s*' + RUN + r'\s*,\s*$')
SKIP = re.compile(r'logger::|spdlog::|ImGui::PushID|ImGui::BeginTable|ImGui::BeginChild|ImGui::OpenPopup|ImGui::BeginPopup|'
                  r'GetValue|SetValue|GetBoolValue|SetBoolValue|GetLongValue|SetLongValue|GetDoubleValue|SetDoubleValue|'
                  r'not translated:|translated where shown')


def unwrapped():
    """Prose literals in the menu sources outside TR()/TRF()/TRC()/TRID(): (file, line, text).
    A line containing 'not translated:' (with a reason) is left out, as are log lines,
    INI accessors and ImGui IDs. Text must contain a space and a word to count."""
    found = []
    for path in sorted(Path('src').glob('Overlay*')):
        source = path.read_text(encoding='utf-8')
        # Blank comments and char literals, keeping offsets and line numbers.
        blank = lambda m: re.sub(r'[^\n]', ' ', m.group(0))
        code = CHAR.sub(blank, COMMENT.sub(blank, source))
        lines = source.split('\n')
        for m in re.finditer(RUN, code):
            text = unescape(''.join(PIECE.findall(source[m.start():m.end()])))
            if ' ' not in text.strip() or not re.search(r'[A-Za-z]{2,}', text) or '##' in text:
                continue
            before = code[max(0, m.start() - 200):m.start()]
            if WRAPPED.search(before) or CONTEXT_ARG.search(before + source[m.start():m.end()] + ','):
                continue
            number = code.count('\n', 0, m.start()) + 1
            statement = '\n'.join(lines[max(0, number - 3):number])
            if SKIP.search(statement):
                continue
            found.append((path.as_posix(), number, text))
    return found


def split(line):
    quoted = False
    i = 0
    while i < len(line):
        if line[i] == '\\' and quoted:
            i += 1
        elif line[i] == '"':
            quoted = not quoted
        elif line[i] == '=' and not quoted:
            return line[:i].strip(), line[i + 1:].strip()
        i += 1
    return None


def decode(text):
    return unescape(text[1:-1]) if len(text) >= 2 and text[0] == text[-1] == '"' else text


def quote(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t') + '"'


def main(argv):
    output = argv[argv.index('--missing') + 1] if '--missing' in argv else None
    args = [a for a in argv if not a.startswith('--') and a != output]
    path = Path(args[0] if args else 'package/SKSE/Plugins/TheosRenderPipeline/Lang/zh-CN.ini')
    lines = path.read_text(encoding='utf-8-sig').splitlines()
    used = source_keys()
    literals = source_texts()
    section, entries, keep = '', set(), []
    for line in lines:
        s = line.strip()
        if s.startswith('['):
            section = s[1:-1].strip()
        elif s and s[0] not in ';#' and section.startswith('Strings') and (parts := split(s)):
            key = (section[8:], decode(parts[0]))
            entries.add(key)
            if key not in used and ('', key[1]) not in used and key[1] not in literals:
                continue  # unused
        keep.append(line)
    plain = {english for _, english in entries}
    missing = sorted(english for context, english in used if english not in plain and (context, english) not in entries)
    unused = len(lines) - len(keep)
    print(f'{path}: {len(entries)} entries, {len(missing)} missing, {unused} unused')
    if '--prune' in argv and unused:
        newline = '\r\n' if b'\r\n' in path.read_bytes() else '\n'
        path.write_bytes((newline.join(keep) + newline).encode('utf-8'))
        print(f'pruned {unused} unused entries')
    rows = []
    if '--unwrapped' in argv or '--check' in argv:
        rows = unwrapped()
        for name, number, text in rows:
            print(f'  unwrapped: {name}:{number}: {text[:110]}')
        print(f'{len(rows)} menu texts outside TR()')
    if output:
        target = Path(output)
        target.write_text(''.join(f'{quote(english)} =\n' for english in missing), encoding='utf-8')
        print(f'wrote {target}')
    elif missing and '--check' not in argv:
        for english in missing:
            print('  missing:', english.replace('\n', '\\n'))
    return 1 if '--check' in argv and rows else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
