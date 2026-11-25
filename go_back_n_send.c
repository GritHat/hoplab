/*
 * go_back_n_send.c - Go-Back-N sender over UDP.
 *
 * The sender may have up to N packets sent and not yet acknowledged: the
 * window [base, base+N-1]. Sequence numbers are taken modulo 2^k (option -b)
 * and a single timer watches the oldest packet in flight.
 *   - data from the application and nextseqnum < base+N
 *                          -> send packet nextseqnum; if the window was
 *                             empty start the timer
 *   - intact ACK n         -> cumulative acknowledgement: every packet up to
 *                             n has arrived, so base = n+1; the timer
 *                             restarts, or stops if nothing is left in flight
 *   - timeout              -> "go back": retransmit ALL the packets from
 *                             base to nextseqnum-1
 *   - corrupted or duplicate ACK -> ignored (the timeout will take care of it)
 * At the end a FIN, numbered like the data, closes the transfer.
 *
 * To show the packets in flight one behind the other the sender transmits
 * at most one every -i milliseconds (the simulated transmission time).
 *
 * Usage: ./go_back_n_send [options] [host] [port]     (-h for help)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "channel.h"
#include "rdt_i18n.h"
#include "rdt.h"
#include "tui.h"

#define PROTOCOL "GO-BACK-N"
#define HIGHLIGHT_MS 900

static const rdt_protocol PROTO = {
    "go-back-N",
    N_("Go-back-N: with N packets in flight the channel is never idle; a single timer and, when it "
       "expires, everything from base onwards is retransmitted."),
    RDT_OPT_WINDOW | RDT_OPT_SEQBITS | RDT_OPT_INTERVAL,
    N_("The receiver must use the same -b.")
};

/* Sender rules, highlighted when they fire */
enum { RULE_NONE = -1, RULE_SEND, RULE_ACK, RULE_TIMEOUT, RULE_IGNORE };

static const tui_rule RULES[] = {
    { T_DATA "▶",
      N_("data from the app, nextseqnum < base+N"),
      N_("send DATA nextseqnum; the first in flight starts the timer") },
    { T_ACK "◀",
      N_("intact ACK n (cumulative)"),
      N_("base = n+1; restart the timer, stop it if nothing is in flight") },
    { T_TIME "◷",
      N_("timeout (a single timer, on base)"),
      N_("restart the timer and retransmit everything from base on") },
    { T_ERR "✗",
      N_("corrupted or duplicate ACK"),
      N_("ignored: if needed, the timeout takes care of it") },
};

/* The rules in the user's language (translated once, on first use) */
static const tui_rule *rules_tr(void)
{
    static tui_rule tr[sizeof(RULES) / sizeof(RULES[0])];
    size_t i;

    if (!tr[0].icon)
        for (i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++) {
            tr[i].icon = RULES[i].icon;
            tr[i].when = _(RULES[i].when);
            tr[i].then = _(RULES[i].then);
        }
    return tr;
}

typedef struct {
    rdt_options opt;
    channel     ch;
    int         sock;
    struct sockaddr_in dst;
    char        local_s[64], remote_s[64];

    rdt_data    data;              /* segments 0..nseg-1; the FIN is packet nseg */
    tui_textbuf view;
    int         total;             /* packets to deliver, FIN included */
    int         M, N;              /* sequence numbers 0..M-1, window N */

    /* protocol state: absolute packet indexes, seq = index mod M */
    int         base;              /* the oldest unacknowledged */
    int         nextseq;           /* the next never sent */
    int         next_tx;           /* the next to transmit (< nextseq: retransmission) */
    long long   next_tx_time;      /* next transmission not before (ms) */
    int         timer_on;
    long long   timer_start, timer_deadline;
    int        *tx_count;          /* transmissions of each packet */
    int         done;
    long long   t_begin, t_end;

    /* statistics */
    unsigned    tx, retx, timeouts, ack_new, ack_dup, ack_bad, ack_late;

    /* visual highlights */
    int         rule;
    long long   rule_t, render_now;
} sender;

static sender S;

/* ------------------------------------------------------------------ */
/* Protocol                                                           */
/* ------------------------------------------------------------------ */

static int seq_of(const sender *s, int idx)
{
    return idx % s->M;
}

static void set_rule(sender *s, int rule, long long now)
{
    s->rule = rule;
    s->rule_t = now;
}

static void start_timer(sender *s, long long now)
{
    s->timer_on       = 1;
    s->timer_start    = now;
    s->timer_deadline = now + s->opt.timeout_ms;
}

/* udt_send of packet idx (new if idx == nextseq, otherwise a retransmission) */
static void transmit(sender *s, int idx, long long now)
{
    rdt_packet p;
    char d[256];

    rdt_make_packet(&s->data, idx, (uint32_t)seq_of(s, idx), &p);
    ch_send(&s->ch, &p, &s->dst);
    s->tx_count[idx]++;
    s->tx++;
    tui_pkt_name(&p, d, sizeof(d));

    if (idx == s->nextseq) {
        int started = s->base == s->nextseq;     /* empty window: the timer starts */
        if (started)
            start_timer(s, now);
        s->nextseq++;
        set_rule(s, RULE_SEND, now);
        tui_log(tm(_("{d}▶ {0}%s sent {m}· in flight %d/%d%s")),
                d, s->nextseq - s->base, s->N, started ? _(" · timer started") : "");
    } else {
        s->retx++;
        tui_log(tm(_("{y}↻ {0}%s retransmitted {m}· attempt %d")), d, s->tx_count[idx]);
    }
}

/* Send what the window allows, at most one packet every interval_ms */
static void pump(sender *s, long long now)
{
    if (s->next_tx < s->base)
        s->next_tx = s->base;
    while (!s->done && now >= s->next_tx_time &&
           s->next_tx < s->base + s->N && s->next_tx < s->total) {
        transmit(s, s->next_tx, now);
        s->next_tx++;
        s->next_tx_time = now + s->opt.interval_ms;
    }
}

static void on_timeout(sender *s, long long now)
{
    int n = s->nextseq - s->base;

    s->timeouts++;
    set_rule(s, RULE_TIMEOUT, now);
    tui_log(tm(_("{t}◷ TIMEOUT{0}: no acknowledgement for {b}seq %d{0} within %.1f s → restarting "
                 "the timer and going back")),
            seq_of(s, s->base), s->opt.timeout_ms / 1000.0);
    tui_log_rule(_("go-back · retransmitting %d %s, seq %d…%d"),
                 n, n == 1 ? _("packet") : _("packets"), seq_of(s, s->base), seq_of(s, s->nextseq - 1));
    start_timer(s, now);
    s->next_tx = s->base;
}

static void handle_ack(sender *s, const ch_flight *f, long long now)
{
    const rdt_packet *a = &f->pkt;
    int outstanding = s->nextseq - s->base;
    unsigned seq;
    int d;
    char flash[48];

    if (!rdt_addr_equal(&f->from, &s->dst)) {
        char from[64];
        rdt_addr_str(&f->from, from, sizeof(from));
        tui_log(tm(_("{m}◀ packet from %s ignored: it is not the receiver")), from);
        return;
    }
    if (s->done) {
        s->ack_late++;
        tui_log(tm(_("{m}◀ %s %u out of time: transfer already finished, ignored")),
                f->valid ? rdt_type_name(a->type) : _("packet"), (unsigned)a->seq);
        tui_channel_flash(TO_SENDER, FLASH_WARN, _("ignored"));
        return;
    }
    if (!f->valid || a->type != PKT_ACK) {
        s->ack_bad++;
        set_rule(s, RULE_IGNORE, now);
        tui_log("%s", tm(_("{r}◀ corrupted ACK{0}: wrong checksum → ignored {m}(if needed, the timeout will retransmit)")));
        tui_channel_flash(TO_SENDER, FLASH_ERR, "✗ checksum");
        return;
    }

    /* distance of the ACK from base in the circular sequence number space */
    seq = a->seq % (unsigned)s->M;
    d = (int)((seq + (unsigned)s->M - (unsigned)seq_of(s, s->base)) % (unsigned)s->M);

    if (d < outstanding) {
        int old = s->base;

        s->base += d + 1;
        if (s->next_tx < s->base)
            s->next_tx = s->base;
        s->ack_new++;
        set_rule(s, RULE_ACK, now);
        snprintf(flash, sizeof(flash), "✓ ACK %u", seq);
        tui_channel_flash(TO_SENDER, FLASH_OK, flash);

        if (s->base >= s->total) {
            s->timer_on = 0;
            s->done = 1;
            s->t_end = now;
        } else if (s->base == s->nextseq) {
            s->timer_on = 0;
        } else {
            start_timer(s, now);
        }
        if (d == 0)
            tui_log(tm(_("{a}◀ ACK %u{0} received: {g}✓ acknowledges seq %u{0} → base = seq %d {m}· %s")),
                    seq, seq, seq_of(s, s->base),
                    s->done ? _("FIN confirmed") : s->timer_on ? _("timer restarted") : _("nothing in flight, timer stopped"));
        else
            tui_log(tm(_("{a}◀ ACK %u{0} received: {g}✓ cumulative, acknowledges seq %d…%u{0} (%d "
                         "packets) → base = seq %d {m}· %s")),
                    seq, seq_of(s, old), seq, d + 1, seq_of(s, s->base),
                    s->done ? _("FIN confirmed") : s->timer_on ? _("timer restarted") : _("nothing in flight, timer stopped"));
        if (s->done)
            tui_log(tm(_("{g}{b}✓ Transfer complete{0}: %d segments, %zu bytes in %.1f s {m}· %u "
                         "transmissions, %u retransmissions")),
                    s->data.nseg, s->data.len, (s->t_end - s->t_begin) / 1000.0, s->tx, s->retx);
    } else {
        s->ack_dup++;
        set_rule(s, RULE_IGNORE, now);
        snprintf(flash, sizeof(flash), "dup ACK %u", seq);
        tui_channel_flash(TO_SENDER, FLASH_WARN, flash);
        tui_log(tm(_("{a}◀ ACK %u{0} {y}duplicate{0}: it only acknowledges packets already confirmed "
                     "{m}(base = seq %d){0} → ignored")), seq, seq_of(s, s->base));
    }
}

/* ------------------------------------------------------------------ */
/* Interface                                                          */
/* ------------------------------------------------------------------ */

static void cell_info(long idx, tui_seqcell *c, const void *ctx)
{
    const sender *s = ctx;
    int fin = idx == s->total - 1;

    c->below = fin ? T_FIN "FIN" : NULL;
    if (idx < s->base) {
        c->st = SC_DONE;
    } else if (idx < s->nextseq) {
        if (idx >= s->next_tx) {             /* queued for the go-back */
            c->st = SC_RETX;
            c->below = T_WARN "↻";
        } else {
            c->st = fin ? SC_FIN : SC_FLIGHT;
        }
        if (idx == s->base && s->timer_on)
            c->timer = (double)(s->render_now - s->timer_start) / s->opt.timeout_ms;
    } else if (idx < s->base + s->N) {
        c->st = SC_USABLE;
    } else {
        c->st = SC_UNUSABLE;
    }
}

static void draw_window(const sender *s, int x, int y, int w)
{
    const tui_legend legend[] = {
        { SC_DONE, _("acknowledged") }, { SC_FLIGHT, _("in flight") }, { SC_RETX, _("to retransmit") },
        { SC_USABLE, _("usable") }, { SC_UNUSABLE, _("not usable") },
    };
    char title[160], wl[32];
    tui_seqstrip st;

    snprintf(title, sizeof(title), _("Sender window · N = %d · seq 0…%d (%d bits) · one transmission every %.2f s"),
             s->N, s->M - 1, s->opt.seq_bits, s->opt.interval_ms / 1000.0);
    snprintf(wl, sizeof(wl), _("window N = %d"), s->N);
    memset(&st, 0, sizeof(st));
    st.modulo         = s->M;
    st.count          = s->total;
    st.win_lo         = s->base;
    st.win_n          = s->done ? 0 : s->N;
    st.win_label      = wl;
    st.ptr_left       = s->done ? -1 : s->base;
    st.ptr_left_name  = "base";
    st.ptr_right      = s->done ? -1 : s->nextseq;
    st.ptr_right_name = "nextseqnum";
    st.accent         = C_DATA;
    st.cell           = cell_info;
    st.ctx            = s;
    st.legend         = legend;
    st.nlegend        = (int)(sizeof(legend) / sizeof(legend[0]));
    tui_draw_seqstrip(x, y, w, title, &st);
}

static tstyle seg_style(int seg, const void *ctx)
{
    const sender *s = ctx;

    if (seg < s->base)
        return TS(C_OK, C_DEFAULT, 0);
    if (seg < s->nextseq)
        return seg >= s->next_tx ? TS(16, C_WARN_BG, 0) : TS(231, C_DATA_BG, TA_BOLD);
    if (seg < s->base + s->N)
        return TS(C_TEXT, C_DEFAULT, 0);
    return TS(C_FAINT, C_DEFAULT, 0);
}

static void draw_transfer(const sender *s, int x, int y, int w, long long now)
{
    int ix = x + 2, iw = w - 4, lab = 12, bw = iw - lab - 16, focus = 0, i;
    int done_segs = s->base < s->data.nseg ? s->base : s->data.nseg;
    tstyle dim = TS(C_DIM, C_DEFAULT, 0), txt = TS(C_TEXT, C_DEFAULT, 0);

    tui_box(x, y, w, 6, TS(C_BORDER, C_DEFAULT, 0), _("Transfer"));

    /* the only timer, the one of packet base */
    if (s->timer_on) {
        double el = (now - s->timer_start) / 1000.0, tot = s->opt.timeout_ms / 1000.0;
        double fr = el / tot;
        short col = fr < 0.5 ? C_OK : fr < 0.8 ? C_WARN : C_ERR;
        tui_print(ix, y + 1, lab, dim, "timer " T_DATA "seq %d", seq_of(s, s->base));
        tui_bar(ix + lab, y + 1, bw, fr, col);
        tui_print(ix + lab + bw + 2, y + 1, 14, TS(col, C_DEFAULT, TA_BOLD), "%.1f / %.1f s", el > tot ? tot : el, tot);
    } else {
        tui_print(ix, y + 1, lab, dim, "timer");
        tui_bar(ix + lab, y + 1, bw, 0.0, C_OK);
        tui_print(ix + lab + bw + 2, y + 1, 14, dim, "%s", _("■ stopped"));
    }

    tui_print(ix, y + 2, lab, dim, "%s", _("confirmed"));
    tui_bar(ix + lab, y + 2, bw, s->data.nseg ? (double)done_segs / s->data.nseg : 0.0, C_ACK);
    tui_print(ix + lab + bw + 2, y + 2, 14, txt, "%d/%d", done_segs, s->data.nseg);

    /* text: confirmed (green), in flight (blue), to retransmit (yellow) */
    tui_print(ix, y + 3, lab, dim, "%s", _("data"));
    focus = s->view.n - 1;
    for (i = 0; i < s->view.n; i++)
        if (s->view.g[i].seg >= s->base) {
            focus = i;
            break;
        }
    tui_draw_text(ix + lab, y + 3, iw - lab, 2, &s->view, focus, seg_style, s);
}

static void draw_stats(const sender *s, int x, int y, int w)
{
    int ix = x + 2, iw = w - 4;
    long long end = s->done ? s->t_end : rdt_now_ms();

    tui_box(x, y, w, 4, TS(C_BORDER, C_DEFAULT, 0), _("Statistics"));
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{d}{b}DATA{0}  {m}sent{0} %u  {m}retransmitted{0} %u  {t}timeouts{0} %u{m}  │  "
                   "{c}channel ▶{0} {m}lost{0} %u/%u  {m}corrupted{0} %u/%u")),
              s->tx, s->retx, s->timeouts, s->ch.n_lost, s->ch.n_sent, s->ch.n_corrupted, s->ch.n_sent);
    tui_print(ix, y + 2, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{a}{b}ACK {0}  {g}new{0} %u  {y}duplicates{0} %u  {r}corrupted{0} %u  {m}out of "
                   "time{0} %u{m}  │  duration{0} %.1f s  {m}useful{0} %.0f%%")),
              s->ack_new, s->ack_dup, s->ack_bad, s->ack_late,
              (end - s->t_begin) / 1000.0, s->tx ? 100.0 * s->base / s->tx : 0.0);
}

static void render(sender *s, long long now)
{
    char l1[320], l2[320], note[32], foot[200];
    int W, H, y = 0, show_rules, show_transfer, show_stats;

    if (!tui_frame_begin())
        return;
    W = tui_width();
    H = tui_height();
    if (W < TUI_MIN_W || H < TUI_MIN_H) {
        tui_too_small(TUI_MIN_W, TUI_MIN_H);
        tui_frame_end();
        return;
    }
    s->render_now = now;
    show_transfer = H >= 30;
    show_stats    = H >= 34;
    show_rules    = H >= 40 && W >= 90;

    snprintf(l1, sizeof(l1),
             T_DATA "●" T_RESET " sender " T_BOLD "%s" T_RESET "   " T_DIM "──▶" T_RESET "   receiver " T_BOLD "%s" T_RESET "%s%s",
             s->local_s, s->remote_s,
             s->N > s->M - 1 ? tm(_("{y}   N > 2^k−1: an old ACK may look new!{0}")) : "",
             s->opt.timeout_ms <= 2 * s->opt.delay_ms ? T_WARN "   timeout ≤ RTT!" : "");
    snprintf(l2, sizeof(l2),
             _("timeout %.1f s · delay %.1f s · loss %.0f%% · corruption %.0f%% · %d×%d B · seed %u"),
             s->opt.timeout_ms / 1000.0, s->opt.delay_ms / 1000.0, s->opt.loss * 100, s->opt.corrupt * 100,
             s->data.nseg, s->opt.chunk, s->opt.seed);
    tui_draw_header(y, W, PROTOCOL, ROLE_SENDER, l1, l2);
    y += 4;

    if (show_rules) {
        tui_draw_rules(0, y, W, _("Go-Back-N sender rules"), rules_tr(), 4,
                       now - s->rule_t < HIGHLIGHT_MS ? s->rule : RULE_NONE);
        y += 6;
    }
    draw_window(s, 0, y, W);
    y += 7;
    if (show_transfer) {
        draw_transfer(s, 0, y, W, now);
        y += 6;
    }

    if (s->done)
        snprintf(note, sizeof(note), "%s", tm(_("{g}✓ done")));
    else
        snprintf(note, sizeof(note), "base " T_DATA T_BOLD "%d", seq_of(s, s->base));
    tui_draw_channel(0, y, W, &s->ch, ROLE_SENDER, now, note);
    y += 7;

    if (show_stats) {
        draw_stats(s, 0, y, W);
        y += 4;
    }
    tui_draw_log(0, y, W, H - 1 - y, _("Event log"));

    snprintf(foot, sizeof(foot), tm(_("{K} q {0} quit     go-back-N · sender · pid %d%s")),
             (int)getpid(), s->done ? tm(_("{g}     ✓ transfer complete, press q to quit")) : "");
    tui_draw_footer(H - 1, W, foot);
    tui_frame_end();
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    sender *s = &S;
    int quit = 0, i;

    rdt_protocol proto = PROTO;

    /* the catalog must be loaded before the default text and the help are translated */
    nl_i18n_init(NULL);
    if (proto.message)
        proto.message = _(proto.message);
    if (proto.note)
        proto.note = _(proto.note);
    rdt_parse_options(argc, argv, ROLE_SENDER, &proto, &s->opt);
    rdt_srand(s->opt.seed);
    s->sock = rdt_open_sender(&s->opt, &s->dst, s->local_s, s->remote_s, sizeof(s->local_s));

    rdt_load_data(&s->opt, &s->data);
    for (i = 0; i < s->data.nseg; i++)
        tui_text_append(&s->view, s->data.text + s->data.off[i], s->data.seg_len[i], i);
    s->total    = s->data.nseg + 1;
    s->M        = 1 << s->opt.seq_bits;
    s->N        = s->opt.window;
    s->tx_count = calloc((size_t)s->total, sizeof(*s->tx_count));
    if (!s->tx_count) {
        fprintf(stderr, "%s", _("out of memory\n"));
        return EXIT_FAILURE;
    }
    ch_init(&s->ch, s->sock, TO_RECEIVER, s->opt.loss, s->opt.corrupt, s->opt.delay_ms);

    tui_init();
    s->rule         = RULE_NONE;
    s->rule_t       = -HIGHLIGHT_MS;
    s->t_begin      = rdt_now_ms();
    s->next_tx_time = s->t_begin + 800;
    tui_log(tm(_("{i}● Sender ready{0}: %zu bytes in %d segments + FIN, window N = %d, seq 0…%d, to %s")),
            s->data.len, s->data.nseg, s->N, s->M - 1, s->remote_s);
    if (s->N > s->M - 1)
        tui_log(tm(_("{y}N = %d with %d sequence numbers: if every ACK of a window is lost, the one "
                     "for the packet before base looks new (N ≤ 2^k − 1 is required)")), s->N, s->M);

    while (!quit && !tui_should_quit()) {
        long long now = rdt_now_ms();
        const ch_flight *ev;
        ch_flight f;
        int k;

        while ((k = tui_key()) >= 0)
            if (k == 'q' || k == 'Q')
                quit = 1;

        ch_poll_socket(&s->ch);
        while ((ev = ch_fate_event(&s->ch, now)) != NULL)
            tui_log_fate(ev);
        while (ch_pop_arrival(&s->ch, now, &f))
            handle_ack(s, &f, now);
        if (!s->done && s->timer_on && now >= s->timer_deadline)
            on_timeout(s, now);
        pump(s, now);
        ch_gc(&s->ch, now);
        render(s, now);

        if (s->done && !tui_interactive())
            break;
        tui_wait(s->sock, 30);
    }
    tui_shutdown();

    if (s->done)
        printf(_("Go-back-N: %d segments (%zu bytes) delivered in %.1f s · %u transmissions, %u "
                 "retransmissions, %u timeouts\n"), s->data.nseg, s->data.len,
               (s->t_end - s->t_begin) / 1000.0, s->tx, s->retx, s->timeouts);
    else
        printf(_("Go-back-N: interrupted after %d/%d segments confirmed\n"),
               s->base < s->data.nseg ? s->base : s->data.nseg, s->data.nseg);

    close(s->sock);
    rdt_free_data(&s->data);
    free(s->tx_count);
    free(s->view.g);
    return EXIT_SUCCESS;
}
