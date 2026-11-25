#!/usr/bin/env python3
"""
extract-strings.py - Collect the translatable strings of hoplab.

    tools/extract-strings.py [FILE.c ...]          write locale/hoplab.pot
    tools/extract-strings.py -o FILE [FILE.c ...]  write the template elsewhere ("-" = stdout)
    tools/extract-strings.py --check locale/it [FILE.c ...]
                                                   compare the sources with the catalogs

A translatable string is the argument of _("...") or N_("..."): one or more
adjacent C string literals (possibly on several lines). Comments are
skipped, C escapes are decoded, UTF-8 is kept as it is.

--check loads every .po file of a directory and lists the source strings
with no translation in any of them (exit status 1 when there are any) and
the catalog entries that match no source string any more.

Without file arguments the default set is used: the shared modules, the
six demo programs and src/*.c. (xgettext loses non-ASCII characters of the
msgids in some environments, hence this script.)
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = ["i18n.c", "rdt.c", "channel.c", "tui.c",
           "stop_and_wait_send.c", "stop_and_wait_recv.c",
           "go_back_n_send.c", "go_back_n_recv.c",
           "selective_repeat_send.c", "selective_repeat_recv.c"]

SIMPLE = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b", "f": "\f",
          "v": "\v", "\\": "\\", '"': '"', "'": "'", "?": "?", "e": "\033"}


def strip_comments(src):
    """Replace comments with spaces (newlines kept), leaving literals alone."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == "\\":
                    j += 1
                if j < n and src[j] == "\n" and c == "'":
                    break
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in src[i:j]))
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def decode_literal(src, i):
    """src[i] is the opening quote. Returns (bytes, index after the closing quote)."""
    out = bytearray()
    i += 1
    n = len(src)
    while i < n and src[i] != '"':
        c = src[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        i += 1
        c = src[i]
        if c in "01234567":
            j = i
            while j < n and j < i + 3 and src[j] in "01234567":
                j += 1
            out.append(int(src[i:j], 8) & 0xFF)
            i = j
        elif c == "x":
            j = i + 1
            while j < n and src[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(src[i + 1:j], 16) & 0xFF)
            i = j
        elif c == "\n":                 # line continuation inside a literal
            i += 1
        else:
            out += SIMPLE.get(c, c).encode("utf-8")
            i += 1
    return bytes(out), i + 1


def is_ident(ch):
    return ch.isalnum() or ch == "_"


def extract(path):
    """Yield (msgid, line) for every _()/N_() call with literal arguments."""
    with open(path, encoding="utf-8") as f:
        src = strip_comments(f.read())
    n = len(src)
    i = 0
    while i < n:
        c = src[i]
        if c == '"':                    # a literal outside _(): skip it whole
            _, i = decode_literal(src, i)
            continue
        if c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                if src[j] == "\\":
                    j += 1
                j += 1
            i = j + 1
            continue
        if c == "_" and (i == 0 or not is_ident(src[i - 1]) or
                         (src[i - 1] == "N" and (i < 2 or not is_ident(src[i - 2])))):
            j = i + 1
            while j < n and src[j] in " \t\n":
                j += 1
            if j < n and src[j] == "(":
                j += 1
                parts = bytearray()
                found = False
                while True:
                    while j < n and src[j] in " \t\n":
                        j += 1
                    if j < n and src[j] == '"':
                        piece, j = decode_literal(src, j)
                        parts += piece
                        found = True
                    else:
                        break
                if found and j < n and src[j] == ")":
                    line = src.count("\n", 0, i) + 1
                    yield parts.decode("utf-8", errors="replace"), line
                    i = j + 1
                    continue
        i += 1


def po_escape(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif o < 32 or o == 127:
            out.append("\\%03o" % o)
        else:
            out.append(ch)
    return "".join(out)


def po_string(keyword, s):
    """A msgid/msgstr entry; multi-line strings are split at the newlines."""
    if "\n" not in s[:-1]:
        return '%s "%s"\n' % (keyword, po_escape(s))
    lines = s.split("\n")
    chunks = [l + "\n" for l in lines[:-1]]
    if lines[-1]:
        chunks.append(lines[-1])
    return '%s ""\n' % keyword + "".join('"%s"\n' % po_escape(c) for c in chunks)


def po_unescape(s):
    data, _ = decode_literal('"' + s + '"', 0)
    return data.decode("utf-8", errors="replace")


def po_load(path):
    """Returns {msgid: msgstr} of a PO file."""
    entries = {}
    msgid = msgstr = None
    state = 0
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()

            def lit(text):
                a, b = text.find('"'), text.rfind('"')
                return po_unescape(text[a + 1:b]) if 0 <= a < b else ""
            if line.startswith("msgid "):
                if msgid is not None and msgstr is not None:
                    entries.setdefault(msgid, msgstr)
                msgid, msgstr, state = lit(line[6:]), None, 1
            elif line.startswith("msgstr "):
                msgstr, state = lit(line[7:]), 2
            elif line.startswith('"'):
                if state == 1:
                    msgid += lit(line)
                elif state == 2:
                    msgstr += lit(line)
    if msgid is not None and msgstr is not None:
        entries.setdefault(msgid, msgstr)
    return entries


def collect(files):
    """{msgid: [(file, line), ...]} keeping the order of first appearance."""
    found = {}
    for path in files:
        rel = os.path.relpath(path, ROOT) if os.path.isabs(path) else path
        for msgid, line in extract(path):
            if msgid:
                found.setdefault(msgid, []).append((rel, line))
    return found


def write_pot(found, out):
    out.write('# hoplab - translation template.\n'
              '# Generated by tools/extract-strings.py: do not edit, copy it to\n'
              '# locale/<lang>/<name>.po and fill in the msgstr lines.\n'
              '#\n'
              'msgid ""\n'
              'msgstr ""\n'
              '"Project-Id-Version: hoplab\\n"\n'
              '"MIME-Version: 1.0\\n"\n'
              '"Content-Type: text/plain; charset=UTF-8\\n"\n'
              '"Content-Transfer-Encoding: 8bit\\n"\n')
    for msgid, refs in found.items():
        out.write("\n")
        line = "#:"
        for f, l in refs:
            ref = " %s:%d" % (f, l)
            if len(line) + len(ref) > 78 and line != "#:":
                out.write(line + "\n")
                line = "#:"
            line += ref
        out.write(line + "\n")
        if "%" in msgid:
            out.write("#, c-format\n")
        out.write(po_string("msgid", msgid))
        out.write('msgstr ""\n')


def check(found, directory):
    catalogs = {}
    for path in sorted(glob.glob(os.path.join(directory, "*.po"))):
        for k, v in po_load(path).items():
            if k and v:
                catalogs.setdefault(k, os.path.basename(path))
    missing = [m for m in found if m not in catalogs]
    stale = [m for m in catalogs if m not in found]
    for m in missing:
        f, l = found[m][0]
        print("untranslated  %s:%d  %s" % (f, l, po_escape(m)[:90]))
    for m in stale:
        print("unused        %s  %s" % (catalogs[m], po_escape(m)[:90]))
    print("%d source strings, %d translated, %d untranslated, %d unused catalog entries"
          % (len(found), len(found) - len(missing), len(missing), len(stale)))
    return 1 if missing else 0


def main(argv):
    out_path = os.path.join(ROOT, "locale", "hoplab.pot")
    check_dir = None
    files = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        if a == "--check" and i + 1 < len(argv):
            check_dir = argv[i + 1]
            i += 2
        elif a == "-o" and i + 1 < len(argv):
            out_path = argv[i + 1]
            i += 2
        else:
            files.append(a)
            i += 1
    if not files:
        files = [os.path.join(ROOT, f) for f in DEFAULT if os.path.exists(os.path.join(ROOT, f))]
        files += sorted(glob.glob(os.path.join(ROOT, "src", "*.c")))
    found = collect(files)
    if check_dir:
        return check(found, check_dir)
    if out_path == "-":
        write_pot(found, sys.stdout)
    else:
        with open(out_path, "w", encoding="utf-8") as f:
            write_pot(found, f)
        print("%s: %d strings from %d files" % (os.path.relpath(out_path), len(found), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
