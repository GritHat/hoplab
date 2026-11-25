/*
 * i18n.h - Translation of user-visible text (gettext-style, PO catalogs).
 *
 *     nl_i18n_init(NULL);              once, at start
 *     printf("%s\n", _("Hello"));      English is the key and the fallback
 *
 * N_() only marks a string so that tools/extract-strings.py finds it; the
 * string is translated later with _(variable).
 */
#ifndef I18N_H
#define I18N_H

/* Catalogs are named after source files (tcp.po translates tcp.c, core.po
 * everything without its own): a lookup made from foo.c prefers foo.po,
 * so the same short word can be translated differently per module. */
const char *nl_tr_from(const char *file, const char *s);
const char *nl_tr(const char *s);
#define _(s)  nl_tr_from(__FILE__, s)
#define N_(s) (s)

/* lang NULL or empty: HOPLAB_LANG, then LC_ALL, LC_MESSAGES, LANG */
void        nl_i18n_init(const char *lang);
const char *nl_lang(void);              /* "en", "it", ... */
/* Directory holding scenarios/, locale/ and guide/: HOPLAB_HOME, or the
 * directory of the executable */
const char *nl_home(void);

#endif
