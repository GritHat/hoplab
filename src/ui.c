/*
 * ui.c - The interactive interface of a running lab: topology map with
 *        packets in flight, node inspector, event log, packet list with
 *        dissection and hex dump, sequence diagram, graphs, guide, and a
 *        command line. One code path serves `run`, `node` and `monitor`.
 */
#include "nl_int.h"
#include "rdt.h"

#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { F_MAP, F_NODE, F_BOTTOM, F_COUNT };
enum { B_LOG, B_PACKETS, B_SEQ, B_GRAPHS, B_GUIDE, B_COUNT };

static struct {
    int      focus, zoom;
    int      sel;                       /* selected node */
    int      node_tab, node_scroll;
    int      btab;
    int      log_off;                   /* lines scrolled back from the tail */
    int      log_only_node;
    uint64_t pkt_sel;                   /* selected capture (0 = follow the newest) */
    int      pkt_detail, field_sel;
    int      guide_scroll;
    int      autoskip;
    int      help;
    int      cmd_active, cmd_cursor;
    char     cmd[256], status[200];
    long long status_until;
    int      status_err;
    char     hist[16][256];
    int      nhist, hist_pos;
    int      note_idx;                  /* next note to announce */
    int      cur_note;
    int      quit;
} U;

#define ST(fg)        TS(fg, C_DEFAULT, 0)
#define STB(fg)       TS(fg, C_DEFAULT, TA_BOLD)

/* ------------------------------------------------------------------ */
/* Keyboard                                                           */
/* ------------------------------------------------------------------ */

int nl_key(void)
{
    int c = tui_key(), c2, c3;

    if (c != 27)
        return c == '\r' ? NK_ENTER : c == 8 ? NK_BACKSPACE : c;
    c2 = tui_key();
    if (c2 < 0)
        return NK_ESC;
    if (c2 != '[' && c2 != 'O')
        return NK_ESC;
    c3 = tui_key();
    switch (c3) {
    case 'A': return NK_UP;
    case 'B': return NK_DOWN;
    case 'C': return NK_RIGHT;
    case 'D': return NK_LEFT;
    case 'H': return NK_HOME;
    case 'F': return NK_END;
    case 'Z': return NK_BTAB;
    case '1': case '7': tui_key(); return NK_HOME;
    case '4': case '8': tui_key(); return NK_END;
    case '3': tui_key(); return NK_DEL;
    case '5': tui_key(); return NK_PGUP;
    case '6': tui_key(); return NK_PGDN;
    default:  return -1;
    }
}

int nl_line_edit(char *buf, size_t size, int *cursor, int key)
{
    size_t len = strlen(buf);
    int cur = *cursor > (int)len ? (int)len : *cursor;

    if (key == NK_LEFT) {
        while (cur > 0 && ((unsigned char)buf[--cur] & 0xC0) == 0x80)
            ;
    } else if (key == NK_RIGHT) {
        if (cur < (int)len)
            cur++;
        while (cur < (int)len && ((unsigned char)buf[cur] & 0xC0) == 0x80)
            cur++;
    } else if (key == NK_HOME) {
        cur = 0;
    } else if (key == NK_END) {
        cur = (int)len;
    } else if (key == NK_BACKSPACE) {
        int start = cur;
        while (start > 0 && ((unsigned char)buf[--start] & 0xC0) == 0x80)
            ;
        memmove(buf + start, buf + cur, len - (size_t)cur + 1);
        cur = start;
    } else if (key == NK_DEL) {
        int end = cur < (int)len ? cur + 1 : cur;
        while (end < (int)len && ((unsigned char)buf[end] & 0xC0) == 0x80)
            end++;
        memmove(buf + cur, buf + end, len - (size_t)end + 1);
    } else if (key == 21) {             /* Ctrl+U */
        buf[0] = '\0';
        cur = 0;
    } else if (key >= 32 && key < 256 && key != 127) {
        if (len + 1 < size) {
            memmove(buf + cur + 1, buf + cur, len - (size_t)cur + 1);
            buf[cur++] = (char)key;
        }
    } else {
        return 0;
    }
    *cursor = cur;
    return 1;
}

/* Number of characters (not bytes) before byte offset n */
static int char_col(const char *s, int n)
{
    int col = 0, i;

    for (i = 0; i < n && s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            col++;
    return col;
}

/* ------------------------------------------------------------------ */
/* Helpers for module panels                                          */
/* ------------------------------------------------------------------ */

int nl_ui_kv(int x, int y, int w, const char *label, const char *fmt, ...)
{
    char val[300];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(val, sizeof(val), fmt, ap);
    va_end(ap);
    tui_print(x, y, w, ST(C_TEXT), T_DIM "%s" T_RESET "  %s", label, val);
    return 1;
}

int nl_ui_wrap(int x, int y, int w, int maxh, tstyle base, const char *text)
{
    return tui_print_wrapped(x, y, w, maxh, base, text);
}

void nl_ui_graph(int x, int y, int w, int h, const nl_series *s, short color, const char *title)
{
    static const char *const lv[] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    double vmin = 0, vmax = 1, last = 0;
    nl_time t0, t1;
    int gw = w - 8, gh = h - 1, i, c;
    float col[512];

    if (h < 3 || gw < 8)
        return;
    if (gw > 512)
        gw = 512;
    if (!s || s->n == 0) {
        tui_print(x, y, w, ST(C_DIM), "%s  " T_DIM "%s", title ? title : "", _("(no data yet)"));
        return;
    }
    t0 = s->t[s->head];
    t1 = s->t[(s->head + s->n - 1) % NL_SERIES_LEN];
    if (t1 <= t0)
        t1 = t0 + 1;
    for (c = 0; c < gw; c++)
        col[c] = -1;
    for (i = 0; i < s->n; i++) {
        int k = (s->head + i) % NL_SERIES_LEN;
        double v = s->v[k];

        c = (int)((double)(s->t[k] - t0) / (double)(t1 - t0) * (gw - 1));
        if (v > col[c])
            col[c] = (float)v;
        if (v > vmax)
            vmax = v;
        last = v;
    }
    for (c = 1; c < gw; c++)
        if (col[c] < 0)
            col[c] = col[c - 1];
    tui_print(x, y, w, ST(C_TEXT), "%s%s" T_RESET "  " T_BOLD "%.4g" T_RESET T_DIM "   %s … %s",
              color == C_ACK ? T_ACK : color == C_OK ? T_OK : color == C_WARN ? T_WARN : T_DATA,
              title ? title : s->name, last, nl_time_str(t0), nl_time_str(t1));
    tui_print(x, y + 1, 7, ST(C_FAINT), "%6.4g", vmax);
    tui_print(x, y + gh, 7, ST(C_FAINT), "%6.4g", vmin);
    for (c = 0; c < gw; c++) {
        double f = col[c] < 0 ? 0 : (col[c] - vmin) / (vmax - vmin) * gh;
        int r;

        for (r = 0; r < gh; r++) {
            double cell = f - r;
            int e = cell >= 1 ? 8 : cell <= 0 ? 0 : (int)(cell * 8);
            tui_put(x + 8 + c, y + gh - r, TS(color, C_DEFAULT, 0), r == 0 && e == 0 ? "▁" : lv[e]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Time                                                               */
/* ------------------------------------------------------------------ */

static nl_time min_flight(void)
{
    return (nl_time)(350000.0 * W.speed);       /* at least ~0.35 s on screen */
}

static int cap_active(const nl_cap *c)
{
    nl_time end = c->t1 > c->t0 + min_flight() ? c->t1 : c->t0 + min_flight();
    return c->fate != NL_FATE_QDROP && c->fate != NL_FATE_DOWN && W.now >= c->t0 && W.now <= end;
}

static int flights_active(void)
{
    uint64_t s;

    for (s = W.cap_seq > 96 ? W.cap_seq - 96 : 1; s < W.cap_seq; s++) {
        const nl_cap *c = nl_cap_get(s);
        if (c && cap_active(c))
            return 1;
    }
    return 0;
}

/* Slow enough for a frame to take about a second to cross a link */
static double auto_speed(void)
{
    double sum = 0;
    int i;

    if (W.nlinks == 0)
        return 1.0;
    for (i = 0; i < W.nlinks; i++) {
        const nl_linkcfg *c = &W.links[i].cfg;
        sum += (double)c->delay + (c->bw > 0 ? 8000.0 * 1e6 / c->bw : 0);
    }
    sum = sum / W.nlinks / 900000.0;
    return sum > 1.0 ? 1.0 : sum < 1e-4 ? 1e-4 : sum;
}

static void advance(double real_ms)
{
    nl_time target;

    while (U.note_idx < W.nnotes && W.notes[U.note_idx].t <= W.now) {
        nl_log(NULL, NL_L_SIM, 1, 0, T_TIME "▸ " T_RESET T_BOLD "%s", W.notes[U.note_idx].text);
        U.cur_note = U.note_idx++;
    }
    if (W.paused || W.waiting)
        return;
    target = W.now + (nl_time)(real_ms * 1000.0 * W.speed);
    if (U.autoskip && W.mode == NL_MODE_RUN && !flights_active()) {
        nl_time next = nl_next_event();
        nl_time note = U.note_idx < W.nnotes ? W.notes[U.note_idx].t : -1;

        if (note >= 0 && (next < 0 || note < next))
            next = note;
        if (next > target)
            target = next;              /* nothing is moving: jump to the next event */
    }
    nl_step(target);
}

/* ------------------------------------------------------------------ */
/* Map                                                                */
/* ------------------------------------------------------------------ */

static short node_color(const nl_node *n)
{
    return !n->up ? C_ERR : n->kind == NL_ROUTER ? C_ACK : n->kind == NL_SWITCH ? C_WARN : C_DATA;
}

static const char *node_glyph(const nl_node *n)
{
    return !n->up ? "✗" : n->kind == NL_ROUTER ? "◆" : n->kind == NL_SWITCH ? "▣" : "●";
}

static void node_xy(const nl_node *n, int x, int y, int w, int h, int *cx, int *cy)
{
    *cx = x + 4 + (int)(n->x / 100.0 * (w - 9) + 0.5);
    *cy = y + 1 + (int)(n->y / 100.0 * (h - 3) + 0.5);
}

static void draw_line(int x0, int y0, int x1, int y1, tstyle st)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy, px = x0, py = y0;

    for (;;) {
        int e2 = 2 * err, nx = px, ny = py;
        const char *g;

        if (px == x1 && py == y1)
            break;
        if (e2 > -dy) { err -= dy; nx += sx; }
        if (e2 < dx)  { err += dx; ny += sy; }
        g = ny == py ? "─" : nx == px ? "│" : (sx == sy ? "╲" : "╱");
        if (px != x0 || py != y0)
            tui_put(px, py, st, g);
        px = nx;
        py = ny;
    }
}

static void draw_map(int x, int y, int w, int h)
{
    int ix = x + 1, iy = y + 1, iw = w - 2, ih = h - 2, i;
    uint64_t s;
    char title[96];

    snprintf(title, sizeof(title), "%s", _("Network"));
    tui_box(x, y, w, h, ST(U.focus == F_MAP ? C_INFO : C_BORDER), title);
    if (iw < 12 || ih < 4)
        return;
    if (W.waiting) {
        char who[200];
        if (W.mon_listen >= 0) {
            nl_net_missing(who, sizeof(who));
            tui_print(ix + 1, iy + ih - 1, iw - 2, STB(C_WARN), "◷ %s %s — %s", _("waiting for:"), who,
                      _("Enter starts anyway"));
        } else {
            tui_print(ix + 1, iy + ih - 1, iw - 2, STB(C_WARN), "◷ %s — %s",
                      _("waiting for the monitor to start the lab"), _("Enter starts now"));
        }
        ih -= 1;
    } else if (U.cur_note >= 0 && U.cur_note < W.nnotes && ih > 8) {
        int rows = tui_print_wrapped(0, 0, iw - 4, 0, ST(C_TEXT), W.notes[U.cur_note].text);
        if (rows > 3)
            rows = 3;
        tui_put(ix + 1, iy + ih - rows, STB(C_TIME), "▸");
        tui_print_wrapped(ix + 3, iy + ih - rows, iw - 4, rows, ST(C_BRIGHT), W.notes[U.cur_note].text);
        ih -= rows;
    }

    for (i = 0; i < W.nlinks; i++) {
        const nl_link *l = &W.links[i];
        int ax, ay, bx, by, sel = l->n[0]->idx == U.sel || l->n[1]->idx == U.sel, d;

        node_xy(l->n[0], ix, iy, iw, ih, &ax, &ay);
        node_xy(l->n[1], ix, iy, iw, ih, &bx, &by);
        draw_line(ax, ay, bx, by, ST(!l->up ? C_ERR : sel ? C_DIM : C_FAINT));
        if (!l->up)
            tui_put((ax + bx) / 2, (ay + by) / 2, STB(C_ERR), "✗");
        for (d = 0; d < 2 && W.detail >= 2; d++)
            if (nl_link_backlog(l, d) > 0) {
                int qx = d ? bx + (ax - bx) / 5 : ax + (bx - ax) / 5;
                int qy = d ? by + (ay - by) / 5 : ay + (by - ay) / 5;
                tui_print(qx, qy, 6, TS(16, C_WARN_BG, TA_BOLD), "%d", nl_link_backlog(l, d));
            }
        if (W.detail >= 3 && W.nlinks <= 12)
            tui_print((ax + bx) / 2 - 5, (ay + by) / 2 + (ay == by ? 1 : 0), 22, ST(C_FAINT), "%s %s",
                      nl_rate_str(l->cfg.bw), nl_time_str(l->cfg.delay));
    }

    for (s = W.cap_seq > 96 ? W.cap_seq - 96 : 1; s < W.cap_seq; s++) {
        const nl_cap *c = nl_cap_get(s);
        const nl_link *l;
        int ax, ay, bx, by, px, py, lost, horiz;
        double p;
        tstyle st;

        if (!c || !cap_active(c) || c->link >= W.nlinks)
            continue;
        l = &W.links[c->link];
        node_xy(l->n[c->dir], ix, iy, iw, ih, &ax, &ay);
        node_xy(l->n[!c->dir], ix, iy, iw, ih, &bx, &by);
        p = c->t1 > c->t0 ? (double)(W.now - c->t0) / (double)(c->t1 - c->t0) : 1.0;
        if (p > 1.0)
            p = 1.0;
        lost = c->fate == NL_FATE_LOST || c->fate == NL_FATE_FCS;
        if (lost && p > c->fate_pos)
            p = c->fate_pos;
        horiz = abs(bx - ax) >= 2 * abs(by - ay);
        px = ax + (int)((bx - ax) * p + 0.5) - (int)strlen(c->proto) / 2;
        py = ay + (int)((by - ay) * p + 0.5) + (horiz && c->dir ? 1 : 0);
        if (!horiz && c->dir)
            px += 3;
        st = TS(16, c->color > 0 ? c->color : C_DIM, TA_BOLD);
        if (lost && (double)(W.now - c->t0) / (double)(c->t1 - c->t0 + 1) >= c->fate_pos)
            tui_print(px, py, 8, TS(231, C_ERR_BG, TA_BOLD), "✗%s", c->proto);
        else
            tui_print(px, py, 8, c->fate == NL_FATE_CORRUPT ? TS(231, C_ERR_BG, TA_BOLD) : st, "%s%s",
                      c->fate == NL_FATE_CORRUPT ? "↯" : c->fate == NL_FATE_FIXED ? "✓" : "", c->proto);
    }

    for (i = 0; i < W.nnodes; i++) {
        const nl_node *n = &W.nodes[i];
        int cx, cy, len = (int)strlen(n->name), sel = i == U.sel;
        short col = node_color(n);
        tstyle st = sel ? TS(16, col, TA_BOLD) : TS(col, C_PANEL, n->local || W.mode == NL_MODE_MONITOR ? TA_BOLD : 0);

        node_xy(n, ix, iy, iw, ih, &cx, &cy);
        tui_print(cx - len / 2 - 2, cy, len + 4, st, " %s %s ", node_glyph(n), n->name);
        if (n->role[0])
            tui_print(cx + len - len / 2 + 2, cy, 12, TS(231, 60, TA_BOLD), " %s ", n->role);
        if (W.detail >= 2 && n->kind != NL_SWITCH && nl_node_ip(n)) {
            tui_print(cx - 6, cy + 1, 16, ST(sel ? C_TEXT : C_DIM), "%s", nl_ip_str(nl_node_ip(n)));
        } else if (W.detail >= 2 && n->kind != NL_SWITCH && nl_ip6_enabled()) {
            nl_ip6 a6;
            if (nl_resolve6(n->name, &a6))
                tui_print(cx - 8, cy + 1, 24, ST(sel ? C_TEXT : C_DIM), "%s", nl_ip6_str(&a6));
        }
        if (!n->local && W.mode == NL_MODE_NODE)
            tui_print(cx - 4, cy - 1, 12, ST(C_FAINT), "%s", _("(remote)"));
    }
}

/* ------------------------------------------------------------------ */
/* Node inspector                                                     */
/* ------------------------------------------------------------------ */

static int node_tabs(const nl_node *n, const char **names, int max)
{
    int k = 0, i;

    names[k++] = _("Node");
    names[k++] = n->kind == NL_SWITCH ? _("MAC table") : _("Routes");
    if (n->kind != NL_SWITCH)
        names[k++] = "ARP";
    names[k++] = _("Links");
    for (i = 0; i < n->nmod && k < max; i++)
        if (n->mod[i].m->panel)
            names[k++] = n->mod[i].m->name;
    return k;
}

static int tab_node(const nl_node *n, int x, int y, int w, int h)
{
    int row = 0, i, k;

    tui_print(x, y + row++, w, ST(C_TEXT), "%s%s " T_BOLD "%s" T_RESET "  " T_DIM "%s%s%s",
              n->kind == NL_ROUTER ? T_ACK : n->kind == NL_SWITCH ? T_WARN : T_DATA, node_glyph(n), n->name,
              n->kind == NL_ROUTER ? _("router") : n->kind == NL_SWITCH ? _("switch") : _("host"),
              n->up ? "" : _(" — DOWN"), !n->local && W.mode != NL_MODE_RUN ? _(" — runs elsewhere") : "");
    row++;
    for (i = 0; i < n->nif && row < h; i++) {
        const nl_iface *f = &n->ifc[i];
        nl_node *peer = nl_if_peer(n, i, NULL);
        int ok = nl_if_usable(n, i);

        if (n->kind == NL_SWITCH)
            tui_print(x, y + row++, w, ST(C_TEXT), "%s%s" T_RESET " %-5s " T_DIM "↔" T_RESET " %-8s%s%s",
                      ok ? T_OK : T_ERR, ok ? "●" : "○", f->name, peer ? peer->name : "—",
                      f->blocked ? T_WARN " blocked" : "",
                      f->vlan == -1 ? T_INFO " trunk" : "");
        else
            tui_print(x, y + row++, w, ST(C_TEXT), "%s%s" T_RESET " %-5s " T_BOLD "%s/%d" T_RESET " " T_DIM "↔" T_RESET " %s",
                      ok ? T_OK : T_ERR, ok ? "●" : "○", f->name, f->ip ? nl_ip_str(f->ip) : "—", f->plen,
                      peer ? peer->name : "—");
        if (n->kind == NL_SWITCH && f->vlan > 0 && row <= h)
            tui_print(x + 28, y + row - 1, w - 28, ST(C_INFO), "vlan %d", f->vlan);
        if (W.detail >= 2 && row < h)
            tui_print(x, y + row++, w, ST(C_DIM), "        %s   rx %llu  tx %llu", nl_mac_str(f->mac),
                      (unsigned long long)f->rx, (unsigned long long)f->tx);
        for (k = 0; k < f->nip6 && row < h; k++) {
            if (W.detail < 2 && (nl_ip6_linklocal(&f->ip6[k].a) || nl_node_ip(n)))
                continue;               /* at detail 1 only what is needed to recognise the node */
            tui_print(x, y + row++, w, ST(C_TEXT), "        %s%s/%d" T_RESET " %s%s%s",
                      nl_ip6_linklocal(&f->ip6[k].a) ? T_DIM : T_BOLD, nl_ip6_str(&f->ip6[k].a), f->ip6[k].plen,
                      f->ip6[k].state == NL_A6_TENTATIVE ? T_WARN : f->ip6[k].state == NL_A6_DUPLICATE ? T_ERR : T_DIM,
                      f->ip6[k].state == NL_A6_TENTATIVE ? _("tentative (DAD)")
                      : f->ip6[k].state == NL_A6_DUPLICATE ? _("DUPLICATE") : "",
                      f->ip6[k].autoconf ? " SLAAC" : "");
        }
    }
    if (W.detail >= 2 && n->ncnt && row + 1 < h) {
        row++;
        tui_print(x, y + row++, w, STB(C_DIM), "%s", _("Counters"));
        for (i = 0; i < n->ncnt && row < h; i++)
            tui_print(x, y + row++, w, ST(C_TEXT), T_DIM "%-22s" T_RESET " %.10g", n->cnt[i].name, n->cnt[i].v);
    }
    return row;
}

static int tab_routes(const nl_node *n, int x, int y, int w, int h)
{
    int row = 0, i;

    if (n->kind == NL_SWITCH) {
        tui_print(x, y + row++, w, STB(C_DIM), "%-18s %-6s %-5s %s", "MAC", _("port"), "vlan", _("age"));
        for (i = 0; i < (int)(sizeof(n->fdb) / sizeof(n->fdb[0])) && row < h; i++) {
            const nl_fdb *e = &n->fdb[i];
            nl_node *peer;
            if (!e->used || e->port < 0)
                continue;
            peer = nl_if_peer(n, e->port, NULL);
            tui_print(x, y + row++, w, ST(C_TEXT), "%-18s %-6s %-5d %s  " T_DIM "%s", nl_mac_str(e->mac),
                      n->ifc[e->port].name, e->vlan, nl_time_str(W.now - e->seen), peer ? peer->name : "");
        }
        if (row == 1)
            tui_print(x, y + row++, w, ST(C_DIM), "%s", _("(empty: nothing learned yet)"));
        return row;
    }
    tui_print(x, y + row++, w, STB(C_DIM), "%-18s %-15s %-5s %3s %s", _("destination"), _("next hop"), _("dev"),
              _("m"), _("source"));
    for (i = 0; i < n->nrt && row < h; i++) {
        const nl_route *r = &n->rt[i];
        char dst[32];
        int usable = nl_if_usable(n, r->ifx), fresh = W.now - r->since < NL_SEC(3) && r->proto > NL_RT_STATIC;

        if (r->plen == 0)
            snprintf(dst, sizeof(dst), "default");
        else
            snprintf(dst, sizeof(dst), "%s/%d", nl_ip_str(r->dst), r->plen);
        tui_print(x, y + row++, w, ST(usable ? C_TEXT : C_FAINT), "%s%-18s" T_RESET " %-15s %-5s %3d %s%s",
                  fresh ? T_OK : "", dst, r->gw ? nl_ip_str(r->gw) : _("direct"), n->ifc[r->ifx].name, r->metric,
                  r->proto == NL_RT_CONNECTED ? T_DIM : r->proto == NL_RT_STATIC ? T_INFO : T_ACK,
                  nl_route_proto_name(r->proto));
    }
    if (n->nrt6 && row + 1 < h) {
        row++;
        tui_print(x, y + row++, w, STB(C_DIM), "IPv6");
    }
    for (i = 0; i < n->nrt6 && row < h; i++) {
        const nl_route6 *r = &n->rt6[i];
        char dst[56];
        int usable = nl_if_usable(n, r->ifx) && !(r->expire && r->expire <= W.now);

        if (r->plen == 0)
            snprintf(dst, sizeof(dst), "default");
        else
            snprintf(dst, sizeof(dst), "%s/%d", nl_ip6_str(&r->dst), r->plen);
        tui_print(x, y + row++, w, ST(usable ? C_TEXT : C_FAINT), "%-20s %-5s %3d %s%-9s" T_RESET " %s", dst,
                  n->ifc[r->ifx].name, r->metric,
                  r->proto == NL_RT_CONNECTED ? T_DIM : r->proto == NL_RT_STATIC ? T_INFO : T_ACK,
                  nl_route_proto_name(r->proto), nl_ip6_zero(&r->gw) ? "" : nl_ip6_str(&r->gw));
    }
    return row;
}

static int tab_arp(const nl_node *n, int x, int y, int w, int h)
{
    int row = 0, i;

    tui_print(x, y + row++, w, STB(C_DIM), "%-16s %-18s %s", "IP", "MAC", _("state"));
    for (i = 0; i < (int)(sizeof(n->arp) / sizeof(n->arp[0])) && row < h; i++) {
        const nl_arp *e = &n->arp[i];
        if (!e->state)
            continue;
        if (e->state == 1)
            tui_print(x, y + row++, w, ST(C_TEXT), "%-16s " T_WARN "%-18s %s (%d)", nl_ip_str(e->ip), "?",
                      _("resolving"), e->nwait);
        else
            tui_print(x, y + row++, w, ST(e->expire > W.now ? C_TEXT : C_FAINT), "%-16s %-18s " T_DIM "%s %s",
                      nl_ip_str(e->ip), nl_mac_str(e->mac), _("expires in"),
                      nl_time_str(e->expire > W.now ? e->expire - W.now : 0));
    }
    if (row == 1)
        tui_print(x, y + row++, w, ST(C_DIM), "%s", _("(empty: no neighbor resolved yet)"));
    if (nl_ip6_enabled() && row + 2 < h) {
        static const char *const st[] = { "", N_("incomplete"), N_("reachable"), N_("stale"), N_("probe") };
        int shown = 0;

        row++;
        tui_print(x, y + row++, w, STB(C_DIM), "%s", _("IPv6 neighbors (NDP)"));
        for (i = 0; i < (int)(sizeof(n->nbr) / sizeof(n->nbr[0])) && row < h; i++) {
            const nl_nbr *e = &n->nbr[i];
            int state = e->state;

            if (!state)
                continue;
            if (state == NL_NBR_REACHABLE && W.now - e->confirmed > NL_SEC(30))
                state = NL_NBR_STALE;
            shown++;
            tui_print(x, y + row++, w, ST(C_TEXT), "%-26s %-18s %s%s%s", nl_ip6_str(&e->ip),
                      state == NL_NBR_INCOMPLETE ? "?" : nl_mac_str(e->mac),
                      state == NL_NBR_REACHABLE ? T_OK : state == NL_NBR_INCOMPLETE ? T_WARN : T_DIM, _(st[state]),
                      e->router ? " R" : "");
        }
        if (!shown && row < h)
            tui_print(x, y + row++, w, ST(C_DIM), "%s", _("(empty: no neighbor resolved yet)"));
    }
    return row;
}

static int tab_links(const nl_node *n, int x, int y, int w, int h)
{
    int row = 0, i;

    for (i = 0; i < n->nif && row + 3 < h; i++) {
        const nl_iface *f = &n->ifc[i];
        const nl_link *l = f->link;
        const nl_linkdir *q;
        const nl_linkcfg *c;
        const nl_qdisc *qd;

        if (!l)
            continue;
        q = &l->d[f->end];
        c = &l->cfg;
        tui_print(x, y + row++, w, ST(C_TEXT), T_BOLD "%s" T_RESET " → %s  %s%s", f->name, l->n[!f->end]->name,
                  l->up ? "" : T_ERR, l->up ? "" : _("DOWN"));
        tui_print(x, y + row++, w, ST(C_DIM), "  %s · %s %s · %s %.3g%% · MTU %d", nl_rate_str(c->bw), _("delay"),
                  nl_time_str(c->delay), _("loss"), c->loss * 100, c->mtu);
        if (c->ber > 0 || c->corrupt > 0 || strcmp(c->codec, "fcs"))
            tui_print(x, y + row++, w, ST(C_DIM), "  BER %.2g · %s %.3g%% · %s %s", c->ber, _("corrupt"),
                      c->corrupt * 100, _("code"), c->codec);
        tui_print(x, y + row, 10, ST(C_DIM), "  %s", _("queue"));
        {
            int backlog = nl_link_backlog(l, f->end), cap = q->qalloc ? q->qalloc : 1;

            tui_bar(x + 9, y + row, w - 24 > 6 ? w - 24 : 6, (double)backlog / cap,
                    backlog * 10 > cap * 8 ? C_ERR : C_WARN);
            tui_print(x + w - 14, y + row++, 14, ST(C_TEXT), "%d/%d %s", backlog, q->qalloc, c->qdisc);
        }
        if (W.detail >= 2 && row < h)
            tui_print(x, y + row++, w, ST(C_DIM), "  tx %llu · %s %llu · %s %llu · %s %llu",
                      (unsigned long long)q->tx, _("lost"), (unsigned long long)q->lost, _("damaged"),
                      (unsigned long long)q->corrupted, _("queue drops"), (unsigned long long)q->qdrops);
        qd = nl_qdisc_find(c->qdisc);
        if (qd && qd->panel && q->qstate && row + 2 < h) {
            qd->panel(q->qstate, (nl_link *)l, f->end, x + 2, y + row, w - 2, 2);
            row += 2;
        }
    }
    return row;
}

static void draw_node(int x, int y, int w, int h)
{
    nl_node *n = nl_node_at(U.sel);
    const char *names[4 + NL_MAX_MODS];
    int ix = x + 2, iy = y + 1, iw = w - 4, ih = h - 2, nt, i, tx;

    tui_box(x, y, w, h, ST(U.focus == F_NODE ? C_INFO : C_BORDER), n ? n->name : _("Node"));
    if (!n || iw < 16 || ih < 3)
        return;
    nt = node_tabs(n, names, 4 + NL_MAX_MODS);
    if (U.node_tab >= nt)
        U.node_tab = 0;
    tx = ix;
    for (i = 0; i < nt && tx < ix + iw - 3; i++) {
        tx += tui_print(tx, iy, ix + iw - tx, i == U.node_tab ? TS(16, C_INFO, TA_BOLD) : TS(C_DIM, C_PANEL, 0),
                        " %s ", names[i]);
        tx++;
    }
    iy += 2;
    ih -= 2;
    if (U.node_tab == 0)
        tab_node(n, ix, iy, iw, ih);
    else if (U.node_tab == 1)
        tab_routes(n, ix, iy, iw, ih);
    else if (U.node_tab == 2 && n->kind != NL_SWITCH)
        tab_arp(n, ix, iy, iw, ih);
    else if (U.node_tab == (n->kind == NL_SWITCH ? 2 : 3))
        tab_links(n, ix, iy, iw, ih);
    else {
        int k = n->kind == NL_SWITCH ? 3 : 4;
        for (i = 0; i < n->nmod; i++)
            if (n->mod[i].m->panel && k++ == U.node_tab) {
                if (!n->local) {
                    tui_print(ix, iy, iw, ST(C_DIM), "%s", _("(this node runs in another process)"));
                } else {
                    tui_print(ix, iy, iw, ST(C_DIM), "%s", _(n->mod[i].m->title));
                    n->mod[i].m->panel(n, ix, iy + 2, iw, ih - 2, W.detail);
                }
                break;
            }
    }
}

/* ------------------------------------------------------------------ */
/* Bottom panel: log, packets, sequence, graphs, guide                */
/* ------------------------------------------------------------------ */

static int log_visible(const nl_logrec *r)
{
    return r->level <= W.detail && (!U.log_only_node || r->node == U.sel || r->node < 0);
}

static void draw_log(int x, int y, int w, int h)
{
    uint64_t s = W.log_seq - 1;
    int row = y + h - 1, skip = U.log_off;

    for (; s >= 1 && row >= y; s--) {
        const nl_logrec *r = nl_log_get(s);
        const nl_node *n;

        if (!r)
            break;
        if (!log_visible(r))
            continue;
        if (skip-- > 0)
            continue;
        n = nl_node_at(r->node);
        tui_print(x, row, 12, ST(C_FAINT), "%11.3f", r->t / 1000.0);
        tui_print(x + 12, row, 8, STB(n ? node_color(n) : C_DIM), "%-7s", n ? n->name : "·");
        tui_print(x + 20, row, 3, ST(nl_layer_color(r->layer)), "L%d", r->layer);
        tui_print(x + 23, row, w - 23, ST(C_TEXT), "%s", r->text);
        row--;
    }
    if (U.log_off)
        tui_print(x + w - 22, y, 22, TS(16, C_WARN, TA_BOLD), " ↑ %d %s ", U.log_off, _("(End: newest)"));
}

static const char *fate_mark(int fate)
{
    switch (fate) {
    case NL_FATE_LOST:    return T_ERR "✗ lost";
    case NL_FATE_FCS:     return T_ERR "✗ FCS";
    case NL_FATE_QDROP:   return T_ERR "✗ queue";
    case NL_FATE_DOWN:    return T_ERR "✗ down";
    case NL_FATE_CORRUPT: return T_WARN "↯ damaged";
    case NL_FATE_FIXED:   return T_OK "✓ fixed";
    default:              return "";
    }
}

static void draw_packet_detail(const nl_cap *c, int x, int y, int w, int h)
{
    nl_dissect *d = malloc(sizeof(*d));
    int row = 0, i, fw = w > 90 ? w - 54 : w, hx = x + fw + 2, start;
    const char *last = "";

    if (!d)
        return;
    nl_dissect_frame(c->pkt.data, c->pkt.len, d);
    if (U.field_sel >= d->nf)
        U.field_sel = d->nf - 1;
    if (U.field_sel < 0)
        U.field_sel = 0;
    start = U.field_sel > h - 3 ? U.field_sel - (h - 3) : 0;
    for (i = start; i < d->nf && row < h; i++) {
        const nl_field *f = &d->f[i];
        int sel = i == U.field_sel;

        if (strcmp(f->layer, last) != 0) {
            last = f->layer;
            if (row < h)
                tui_print(x, y + row++, fw, STB(C_INFO), "▾ %s", f->layer);
            if (row >= h)
                break;
        }
        if (sel)
            tui_fill(x, y + row, fw, 1, TS(C_DEFAULT, C_SELECT, 0));
        tui_print(x + 2, y + row++, fw - 2, TS(C_TEXT, sel ? C_SELECT : C_DEFAULT, 0), T_DIM "%-18s" T_RESET " %s",
                  f->name, f->value);
    }
    if (w > 90) {                       /* hex dump with the selected field highlighted */
        const nl_field *sf = d->nf ? &d->f[U.field_sel] : NULL;
        int off;

        for (off = 0, row = 0; off < c->pkt.len && row < h; off += 16, row++) {
            int k;
            tui_print(hx, y + row, 5, ST(C_FAINT), "%04x", off);
            for (k = 0; k < 16 && off + k < c->pkt.len; k++) {
                int hit = sf && sf->len > 0 && off + k >= sf->off && off + k < sf->off + sf->len;
                uint8_t b = c->pkt.data[off + k];
                tui_print(hx + 6 + k * 2 + k / 2, y + row, 2, hit ? TS(16, C_INFO, TA_BOLD) : ST(C_TEXT), "%02x", b);
                tui_print(hx + 6 + 41 + k, y + row, 1, hit ? TS(16, C_INFO, 0) : ST(C_DIM), "%c",
                          b >= 32 && b < 127 ? b : '.');
            }
        }
    }
    free(d);
}

static void draw_packets(int x, int y, int w, int h)
{
    uint64_t newest = W.cap_seq - 1, sel = U.pkt_sel && nl_cap_get(U.pkt_sel) ? U.pkt_sel : newest, s, top;
    int lw = U.pkt_detail && w > 70 ? w * 2 / 5 : w, row, rows = h - 1;

    if (newest < 1) {
        tui_print(x, y, w, ST(C_DIM), "%s", _("(no frame transmitted yet)"));
        return;
    }
    if (U.pkt_detail && w <= 70) {
        const nl_cap *c = nl_cap_get(sel);
        if (c)
            draw_packet_detail(c, x, y, w, h);
        return;
    }
    tui_print(x, y, lw, STB(C_DIM), "%6s %11s  %-13s %-6s %s", "#", "ms", _("link"), "", _("summary"));
    top = sel + (uint64_t)(rows / 2) > newest ? newest : sel + (uint64_t)(rows / 2);
    for (s = top, row = 0; s >= 1 && row < rows; s--, row++) {
        const nl_cap *c = nl_cap_get(s);
        tstyle base;

        if (!c)
            break;
        base = TS(C_TEXT, s == sel ? C_SELECT : C_DEFAULT, 0);
        if (s == sel)
            tui_fill(x, y + 1 + row, lw, 1, TS(C_DEFAULT, C_SELECT, 0));
        tui_print(x, y + 1 + row, lw, base, T_DIM "%6llu %11.3f" T_RESET "  %-6s→ %-5s ", (unsigned long long)c->seq,
                  c->t0 / 1000.0, W.nodes[c->from].name, W.nodes[c->to].name);
        tui_print(x + 34, y + 1 + row, 7, TS(16, c->color > 0 ? c->color : C_DIM, TA_BOLD), " %-5s", c->proto);
        tui_print(x + 42, y + 1 + row, lw - 42, base, "%s %s" T_RESET "  " T_DIM "%s", c->summary, fate_mark(c->fate),
                  U.pkt_detail ? "" : c->addr);
    }
    if (U.pkt_detail) {
        const nl_cap *c = nl_cap_get(sel);
        tui_vline(x + lw, y, h, ST(C_FAINT), "│");
        if (c)
            draw_packet_detail(c, x + lw + 2, y, w - lw - 2, h);
    }
}

static void draw_sequence(int x, int y, int w, int h)
{
    int cols[NL_MAX_NODES], ncol = 0, i, row, rows = h - 2, colw;
    uint64_t newest = W.cap_seq - 1, s, first;
    int used[NL_MAX_NODES] = { 0 };

    if (newest < 1 || rows < 2) {
        tui_print(x, y, w, ST(C_DIM), "%s", _("(no frame transmitted yet)"));
        return;
    }
    first = newest >= (uint64_t)rows ? newest - (uint64_t)rows + 1 : 1;
    if (U.pkt_sel && U.pkt_sel < newest && nl_cap_get(U.pkt_sel)) {
        newest = U.pkt_sel + (uint64_t)rows / 2 > newest ? newest : U.pkt_sel + (uint64_t)rows / 2;
        first = newest >= (uint64_t)rows ? newest - (uint64_t)rows + 1 : 1;
    }
    for (s = first; s <= newest; s++) {
        const nl_cap *c = nl_cap_get(s);
        if (c) {
            used[c->from] = used[c->to] = 1;
        }
    }
    for (i = 0; i < W.nnodes; i++)
        if (used[i])
            cols[i] = ncol++;
    if (!ncol)
        return;
    colw = (w - 12) / ncol;
    if (colw < 6) {
        tui_print(x, y, w, ST(C_DIM), "%s", _("(too many nodes for this width: widen the window or zoom with z)"));
        return;
    }
    for (i = 0; i < W.nnodes; i++)
        if (used[i]) {
            int cx = x + 12 + cols[i] * colw + colw / 2;
            tui_print_center(cx - colw / 2, y, colw, STB(node_color(&W.nodes[i])), "%s", W.nodes[i].name);
            tui_vline(cx, y + 1, h - 1, ST(C_FAINT), "│");
        }
    for (s = first, row = 0; s <= newest && row < rows; s++, row++) {
        const nl_cap *c = nl_cap_get(s);
        int ax, bx, lo, hi, ry = y + 2 + row, lost;
        tstyle st;

        if (!c)
            continue;
        ax = x + 12 + cols[c->from] * colw + colw / 2;
        bx = x + 12 + cols[c->to] * colw + colw / 2;
        lo = ax < bx ? ax : bx;
        hi = ax < bx ? bx : ax;
        lost = c->fate == NL_FATE_LOST || c->fate == NL_FATE_FCS || c->fate == NL_FATE_QDROP || c->fate == NL_FATE_DOWN;
        st = ST(lost ? C_ERR : c->color > 0 ? c->color : C_DIM);
        tui_print(x, ry, 11, ST(s == U.pkt_sel ? C_BRIGHT : C_FAINT), "%10.3f", c->t0 / 1000.0);
        tui_hline(lo + 1, ry, hi - lo - 1, st, "─");
        if (hi - lo > 8)
            tui_print(lo + 2, ry, hi - lo - 4, TS(st.fg, C_DEFAULT, TA_BOLD), " %s %s ", c->proto,
                      hi - lo > 24 ? c->summary : "");
        if (lost)
            tui_print(ax < bx ? hi - 2 : lo + 1, ry, 2, STB(C_ERR), "%s", ax < bx ? "─✗" : "✗─");
        else
            tui_put(ax < bx ? hi - 1 : lo + 1, ry, st, ax < bx ? "▶" : "◀");
    }
}

static void draw_graphs(int x, int y, int w, int h)
{
    nl_node *n = nl_node_at(U.sel);
    static const short colors[] = { C_DATA, C_ACK, C_WARN, C_FIN, C_CHAN, C_INFO, C_OK, C_TIME };
    int i, per, shown = 0;

    if (!n || n->nser == 0) {
        tui_print(x, y, w, ST(C_DIM), "%s", _("(the selected node records no time series; pick another node with n)"));
        return;
    }
    per = h / n->nser < 5 ? 5 : h / n->nser;
    for (i = 0; i < n->nser && (shown + 1) * per <= h; i++, shown++)
        nl_ui_graph(x, y + shown * per, w, per - 1, n->ser[i], colors[i % 8], n->ser[i]->name);
}

/* Inline Markdown to tui markup: **bold**, `code`, [text](url) */
static void md_inline(const char *in, char *out, size_t n)
{
    size_t k = 0;
    int bold = 0, code = 0;

    while (*in && k + 4 < n) {
        if (in[0] == '*' && in[1] == '*' && !code) {
            bold = !bold;
            out[k++] = '\001';
            out[k++] = bold ? 'b' : '0';
            in += 2;
        } else if (in[0] == '`') {
            code = !code;
            out[k++] = '\001';
            out[k++] = code ? 'a' : '0';
            in++;
        } else if (in[0] == '[' && !code && strchr(in, ']') && strchr(in, ']')[1] == '(' && strchr(in, ')')) {
            const char *e = strchr(in, ']');
            in++;
            while (in < e && k + 4 < n)
                out[k++] = *in++;
            in = strchr(e, ')') + 1;
        } else {
            out[k++] = *in++;
        }
    }
    out[k] = '\0';
}

/* A small Markdown subset: # headings, - bullets, paragraphs, **bold**,
 * `code`, fenced or indented code blocks. A line `<!-- detail: N -->`
 * hides what follows below detail level N (until the next such line);
 * `>N ` at the start of a line does the same for that line only. Other
 * HTML comments are skipped. Draws from row *scroll and clamps it. */
void nl_md_draw(int x, int y, int w, int h, const char *text, int *scroll, int detail)
{
    const char *p = text;
    int row = -*scroll, total = 0, block_detail = 1, fence = 0;
    char raw[4096], line[4400];

    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        tstyle st = ST(C_TEXT);
        const char *s = raw;
        int indent = 0, rows, min_detail = block_detail, code = fence;

        snprintf(raw, sizeof(raw), "%.*s", (int)len, p);
        p += len + (e ? 1 : 0);
        /* a paragraph may span several source lines: join them */
        while (!fence && raw[0] && raw[0] != '#' && raw[0] != '<' && raw[0] != '>' && raw[0] != ' ' &&
               strncmp(raw, "```", 3) != 0 && !((raw[0] == '-' || raw[0] == '*') && raw[1] == ' ') &&
               *p && *p != '\n' && *p != '#' && *p != '<' && *p != '>' && *p != ' ' && strncmp(p, "```", 3) != 0 &&
               !((*p == '-' || *p == '*') && p[1] == ' ')) {
            size_t have = strlen(raw);
            e = strchr(p, '\n');
            len = e ? (size_t)(e - p) : strlen(p);
            if (have + len + 2 >= sizeof(raw))
                break;
            raw[have] = ' ';
            memcpy(raw + have + 1, p, len);
            raw[have + 1 + len] = '\0';
            p += len + (e ? 1 : 0);
        }
        if (strncmp(s, "<!--", 4) == 0) {
            const char *d = strstr(s, "detail:");
            if (d)
                block_detail = atoi(d + 7) > 0 ? atoi(d + 7) : 1;
            continue;
        }
        if (strncmp(s, "```", 3) == 0) {
            fence = !fence;
            continue;
        }
        if (!code && s[0] == '>' && s[1] >= '1' && s[1] <= '3') {
            min_detail = s[1] - '0';
            s += 2;
            while (*s == ' ')
                s++;
        }
        if (min_detail > detail)
            continue;
        if (code) {
            st = ST(C_ACK);
            indent = 2;
        } else if (s[0] == '#') {
            int lvl = 0;
            while (s[lvl] == '#')
                lvl++;
            s += lvl;
            while (*s == ' ')
                s++;
            st = lvl == 1 ? TS(C_BRIGHT, C_DEFAULT, TA_BOLD | TA_UNDER) : STB(lvl == 2 ? C_INFO : C_WARN);
            if (total > 0) {
                row++;
                total++;
            }
        } else if ((s[0] == '-' || s[0] == '*') && s[1] == ' ') {
            if (row >= 0 && row < h)
                tui_put(x + 1, y + row, ST(C_INFO), "•");
            s += 2;
            indent = 3;
        } else if (s[0] == ' ' && s[1] == ' ' && s[2] == ' ' && s[3] == ' ') {
            st = ST(C_ACK);
            indent = 2;
            s += 4;
            code = 1;
        }
        if (code)
            snprintf(line, sizeof(line), "%s", s);
        else
            md_inline(s, line, sizeof(line));
        rows = *line ? tui_print_wrapped(0, 0, w - indent, 0, st, line) : 1;
        if (row >= 0 && row < h && *line)   /* a paragraph cut by the top edge is not drawn */
            tui_print_wrapped(x + indent, y + row, w - indent, h - row, st, line);
        row += rows;
        total += rows;
    }
    if (*scroll > total - h)
        *scroll = total - h > 0 ? total - h : 0;
    if (total > h)
        tui_print(x + w - 12, y + h - 1, 12, TS(16, C_DIM, 0), " %d/%d ↕ ", *scroll + h, total);
}

static void draw_guide(int x, int y, int w, int h)
{
    static char text[9000];
    nl_node *n = nl_node_at(U.sel);
    size_t len;
    int i;

    len = (size_t)snprintf(text, sizeof(text), "# %s\n%s\n", W.m.title, W.m.about);
    if (W.nnotes) {
        len += (size_t)snprintf(text + len, sizeof(text) - len, "## %s\n", _("What happens"));
        for (i = 0; i < W.nnotes && len < sizeof(text) - 500; i++)
            len += (size_t)snprintf(text + len, sizeof(text) - len, "- %s%s  %s\n",
                                    W.notes[i].t <= W.now ? "✓ " : "", nl_time_str(W.notes[i].t), W.notes[i].text);
    }
    for (i = 0; n && i < n->nmod && len < sizeof(text) - 2500; i++) {
        const nl_module *m = n->mod[i].m;
        len += (size_t)snprintf(text + len, sizeof(text) - len, "## %s — %s\n%s\n>2     %s\n", m->name, _(m->title),
                                m->theory ? _(m->theory) : "", m->usage ? _(m->usage) : "");
    }
    snprintf(text + len, sizeof(text) - len, "## %s\n%s\n", _("Try it"),
             _("Press : and type a command, for example:  h1 ping h2   ·   link r1 r2 down   ·   "
               "link h1 r1 loss=0.3   ·   node r2 down   ·   help"));
    nl_md_draw(x, y, w, h, text, &U.guide_scroll, W.detail);
}

static void draw_bottom(int x, int y, int w, int h)
{
    static const char *const names[B_COUNT] = { N_("Log"), N_("Packets"), N_("Sequence"), N_("Graphs"), N_("Guide") };
    int ix = x + 2, iy = y + 1, iw = w - 4, ih = h - 2, i, tx = x + 2;

    tui_box(x, y, w, h, ST(U.focus == F_BOTTOM ? C_INFO : C_BORDER), NULL);
    for (i = 0; i < B_COUNT; i++)
        tx += tui_print(tx, y, w - (tx - x) - 2, i == U.btab ? TS(16, C_INFO, TA_BOLD) : ST(C_DIM), " %s ",
                        _(names[i])) + 1;
    if (U.btab == B_LOG && U.log_only_node && nl_node_at(U.sel))
        tui_print(tx + 2, y, 30, ST(C_WARN), " %s %s ", _("only"), nl_node_at(U.sel)->name);
    if (iw < 20 || ih < 1)
        return;
    switch (U.btab) {
    case B_LOG:     draw_log(ix, iy, iw, ih); break;
    case B_PACKETS: draw_packets(ix, iy, iw, ih); break;
    case B_SEQ:     draw_sequence(ix, iy, iw, ih); break;
    case B_GRAPHS:  draw_graphs(ix, iy, iw, ih); break;
    default:        draw_guide(ix, iy, iw, ih); break;
    }
}

/* ------------------------------------------------------------------ */
/* Frame                                                              */
/* ------------------------------------------------------------------ */

static void draw_help(int W_, int H)
{
    static const char *const keys[][2] = {
        { "space", N_("pause / resume") },
        { ".", N_("when paused: advance to the next event") },
        { "+  -", N_("faster / slower") },
        { "a", N_("skip idle time automatically (on/off)") },
        { "1 2 3", N_("detail: overview, protocol messages, every frame") },
        { "Tab", N_("move the focus: network, node, bottom panel") },
        { "← → ↑ ↓", N_("focused panel: select node / change tab / scroll") },
        { "n  N", N_("next / previous node") },
        { "l p s g u", N_("bottom panel: log, packets, sequence, graphs, guide") },
        { "Enter", N_("packets: open or close the dissection of the selected frame") },
        { "[  ]", N_("packet dissection: previous / next field (highlights its bytes)") },
        { "f", N_("log: show only the selected node (on/off)") },
        { "z", N_("zoom the focused panel to the whole window") },
        { "k", N_("crash / recover the selected node") },
        { ":", N_("command line (h1 ping h2 · link a b down · help)") },
        { "?", N_("this help") },
        { "q", N_("quit") },
    };
    int n = (int)(sizeof(keys) / sizeof(keys[0])), w = W_ < 96 ? W_ : 96, h = n + 4, x = (W_ - w) / 2,
        y = (H - h) / 2, i;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    tui_fill(x, y, w, h, TS(C_TEXT, C_PANEL, 0));
    tui_box(x, y, w, h, TS(C_INFO, C_PANEL, 0), _("Keys"));
    for (i = 0; i < n; i++)
    {
        tui_print(x + 3, y + 2 + i, 12, TS(C_TEXT, C_PANEL, 0), T_KEY " %s ", keys[i][0]);
        tui_print(x + 16, y + 2 + i, w - 19, TS(C_TEXT, C_PANEL, 0), "%s", _(keys[i][1]));
    }
}

static void render(void)
{
    int Wd, H, top = 1, mh, bh, rw, x0 = 0, compact;
    char sp[32];

    if (!tui_frame_begin())
        return;
    Wd = tui_width();
    H = tui_height();
    if (Wd < 44 || H < 12) {
        tui_too_small(44, 12);
        tui_frame_end();
        return;
    }
    compact = Wd < 100 || H < 28;
    if (W.speed >= 0.995)
        snprintf(sp, sizeof(sp), "%.3g×", W.speed);
    else
        snprintf(sp, sizeof(sp), "1/%.3g×", 1.0 / W.speed);
    tui_fill(0, 0, Wd, 1, TS(C_TEXT, C_PANEL, 0));
    if (compact) {
        const nl_node *n = nl_node_at(U.sel);
        tui_print(1, 0, Wd - 30, TS(C_BRIGHT, C_PANEL, TA_BOLD), T_INFO "hoplab" T_RESET " ▸ %s",
                  W.mode == NL_MODE_NODE && n ? n->name : W.m.title);
        tui_print(Wd - 28, 0, 28, TS(C_TEXT, C_PANEL, 0), "%10.3f ms %s %s " T_CHIP_DATA " %d ", W.now / 1000.0,
                  W.paused ? T_WARN "‖" T_RESET : T_OK "▶" T_RESET, sp, W.detail);
    } else {
        tui_print(1, 0, Wd - 60, TS(C_BRIGHT, C_PANEL, TA_BOLD), T_INFO "hoplab" T_RESET " ▸ %s%s", W.m.title,
                  W.mode == NL_MODE_NODE ? _("  [node]") : W.mode == NL_MODE_MONITOR ? _("  [monitor]") : "");
        tui_print(Wd - 58, 0, 58, TS(C_TEXT, C_PANEL, 0),
                  T_DIM "t" T_RESET " %10.3f ms  %s  " T_DIM "%s" T_RESET " %s%s  " T_DIM "%s" T_RESET " %s%s%s",
                  W.now / 1000.0, W.paused ? T_CHIP_WARN " ‖ " T_RESET : T_OK "▶" T_RESET, _("speed"), sp,
                  U.autoskip && W.mode == NL_MODE_RUN ? T_DIM "+skip" T_RESET : "", _("detail"),
                  W.detail == 1 ? T_CHIP_DATA " 1 " : T_CHIP_OFF " 1 ",
                  W.detail == 2 ? T_CHIP_DATA " 2 " : T_CHIP_OFF " 2 ",
                  W.detail == 3 ? T_CHIP_DATA " 3 " : T_CHIP_OFF " 3 ");
    }

    if (U.zoom) {
        if (U.focus == F_MAP)       draw_map(0, top, Wd, H - 1 - top);
        else if (U.focus == F_NODE) draw_node(0, top, Wd, H - 1 - top);
        else                        draw_bottom(0, top, Wd, H - 1 - top);
    } else if (compact) {
        /* one column: the map or the node (whichever has the focus, or was
         * focused last) above the bottom panel */
        static int upper = F_NODE;

        if (U.focus != F_BOTTOM)
            upper = U.focus;
        mh = (H - 2) / 2;
        if (upper == F_MAP) draw_map(0, top, Wd, mh);
        else                draw_node(0, top, Wd, mh);
        draw_bottom(0, top + mh, Wd, H - 2 - mh);
    } else {
        bh = (H - 2) * 2 / 5;
        if (bh < 8) bh = 8;
        mh = H - 2 - bh;
        rw = Wd >= 150 ? 62 : Wd >= 110 ? 48 : 38;
        draw_map(x0, top, Wd - rw, mh);
        draw_node(Wd - rw, top, rw, mh);
        draw_bottom(0, top + mh, Wd, bh);
    }

    if (U.cmd_active) {
        tui_fill(0, H - 1, Wd, 1, TS(C_TEXT, C_PANEL, 0));
        tui_print(0, H - 1, Wd, TS(C_BRIGHT, C_PANEL, 0), T_INFO ":" T_RESET "%s", U.cmd);
        tui_put(1 + char_col(U.cmd, U.cmd_cursor), H - 1, TS(16, C_BRIGHT, 0),
                U.cmd[U.cmd_cursor] && (unsigned char)U.cmd[U.cmd_cursor] < 0x80
                ? (char[]){ U.cmd[U.cmd_cursor], 0 } : " ");
    } else if (U.status[0] && rdt_now_ms() < U.status_until) {
        tui_print(1, H - 1, Wd - 2, ST(U.status_err ? C_ERR : C_OK), "%s", U.status);
    } else {
        static const char *const keys[][2] = {
            { "space", N_("pause") }, { ".", N_("step") }, { "+ -", N_("speed") }, { "1 2 3", N_("detail") },
            { "Tab", N_("focus") }, { "n", N_("node") }, { "l p s g u", N_("views") }, { ":", N_("command") },
            { "?", N_("help") }, { "q", N_("quit") },
        };
        char foot[600];
        size_t len = 0, i;

        for (i = 0; i < sizeof(keys) / sizeof(keys[0]) && len < sizeof(foot) - 60; i++)
            len += (size_t)snprintf(foot + len, sizeof(foot) - len, T_KEY " %s " T_RESET " %s  ", keys[i][0],
                                    _(keys[i][1]));
        tui_draw_footer(H - 1, Wd, foot);
    }
    if (U.help)
        draw_help(Wd, H);
    tui_frame_end();
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

static void set_status(int err, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(U.status, sizeof(U.status), fmt, ap);
    va_end(ap);
    U.status_err = err;
    U.status_until = rdt_now_ms() + 4000;
}

static void select_node(int delta)
{
    if (W.nnodes == 0)
        return;
    U.sel = (U.sel + delta + W.nnodes) % W.nnodes;
    U.node_scroll = 0;
}

/* Move the selection to the nearest node in a direction on the map */
static void select_dir(int dx, int dy)
{
    const nl_node *cur = nl_node_at(U.sel);
    double best = 1e18;
    int i, pick = -1;

    if (!cur)
        return;
    for (i = 0; i < W.nnodes; i++) {
        double ddx = W.nodes[i].x - cur->x, ddy = (W.nodes[i].y - cur->y) * 0.5, along, across, score;
        if (i == U.sel)
            continue;
        along = ddx * dx + ddy * dy;
        across = fabs(ddx * dy) + fabs(ddy * dx);
        if (along <= 0.5)
            continue;
        score = along + across * 2.5;
        if (score < best) {
            best = score;
            pick = i;
        }
    }
    if (pick >= 0)
        U.sel = pick;
}

static void move_packet(int delta)
{
    uint64_t newest = W.cap_seq - 1, sel = U.pkt_sel && nl_cap_get(U.pkt_sel) ? U.pkt_sel : newest;

    if (newest < 1)
        return;
    if (delta > 0)
        sel = sel + (uint64_t)delta >= newest ? 0 : sel + (uint64_t)delta;      /* 0 = follow */
    else
        sel = sel > (uint64_t)(-delta) ? sel - (uint64_t)(-delta) : 1;
    if (sel && !nl_cap_get(sel))
        sel = 0;
    U.pkt_sel = sel;
    U.field_sel = 0;
}

static void run_command(void)
{
    if (!U.cmd[0])
        return;
    if (U.nhist == 0 || strcmp(U.hist[(U.nhist - 1) % 16], U.cmd) != 0)
        snprintf(U.hist[U.nhist++ % 16], sizeof(U.hist[0]), "%s", U.cmd);
    if (!strcmp(U.cmd, "q") || !strcmp(U.cmd, "quit")) {
        U.quit = 1;
    } else if (!strcmp(U.cmd, "pause")) {
        W.paused = 1;
    } else if (!strcmp(U.cmd, "resume")) {
        W.paused = 0;
    } else if (nl_exec(U.cmd) != 0) {
        set_status(1, "✗ %s", W.errmsg);
    } else {
        set_status(0, "✓ %s", U.cmd);
        if (!strncmp(U.cmd, "help", 4))
            U.btab = B_LOG;
    }
    U.cmd[0] = '\0';
    U.cmd_cursor = 0;
}

static void on_key(int k)
{
    nl_node *n = nl_node_at(U.sel);
    const char *names[4 + NL_MAX_MODS];
    int nt = n ? node_tabs(n, names, 4 + NL_MAX_MODS) : 1;

    if (U.cmd_active) {
        if (k == NK_ENTER) {
            U.cmd_active = 0;
            run_command();
        } else if (k == NK_ESC) {
            U.cmd_active = 0;
            U.cmd[0] = '\0';
        } else if (k == NK_UP && U.nhist) {
            if (U.hist_pos > 0 && U.hist_pos > U.nhist - 16)
                U.hist_pos--;
            snprintf(U.cmd, sizeof(U.cmd), "%s", U.hist[U.hist_pos % 16]);
            U.cmd_cursor = (int)strlen(U.cmd);
        } else if (k == NK_DOWN) {
            if (U.hist_pos < U.nhist - 1)
                snprintf(U.cmd, sizeof(U.cmd), "%s", U.hist[++U.hist_pos % 16]);
            else
                U.cmd[0] = '\0';
            U.cmd_cursor = (int)strlen(U.cmd);
        } else if (k == NK_TAB) {
            /* complete a node name */
            char *sp = strrchr(U.cmd, ' ');
            const char *word = sp ? sp + 1 : U.cmd;
            int i;
            for (i = 0; *word && i < W.nnodes; i++)
                if (strncmp(W.nodes[i].name, word, strlen(word)) == 0) {
                    snprintf(U.cmd + (word - U.cmd), sizeof(U.cmd) - (size_t)(word - U.cmd), "%s ", W.nodes[i].name);
                    U.cmd_cursor = (int)strlen(U.cmd);
                    break;
                }
        } else {
            nl_line_edit(U.cmd, sizeof(U.cmd), &U.cmd_cursor, k);
        }
        return;
    }
    if (U.help) {
        U.help = 0;
        return;
    }
    if (W.waiting && k == NK_ENTER) {
        nl_net_start();
        return;
    }
    switch (k) {
    case 'q': case 'Q': U.quit = 1; return;
    case ' ': W.paused = !W.paused; return;
    case '.':
        if (W.paused) {
            nl_time t = nl_next_event();
            if (t >= 0)
                nl_step(t);
        }
        return;
    case '+': case '=': W.speed *= 2; if (W.speed > 1000) W.speed = 1000; return;
    case '-': case '_': W.speed /= 2; if (W.speed < 1e-5) W.speed = 1e-5; return;
    case 'a': U.autoskip = !U.autoskip; set_status(0, U.autoskip ? _("idle time is skipped") : _("real pace: idle time is not skipped")); return;
    case '1': case '2': case '3': W.detail = k - '0'; return;
    case NK_TAB: U.focus = (U.focus + 1) % F_COUNT; return;
    case NK_BTAB: U.focus = (U.focus + F_COUNT - 1) % F_COUNT; return;
    case 'z': U.zoom = !U.zoom; return;
    case 'n': select_node(1); return;
    case 'N': select_node(-1); return;
    case 'l': U.btab = B_LOG; U.focus = F_BOTTOM; return;
    case 'p': U.btab = B_PACKETS; U.focus = F_BOTTOM; return;
    case 's': U.btab = B_SEQ; U.focus = F_BOTTOM; return;
    case 'g': U.btab = B_GRAPHS; U.focus = F_BOTTOM; return;
    case 'u': U.btab = B_GUIDE; U.focus = F_BOTTOM; return;
    case 'f': U.log_only_node = !U.log_only_node; return;
    case '?': case 'h': U.help = 1; return;
    case ':': case '/': U.cmd_active = 1; U.cmd[0] = '\0'; U.cmd_cursor = 0; U.hist_pos = U.nhist; return;
    case '[': if (U.field_sel > 0) U.field_sel--; return;
    case ']': U.field_sel++; return;
    case 'k':
        if (n && n->local) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "node %s %s", n->name, n->up ? "down" : "up");
            nl_exec(cmd);
        }
        return;
    default: break;
    }
    if (U.focus == F_MAP) {
        if (k == NK_LEFT)       select_dir(-1, 0);
        else if (k == NK_RIGHT) select_dir(1, 0);
        else if (k == NK_UP)    select_dir(0, -1);
        else if (k == NK_DOWN)  select_dir(0, 1);
        else if (k == NK_ENTER) U.focus = F_NODE;
    } else if (U.focus == F_NODE) {
        if (k == NK_LEFT)       U.node_tab = (U.node_tab + nt - 1) % nt;
        else if (k == NK_RIGHT) U.node_tab = (U.node_tab + 1) % nt;
        else if (k == NK_UP)    select_node(-1);
        else if (k == NK_DOWN)  select_node(1);
    } else {
        int page = tui_height() / 3;
        if (k == NK_LEFT)       U.btab = (U.btab + B_COUNT - 1) % B_COUNT;
        else if (k == NK_RIGHT) U.btab = (U.btab + 1) % B_COUNT;
        else if (U.btab == B_LOG) {
            if (k == NK_UP)        U.log_off++;
            else if (k == NK_DOWN) U.log_off = U.log_off > 0 ? U.log_off - 1 : 0;
            else if (k == NK_PGUP) U.log_off += page;
            else if (k == NK_PGDN) U.log_off = U.log_off > page ? U.log_off - page : 0;
            else if (k == NK_END)  U.log_off = 0;
        } else if (U.btab == B_PACKETS || U.btab == B_SEQ) {
            if (k == NK_UP)        move_packet(U.btab == B_SEQ ? -1 : 1);
            else if (k == NK_DOWN) move_packet(U.btab == B_SEQ ? 1 : -1);
            else if (k == NK_PGUP) move_packet(U.btab == B_SEQ ? -page : page);
            else if (k == NK_PGDN) move_packet(U.btab == B_SEQ ? page : -page);
            else if (k == NK_END)  U.pkt_sel = 0;
            else if (k == NK_ENTER) {
                if (!U.pkt_sel)
                    U.pkt_sel = W.cap_seq - 1;
                U.btab = B_PACKETS;
                U.pkt_detail = !U.pkt_detail;
            }
        } else if (U.btab == B_GUIDE) {
            if (k == NK_UP)        U.guide_scroll = U.guide_scroll > 0 ? U.guide_scroll - 1 : 0;
            else if (k == NK_DOWN) U.guide_scroll++;
            else if (k == NK_PGUP) U.guide_scroll = U.guide_scroll > page ? U.guide_scroll - page : 0;
            else if (k == NK_PGDN) U.guide_scroll += page;
        }
    }
}

int nl_ui_run(void)
{
    struct pollfd pf[NL_MAX_NODES + 2];
    int fds[NL_MAX_NODES + 1], nf, i, k;
    long long last;

    memset(&U, 0, sizeof(U));
    U.autoskip = 1;
    U.cur_note = -1;
    U.pkt_detail = 0;
    /* start on the first node that is ours, preferring one that is not a switch */
    for (i = W.nnodes - 1; i >= 0; i--)
        if (W.nodes[i].local || W.mode == NL_MODE_MONITOR)
            U.sel = i;
    for (i = W.nnodes - 1; i >= 0; i--)
        if ((W.nodes[i].local || W.mode == NL_MODE_MONITOR) && W.nodes[i].kind != NL_SWITCH)
            U.sel = i;
    if (!W.speed_set && W.mode == NL_MODE_RUN)    /* not chosen by the user or the scenario */
        W.speed = auto_speed();
    if (!tui_init())
        return 1;
    nf = nl_net_fds(fds, NL_MAX_NODES + 1);
    pf[0].fd = STDIN_FILENO;
    pf[0].events = POLLIN;
    for (i = 0; i < nf; i++) {
        pf[i + 1].fd = fds[i];
        pf[i + 1].events = POLLIN;
    }
    nl_log(NULL, NL_L_SIM, 1, 0, T_INFO "%s" T_RESET " — %s", W.m.title, _("press ? for the keys, u for the guide"));
    last = rdt_now_ms();
    while (!U.quit && !tui_should_quit()) {
        long long now = rdt_now_ms();
        double dt = (double)(now - last);

        last = now;
        if (dt > 200)
            dt = 200;                   /* after a stop (Ctrl+Z) do not jump ahead */
        while ((k = nl_key()) >= 0)
            on_key(k);
        nl_net_sync();
        advance(dt);
        if (W.mode != NL_MODE_RUN)
            nl_net_poll();              /* after advancing: arrivals are stamped with the current time */
        render();
        poll(pf, (nfds_t)(nf + 1), 33);
    }
    tui_shutdown();
    if (W.nexpects) {
        printf("%s — %s\n", W.m.title, _("expectations"));
        nl_expect_check(1);
    }
    return 0;
}
