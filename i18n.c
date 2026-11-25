/*
 * i18n.c - Message catalogs. The English text in the source is the key;
 *          translations are standard PO files, one or more per language,
 *          in locale/<lang>/ next to the program. No external library.
 */
#include "i18n.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The same short English word can need different translations in different
 * places ("sent" as a column header, or in a sentence). Each catalog is
 * named after the source file it translates (tcp.po for tcp.c; core.po for
 * everything without a catalog of its own), and a lookup made from foo.c
 * prefers foo.po, then core.po, then any other catalog. */
typedef struct { char *id, *str; const char *cat; } tr_entry;
/* A catalog that translates several source files lists them in a comment:
 *     # files: rdt tui channel                                            */
static struct { char file[40]; const char *cat; } g_alias[96];
static int g_nalias;
static tr_entry *g_tr;
static size_t    g_tr_cap, g_tr_n;
static char      g_lang[16] = "en";
static char      g_home[PATH_MAX];

static uint32_t str_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void tr_put(char *id, char *str, const char *cat)
{
    size_t i;

    if (!*id || !*str) {
        free(id);
        free(str);
        return;
    }
    if ((g_tr_n + 1) * 2 > g_tr_cap) {
        size_t ncap = g_tr_cap ? g_tr_cap * 2 : 2048, k;
        tr_entry *nt = calloc(ncap, sizeof(*nt));
        if (!nt)
            return;
        for (k = 0; k < g_tr_cap; k++)
            if (g_tr[k].id) {
                i = str_hash(g_tr[k].id) & (ncap - 1);
                while (nt[i].id)
                    i = (i + 1) & (ncap - 1);
                nt[i] = g_tr[k];
            }
        free(g_tr);
        g_tr = nt;
        g_tr_cap = ncap;
    }
    i = str_hash(id) & (g_tr_cap - 1);
    while (g_tr[i].id) {
        if (strcmp(g_tr[i].id, id) == 0 && strcmp(g_tr[i].cat, cat) == 0) {
            free(id);
            free(str);
            return;
        }
        i = (i + 1) & (g_tr_cap - 1);
    }
    g_tr[i].id = id;
    g_tr[i].str = str;
    g_tr[i].cat = cat;
    g_tr_n++;
}

/* Append the C-escaped string literal found in line (between quotes) to *dst */
static void po_append(char **dst, const char *line)
{
    const char *p = strchr(line, '"'), *q;
    size_t len = *dst ? strlen(*dst) : 0, add = 0;
    char *out;

    if (!p)
        return;
    q = strrchr(p + 1, '"');
    if (!q)
        return;
    out = realloc(*dst, len + (size_t)(q - p) + 1);
    if (!out)
        return;
    for (p++; p < q; p++) {
        if (*p == '\\' && p + 1 < q) {
            p++;
            if (*p == 'n') out[len + add++] = '\n';
            else if (*p == 't') out[len + add++] = '\t';
            else if (*p >= '0' && *p <= '7') {
                int v = 0, k = 0;
                while (k < 3 && p < q && *p >= '0' && *p <= '7') {
                    v = v * 8 + (*p - '0');
                    p++;
                    k++;
                }
                p--;
                out[len + add++] = (char)v;
            } else out[len + add++] = *p;
        } else {
            out[len + add++] = *p;
        }
    }
    out[len + add] = '\0';
    *dst = out;
}

static void po_load(const char *path, const char *cat)
{
    FILE *f = fopen(path, "r");
    char line[4096], *id = NULL, *str = NULL;
    int in = 0;                         /* 1 = msgid, 2 = msgstr */

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (strncmp(s, "# files:", 8) == 0) {
            char *tok, *save = NULL;
            for (tok = strtok_r(s + 8, " \t\r\n", &save); tok && g_nalias < 96; tok = strtok_r(NULL, " \t\r\n", &save)) {
                snprintf(g_alias[g_nalias].file, sizeof(g_alias[0].file), "%s", tok);
                g_alias[g_nalias++].cat = cat;
            }
        } else if (strncmp(s, "msgid ", 6) == 0) {
            if (id && str)
                tr_put(id, str, cat);
            else {
                free(id);
                free(str);
            }
            id = str = NULL;
            in = 1;
            po_append(&id, s + 6);
        } else if (strncmp(s, "msgstr ", 7) == 0) {
            in = 2;
            po_append(&str, s + 7);
        } else if (*s == '"') {
            po_append(in == 1 ? &id : &str, s);
        }
    }
    if (id && str)
        tr_put(id, str, cat);
    fclose(f);
}

const char *nl_tr_from(const char *file, const char *s)
{
    const char *base = file ? strrchr(file, '/') : NULL, *fallback = NULL;
    size_t i, bl;
    int fallback_core = 0;

    if (!g_tr_n || !s || !*s)
        return s;
    base = base ? base + 1 : file ? file : "";
    bl = strcspn(base, ".");
    for (i = 0; i < (size_t)g_nalias; i++)
        if (strncmp(g_alias[i].file, base, bl) == 0 && g_alias[i].file[bl] == '\0') {
            base = g_alias[i].cat;
            bl = strlen(base);
            break;
        }
    i = str_hash(s) & (g_tr_cap - 1);
    while (g_tr[i].id) {
        if (strcmp(g_tr[i].id, s) == 0) {
            const char *cat = g_tr[i].cat;
            if (bl && strncmp(cat, base, bl) == 0 && cat[bl] == '\0')
                return g_tr[i].str;
            if (!fallback_core && strcmp(cat, "core") == 0) {
                fallback = g_tr[i].str;
                fallback_core = 1;
            } else if (!fallback) {
                fallback = g_tr[i].str;
            }
        }
        i = (i + 1) & (g_tr_cap - 1);
    }
    return fallback ? fallback : s;
}

const char *nl_tr(const char *s)
{
    return nl_tr_from(NULL, s);
}

const char *nl_lang(void) { return g_lang; }

const char *nl_home(void)
{
    const char *env = getenv("NETLAB_HOME");
    char probe[PATH_MAX + 32];
    struct stat st;
    ssize_t n;
    char *slash;

    if (g_home[0])
        return g_home;
    if (env && *env) {
        snprintf(g_home, sizeof(g_home), "%s", env);
        return g_home;
    }
    n = readlink("/proc/self/exe", g_home, sizeof(g_home) - 1);
    if (n <= 0) {
        strcpy(g_home, ".");
        return g_home;
    }
    g_home[n] = '\0';
    slash = strrchr(g_home, '/');
    if (slash)
        *slash = '\0';
    snprintf(probe, sizeof(probe), "%s/scenarios", g_home);
    if (stat(probe, &st) != 0) {
        snprintf(probe, sizeof(probe), "%s/../share/netlab/scenarios", g_home);
        if (stat(probe, &st) == 0)
            strncat(g_home, "/../share/netlab", sizeof(g_home) - strlen(g_home) - 1);
    }
    return g_home;
}

void nl_i18n_init(const char *lang)
{
    static const char *const vars[] = { "NETLAB_LANG", "LC_ALL", "LC_MESSAGES", "LANG" };
    char path[PATH_MAX + 64];
    size_t i;

    for (i = 0; (!lang || !*lang) && i < sizeof(vars) / sizeof(vars[0]); i++)
        lang = getenv(vars[i]);
    if (!lang || !*lang || strcmp(lang, "C") == 0 || strcmp(lang, "POSIX") == 0)
        lang = "en";
    snprintf(g_lang, sizeof(g_lang), "%.2s", lang);
    for (i = 0; g_lang[i]; i++)
        g_lang[i] = (char)tolower((unsigned char)g_lang[i]);
    if (strcmp(g_lang, "en") == 0)
        return;
    /* every catalog in locale/<lang>/, in name order so that runs agree */
    snprintf(path, sizeof(path), "%s/locale/%s", nl_home(), g_lang);
    {
        struct dirent **list = NULL;
        int n = scandir(path, &list, NULL, alphasort), k;
        char file[PATH_MAX + 400];

        for (k = 0; k < n; k++) {
            size_t el = strlen(list[k]->d_name);
            if (el > 3 && strcmp(list[k]->d_name + el - 3, ".po") == 0) {
                char *cat = strdup(list[k]->d_name);
                if (cat) {
                    cat[el - 3] = '\0';
                    snprintf(file, sizeof(file), "%s/%s", path, list[k]->d_name);
                    po_load(file, cat);
                }
            }
            free(list[k]);
        }
        free(list);
    }
}
