# Localisation

hoplab is written in English and translated at run time. Nothing needs to be
compiled: catalogs are plain text files read when the program starts.

## How it works

- **The English text in the source is the key.** User-visible strings are
  wrapped in `_("...")`; strings stored in tables are marked with `N_("...")`
  and translated later with `_(variable)`. A string with no translation is
  shown in English.
- **Catalogs are standard gettext PO files** in `locale/<lang>/`, one per
  module (`core.po`, `tcp.po`, `dns.po`, ...). Every `.po` file in the
  directory of the current language is loaded. A catalog is named after the
  source file it translates (`tcp.po` for `src/tcp.c`); one that covers
  several files lists them in a first-line comment (`# files: rdt tui
  channel`). A lookup made from `foo.c` prefers `foo.po`, then `core.po`,
  then any other catalog, so the same short word (`sent`, `lost`, `from`)
  can be translated differently where the context differs.
- **The language** is taken from `--lang xx`, or else from the first of
  `HOPLAB_LANG`, `LC_ALL`, `LC_MESSAGES`, `LANG` that is set. Only the first
  two letters matter (`it_IT.UTF-8` → `it`).
- **Scenario files translate themselves**: next to `title`, `about`, `note`
  and `param` a scenario may carry `title[it]`, `about[it]`, `note[it] TIME
  text` and `param[it] NAME label`. See `docs/scenario-format.md`.
- **The study guide** is a set of Markdown files in `guide/<lang>/`. English
  (`guide/en/`) is the reference; a topic missing in another language falls
  back to it.

Protocol jargon in log lines (`ARP who has ...`, `[SYN, ACK]`) is not
translated, as in any packet analyser.

## Tools

    tools/extract-strings.py                    # rebuild locale/hoplab.pot from the sources
    tools/extract-strings.py --check locale/it  # what is untranslated, what is stale
    msgfmt -c -o /dev/null locale/it/core.po    # optional: validate a catalog (gettext)

`--check` exits with status 1 when a source string has no translation in any
catalog of the directory, so it can run in CI. Pass source files after the
options to restrict the check to them.

## Adding a language

1. `mkdir locale/xx` (the two-letter ISO 639-1 code).
2. Run `tools/extract-strings.py`, copy `locale/hoplab.pot` to
   `locale/xx/hoplab.po` (one file is fine; splitting per module is optional)
   and fill in every `msgstr`. Keep the `%s`, `%d`, ... specifiers in the same
   order, and keep leading and trailing spaces and newlines.
3. Run `tools/extract-strings.py --check locale/xx` until nothing is
   reported as untranslated.
4. Try it: `hoplab --lang xx`.
5. Optionally add `[xx]` lines to the scenarios and translate the guide into
   `guide/xx/` (same file names as `guide/en/`).

## Writing translatable code

- Keep the `T_*` colour markup outside `_()`: `T_ERR "%s"` with `_("lost")`
  as the argument, not `_(T_ERR "lost")`.
- Prefer whole sentences to fragments glued together with `%s`: word order
  and agreement differ between languages.
- Usage text aligned in columns must stay aligned in the translation.
