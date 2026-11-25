/*
 * menu.c - The interactive front door: scenario catalog, launch forms with
 *          simple and advanced settings, topology wizard, distributed
 *          set-up, module reference and the classic reliable-transfer demos.
 */
#include "nl_int.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ST(fg)  TS(fg, C_DEFAULT, 0)
#define STB(fg) TS(fg, C_DEFAULT, TA_BOLD)

/* ------------------------------------------------------------------ */
/* Running a child netlab (or the demo script) and coming back        */
/* ------------------------------------------------------------------ */

int nl_spawn_wait(const char *prog, char **argv, int pause)
{
    pid_t pid;
    int status = 0, c;

    tui_shutdown();
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        execv(prog, argv);
        perror(prog);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, &status, 0);
    if (pause) {
        printf("\n%s ", _("Press Enter to go back to the menu..."));
        fflush(stdout);
        while ((c = getchar()) != '\n' && c != EOF)
            ;
    }
    tui_init();
    return status;
}

static const char *user_dir(void)
{
    static char dir[600];
    const char *home = getenv("HOME");
    char cmd[700];

    if (!dir[0]) {
        snprintf(dir, sizeof(dir), "%s/.local/share/netlab/scenarios", home ? home : "/tmp");
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>/dev/null", dir);
        if (system(cmd) != 0)
            snprintf(dir, sizeof(dir), "/tmp");
    }
    return dir;
}

/* ------------------------------------------------------------------ */
/* Forms                                                              */
/* ------------------------------------------------------------------ */

enum { FT_TEXT, FT_CHOICE, FT_BUTTON, FT_HEAD };

typedef struct {
    int          type;
    const char  *label;
    char        *buf;                   /* FT_TEXT: the value being edited */
    size_t       size;
    const char *const *choices;         /* FT_CHOICE */
    int          nchoices, *sel;
    int          rebuild;               /* changing it changes the form: return FORM_REBUILD */
    int          cursor;
} field;

#define FORM_CANCEL  (-1)
#define FORM_REBUILD (-2)

static int form_run(const char *title, const char *hint, field *f, int n, int *pos)
{
    for (;;) {
        int Wd, H, w, h, x, y, i, k, row, lw = 0;

        if (tui_should_quit())
            return FORM_CANCEL;
        while (*pos < n - 1 && f[*pos].type == FT_HEAD)
            (*pos)++;
        if (tui_frame_begin()) {
            Wd = tui_width();
            H = tui_height();
            for (i = 0; i < n; i++)
                if (f[i].type != FT_HEAD && f[i].type != FT_BUTTON && tui_text_width(f[i].label) > lw)
                    lw = tui_text_width(f[i].label);
            w = Wd < 104 ? Wd - 2 : 104;
            if (lw > w / 2)
                lw = w / 2;
            h = n + 6 < H - 2 ? n + 6 : H - 2;
            x = (Wd - w) / 2;
            y = (H - h) / 2;
            tui_box(x, y, w, h, ST(C_INFO), title);
            for (i = 0, row = y + 2; i < n && row < y + h - 3; i++, row++) {
                int cur = i == *pos;
                tstyle lab = cur ? STB(C_BRIGHT) : ST(C_TEXT);

                if (f[i].type == FT_HEAD) {
                    tui_print(x + 3, row, w - 6, STB(C_WARN), "%s", f[i].label);
                    continue;
                }
                tui_put(x + 2, row, STB(C_INFO), cur ? "▸" : " ");
                if (f[i].type == FT_BUTTON) {
                    tui_print(x + 4, row, w - 8, cur ? TS(16, C_OK, TA_BOLD) : TS(C_OK, C_PANEL, TA_BOLD), "  %s  ",
                              f[i].label);
                    continue;
                }
                tui_print(x + 4, row, lw, lab, "%s", f[i].label);
                if (f[i].type == FT_TEXT) {
                    int vx = x + 6 + lw, vw = w - lw - 10;
                    tui_fill(vx, row, vw, 1, TS(C_TEXT, cur ? C_SELECT : C_PANEL, 0));
                    tui_print(vx + 1, row, vw - 2, TS(C_BRIGHT, cur ? C_SELECT : C_PANEL, 0), "%s", f[i].buf);
                    if (cur)
                        tui_put(vx + 1 + f[i].cursor, row, TS(16, C_BRIGHT, 0),
                                f[i].buf[f[i].cursor] ? (char[]){ f[i].buf[f[i].cursor], 0 } : " ");
                } else {
                    int vx = x + 6 + lw, c, need = 0;
                    tstyle on = TS(16, cur ? C_INFO : 250, TA_BOLD);

                    for (c = 0; c < f[i].nchoices; c++)
                        need += tui_text_width(_(f[i].choices[c])) + 3;
                    if (need <= w - lw - 9) {
                        for (c = 0; c < f[i].nchoices; c++)
                            vx += tui_print(vx, row, x + w - 3 - vx, c == *f[i].sel ? on : TS(C_DIM, C_PANEL, 0),
                                            " %s ", _(f[i].choices[c])) + 1;
                    } else {            /* too many to show side by side: only the current one */
                        vx += tui_print(vx, row, 2, ST(C_DIM), "◂ ");
                        vx += tui_print(vx, row, x + w - 12 - vx, on, " %s ", _(f[i].choices[*f[i].sel]));
                        tui_print(vx, row, 10, ST(C_DIM), " ▸ %d/%d", *f[i].sel + 1, f[i].nchoices);
                    }
                }
            }
            tui_print(x + 3, y + h - 2, w - 6, ST(C_DIM), "%s",
                      hint ? hint : _("↑ ↓ move · ← → choose · type to edit · Enter confirm · Esc back"));
            tui_frame_end();
        }
        tui_wait(-1, 50);
        while ((k = nl_key()) >= 0) {
            field *c = &f[*pos];

            if (k == NK_ESC)
                return FORM_CANCEL;
            if (k == NK_UP || k == NK_BTAB) {
                do
                    *pos = (*pos + n - 1) % n;
                while (f[*pos].type == FT_HEAD);
            } else if (k == NK_DOWN || k == NK_TAB) {
                do
                    *pos = (*pos + 1) % n;
                while (f[*pos].type == FT_HEAD);
            } else if (k == NK_ENTER) {
                if (c->type == FT_BUTTON)
                    return *pos;
                do
                    *pos = (*pos + 1) % n;
                while (f[*pos].type == FT_HEAD);
            } else if (c->type == FT_CHOICE && (k == NK_LEFT || k == NK_RIGHT || k == ' ')) {
                *c->sel = (*c->sel + (k == NK_LEFT ? c->nchoices - 1 : 1)) % c->nchoices;
                if (c->rebuild)
                    return FORM_REBUILD;
            } else if (c->type == FT_TEXT) {
                nl_line_edit(c->buf, c->size, &c->cursor, k);
            }
        }
    }
}

static void f_text(field *f, const char *label, char *buf, size_t size)
{
    memset(f, 0, sizeof(*f));
    f->type = FT_TEXT;
    f->label = label;
    f->buf = buf;
    f->size = size;
    f->cursor = (int)strlen(buf);
}

static void f_choice(field *f, const char *label, const char *const *choices, int n, int *sel, int rebuild)
{
    memset(f, 0, sizeof(*f));
    f->type = FT_CHOICE;
    f->label = label;
    f->choices = choices;
    f->nchoices = n;
    f->sel = sel;
    f->rebuild = rebuild;
}

static void f_simple(field *f, int type, const char *label)
{
    memset(f, 0, sizeof(*f));
    f->type = type;
    f->label = label;
}

/* A screenful of text with a title; any key returns */
static void text_screen(const char *title, const char *text)
{
    int scroll = 0, k;

    for (;;) {
        if (tui_should_quit())
            return;
        if (tui_frame_begin()) {
            int Wd = tui_width(), H = tui_height();
            tui_box(1, 0, Wd - 2, H - 1, ST(C_INFO), title);
            nl_md_draw(4, 2, Wd - 8, H - 5, text, &scroll, 3);
            tui_print(2, H - 1, Wd - 4, ST(C_DIM), "%s", _("↑ ↓ scroll · Esc back"));
            tui_frame_end();
        }
        tui_wait(-1, 80);
        while ((k = nl_key()) >= 0) {
            if (k == NK_UP)             scroll = scroll > 0 ? scroll - 1 : 0;
            else if (k == NK_DOWN)      scroll++;
            else if (k == NK_PGUP)      scroll = scroll > 10 ? scroll - 10 : 0;
            else if (k == NK_PGDN || k == ' ') scroll += 10;
            else                        return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Launching a scenario                                               */
/* ------------------------------------------------------------------ */

static const char *const MODES[] = { N_("this window"), N_("tiled windows"), N_("several machines") };
static const char *const SETTINGS[] = { N_("simple"), N_("advanced") };
static const char *const DETAILS[] = { N_("1 overview"), N_("2 protocols"), N_("3 every frame") };

static void distributed_setup(nl_meta *m);

static void launch_scenario(nl_meta *m)
{
    static int mode = 0, advanced = 0, detail = 1;
    static char seed[24], speed[24];
    field f[48];
    int pos = 0, n, i, r, start;
    char *argv[80], pbuf[40][96], self[] = "netlab";
    int argc;

    for (;;) {
        n = 0;
        f_choice(&f[n++], _("Settings"), SETTINGS, 2, &advanced, 1);
        for (i = 0; i < m->nparams && n < 40; i++)
            if (!m->params[i].advanced || advanced)
                f_text(&f[n++], m->params[i].label, m->params[i].value, sizeof(m->params[i].value));
        f_choice(&f[n++], _("Run in"), MODES, 3, &mode, 0);
        if (advanced) {
            f_choice(&f[n++], _("Detail"), DETAILS, 3, &detail, 0);
            f_text(&f[n++], _("Time scale (empty = automatic)"), speed, sizeof(speed));
            f_text(&f[n++], _("Random seed (empty = new each run)"), seed, sizeof(seed));
        }
        start = n;
        f_simple(&f[n++], FT_BUTTON, _("Start"));
        if (pos >= n)
            pos = start;
        if (pos == 0 && !advanced)
            pos = start;
        r = form_run(m->title, NULL, f, n, &pos);
        if (r == FORM_REBUILD)
            continue;
        if (r == FORM_CANCEL)
            return;
        break;
    }
    if (mode == 2) {
        distributed_setup(m);
        return;
    }
    argc = 0;
    argv[argc++] = self;
    argv[argc++] = mode == 1 ? "windows" : "run";
    argv[argc++] = m->file;
    for (i = 0; i < m->nparams && i < 40; i++) {
        snprintf(pbuf[i], sizeof(pbuf[i]), "%s=%s", m->params[i].name, m->params[i].value);
        argv[argc++] = pbuf[i];
    }
    if (mode == 0) {
        argv[argc++] = "--detail";
        argv[argc++] = detail == 0 ? "1" : detail == 1 ? "2" : "3";
    }
    if (speed[0]) {
        argv[argc++] = "--speed";
        argv[argc++] = speed;
    }
    if (seed[0]) {
        argv[argc++] = "--seed";
        argv[argc++] = seed;
    }
    argv[argc++] = "--lang";
    argv[argc++] = (char *)nl_lang();
    argv[argc] = NULL;
    nl_spawn_wait("/proc/self/exe", argv, 1);
}

/* Ask where every node runs, write a copy of the scenario with `place`
 * lines, and show the commands to type on each machine. */
static void distributed_setup(nl_meta *m)
{
    static char hosts[NL_MAX_NODES][64], names[NL_MAX_NODES][16], monitor[64] = "127.0.0.1";
    field f[NL_MAX_NODES + 4];
    char out[700], line[1024], *argv[6], self[] = "netlab";
    const char *base;
    FILE *in, *o;
    int n = 0, nn = 0, pos = 0, i, r;

    in = fopen(m->file, "r");
    if (!in)
        return;
    while (fgets(line, sizeof(line), in) && nn < NL_MAX_NODES) {
        char name[16];
        if (sscanf(line, " node %15s", name) == 1) {
            snprintf(names[nn], sizeof(names[nn]), "%s", name);
            if (!hosts[nn][0])
                snprintf(hosts[nn], sizeof(hosts[nn]), "127.0.0.1");
            nn++;
        }
    }
    f_simple(&f[n++], FT_HEAD, _("Address of the machine that runs each node (same address = same machine)"));
    for (i = 0; i < nn; i++)
        f_text(&f[n++], names[i], hosts[i], sizeof(hosts[i]));
    f_text(&f[n++], _("Monitor (global view)"), monitor, sizeof(monitor));
    f_simple(&f[n++], FT_BUTTON, _("Write the scenario and show the commands"));
    r = form_run(_("Distributed lab"), NULL, f, n, &pos);
    if (r < 0) {
        fclose(in);
        return;
    }
    base = strrchr(m->file, '/');
    base = base ? base + 1 : m->file;
    snprintf(out, sizeof(out), "%s/dist-%s", user_dir(), base);
    o = fopen(out, "w");
    if (!o) {
        fclose(in);
        return;
    }
    rewind(in);
    while (fgets(line, sizeof(line), in))
        if (strncmp(line, "place ", 6) != 0 && strncmp(line, "monitor ", 8) != 0)
            fputs(line, o);
    fprintf(o, "\n# where each node runs (written by the netlab menu)\n");
    for (i = 0; i < nn; i++)
        fprintf(o, "place %s %s:%d\n", names[i], hosts[i], 47000 + i);
    fprintf(o, "monitor %s:46999\n", monitor);
    fclose(o);
    fclose(in);
    argv[0] = self;
    argv[1] = "plan";
    argv[2] = out;
    argv[3] = "--lang";
    argv[4] = (char *)nl_lang();
    argv[5] = NULL;
    nl_spawn_wait("/proc/self/exe", argv, 1);
}

/* ------------------------------------------------------------------ */
/* Catalog                                                            */
/* ------------------------------------------------------------------ */

static nl_meta *g_cat;
static int      g_ncat;

static int cmp_meta(const void *a, const void *b)
{
    const nl_meta *x = a, *y = b;
    int lx = x->level ? x->level : 9, ly = y->level ? y->level : 9;
    const char *bx = strrchr(x->file, '/'), *by = strrchr(y->file, '/');

    if (lx != ly)
        return lx - ly;
    return strcmp(bx ? bx : x->file, by ? by : y->file);
}

static void catalog_scan_dir(const char *dir, int user)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        size_t el = strlen(e->d_name);
        char path[1024];
        nl_meta *nm;

        if (el < 4 || strcmp(e->d_name + el - 3, ".nl") != 0)
            continue;
        nm = realloc(g_cat, (size_t)(g_ncat + 1) * sizeof(*g_cat));
        if (!nm)
            break;
        g_cat = nm;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        nl_scenario_peek(path, &g_cat[g_ncat]);
        if (user)
            g_cat[g_ncat].level = 0;
        g_ncat++;
    }
    closedir(d);
}

static void catalog_load(void)
{
    char dir[700];

    free(g_cat);
    g_cat = NULL;
    g_ncat = 0;
    snprintf(dir, sizeof(dir), "%s/scenarios", nl_home());
    catalog_scan_dir(dir, 0);
    catalog_scan_dir(user_dir(), 1);
    if (g_ncat)
        qsort(g_cat, (size_t)g_ncat, sizeof(*g_cat), cmp_meta);
}

static const char *level_name(int level)
{
    switch (level) {
    case 1:  return _("Foundations");
    case 2:  return _("Core");
    case 3:  return _("Advanced");
    case 4:  return _("Frontier");
    default: return _("My scenarios");
    }
}

static int matches(const nl_meta *m, const char *filter, int layer)
{
    char hay[900], needle[64];
    size_t i;

    if (layer && m->layer != layer)
        return 0;
    if (!filter[0])
        return 1;
    snprintf(hay, sizeof(hay), "%s %s %s", m->title, m->topic, m->file);
    snprintf(needle, sizeof(needle), "%s", filter);
    for (i = 0; hay[i]; i++)
        if (hay[i] >= 'A' && hay[i] <= 'Z') hay[i] += 32;
    for (i = 0; needle[i]; i++)
        if (needle[i] >= 'A' && needle[i] <= 'Z') needle[i] += 32;
    return strstr(hay, needle) != NULL;
}

static void catalog_screen(void)
{
    static const int layers[] = { 0, 1, 2, 3, 4, 7, 8 };
    char filter[48] = "";
    int sel = 0, top = 0, filtering = 0, fcur = 0, layer_i = 0, k, i;

    catalog_load();
    for (;;) {
        int vis[512], nvis = 0, Wd, H, lw, rows, row, last_level = -1, layer = layers[layer_i];

        if (tui_should_quit())
            return;
        for (i = 0; i < g_ncat && nvis < 512; i++)
            if (matches(&g_cat[i], filter, layer))
                vis[nvis++] = i;
        if (sel >= nvis)
            sel = nvis ? nvis - 1 : 0;
        if (tui_frame_begin()) {
            const nl_meta *m = nvis ? &g_cat[vis[sel]] : NULL;

            Wd = tui_width();
            H = tui_height();
            lw = Wd * 45 / 100;
            if (lw > 70) lw = 70;
            rows = H - 5;
            tui_box(0, 0, lw, H - 1, ST(C_INFO), _("Scenarios"));
            tui_print(2, 1, lw - 4, ST(C_DIM), "%s %s%s" T_RESET "    %s " T_BOLD "%s", _("layer:"), T_INFO,
                      layer ? nl_layer_name(layer) : _("all"), _("search:"), filter[0] || filtering ? filter : "—");
            if (filtering)
                tui_put(2 + tui_text_width(_("layer:")) + 1 + tui_text_width(layer ? nl_layer_name(layer) : _("all")) +
                        4 + tui_text_width(_("search:")) + 1 + fcur, 1, TS(16, C_BRIGHT, 0), " ");
            /* keep the selection in view: count the rows including headers */
            if (sel < top)
                top = sel;
            for (;;) {
                int need = 0, lv = -1;
                for (i = top; i <= sel; i++) {
                    if (g_cat[vis[i]].level != lv) {
                        lv = g_cat[vis[i]].level;
                        need += i == top ? 1 : 2;
                    }
                    need++;
                }
                if (need <= rows || top >= sel)
                    break;
                top++;
            }
            for (i = top, row = 3; i < nvis && row < H - 2; i++) {
                const nl_meta *e = &g_cat[vis[i]];
                const char *base = strrchr(e->file, '/');
                int cur = i == sel;

                if (e->level != last_level) {
                    last_level = e->level;
                    if (row > 3)
                        row++;
                    if (row >= H - 2)
                        break;
                    tui_print(2, row++, lw - 4, STB(C_WARN), "%s", level_name(e->level));
                    if (row >= H - 2)
                        break;
                }
                if (cur)
                    tui_fill(1, row, lw - 2, 1, TS(C_DEFAULT, C_SELECT, 0));
                tui_print(2, row, 4, TS(nl_layer_color(e->layer), cur ? C_SELECT : C_DEFAULT, TA_BOLD), "L%d", e->layer);
                tui_print(6, row, lw - 8, TS(cur ? C_BRIGHT : C_TEXT, cur ? C_SELECT : C_DEFAULT, cur ? TA_BOLD : 0),
                          "%s", e->title);
                (void)base;
                row++;
            }
            if (!nvis)
                tui_print(2, 4, lw - 4, ST(C_DIM), "%s", _("(nothing matches)"));
            tui_box(lw, 0, Wd - lw, H - 1, ST(C_BORDER), m ? m->title : "");
            if (m) {
                const char *base = strrchr(m->file, '/');
                int scroll = 0, y = 2, used;

                tui_print(lw + 2, y++, Wd - lw - 4, ST(C_TEXT), T_DIM "%s" T_RESET " %s   " T_DIM "%s" T_RESET " %s   "
                          T_DIM "%s" T_RESET " %d   " T_DIM "%s" T_RESET " %d", _("level"), level_name(m->level),
                          _("layer"), nl_layer_name(m->layer), _("nodes"), m->nnodes, _("links"), m->nlinks);
                tui_print(lw + 2, y++, Wd - lw - 4, ST(C_FAINT), "%s", base ? base + 1 : m->file);
                y++;
                used = H - 4 - y - (m->nparams ? m->nparams + 2 : 0);
                nl_md_draw(lw + 2, y, Wd - lw - 4, used > 3 ? used : 3, m->about, &scroll, 3);
                if (m->nparams) {
                    y = H - 3 - m->nparams;
                    tui_print(lw + 2, y - 1, Wd - lw - 4, STB(C_DIM), "%s", _("Parameters"));
                    for (i = 0; i < m->nparams && y < H - 2; i++, y++)
                        tui_print(lw + 2, y, Wd - lw - 4, ST(C_TEXT), "%s%-10s" T_RESET " = " T_BOLD "%-8s" T_RESET T_DIM " %s",
                                  m->params[i].advanced ? T_DIM : T_INFO, m->params[i].name, m->params[i].value,
                                  m->params[i].label);
                }
            }
            tui_print(1, H - 1, Wd - 2, ST(C_DIM), "%s",
                      _("↑ ↓ choose · Enter configure and start · / search · Tab filter by layer · Esc back"));
            tui_frame_end();
        }
        tui_wait(-1, 80);
        while ((k = nl_key()) >= 0) {
            if (filtering) {
                if (k == NK_ENTER || k == NK_ESC || k == NK_DOWN || k == NK_UP)
                    filtering = 0;
                else
                    nl_line_edit(filter, sizeof(filter), &fcur, k);
                sel = top = 0;
                continue;
            }
            if (k == NK_ESC || k == 'q')        return;
            else if (k == NK_UP)                sel = sel > 0 ? sel - 1 : 0;
            else if (k == NK_DOWN)              sel = sel < nvis - 1 ? sel + 1 : sel;
            else if (k == NK_PGUP)              sel = sel > 10 ? sel - 10 : 0;
            else if (k == NK_PGDN)              sel = sel + 10 < nvis ? sel + 10 : nvis - 1;
            else if (k == NK_HOME)              sel = 0;
            else if (k == NK_END)               sel = nvis - 1;
            else if (k == '/')                  { filtering = 1; fcur = (int)strlen(filter); }
            else if (k == NK_TAB)               { layer_i = (layer_i + 1) % 7; sel = top = 0; }
            else if (k == NK_ENTER && nvis)     launch_scenario(&g_cat[vis[sel]]);
            if (sel < 0)
                sel = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Topology wizard                                                    */
/* ------------------------------------------------------------------ */

static const char *const TOPO[] = { N_("LAN"), N_("line"), N_("ring"), N_("star"), N_("tree"), N_("mesh"),
                                    N_("dumbbell") };
static const char *const ROUTING[] = { N_("automatic static"), "RIP", "OSPF" };
static const char *const TRAFFIC[] = { N_("ping"), N_("ping + traceroute"), N_("UDP stream"), N_("TCP transfer"),
                                       N_("none") };

static void wizard(void)
{
    static int topo = 1, routing = 0, traffic = 1;
    static char name[40] = "my-network", size[8] = "4", hosts[8] = "1", bw[16] = "10M", delay[16] = "10ms",
                loss[16] = "0", bbw[16] = "1M", queue[8] = "50";
    field f[16];
    int n = 0, pos = 0, r, N, Hh, i, k, nh = 0;
    char path[700], first[16] = "", last[16] = "", hostname[64][16];
    FILE *o;
    nl_meta *m;

    f_text(&f[n++], _("Name"), name, sizeof(name));
    f_choice(&f[n++], _("Topology"), TOPO, 7, &topo, 0);
    f_text(&f[n++], _("Routers (LAN: hosts; dumbbell: pairs)"), size, sizeof(size));
    f_text(&f[n++], _("Hosts per router"), hosts, sizeof(hosts));
    f_text(&f[n++], _("Link bandwidth"), bw, sizeof(bw));
    f_text(&f[n++], _("Link delay"), delay, sizeof(delay));
    f_text(&f[n++], _("Link loss (0..1)"), loss, sizeof(loss));
    f_text(&f[n++], _("Bottleneck bandwidth (dumbbell)"), bbw, sizeof(bbw));
    f_text(&f[n++], _("Queue length (packets)"), queue, sizeof(queue));
    f_choice(&f[n++], _("Routing"), ROUTING, 3, &routing, 0);
    f_choice(&f[n++], _("Traffic"), TRAFFIC, 5, &traffic, 0);
    f_simple(&f[n++], FT_BUTTON, _("Create and open"));
    r = form_run(_("Build your own network"), NULL, f, n, &pos);
    if (r < 0)
        return;

    N = atoi(size);
    Hh = atoi(hosts);
    if (N < 2) N = 2;
    if (N > 12) N = 12;
    if (Hh < 0) Hh = 0;
    if (Hh > 3) Hh = 3;
    for (i = 0; name[i]; i++)
        if (name[i] == ' ' || name[i] == '/')
            name[i] = '-';
    snprintf(path, sizeof(path), "%s/%s.nl", user_dir(), name[0] ? name : "my-network");
    o = fopen(path, "w");
    if (!o)
        return;
    fprintf(o, "title %s (%s, %d)\nlevel 0\nlayer 3\n", name, TOPO[topo], N);
    fprintf(o, "about Built with the netlab wizard. Edit this file freely: see docs/scenario-format.md.\n");
    fprintf(o, "param bw %s Link bandwidth\nparam delay %s Link delay\nparam loss %s Link loss\n", bw, delay, loss);
    fprintf(o, "param+ queue %s Queue length (packets)\n", queue);
    if (topo == 6)
        fprintf(o, "param bottleneck %s Bottleneck bandwidth\n", bbw);
    fprintf(o, "\n");
#define LINKP "bw=$bw delay=$delay loss=$loss queue=$queue"
    if (topo == 0) {                                    /* LAN: hosts on a switch */
        fprintf(o, "node s1 switch\n");
        for (i = 1; i <= N; i++) {
            fprintf(o, "node h%d host\nlink h%d s1 " LINKP "\n", i, i);
            snprintf(hostname[nh++], 16, "h%d", i);
        }
    } else if (topo == 6) {                             /* dumbbell */
        fprintf(o, "node r1 router\nnode r2 router\nlink r1 r2 bw=$bottleneck delay=$delay loss=$loss queue=$queue\n");
        for (i = 1; i <= N; i++) {
            fprintf(o, "node a%d host\nlink a%d r1 bw=$bw delay=1ms\n", i, i);
            snprintf(hostname[nh++], 16, "a%d", i);
        }
        for (i = 1; i <= N; i++) {
            fprintf(o, "node b%d host\nlink b%d r2 bw=$bw delay=1ms\n", i, i);
            snprintf(hostname[nh++], 16, "b%d", i);
        }
    } else {
        for (i = 1; i <= N; i++)
            fprintf(o, "node r%d router\n", i);
        for (i = 1; i <= N; i++) {
            if (topo == 1 && i < N)                     fprintf(o, "link r%d r%d " LINKP "\n", i, i + 1);
            if (topo == 2 && (N > 2 || i < N))          fprintf(o, "link r%d r%d " LINKP "\n", i, i % N + 1);
            if (topo == 3 && i > 1)                     fprintf(o, "link r1 r%d " LINKP "\n", i);
            if (topo == 4 && i > 1)                     fprintf(o, "link r%d r%d " LINKP "\n", i / 2, i);
            for (k = i + 1; topo == 5 && k <= N; k++)   fprintf(o, "link r%d r%d " LINKP "\n", i, k);
        }
        for (i = 1; i <= N; i++) {
            if (Hh > 1)
                fprintf(o, "node s%d switch\nlink s%d r%d bw=$bw delay=1ms\n", i, i, i);
            for (k = 1; k <= Hh && nh < 64; k++) {
                snprintf(hostname[nh], 16, "h%d%c", i, Hh > 1 ? 'a' + k - 1 : '\0');
                fprintf(o, "node %s host\nlink %s %c%d bw=$bw delay=1ms\n", hostname[nh], hostname[nh],
                        Hh > 1 ? 's' : 'r', i);
                nh++;
            }
        }
    }
    if (nh >= 2) {
        snprintf(first, sizeof(first), "%s", hostname[0]);
        snprintf(last, sizeof(last), "%s", hostname[nh - 1]);
    } else if (topo != 0) {
        snprintf(first, sizeof(first), "r1");
        snprintf(last, sizeof(last), "r%d", N);
    }
    fprintf(o, "\n%s\n", routing == 0 || topo == 0 ? "auto all" : "auto addr");
    if (routing && topo != 0)
        for (i = 1; i <= (topo == 6 ? 2 : N); i++)
            fprintf(o, "run r%d %s\n", i, routing == 1 ? "rip update=5s" : "ospf");
    fprintf(o, "\n");
    if (first[0]) {
        const char *t0 = routing ? "at 12s " : "";
        if (traffic == 0 || traffic == 1) fprintf(o, "%srun %s ping %s count=5\n", t0, first, last);
        if (traffic == 1) fprintf(o, "at %ds run %s traceroute %s\n", routing ? 20 : 6, first, last);
        if (traffic == 2) fprintf(o, "run %s sink\n%srun %s cbr %s rate=500k duration=20s\n", last, t0, first, last);
        if (traffic == 3) fprintf(o, "run %s tcp sink\n%srun %s tcp send %s bytes=500k\n", last, t0, first, last);
    }
    fprintf(o, "duration 40s\n");
    fclose(o);
    m = malloc(sizeof(*m));
    if (!m)
        return;
    if (nl_scenario_peek(path, m) == 0 || m->title[0])
        launch_scenario(m);
    free(m);
}

/* ------------------------------------------------------------------ */
/* Module reference                                                   */
/* ------------------------------------------------------------------ */

static void modules_screen(void)
{
    int sel = 0, scroll = 0, k, n = 0, i;

    while (nl_module_at(n))
        n++;
    for (;;) {
        if (tui_should_quit() || n == 0)
            return;
        if (tui_frame_begin()) {
            int Wd = tui_width(), H = tui_height(), lw = 30, row = 2, last = -1, top = sel > H - 8 ? sel - (H - 8) : 0;
            const nl_module *m = nl_module_at(sel);
            static char text[6000];

            tui_box(0, 0, lw, H - 1, ST(C_INFO), _("Modules"));
            for (i = top; i < n && row < H - 2; i++) {
                const nl_module *e = nl_module_at(i);
                if (e->layer != last) {
                    last = e->layer;
                    tui_print(2, row++, lw - 4, STB(nl_layer_color(e->layer)), "%s", nl_layer_name(e->layer));
                    if (row >= H - 2)
                        break;
                }
                if (i == sel)
                    tui_fill(1, row, lw - 2, 1, TS(C_DEFAULT, C_SELECT, 0));
                tui_print(4, row++, lw - 6, TS(i == sel ? C_BRIGHT : C_TEXT, i == sel ? C_SELECT : C_DEFAULT, 0), "%s",
                          e->name);
            }
            tui_box(lw, 0, Wd - lw, H - 1, ST(C_BORDER), m->name);
            snprintf(text, sizeof(text), "# %s\n%s\n## %s\n%s\n", _(m->title), m->theory ? _(m->theory) : "",
                     _("Usage"), m->usage ? _(m->usage) : "");
            /* usage lines are preformatted */
            nl_md_draw(lw + 3, 2, Wd - lw - 6, H - 5, text, &scroll, 3);
            tui_print(1, H - 1, Wd - 2, ST(C_DIM), "%s", _("↑ ↓ choose · PgUp PgDn scroll · Esc back"));
            tui_frame_end();
        }
        tui_wait(-1, 80);
        while ((k = nl_key()) >= 0) {
            if (k == NK_ESC || k == 'q')   return;
            else if (k == NK_UP)           { sel = sel > 0 ? sel - 1 : 0; scroll = 0; }
            else if (k == NK_DOWN)         { sel = sel < n - 1 ? sel + 1 : sel; scroll = 0; }
            else if (k == NK_PGUP)         scroll = scroll > 5 ? scroll - 5 : 0;
            else if (k == NK_PGDN)         scroll += 5;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Classic demos (the original stop-and-wait / GBN / SR programs)     */
/* ------------------------------------------------------------------ */

static void classic_demos(void)
{
    static const char *const protos[] = { "stop-and-wait", "Go-Back-N", "Selective Repeat" };
    static const char *const how[] = { N_("new window"), N_("split this terminal") };
    static int proto = 0, mode = 0;
    static char loss[12] = "0.15", corrupt[12] = "0.10", delay[12] = "1000", window[8] = "4", bits[8] = "3",
                text[120] = "";
    field f[10];
    int n = 0, pos = 0;
    char script[700], sopt[300], ropt[100], *argv[16];
    int argc = 0;

    f_choice(&f[n++], _("Protocol"), protos, 3, &proto, 0);
    f_text(&f[n++], _("Loss probability"), loss, sizeof(loss));
    f_text(&f[n++], _("Corruption probability"), corrupt, sizeof(corrupt));
    f_text(&f[n++], _("Propagation delay (ms)"), delay, sizeof(delay));
    f_text(&f[n++], _("Window N (GBN, SR)"), window, sizeof(window));
    f_text(&f[n++], _("Sequence number bits (GBN, SR)"), bits, sizeof(bits));
    f_text(&f[n++], _("Text to send (empty = default)"), text, sizeof(text));
    f_choice(&f[n++], _("Open in"), how, 2, &mode, 0);
    f_simple(&f[n++], FT_BUTTON, _("Start"));
    if (form_run(_("Reliable transfer over an unreliable channel"), NULL, f, n, &pos) < 0)
        return;
    snprintf(script, sizeof(script), "%s/demo.sh", nl_home());
    snprintf(sopt, sizeof(sopt), "-l %s -c %s -d %s", loss, corrupt, delay);
    if (text[0]) {
        size_t len = strlen(sopt);
        char *q;
        for (q = text; *q; q++)
            if (*q == '\'')
                *q = ' ';
        snprintf(sopt + len, sizeof(sopt) - len, " -m '%s'", text);
    }
    snprintf(ropt, sizeof(ropt), "-l %s -c %s -d %s", loss, corrupt, delay);
    argv[argc++] = script;
    argv[argc++] = "-s";
    argv[argc++] = sopt;
    argv[argc++] = "-r";
    argv[argc++] = ropt;
    if (proto) {
        argv[argc++] = "-w";
        argv[argc++] = window;
        argv[argc++] = "-b";
        argv[argc++] = bits;
    }
    if (mode == 1) {
        argv[argc++] = "-m";
        argv[argc++] = "split";
    }
    argv[argc++] = proto == 0 ? "sw" : proto == 1 ? "gbn" : "sr";
    argv[argc] = NULL;
    nl_spawn_wait(script, argv, mode == 1 ? 1 : 1);
}

/* ------------------------------------------------------------------ */
/* Home                                                               */
/* ------------------------------------------------------------------ */

static void open_file(void)
{
    static char path[400];
    field f[2];
    int pos = 0;
    char found[1024];
    nl_meta *m;

    f_text(&f[0], _("Scenario file or catalog name"), path, sizeof(path));
    f_simple(&f[1], FT_BUTTON, _("Open"));
    if (form_run(_("Open a scenario"), NULL, f, 2, &pos) < 0 || !path[0])
        return;
    if (!nl_scenario_path(path, found, sizeof(found))) {
        text_screen(_("Open a scenario"), _("File not found."));
        return;
    }
    m = malloc(sizeof(*m));
    if (!m)
        return;
    nl_scenario_peek(found, m);
    launch_scenario(m);
    free(m);
}

int nl_menu(void)
{
    static const char *const items[][2] = {
        { N_("Scenario catalog"), N_("ready-made labs, from the first ping to consensus algorithms") },
        { N_("Study guide"), N_("the curriculum by level and topic, with theory and linked labs") },
        { N_("Build your own network"), N_("guided: topology, links, routing, traffic") },
        { N_("Open a scenario file"), N_("run a .nl file you wrote or downloaded") },
        { N_("Modules"), N_("every protocol and tool: what it is, how to use it") },
        { N_("Reliable transfer demos"), N_("stop-and-wait, Go-Back-N, Selective Repeat over real UDP") },
        { N_("Quit"), "" },
    };
    static const char *const logo[] = {
        "      ●─────────◆─────────●      ",
        "      │  ▸▸▸    │    ▸▸▸  │      ",
        "      ◆─────────▣─────────◆      ",
        "      │    ◂◂◂  │  ◂◂◂    │      ",
        "      ●─────────◆─────────●      ",
    };
    int n = (int)(sizeof(items) / sizeof(items[0])), sel = 0, k, i;

    if (!tui_init())
        return 1;
    for (;;) {
        if (tui_should_quit())
            break;
        if (tui_frame_begin()) {
            int Wd = tui_width(), H = tui_height(), y = H > 30 ? (H - 26) / 2 : 1, x = (Wd - 104) / 2;

            if (x < 1)
                x = 1;
            for (i = 0; i < 5 && H > 24; i++)
                tui_print_center(0, y + i, Wd, STB(i % 2 ? C_ACK : C_INFO), "%s", logo[i]);
            if (H > 24)
                y += 6;
            tui_print_center(0, y, Wd, TS(C_BRIGHT, C_DEFAULT, TA_BOLD), "n e t l a b");
            tui_print_center(0, y + 1, Wd, ST(C_DIM), "%s", _("a network laboratory in your terminal"));
            y += 4;
            for (i = 0; i < n; i++) {
                int cur = i == sel;
                if (cur)
                    tui_fill(x, y + i * 2, Wd - 2 * x, 1, TS(C_DEFAULT, C_SELECT, 0));
                tui_print(x + 1, y + i * 2, Wd - 2 * x - 2, TS(cur ? C_BRIGHT : C_TEXT, cur ? C_SELECT : C_DEFAULT, TA_BOLD),
                          "%s %d  %-28s" T_RESET "%s%s", cur ? "▸" : " ", i + 1, _(items[i][0]), T_DIM, _(items[i][1]));
            }
            tui_print_center(0, H - 1, Wd, ST(C_DIM), "%s  ·  %s %s",
                             _("↑ ↓ and Enter, or the number · q quit"), _("language:"), nl_lang());
            tui_frame_end();
        }
        tui_wait(-1, 100);
        while ((k = nl_key()) >= 0) {
            int go = -1;

            if (k == 'q' || k == NK_ESC)
                go = n - 1;
            else if (k == NK_UP)
                sel = (sel + n - 1) % n;
            else if (k == NK_DOWN)
                sel = (sel + 1) % n;
            else if (k == NK_ENTER)
                go = sel;
            else if (k >= '1' && k < '1' + n)
                go = sel = k - '1';
            switch (go) {
            case 0: catalog_screen(); break;
            case 1: tui_shutdown(); nl_guide(NULL); tui_init(); break;
            case 2: wizard(); break;
            case 3: open_file(); break;
            case 4: modules_screen(); break;
            case 5: classic_demos(); break;
            case 6: tui_shutdown(); return 0;
            default: break;
            }
        }
    }
    tui_shutdown();
    return 0;
}
