/*
 * go_back_n_recv.c - Go-Back-N receiver over UDP.
 *
 * The receiver has no buffer: it accepts only the packet with the expected
 * sequence number (expectedseqnum, modulo 2^k) and discards everything else.
 *   - intact packet with seq = expectedseqnum
 *                          -> deliver the data to the application, send ACK
 *                             seq and wait for seq+1
 *   - packet out of order or duplicate
 *                          -> discarded; resend the ACK of the last packet
 *                             received in order (cumulative acknowledgement)
 *   - corrupted packet     -> as above
 * Before anything has been received in order the "last acknowledged" is the
 * number that precedes 0, that is 2^k-1: the sender recognises it as old.
 *
 * Usage: ./go_back_n_recv [options] [host] [port]     (-h for help)
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
#define MARK_MS 1500

static const rdt_protocol PROTO = {
    "go-back-N", NULL, RDT_OPT_SEQBITS,
    N_("The sender must use the same -b; the Go-Back-N receiver window is always 1.")
};

enum { RULE_NONE = -1, RULE_OK, RULE_OOO, RULE_BAD };

static const tui_rule RULES[] = {
    { T_DATA "◀",
      N_("intact DATA n, n = expectedseqnum"),
      N_("deliver to the app, send ACK n, expectedseqnum + 1") },
    { T_WARN "◀",
      N_("DATA n out of order or duplicate"),
      N_("discarded (no buffer); resend the ACK of the last in order") },
    { T_ERR "✗",
      N_("corrupted packet"),
      N_("discarded; resend the ACK of the last in order") },
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
    int         M;                 /* sequence numbers 0..M-1 */

    /* protocol state */
    int         expected;          /* expectedseqnum */
    int         delivered;         /* packets delivered = absolute index of expectedseqnum */
    int         done;              /* FIN delivered */
    int         last_ack;          /* -1 = none */

    /* data delivered to the application */
    rdt_buffer  buf;
    int         nseg;
    tui_textbuf view;
    long long   last_deliver_t;

    /* cell where the last discarded packet landed */
    long        mark_idx;
    long long   mark_t;

    /* statistics */
    unsigned    rx, rx_new, rx_ooo, rx_bad, acks, sessions;
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
    r->expected  = 0;
    r->delivered = 0;
    r->done      = 0;
    r->last_ack  = -1;
    r->buf.len   = 0;
    r->nseg      = 0;
    r->mark_idx  = -1;
    r->rx = r->rx_new = r->rx_ooo = r->rx_bad = r->acks = 0;
    tui_text_clear(&r->view);
}

static void set_rule(receiver *r, int rule, long long now)
{
    r->rule = rule;
    r->rule_t = now;
}

static void deliver(receiver *r, const rdt_packet *p, long long now)
{
    if (rdt_buffer_append(&r->buf, p->data, p->len) < 0)
        return;
    tui_text_append(&r->view, p->data, p->len, r->nseg);
    r->nseg++;
    r->last_deliver_t = now;
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

/* cumulative ACK of the last packet received in order */
static void resend_last_ack(receiver *r, const struct sockaddr_in *to)
{
    unsigned last = (unsigned)((r->expected + r->M - 1) % r->M);

    send_ack(r, last, to, r->delivered ? _("last received in order")
                                       : _("none in order yet: the number before 0"));
}

static void handle_packet(receiver *r, const ch_flight *f, long long now)
{
    const rdt_packet *p = &f->pkt;
    char d[256], flash[48];
    unsigned seq;

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
        set_rule(r, RULE_BAD, now);
        tui_channel_flash(TO_RECEIVER, FLASH_ERR, "✗ checksum");
        tui_log(tm(_("{r}◀ corrupted packet{0}: wrong checksum {m}(it reads %s %u){0} → discarded")), rdt_type_name(p->type), (unsigned)p->seq);
        resend_last_ack(r, &f->from);
        return;
    }
    if (p->type != PKT_DATA && p->type != PKT_FIN) {
        tui_log(tm(_("{m}◀ unexpected %s %u from the sender, ignored")), rdt_type_name(p->type), (unsigned)p->seq);
        return;
    }

    tui_pkt_name(p, d, sizeof(d));
    seq = p->seq % (unsigned)r->M;
    if (!r->done && seq == (unsigned)r->expected) {
        r->rx_new++;
        set_rule(r, RULE_OK, now);
        snprintf(flash, sizeof(flash), "✓ %s %u", rdt_type_name(p->type), seq);
        tui_channel_flash(TO_RECEIVER, FLASH_OK, flash);
        if (p->type == PKT_DATA) {
            deliver(r, p, now);
            tui_log(tm(_("{d}◀ {0}%s received: {g}✓ in order{0} → delivered to the application")), d);
        } else {
            r->done = 1;
            r->t_done = now;
            r->last_deliver_t = now;
            tui_log(tm(_("{f}◀ {0}%s received: {g}✓ in order{0} → end of data")), d);
        }
        send_ack(r, seq, &f->from, _("acknowledges everything up to this packet"));
        r->expected = (r->expected + 1) % r->M;
        r->delivered++;
        if (r->done)
            tui_log(tm(_("{g}{b}✓ Transfer complete{0}: %d segments, %zu bytes delivered in %.1f s")),
                    r->nseg, r->buf.len, (r->t_done - r->t_first) / 1000.0);
    } else {
        /* distance from the expected one: small = early, large = already past */
        unsigned dist = (seq + (unsigned)r->M - (unsigned)r->expected) % (unsigned)r->M;

        r->rx_ooo++;
        set_rule(r, RULE_OOO, now);
        r->mark_idx = dist < (unsigned)r->M / 2 ? r->delivered + (long)dist
                                                : r->delivered - (long)(r->M - dist);
        r->mark_t = now;
        snprintf(flash, sizeof(flash), _("drop %u"), seq);
        tui_channel_flash(TO_RECEIVER, FLASH_WARN, flash);
        tui_log(tm(_("{y}◀ {0}%s {y}out of order{0} (expected seq %d) → discarded")), d, r->expected);
        resend_last_ack(r, &f->from);
    }
}

/* ------------------------------------------------------------------ */
/* Interface                                                          */
/* ------------------------------------------------------------------ */

static void cell_info(long idx, tui_seqcell *c, const void *ctx)
{
    const receiver *r = ctx;

    if (idx < r->delivered)
        c->st = r->done && idx == r->delivered - 1 ? SC_FIN : SC_DONE;
    else if (idx == r->delivered && !r->done)
        c->st = SC_EXPECT;
    else
        c->st = SC_UNUSABLE;
    if (r->done && idx == r->delivered - 1)
        c->below = T_FIN "FIN";
    else if (idx == r->delivered - 1 && r->render_now - r->last_deliver_t < MARK_MS)
        c->below = T_OK "✓";
    if (idx == r->mark_idx && r->render_now - r->mark_t < MARK_MS)
        c->below = T_ERR "✗";
}

static void draw_window(const receiver *r, int x, int y, int w)
{
    const tui_legend legend[] = {
        { SC_DONE, _("already delivered") }, { SC_EXPECT, _("expected (expectedseqnum)") },
        { SC_UNUSABLE, _("out of order: discarded if they arrive") },
    };
    char title[160];
    tui_seqstrip st;

    snprintf(title, sizeof(title), _("Receiver window · seq 0…%d (%d bits) · no buffer"),
             r->M - 1, r->opt.seq_bits);
    memset(&st, 0, sizeof(st));
    st.modulo         = r->M;
    st.count          = r->done ? r->delivered : -1;
    st.win_lo         = r->delivered;
    st.win_n          = r->done ? 0 : 1;
    st.win_label      = _("window 1");
    st.ptr_left       = -1;
    st.ptr_right      = r->done ? -1 : r->delivered;
    st.ptr_right_name = "expectedseqnum";
    st.accent         = C_ACK;
    st.cell           = cell_info;
    st.ctx            = r;
    st.legend         = legend;
    st.nlegend        = (int)(sizeof(legend) / sizeof(legend[0]));
    tui_draw_seqstrip(x, y, w, title, &st);
}

static tstyle seg_style(int seg, const void *ctx)
{
    const receiver *r = ctx;

    if (seg == r->nseg - 1 && r->render_now - r->last_deliver_t < MARK_MS)
        return TS(231, C_ACK_BG, TA_BOLD);
    return TS(C_TEXT, C_DEFAULT, 0);
}

static void draw_delivery(const receiver *r, int x, int y, int w)
{
    int ix = x + 2, iw = w - 4, lab = 12;
    char ack[64], expect[64];

    tui_box(x, y, w, 7, TS(C_BORDER, C_DEFAULT, 0), _("Delivery to the application"));

    if (r->last_ack < 0)
        snprintf(ack, sizeof(ack), "%s", tm(_("{m}none")));
    else
        snprintf(ack, sizeof(ack), T_CHIP_ACK " ACK %d ", r->last_ack);
    if (r->done)
        snprintf(expect, sizeof(expect), "%s", tm(_("{g}✓ FIN received")));
    else
        snprintf(expect, sizeof(expect), T_CHIP_WARN " %d ", r->expected);
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{m}expectedseqnum{0} %s{0}     {m}delivered{0} %d %s, %zu bytes     {m}last ACK sent{0} %s")),
              expect, r->nseg, r->nseg == 1 ? _("segment") : _("segments"), r->buf.len, ack);

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
              tm(_("{d}{b}DATA{0}  {m}received{0} %u  {g}in order{0} %u  {y}out of order "
                   "(discarded){0} %u  {r}corrupted{0} %u{m}  │  duration{0} %.1f s")),
              r->rx, r->rx_new, r->rx_ooo, r->rx_bad, r->has_peer ? (end - r->t_first) / 1000.0 : 0.0);
    tui_print(ix, y + 2, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{a}{b}ACK {0}  {m}sent{0} %u{m}  │  {c}channel ◀{0} {m}lost{0} %u/%u  {m}corrupted{0} %u/%u")),
              r->acks, r->ch.n_lost, r->ch.n_sent, r->ch.n_corrupted, r->ch.n_sent);
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
                 tm(_("{a}●{0} listening on {b}%s{0}   {m}◀──{0}   sender {b}%s")),
                 r->local_s, r->peer_s);
    else
        snprintf(l1, sizeof(l1),
                 tm(_("{a}●{0} listening on {b}%s{0}   {m}waiting for the first packet…")),
                 r->local_s);
    snprintf(l2, sizeof(l2), _("seq 0…%d · delay %.1f s · ACK loss %.0f%% · ACK corruption %.0f%% · seed %u"),
             r->M - 1, r->opt.delay_ms / 1000.0, r->opt.loss * 100, r->opt.corrupt * 100, r->opt.seed);
    tui_draw_header(y, W, PROTOCOL, ROLE_RECEIVER, l1, l2);
    y += 4;

    if (show_rules) {
        tui_draw_rules(0, y, W, _("Go-Back-N receiver rules"), rules_tr(), 3,
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
        snprintf(note, sizeof(note), tm(_("expecting {a}{b}%d")), r->expected);
    tui_draw_channel(0, y, W, &r->ch, ROLE_RECEIVER, now, note);
    y += 7;

    if (show_stats) {
        draw_stats(r, 0, y, W);
        y += 4;
    }
    tui_draw_log(0, y, W, H - 1 - y, _("Event log"));

    snprintf(foot, sizeof(foot), tm(_("{K} q {0} quit     go-back-N · receiver · pid %d%s")),
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
    r->M = 1 << r->opt.seq_bits;
    ch_init(&r->ch, r->sock, TO_SENDER, r->opt.loss, r->opt.corrupt, r->opt.delay_ms);
    reset_session(r);

    /* without a screen (redirected output) exit a little after the FIN, or
     * after a longer silence if the FIN never arrives */
    linger = 6LL * r->opt.delay_ms > 4000 ? 6LL * r->opt.delay_ms : 4000;

    tui_init();
    r->rule = RULE_NONE;
    r->rule_t = r->last_deliver_t = r->mark_t = -MARK_MS * 10LL;
    tui_log(tm(_("{i}● Receiver listening{0} on %s · seq 0…%d · expecting seq 0")), r->local_s, r->M - 1);

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
        printf(_("Go-back-N: received %d segments (%zu bytes) from %s:\n"), r->nseg, r->buf.len, r->peer_s);
        fwrite(r->buf.data, 1, r->buf.len, stdout);
        printf("\n");
    } else {
        printf(_("Go-back-N: interrupted after %d segments (%zu bytes) delivered\n"), r->nseg, r->buf.len);
    }

    close(r->sock);
    free(r->buf.data);
    free(r->view.g);
    return EXIT_SUCCESS;
}
