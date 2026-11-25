/*
 * selective_repeat_recv.c - Selective Repeat receiver over UDP.
 *
 * The receiver has a window of N slots [rcv_base, rcv_base+N-1] (sequence
 * numbers modulo 2^k) and acknowledges every packet individually:
 *   - intact DATA n inside the window
 *                          -> send ACK n; if it did not have it yet, put it
 *                             in the buffer; if n = rcv_base deliver n and
 *                             every consecutive packet already in the buffer
 *                             to the application, and the window slides
 *   - intact DATA n in [rcv_base-N, rcv_base-1]
 *                          -> already delivered, but the sender did not get
 *                             the ACK (or it would not retransmit): resend ACK n
 *   - otherwise, or corrupted packet -> ignored, no ACK
 *
 * Usage: ./selective_repeat_recv [options] [host] [port]     (-h for help)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "channel.h"
#include "rdt_i18n.h"
#include "rdt.h"
#include "tui.h"

#define PROTOCOL "SELECTIVE REPEAT"
#define HIGHLIGHT_MS 900
#define MARK_MS 1500

static const rdt_protocol PROTO = {
    "selective repeat", NULL, RDT_OPT_WINDOW | RDT_OPT_SEQBITS,
    N_("The sender must use the same -w and -b.")
};

enum { RULE_NONE = -1, RULE_WINDOW, RULE_OLD, RULE_IGNORE };

static const tui_rule RULES[] = {
    { T_DATA "◀",
      "DATA n in [rcv_base, rcv_base+N−1]",
      N_("ACK n, buffer it; if n = rcv_base deliver and slide") },
    { T_WARN "◀",
      "DATA n in [rcv_base−N, rcv_base−1]",
      N_("already delivered: resend ACK n (the previous one was lost)") },
    { T_ERR "✗",
      N_("corrupted or outside both"),
      N_("ignored, no ACK: the sender will retransmit on timeout") },
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
    struct sockaddr_in local, peer;
    int         has_peer;
    char        local_s[64], peer_s[64];
    int         M, N;              /* sequence numbers 0..M-1, window N */

    /* protocol state */
    int         rcv_base;          /* the next to deliver (mod M) */
    int         delivered;         /* packets delivered = absolute index of rcv_base */
    int         done;              /* FIN delivered */
    unsigned char *have;           /* [M] packet in the buffer, by sequence number */
    rdt_packet *slot;              /* [M] */
    int         nbuf;
    int         last_ack;          /* -1 = none */

    /* data delivered to the application */
    rdt_buffer  buf;
    int         nseg, batch_first; /* segments delivered; first of the last delivery */
    tui_textbuf view;
    long long   last_deliver_t;

    /* cell where the last already-delivered packet landed */
    long        mark_idx;
    long long   mark_t;

    /* statistics */
    unsigned    rx, rx_new, rx_buf, rx_dup, rx_old, rx_ign, rx_bad, acks, sessions;
    long long   t_first, t_done, t_last_pkt;

    /* visual highlights */
    int         rule;
    long long   rule_t, render_now;
} receiver;

static receiver R;

/* ------------------------------------------------------------------ */
/* Protocol                                                           */
/* ------------------------------------------------------------------ */

static void reset_session(receiver *r)
{
    r->rcv_base    = 0;
    r->delivered   = 0;
    r->done        = 0;
    r->nbuf        = 0;
    r->last_ack    = -1;
    r->buf.len     = 0;
    r->nseg        = 0;
    r->batch_first = 0;
    r->mark_idx    = -1;
    memset(r->have, 0, (size_t)r->M);
    r->rx = r->rx_new = r->rx_buf = r->rx_dup = r->rx_old = r->rx_ign = r->rx_bad = r->acks = 0;
    tui_text_clear(&r->view);
}

static void set_rule(receiver *r, int rule, long long now)
{
    r->rule = rule;
    r->rule_t = now;
}

/* udt_send(ACK) */
static void send_ack(receiver *r, unsigned seq, const struct sockaddr_in *to, const char *why)
{
    rdt_packet a;

    memset(&a, 0, sizeof(a));
    a.type = PKT_ACK;
    a.seq  = seq;
    ch_send(&r->ch, &a, to);
    r->acks++;
    r->last_ack = (int)seq;
    tui_log(tm(_("{a}▶ {b}ACK %u{0} sent {m}· %s")), seq, why);
}

/* Deliver rcv_base and the following ones already in the buffer; returns how many */
static int deliver_in_order(receiver *r, long long now)
{
    int k = 0;

    r->batch_first = r->nseg;
    while (!r->done && r->have[r->rcv_base]) {
        const rdt_packet *q = &r->slot[r->rcv_base];

        if (q->type == PKT_DATA) {
            if (rdt_buffer_append(&r->buf, q->data, q->len) == 0) {
                tui_text_append(&r->view, q->data, q->len, r->nseg);
                r->nseg++;
            }
        } else {
            r->done = 1;
            r->t_done = now;
        }
        r->have[r->rcv_base] = 0;
        r->nbuf--;
        r->rcv_base = (r->rcv_base + 1) % r->M;
        r->delivered++;
        k++;
    }
    r->last_deliver_t = now;
    return k;
}

static void handle_packet(receiver *r, const ch_flight *f, long long now)
{
    const rdt_packet *p = &f->pkt;
    char d[256], flash[48];
    unsigned seq, dist;

    if (!r->has_peer || !rdt_addr_equal(&f->from, &r->peer)) {
        if (r->has_peer) {
            reset_session(r);
            tui_log_rule("%s", _("new session"));
        }
        r->peer = f->from;
        r->has_peer = 1;
        r->sessions++;
        r->t_first = now;
        rdt_addr_str(&r->peer, r->peer_s, sizeof(r->peer_s));
        tui_log(tm(_("{i}● First packet from sender %s")), r->peer_s);
    }
    r->rx++;
    r->t_last_pkt = now;

    if (!f->valid) {
        r->rx_bad++;
        set_rule(r, RULE_IGNORE, now);
        tui_channel_flash(TO_RECEIVER, FLASH_ERR, "✗ checksum");
        tui_log(tm(_("{r}◀ corrupted packet{0}: wrong checksum {m}(it reads %s %u){0} → discarded, "
                     "no ACK {m}(the sender will retransmit on timeout)")),
                rdt_type_name(p->type), (unsigned)p->seq);
        return;
    }
    if (p->type != PKT_DATA && p->type != PKT_FIN) {
        tui_log(tm(_("{m}◀ unexpected %s %u from the sender, ignored")), rdt_type_name(p->type), (unsigned)p->seq);
        return;
    }

    tui_pkt_name(p, d, sizeof(d));
    seq  = p->seq % (unsigned)r->M;
    dist = (seq + (unsigned)r->M - (unsigned)r->rcv_base) % (unsigned)r->M;

    if (!r->done && dist < (unsigned)r->N) {
        /* inside the window */
        set_rule(r, RULE_WINDOW, now);
        if (r->have[seq]) {
            r->rx_dup++;
            snprintf(flash, sizeof(flash), "dup %u", seq);
            tui_channel_flash(TO_RECEIVER, FLASH_WARN, flash);
            tui_log(tm(_("{y}◀ {0}%s {y}duplicate{0}: it is already in the buffer → ACK only")), d);
            send_ack(r, seq, &f->from, _("repeated, the previous one did not arrive"));
            return;
        }
        r->have[seq] = 1;
        r->slot[seq] = *p;
        r->nbuf++;
        if (dist == 0) {
            int first = r->delivered, k;
            r->rx_new++;
            snprintf(flash, sizeof(flash), "✓ %s %u", rdt_type_name(p->type), seq);
            tui_channel_flash(TO_RECEIVER, FLASH_OK, flash);
            k = deliver_in_order(r, now);
            if (k == 1)
                tui_log(tm(_("{d}◀ {0}%s received: {g}✓ it is rcv_base{0} → %s, rcv_base = seq %d")),
                        d, r->done ? _("end of data") : _("delivered to the app"), r->rcv_base);
            else
                tui_log(tm(_("{d}◀ {0}%s received: {g}✓ it is rcv_base{0} → delivered %d packets "
                             "(seq %d…%d, %d from the buffer), rcv_base = seq %d")),
                        d, k, first % r->M, (first + k - 1) % r->M, k - 1, r->rcv_base);
        } else {
            r->rx_buf++;
            snprintf(flash, sizeof(flash), "buffer %u", seq);
            tui_channel_flash(TO_RECEIVER, FLASH_OK, flash);
            tui_log(tm(_("{d}◀ {0}%s received {i}out of order{0} → buffered {m}(seq %d, rcv_base, is missing)")), d, r->rcv_base);
        }
        send_ack(r, seq, &f->from, _("acknowledgement of this packet"));
        if (r->done)
            tui_log(tm(_("{g}{b}✓ Transfer complete{0}: %d segments, %zu bytes delivered in %.1f s")),
                    r->nseg, r->buf.len, (r->t_done - r->t_first) / 1000.0);
    } else if (dist >= (unsigned)(r->M - r->N)) {
        /* in the previous window: already delivered */
        r->rx_old++;
        set_rule(r, RULE_OLD, now);
        r->mark_idx = r->delivered - (long)(r->M - dist);
        r->mark_t = now;
        snprintf(flash, sizeof(flash), _("seen %u"), seq);
        tui_channel_flash(TO_RECEIVER, FLASH_WARN, flash);
        tui_log(tm(_("{y}◀ {0}%s {y}already delivered{0}: the sender did not get the ACK → resending "
                     "it, without delivering again")), d);
        send_ack(r, seq, &f->from, _("repeated for a packet already delivered"));
    } else {
        r->rx_ign++;
        set_rule(r, RULE_IGNORE, now);
        tui_channel_flash(TO_RECEIVER, FLASH_WARN, _("ignored"));
        tui_log(tm(_("{m}◀ %s outside both windows (rcv_base = seq %d) → ignored")), d, r->rcv_base);
    }
}

/* ------------------------------------------------------------------ */
/* Interface                                                          */
/* ------------------------------------------------------------------ */

static void cell_info(long idx, tui_seqcell *c, const void *ctx)
{
    const receiver *r = ctx;
    int fin_here = r->done && idx == r->delivered - 1;

    if (idx < r->delivered) {
        c->st = fin_here ? SC_FIN : SC_DONE;
        if (fin_here)
            c->below = T_FIN "FIN";
    } else if (!r->done && idx < r->delivered + r->N) {
        int seq = (int)(idx % r->M);
        if (r->have[seq]) {
            c->st = SC_ACKED;
            c->below = r->slot[seq].type == PKT_FIN ? T_FIN "FIN" : T_OK "✓";
        } else {
            c->st = idx == r->delivered ? SC_EXPECT : SC_USABLE;
        }
    } else {
        c->st = SC_UNUSABLE;
    }
    if (idx == r->mark_idx && r->render_now - r->mark_t < MARK_MS)
        c->below = T_WARN "↻";
}

static void draw_window(const receiver *r, int x, int y, int w)
{
    const tui_legend legend[] = {
        { SC_DONE, _("already delivered") }, { SC_ACKED, _("in the buffer (acknowledged)") }, { SC_EXPECT, _("expected") },
        { SC_USABLE, _("acceptable") }, { SC_UNUSABLE, _("not usable") },
    };
    char title[160], wl[32];
    tui_seqstrip st;

    snprintf(title, sizeof(title), _("Receiver window · N = %d · seq 0…%d (%d bits) · buffer %d/%d"),
             r->N, r->M - 1, r->opt.seq_bits, r->nbuf, r->N);
    snprintf(wl, sizeof(wl), _("window N = %d"), r->N);
    memset(&st, 0, sizeof(st));
    st.modulo        = r->M;
    st.count         = r->done ? r->delivered : -1;
    st.win_lo        = r->delivered;
    st.win_n         = r->done ? 0 : r->N;
    st.win_label     = wl;
    st.shadow_lo     = r->delivered - r->N;
    st.shadow_n      = r->N;
    st.shadow_label  = _("resend ACK");
    st.ptr_left      = r->done ? -1 : r->delivered;
    st.ptr_left_name = "rcv_base";
    st.ptr_right     = -1;
    st.accent        = C_ACK;
    st.cell          = cell_info;
    st.ctx           = r;
    st.legend        = legend;
    st.nlegend       = (int)(sizeof(legend) / sizeof(legend[0]));
    tui_draw_seqstrip(x, y, w, title, &st);
}

static tstyle seg_style(int seg, const void *ctx)
{
    const receiver *r = ctx;

    if (seg >= r->batch_first && r->render_now - r->last_deliver_t < MARK_MS)
        return TS(231, C_ACK_BG, TA_BOLD);
    return TS(C_TEXT, C_DEFAULT, 0);
}

static void draw_delivery(const receiver *r, int x, int y, int w)
{
    int ix = x + 2, iw = w - 4, lab = 12;
    char ack[64], base[64];

    tui_box(x, y, w, 7, TS(C_BORDER, C_DEFAULT, 0), _("Delivery to the application"));

    if (r->last_ack < 0)
        snprintf(ack, sizeof(ack), "%s", tm(_("{m}none")));
    else
        snprintf(ack, sizeof(ack), T_CHIP_ACK " ACK %d ", r->last_ack);
    if (r->done)
        snprintf(base, sizeof(base), "%s", tm(_("{g}✓ FIN received")));
    else
        snprintf(base, sizeof(base), T_CHIP_WARN " %d ", r->rcv_base);
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{m}rcv_base{0} %s{0}    {m}delivered{0} %d %s, %zu bytes    {m}in the buffer{0} "
                   "%d    {m}last ACK{0} %s")),
              base, r->nseg, r->nseg == 1 ? _("segment") : _("segments"), r->buf.len, r->nbuf, ack);

    tui_print(ix, y + 2, lab, TS(C_DIM, C_DEFAULT, 0), "%s", _("data"));
    if (r->view.n == 0)
        tui_print(ix + lab, y + 2, iw - lab, TS(C_FAINT, C_DEFAULT, 0), "%s", _("(nothing yet)"));
    else
        tui_draw_text(ix + lab, y + 2, iw - lab, 4, &r->view, r->view.n - 1, seg_style, r);
}

static void draw_stats(const receiver *r, int x, int y, int w)
{
    int ix = x + 2, iw = w - 4;
    long long end = r->done ? r->t_done : r->has_peer ? rdt_now_ms() : r->t_first;

    tui_box(x, y, w, 4, TS(C_BORDER, C_DEFAULT, 0), _("Statistics"));
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{d}{b}DATA{0}  {m}received{0} %u  {g}in order{0} %u  {i}buffered{0} %u  "
                   "{y}duplicates{0} %u  {y}already delivered{0} %u  {r}corrupted{0} %u  "
                   "{m}ignored{0} %u")),
              r->rx, r->rx_new, r->rx_buf, r->rx_dup, r->rx_old, r->rx_bad, r->rx_ign);
    tui_print(ix, y + 2, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{a}{b}ACK {0}  {m}sent{0} %u{m}  │  {c}channel ◀{0} {m}lost{0} %u/%u  "
                   "{m}corrupted{0} %u/%u{m}  │  duration{0} %.1f s")),
              r->acks, r->ch.n_lost, r->ch.n_sent, r->ch.n_corrupted, r->ch.n_sent,
              r->has_peer ? (end - r->t_first) / 1000.0 : 0.0);
}

static void render(receiver *r, long long now)
{
    char l1[320], l2[320], note[32], foot[220];
    int W, H, y = 0, show_rules, show_delivery, show_stats;

    if (!tui_frame_begin())
        return;
    W = tui_width();
    H = tui_height();
    if (W < TUI_MIN_W || H < TUI_MIN_H) {
        tui_too_small(TUI_MIN_W, TUI_MIN_H);
        tui_frame_end();
        return;
    }
    r->render_now = now;
    show_delivery = H >= 31;
    show_stats    = H >= 35;
    show_rules    = H >= 40 && W >= 90;

    if (r->has_peer)
        snprintf(l1, sizeof(l1),
                 tm(_("{a}●{0} listening on {b}%s{0}   {m}◀──{0}   sender {b}%s{0}%s")),
                 r->local_s, r->peer_s, r->N > r->M / 2 ? tm(_("{y}   N > 2^(k−1): old and new packets "
                                                               "get mixed up!")) : "");
    else
        snprintf(l1, sizeof(l1),
                 tm(_("{a}●{0} listening on {b}%s{0}   {m}waiting for the first packet…")),
                 r->local_s);
    snprintf(l2, sizeof(l2), _("delay %.1f s · ACK loss %.0f%% · ACK corruption %.0f%% · seed %u"),
             r->opt.delay_ms / 1000.0, r->opt.loss * 100, r->opt.corrupt * 100, r->opt.seed);
    tui_draw_header(y, W, PROTOCOL, ROLE_RECEIVER, l1, l2);
    y += 4;

    if (show_rules) {
        tui_draw_rules(0, y, W, _("Selective Repeat receiver rules"), rules_tr(), 3,
                       now - r->rule_t < HIGHLIGHT_MS ? r->rule : RULE_NONE);
        y += 5;
    }
    draw_window(r, 0, y, W);
    y += 7;
    if (show_delivery) {
        draw_delivery(r, 0, y, W);
        y += 7;
    }

    if (r->done)
        snprintf(note, sizeof(note), "%s", tm(_("{g}✓ done")));
    else
        snprintf(note, sizeof(note), "base " T_ACK T_BOLD "%d", r->rcv_base);
    tui_draw_channel(0, y, W, &r->ch, ROLE_RECEIVER, now, note);
    y += 7;

    if (show_stats) {
        draw_stats(r, 0, y, W);
        y += 4;
    }
    tui_draw_log(0, y, W, H - 1 - y, _("Event log"));

    snprintf(foot, sizeof(foot), tm(_("{K} q {0} quit     selective repeat · receiver · pid %d%s")),
             (int)getpid(), r->done ? tm(_("{g}     ✓ complete · still listening")) : "");
    tui_draw_footer(H - 1, W, foot);
    tui_frame_end();
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    receiver *r = &R;
    int quit = 0;
    long long linger;

    rdt_protocol proto = PROTO;

    /* the catalog must be loaded before the default text and the help are translated */
    nl_i18n_init(NULL);
    if (proto.message)
        proto.message = _(proto.message);
    if (proto.note)
        proto.note = _(proto.note);
    rdt_parse_options(argc, argv, ROLE_RECEIVER, &proto, &r->opt);
    rdt_srand(r->opt.seed);
    r->sock = rdt_open_receiver(&r->opt, &r->local, r->local_s, sizeof(r->local_s));
    r->M    = 1 << r->opt.seq_bits;
    r->N    = r->opt.window;
    r->have = calloc((size_t)r->M, 1);
    r->slot = calloc((size_t)r->M, sizeof(*r->slot));
    if (!r->have || !r->slot) {
        fprintf(stderr, "%s", _("out of memory\n"));
        return EXIT_FAILURE;
    }
    ch_init(&r->ch, r->sock, TO_SENDER, r->opt.loss, r->opt.corrupt, r->opt.delay_ms);
    reset_session(r);

    /* without a screen (redirected output) exit a little after the FIN, or
     * after a longer silence if the FIN never arrives */
    linger = 6LL * r->opt.delay_ms > 4000 ? 6LL * r->opt.delay_ms : 4000;

    tui_init();
    r->rule = RULE_NONE;
    r->rule_t = r->last_deliver_t = r->mark_t = -MARK_MS * 10LL;
    tui_log(tm(_("{i}● Receiver listening{0} on %s · window N = %d · seq 0…%d")),
            r->local_s, r->N, r->M - 1);
    if (r->N > r->M / 2)
        tui_log(tm(_("{y}N = %d with %d sequence numbers: a retransmitted packet can fall in the new "
                     "window and be delivered twice (N ≤ 2^(k−1) is required)")), r->N, r->M);

    while (!quit && !tui_should_quit()) {
        long long now = rdt_now_ms();
        const ch_flight *ev;
        ch_flight f;
        int k;

        while ((k = tui_key()) >= 0)
            if (k == 'q' || k == 'Q')
                quit = 1;

        ch_poll_socket(&r->ch);
        while ((ev = ch_fate_event(&r->ch, now)) != NULL)
            tui_log_fate(ev);
        while (ch_pop_arrival(&r->ch, now, &f))
            handle_packet(r, &f, now);
        ch_gc(&r->ch, now);
        render(r, now);

        if (!tui_interactive() && r->has_peer &&
            now - r->t_last_pkt > (r->done ? linger : 3 * linger))
            break;
        tui_wait(r->sock, 30);
    }
    tui_shutdown();

    if (r->done) {
        printf(_("Selective repeat: received %d segments (%zu bytes) from %s:\n"), r->nseg, r->buf.len, r->peer_s);
        fwrite(r->buf.data, 1, r->buf.len, stdout);
        printf("\n");
    } else {
        printf(_("Selective repeat: interrupted after %d segments (%zu bytes) delivered\n"), r->nseg, r->buf.len);
    }

    close(r->sock);
    free(r->buf.data);
    free(r->have);
    free(r->slot);
    free(r->view.g);
    return EXIT_SUCCESS;
}
