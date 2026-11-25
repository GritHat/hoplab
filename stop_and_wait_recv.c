/*
 * stop_and_wait_recv.c - Stop-and-Wait receiver (alternating-bit protocol,
 *                        rdt 3.0) over UDP.
 *
 * The receiver expects one sequence bit at a time (first 0, then 1, ...):
 *   - intact packet with the expected bit -> deliver the data to the
 *                                            application, send an ACK with the
 *                                            same bit and wait for the other bit
 *   - intact packet with the other bit    -> it is a duplicate (the sender did
 *                                            not get our ACK): do not deliver
 *                                            it again, but retransmit the ACK
 *                                            for that bit
 *   - corrupted packet                    -> send the ACK of the last bit
 *                                            received correctly: the sender
 *                                            gets a bit different from the one
 *                                            it sent and knows it must
 *                                            retransmit
 *
 * Usage: ./stop_and_wait_recv [options] [host] [port]     (-h for help)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "channel.h"
#include "rdt_i18n.h"
#include "rdt.h"
#include "tui.h"

#define PROTOCOL "STOP-AND-WAIT"
#define HIGHLIGHT_MS 900

static const rdt_protocol PROTO = { "stop-and-wait", NULL, 0, NULL };

/* State machine transitions (to highlight the last one taken) */
enum { EDGE_NONE = -1, EDGE_RECV0, EDGE_RECV1 };

typedef struct {
    rdt_options opt;
    channel     ch;
    int         sock;
    struct sockaddr_in local, peer;
    int         has_peer;
    char        local_s[64], peer_s[64];

    /* protocol state */
    int         expected;          /* expected sequence bit */
    int         done;              /* FIN received */
    int         last_ack;          /* -1 = none */

    /* data delivered to the application */
    rdt_buffer  buf;
    int         nseg;
    tui_textbuf view;
    long long   last_deliver_t;

    /* statistics */
    unsigned    rx, rx_new, rx_dup, rx_bad, acks, sessions;
    long long   t_first, t_done, t_last_pkt;

    /* visual highlights */
    int         edge;
    long long   edge_t, loop_t, render_now;
} receiver;

static receiver R;

/* ------------------------------------------------------------------ */
/* Protocol                                                           */
/* ------------------------------------------------------------------ */

static void reset_session(receiver *r)
{
    r->expected = 0;
    r->done     = 0;
    r->last_ack = -1;
    r->buf.len     = 0;
    r->nseg     = 0;
    r->rx = r->rx_new = r->rx_dup = r->rx_bad = r->acks = 0;
    tui_text_clear(&r->view);
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
static void send_ack(receiver *r, unsigned bit, const struct sockaddr_in *to, const char *why)
{
    rdt_packet a;

    memset(&a, 0, sizeof(a));
    a.type = PKT_ACK;
    a.seq  = bit;
    ch_send(&r->ch, &a, to);
    r->acks++;
    r->last_ack = (int)bit;
    tui_log(tm(_("{a}▶ {b}ACK %u{0} sent {m}· %s")), bit, why);
}

static void handle_packet(receiver *r, const ch_flight *f, long long now)
{
    const rdt_packet *p = &f->pkt;
    char d[256], flash[48];

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
        r->loop_t = now;
        tui_channel_flash(TO_RECEIVER, FLASH_ERR, "✗ checksum");
        tui_log(tm(_("{r}◀ corrupted packet{0}: wrong checksum {m}(it reads %s %u){0} → discarded")), rdt_type_name(p->type), (unsigned)p->seq);
        send_ack(r, (unsigned)(r->expected ^ 1),
                 &f->from, _("bit of the last correct packet: the sender will retransmit"));
        return;
    }
    if (p->type != PKT_DATA && p->type != PKT_FIN) {
        tui_log(tm(_("{m}◀ unexpected %s %u from the sender, ignored")), rdt_type_name(p->type), (unsigned)p->seq);
        return;
    }

    tui_pkt_name(p, d, sizeof(d));
    if (p->seq == (uint32_t)r->expected) {
        r->rx_new++;
        snprintf(flash, sizeof(flash), "✓ %s %u", rdt_type_name(p->type), (unsigned)p->seq);
        tui_channel_flash(TO_RECEIVER, FLASH_OK, flash);
        if (p->type == PKT_DATA) {
            deliver(r, p, now);
            tui_log(tm(_("{d}◀ {0}%s received: {g}✓ expected bit{0} → delivered to the application")), d);
        } else {
            r->done = 1;
            r->t_done = now;
            tui_log(tm(_("{f}◀ {0}%s received: {g}✓ expected bit{0} → end of data")), d);
        }
        r->edge = r->expected == 0 ? EDGE_RECV0 : EDGE_RECV1;
        r->edge_t = now;
        send_ack(r, p->seq, &f->from, _("same bit as the packet received"));
        r->expected ^= 1;
        if (r->done)
            tui_log(tm(_("{g}{b}✓ Transfer complete{0}: %d segments, %zu bytes delivered in %.1f s")),
                    r->nseg, r->buf.len, (r->t_done - r->t_first) / 1000.0);
    } else {
        r->rx_dup++;
        r->loop_t = now;
        snprintf(flash, sizeof(flash), "dup %s %u", rdt_type_name(p->type), (unsigned)p->seq);
        tui_channel_flash(TO_RECEIVER, FLASH_WARN, flash);
        tui_log(tm(_("{y}◀ {0}%s {y}duplicate{0} (expected bit %d): already delivered, my ACK did "
                     "not arrive intact → not delivered again")), d, r->expected);
        send_ack(r, p->seq, &f->from, _("acknowledgement retransmitted"));
    }
}

/* ------------------------------------------------------------------ */
/* Interface                                                          */
/* ------------------------------------------------------------------ */

static tstyle edge_style(const receiver *r, int edge, long long now)
{
    if (r->edge == edge && now - r->edge_t < HIGHLIGHT_MS)
        return TS(C_INFO, C_DEFAULT, TA_BOLD);
    return TS(C_FAINT, C_DEFAULT, 0);
}

/*
 *              ╭────── DATA 0 ok · deliver · ACK 0 ───────╮
 *              │                                           ▼
 *  ↻ ▐  WAIT DATA 0  ▌                           ▐  WAIT DATA 1  ▌ ↻
 *              ▲                                           │
 *              ╰────── DATA 1 ok · deliver · ACK 1 ───────╯
 */
static void draw_fsm(const receiver *r, int x, int y, int w, long long now)
{
    const int P = 19;
    int ix = x + 2, iw = w - 4;
    int gap = iw - 2 * P - 8;
    int left, lx, rx, cl, cr, r0 = y + 1, r2 = y + 3, r4 = y + 5;
    int looping = now - r->loop_t < HIGHLIGHT_MS;
    tstyle e0 = edge_style(r, EDGE_RECV0, now), e1 = edge_style(r, EDGE_RECV1, now);
    tstyle loop_on = TS(C_ERR, C_DEFAULT, TA_BOLD), loop_off = TS(C_FAINT, C_DEFAULT, 0);

    if (gap > 40)
        gap = 40;
    left = ix + (iw - (2 * P + gap)) / 2;
    lx = left;
    rx = left + P + gap;
    cl = lx + P / 2;
    cr = rx + P / 2;

    tui_box(x, y, w, 8, TS(C_BORDER, C_DEFAULT, 0), _("Receiver state machine (rdt 3.0)"));

    /* wait 0 -> wait 1 (top) */
    tui_put(cl, r0, e0, "╭");
    tui_hline(cl + 1, r0, cr - cl - 1, e0, "─");
    tui_put(cr, r0, e0, "╮");
    tui_put(cl, r0 + 1, e0, "│");
    tui_put(cr, r0 + 1, e0, "▼");
    tui_print_center(cl + 1, r0, cr - cl - 1, e0, "%s", _(" DATA 0 ok · deliver · ACK 0 "));

    /* wait 1 -> wait 0 (bottom) */
    tui_put(cr, r2 + 1, e1, "│");
    tui_put(cl, r2 + 1, e1, "▲");
    tui_put(cl, r4, e1, "╰");
    tui_hline(cl + 1, r4, cr - cl - 1, e1, "─");
    tui_put(cr, r4, e1, "╯");
    tui_print_center(cl + 1, r4, cr - cl - 1, e1, "%s", _(" DATA 1 ok · deliver · ACK 1 "));

    tui_pill(lx, r2, P, _("WAIT DATA 0"), r->expected == 0, C_ACK);
    tui_pill(rx, r2, P, _("WAIT DATA 1"), r->expected == 1, C_ACK);
    tui_put(lx - 2, r2, looping && r->expected == 0 ? loop_on : loop_off, "↻");
    tui_put(rx + P + 1, r2, looping && r->expected == 1 ? loop_on : loop_off, "↻");

    if (r->done)
        tui_print_center(lx + P + 1, r2, gap - 2, TS(C_OK, C_DEFAULT, TA_BOLD), "%s", _("✓ FIN received"));
    else
        tui_print_center(lx + P + 1, r2, gap - 2, TS(C_TEXT, C_DEFAULT, 0),
                         tm(_("expecting bit {a}{b}%d")), r->expected);

    tui_print(ix, y + 6, iw, TS(C_DIM, C_DEFAULT, 0),
              "%s", _("↻ corrupted or duplicate packet  ⇒  resend the last ACK (the bit of the other state)"));
}

static tstyle seg_style(int seg, const void *ctx)
{
    const receiver *r = ctx;

    if (seg == r->nseg - 1 && r->render_now - r->last_deliver_t < 1500)
        return TS(231, C_ACK_BG, TA_BOLD);
    return TS(C_TEXT, C_DEFAULT, 0);
}

static void draw_delivery(const receiver *r, int x, int y, int w)
{
    int ix = x + 2, iw = w - 4, lab = 12;
    char ack[64];

    tui_box(x, y, w, 7, TS(C_BORDER, C_DEFAULT, 0), _("Delivery to the application"));

    if (r->last_ack < 0)
        snprintf(ack, sizeof(ack), "%s", tm(_("{m}none")));
    else
        snprintf(ack, sizeof(ack), T_CHIP_ACK " ACK %d ", r->last_ack);
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{m}expected bit{0}    %s{0}     {m}delivered{0} %d %s, %zu bytes     {m}last ACK sent{0} %s")),
              r->expected == 0 ? T_CHIP_ACK " 0 " T_RESET " " T_CHIP_OFF " 1 "
                               : T_CHIP_OFF " 0 " T_RESET " " T_CHIP_ACK " 1 ",
              r->nseg, r->nseg == 1 ? _("segment") : _("segments"), r->buf.len, ack);

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
              tm(_("{d}{b}DATA{0}  {m}received{0} %u  {g}new{0} %u  {y}duplicates{0} %u  "
                   "{r}corrupted{0} %u{m}  │  duration{0} %.1f s")),
              r->rx, r->rx_new, r->rx_dup, r->rx_bad, r->has_peer ? (end - r->t_first) / 1000.0 : 0.0);
    tui_print(ix, y + 2, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{a}{b}ACK {0}  {m}sent{0} %u{m}  │  {c}channel ◀{0} {m}lost{0} %u/%u  {m}corrupted{0} %u/%u")),
              r->acks, r->ch.n_lost, r->ch.n_sent, r->ch.n_corrupted, r->ch.n_sent);
}

static void render(receiver *r, long long now)
{
    char l1[320], l2[320], note[32], foot[220];
    int W, H, y = 0, show_fsm, show_stats;

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
    show_stats = H >= 19 + 4 + 5;
    show_fsm   = H >= 19 + 4 + 8 + 5;

    if (r->has_peer)
        snprintf(l1, sizeof(l1),
                 tm(_("{a}●{0} listening on {b}%s{0}   {m}◀──{0}   sender {b}%s")),
                 r->local_s, r->peer_s);
    else
        snprintf(l1, sizeof(l1),
                 tm(_("{a}●{0} listening on {b}%s{0}   {m}waiting for the first packet…")),
                 r->local_s);
    snprintf(l2, sizeof(l2), _("delay %.1f s · ACK loss %.0f%% · ACK corruption %.0f%% · seed %u"),
             r->opt.delay_ms / 1000.0, r->opt.loss * 100, r->opt.corrupt * 100, r->opt.seed);
    tui_draw_header(y, W, PROTOCOL, ROLE_RECEIVER, l1, l2);
    y += 4;

    if (show_fsm) {
        draw_fsm(r, 0, y, W, now);
        y += 8;
    }
    draw_delivery(r, 0, y, W);
    y += 7;

    snprintf(note, sizeof(note), tm(_("expecting {a}{b}%d")), r->expected);
    tui_draw_channel(0, y, W, &r->ch, ROLE_RECEIVER, now, note);
    y += 7;

    if (show_stats) {
        draw_stats(r, 0, y, W);
        y += 4;
    }
    tui_draw_log(0, y, W, H - 1 - y, _("Event log"));

    snprintf(foot, sizeof(foot), tm(_("{K} q {0} quit     stop-and-wait · receiver · pid %d%s")),
             (int)getpid(), r->done ? tm(_("{g}     ✓ complete · listening for duplicate FINs")) : "");
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
    ch_init(&r->ch, r->sock, TO_SENDER, r->opt.loss, r->opt.corrupt, r->opt.delay_ms);
    reset_session(r);

    /* without a screen (redirected output) exit a little after the FIN, or
     * after a longer silence if the FIN never arrives */
    linger = 6LL * r->opt.delay_ms > 4000 ? 6LL * r->opt.delay_ms : 4000;

    tui_init();
    r->edge = EDGE_NONE;
    r->loop_t = r->edge_t = r->last_deliver_t = -HIGHLIGHT_MS * 10LL;
    tui_log(tm(_("{i}● Receiver listening{0} on %s · expecting bit 0")), r->local_s);

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
        printf(_("Stop-and-wait: received %d segments (%zu bytes) from %s:\n"), r->nseg, r->buf.len, r->peer_s);
        fwrite(r->buf.data, 1, r->buf.len, stdout);
        printf("\n");
    } else {
        printf(_("Stop-and-wait: interrupted after %d segments (%zu bytes) delivered\n"), r->nseg, r->buf.len);
    }

    close(r->sock);
    free(r->buf.data);
    free(r->view.g);
    return EXIT_SUCCESS;
}
