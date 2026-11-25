/*
 * guide.c - The study guide: a curriculum organised by level and topic,
 *           written as Markdown files in guide/<lang>/ (English is the
 *           reference; a missing translation falls back to it). Each topic
 *           can be read at three depths and lists the labs that show it.
 *
 * File header (HTML comments, invisible when the file is read elsewhere):
 *     <!-- level: 2 -->            0 getting started, 1..4 the tracks
 *     <!-- labs: 101-ping, 220-tcp-loss -->
 *     # Title
 * and in the body <!-- detail: 2 --> / <!-- detail: 3 --> switch the depth.
 */
#include "nl_int.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ST(fg)  TS(fg, C_DEFAULT, 0)
#define STB(fg) TS(fg, C_DEFAULT, TA_BOLD)

typedef struct {
    char  name[64];                     /* file name */
    char  title[120];
    int   level;
    char  labs[8][40];
    int   nlabs;
    char *text;
} topic;

static topic *g_top;
static int    g_ntop;

static int cmp_topic(const void *a, const void *b)
{
    const topic *x = a, *y = b;
    return x->level != y->level ? x->level - y->level : strcmp(x->name, y->name);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    long size;
    char *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    buf = malloc((size_t)size + 1);
    if (buf)
        buf[fread(buf, 1, (size_t)size, f)] = '\0';
    fclose(f);
    return buf;
}

static void load_topics(void)
{
    char dir[700], path[1100];
    DIR *d;
    struct dirent *e;

    snprintf(dir, sizeof(dir), "%s/guide/en", nl_home());
    d = opendir(dir);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        size_t el = strlen(e->d_name);
        topic *t, *nt;
        char *s, *line, *save = NULL, *copy;

        if (el < 4 || strcmp(e->d_name + el - 3, ".md") != 0)
            continue;
        nt = realloc(g_top, (size_t)(g_ntop + 1) * sizeof(*g_top));
        if (!nt)
            break;
        g_top = nt;
        t = &g_top[g_ntop];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", e->d_name);
        snprintf(path, sizeof(path), "%s/guide/%s/%s", nl_home(), nl_lang(), e->d_name);
        t->text = read_file(path);
        if (!t->text) {
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            t->text = read_file(path);
        }
        if (!t->text)
            continue;
        copy = strdup(t->text);
        for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if ((s = strstr(line, "level:")) != NULL && strncmp(line, "<!--", 4) == 0)
                t->level = atoi(s + 6);
            else if ((s = strstr(line, "labs:")) != NULL && strncmp(line, "<!--", 4) == 0) {
                char *tok, *sv = NULL;
                for (tok = strtok_r(s + 5, ", ", &sv); tok && t->nlabs < 8; tok = strtok_r(NULL, ", ", &sv))
                    if (strcmp(tok, "-->") != 0)
                        snprintf(t->labs[t->nlabs++], sizeof(t->labs[0]), "%s", tok);
            } else if (line[0] == '#' && !t->title[0]) {
                while (*line == '#' || *line == ' ')
                    line++;
                snprintf(t->title, sizeof(t->title), "%s", line);
            }
        }
        free(copy);
        if (!t->title[0])
            snprintf(t->title, sizeof(t->title), "%s", t->name);
        g_ntop++;
    }
    closedir(d);
    if (g_ntop)
        qsort(g_top, (size_t)g_ntop, sizeof(*g_top), cmp_topic);
}

static const char *track_name(int level)
{
    switch (level) {
    case 0:  return _("Getting started");
    case 1:  return _("Foundations");
    case 2:  return _("Core");
    case 3:  return _("Advanced");
    default: return _("Frontier");
    }
}

static int find_topic(const char *name)
{
    int i;

    for (i = 0; name && i < g_ntop; i++)
        if (strstr(g_top[i].name, name))
            return i;
    return -1;
}

static void run_lab(const char *lab)
{
    char *argv[6], self[] = "hoplab", path[1024];

    if (!nl_scenario_path(lab, path, sizeof(path)))
        return;
    argv[0] = self;
    argv[1] = "run";
    argv[2] = path;
    argv[3] = "--lang";
    argv[4] = (char *)nl_lang();
    argv[5] = NULL;
    nl_spawn_wait("/proc/self/exe", argv, 1);
}

int nl_guide(const char *name)
{
    static const char *const depth[] = { N_("essentials"), N_("standard"), N_("in depth") };
    int sel, top = 0, scroll = 0, detail = 2, focus = 0, k, i;
    char filter[40] = "";
    int filtering = 0, fcur = 0;

    load_topics();
    if (g_ntop == 0) {
        fprintf(stderr, "hoplab: %s %s/guide\n", _("no guide found in"), nl_home());
        return 1;
    }
    sel = find_topic(name);
    if (!isatty(STDOUT_FILENO)) {       /* piped: plain text */
        if (sel >= 0) {
            fputs(g_top[sel].text, stdout);
        } else {
            int last = -1;
            for (i = 0; i < g_ntop; i++) {
                if (g_top[i].level != last) {
                    last = g_top[i].level;
                    printf("\n%s\n", track_name(last));
                }
                printf("  %-28.*s %s\n", (int)strlen(g_top[i].name) - 3, g_top[i].name, g_top[i].title);
            }
        }
        return 0;
    }
    if (sel < 0)
        sel = 0;
    else
        focus = 1;
    if (!tui_init())
        return 1;
    for (;;) {
        int vis[256], nvis = 0, cur = 0;

        if (tui_should_quit())
            break;
        for (i = 0; i < g_ntop && nvis < 256; i++) {
            if (filter[0]) {
                char *hay = g_top[i].text;
                if (!strstr(g_top[i].title, filter) && !(hay && strstr(hay, filter)))
                    continue;
            }
            if (i == sel)
                cur = nvis;
            vis[nvis++] = i;
        }
        if (nvis && vis[cur] != sel)
            sel = vis[cur = 0];
        if (tui_frame_begin()) {
            int Wd = tui_width(), H = tui_height(), lw = Wd > 120 ? 40 : Wd > 90 ? 32 : 26, row = 2, last = -1;
            const topic *t = &g_top[sel];
            int tx;

            tui_box(0, 0, lw, H - 1, ST(focus == 0 ? C_INFO : C_BORDER), _("Study guide"));
            if (cur < top)
                top = cur;
            while (cur - top > H - 12)
                top++;
            for (i = top; i < nvis && row < H - 2; i++) {
                const topic *e = &g_top[vis[i]];
                int c = vis[i] == sel;

                if (e->level != last) {
                    last = e->level;
                    if (row > 2)
                        row++;
                    if (row >= H - 2)
                        break;
                    tui_print(2, row++, lw - 4, STB(C_WARN), "%s", track_name(e->level));
                    if (row >= H - 2)
                        break;
                }
                if (c)
                    tui_fill(1, row, lw - 2, 1, TS(C_DEFAULT, C_SELECT, 0));
                tui_print(3, row++, lw - 5, TS(c ? C_BRIGHT : C_TEXT, c ? C_SELECT : C_DEFAULT, c ? TA_BOLD : 0), "%s",
                          e->title);
            }
            tui_box(lw, 0, Wd - lw, H - 1, ST(focus == 1 ? C_INFO : C_BORDER), t->title);
            tx = lw + 3;
            tx += tui_print(tx, 1, Wd - tx - 2, ST(C_DIM), "%s ", _("depth:"));
            for (i = 0; i < 3; i++)
                tx += tui_print(tx, 1, Wd - tx - 2, i + 1 == detail ? TS(16, C_INFO, TA_BOLD) : TS(C_DIM, C_PANEL, 0),
                                " %d %s ", i + 1, _(depth[i])) + 1;
            nl_md_draw(lw + 3, 3, Wd - lw - 6, H - 5 - (t->nlabs ? 2 : 0), t->text, &scroll, detail);
            if (t->nlabs) {
                tx = lw + 3;
                tx += tui_print(tx, H - 3, Wd - tx - 2, STB(C_OK), "%s ", _("Labs:"));
                for (i = 0; i < t->nlabs && tx < Wd - 10; i++)
                    tx += tui_print(tx, H - 3, Wd - tx - 2, ST(C_TEXT), T_KEY " %c " T_RESET " %s  ", 'a' + i,
                                    t->labs[i]);
            }
            if (filtering || filter[0])
                tui_print(1, H - 1, Wd - 2, ST(C_BRIGHT), "%s %s%s", _("search:"), filter, filtering ? "▏" : "");
            else
                tui_print(1, H - 1, Wd - 2, ST(C_DIM), "%s",
                          _("↑ ↓ topic · → read · PgUp PgDn scroll · 1 2 3 depth · a b c run a lab · / search · Esc back"));
            tui_frame_end();
        }
        tui_wait(-1, 80);
        while ((k = nl_key()) >= 0) {
            const topic *t = &g_top[sel];

            if (filtering) {
                if (k == NK_ENTER || k == NK_ESC)
                    filtering = 0;
                else
                    nl_line_edit(filter, sizeof(filter), &fcur, k);
                continue;
            }
            if (k == NK_ESC || k == 'q') {
                if (filter[0]) {
                    filter[0] = '\0';
                    continue;
                }
                tui_shutdown();
                return 0;
            }
            if (k >= '1' && k <= '3')            detail = k - '0';
            else if (k == '/')                   { filtering = 1; fcur = (int)strlen(filter); }
            else if (k == NK_TAB)                focus = !focus;
            else if (k == NK_RIGHT || k == NK_ENTER) focus = 1;
            else if (k == NK_LEFT)               focus = 0;
            else if (k == NK_PGDN || k == ' ')   scroll += 10;
            else if (k == NK_PGUP)               scroll = scroll > 10 ? scroll - 10 : 0;
            else if (k >= 'a' && k < 'a' + t->nlabs) run_lab(t->labs[k - 'a']);
            else if (focus == 1 && k == NK_DOWN) scroll++;
            else if (focus == 1 && k == NK_UP)   scroll = scroll > 0 ? scroll - 1 : 0;
            else if (k == NK_DOWN && cur < nvis - 1) { sel = vis[cur + 1]; scroll = 0; }
            else if (k == NK_UP && cur > 0)      { sel = vis[cur - 1]; scroll = 0; }
        }
    }
    tui_shutdown();
    return 0;
}
