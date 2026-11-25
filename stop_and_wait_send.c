/*
 * stop_and_wait_send.c - Stop-and-Wait sender (alternating-bit protocol,
 *                        rdt 3.0) over UDP.
 *
 * The sender transmits one segment at a time, marked with a single sequence
 * bit (0/1), and does not move on to the next one until it receives an ACK
 * carrying the same bit:
 *   - ACK with the same bit   -> segment confirmed, the bit flips
 *   - ACK with the other bit  -> it acknowledges the previous transfer, so
 *                                the current segment did not arrive intact:
 *                                retransmit
 *   - corrupted ACK           -> no way to know what it acknowledges: retransmit
 *   - no ACK within the timeout (data or ACK lost): retransmit
 * At the end a FIN, numbered and acknowledged like the data, closes the transfer.
 *
 * Usage: ./stop_and_wait_send [options] [host] [port]     (-h for help)
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

static const rdt_protocol PROTO = {
    "stop-and-wait",
    N_("Stop-and-wait: a single alternating bit is enough to make a channel that loses and corrupts "
       "packets reliable."),
    0, NULL
};

typedef enum { ST_WAIT_CALL, ST_WAIT_ACK, ST_DONE } sw_state;

/* State machine transitions (to highlight the last one taken) */
enum { EDGE_NONE = -1, EDGE_SEND0, EDGE_ACK0, EDGE_SEND1, EDGE_ACK1 };

typedef struct {
    rdt_options opt;
    channel     ch;
    int         sock;
    struct sockaddr_in dst;
    char        local_s[64], remote_s[64];

    /* application data split into segments; the FIN is segment nseg */
    rdt_data    data;
    tui_textbuf view;

    /* protocol state */
    sw_state    state;
    int         bit;               /* sequence bit of the current segment */
    int         seg;               /* index of the current segment */
    rdt_packet  cur;               /* copy of the packet, for retransmissions */
    int         attempts;          /* transmissions of the current segment */
    int         timer_on;
    long long   timer_start, timer_deadline;
    long long   next_send;         /* when the application hands over the next segment */
    long long   t_begin, t_end;

    /* statistics */
    unsigned    tx, retx, timeouts, ack_ok, ack_dup, ack_bad, ack_late;

    /* visual highlights */
    int         edge;
    long long   edge_t, loop_t, render_now;
} sender;

static sender S;

/* ------------------------------------------------------------------ */
/* Protocol                                                           */
/* ------------------------------------------------------------------ */

static void set_edge(sender *s, int edge, long long now)
{
    s->edge = edge;
    s->edge_t = now;
}

/* udt_send + start the timer */
static void transmit(sender *s, long long now)
{
    char d[256];
    int retx = s->attempts > 0;

    s->attempts++;
    s->tx++;
    if (retx)
        s->retx++;
    ch_send(&s->ch, &s->cur, &s->dst);
    s->timer_on       = 1;
    s->timer_start    = now;
    s->timer_deadline = now + s->opt.timeout_ms;

    tui_pkt_name(&s->cur, d, sizeof(d));
    if (!retx)
        tui_log(tm(_("{d}▶ {0}%s sent {m}· timer started (%.1f s)")),
                d, s->opt.timeout_ms / 1000.0);
    else
        tui_log(tm(_("{y}↻ {0}%s retransmitted {m}· attempt %d · timer restarted")),
                d, s->attempts);
}

/* rdt_send(): the application hands over the next segment */
static void start_segment(sender *s, long long now)
{
    if (s->seg < s->data.nseg)
        tui_log_rule(_("segment %d/%d · bit %d"), s->seg + 1, s->data.nseg, s->bit);
    else
        tui_log_rule(_("closing · FIN · bit %d"), s->bit);
    rdt_make_packet(&s->data, s->seg, (uint32_t)s->bit, &s->cur);
    s->attempts = 0;
    s->state = ST_WAIT_ACK;
    set_edge(s, s->bit == 0 ? EDGE_SEND0 : EDGE_SEND1, now);
    transmit(s, now);
}

static void on_timeout(sender *s, long long now)
{
    s->timeouts++;
    tui_log(tm(_("{t}◷ TIMEOUT{0}: no ACK %d within %.1f s {m}(packet or acknowledgement lost){0} → "
                 "retransmitting")), s->bit, s->opt.timeout_ms / 1000.0);
    s->loop_t = now;
    transmit(s, now);
}

static void handle_ack(sender *s, const ch_flight *f, long long now)
{
    const rdt_packet *a = &f->pkt;
    char flash[48];

    if (!rdt_addr_equal(&f->from, &s->dst)) {
        char from[64];
        rdt_addr_str(&f->from, from, sizeof(from));
        tui_log(tm(_("{m}◀ packet from %s ignored: it is not the receiver")), from);
        return;
    }

    if (s->state != ST_WAIT_ACK) {
        s->ack_late++;
        tui_log(tm(_("{m}◀ %s %u out of time: no packet is waiting for an acknowledgement, ignored")),
                f->valid ? rdt_type_name(a->type) : _("packet"), (unsigned)a->seq);
        tui_channel_flash(TO_SENDER, FLASH_WARN, _("ignored"));
        return;
    }

    if (!f->valid || a->type != PKT_ACK) {
        s->ack_bad++;
        tui_log("%s", tm(_("{r}◀ corrupted ACK{0}: wrong checksum, no way to know what it acknowledges → retransmitting")));
        tui_channel_flash(TO_SENDER, FLASH_ERR, "✗ checksum");
        s->loop_t = now;
        transmit(s, now);
        return;
    }

    if (a->seq == (uint32_t)s->bit) {
        double rtt = (now - s->timer_start) / 1000.0;

        s->ack_ok++;
        s->timer_on = 0;
        snprintf(flash, sizeof(flash), "✓ ACK %d", s->bit);
        tui_channel_flash(TO_SENDER, FLASH_OK, flash);
        tui_log(tm(_("{a}◀ ACK %d{0} received: {g}✓ same bit{0} → timer stopped, %s confirmed {m}(RTT %.1f s)")),
                s->bit, s->cur.type == PKT_FIN ? "FIN" : _("segment"), rtt);
        set_edge(s, s->bit == 0 ? EDGE_ACK0 : EDGE_ACK1, now);
        s->seg++;
        s->bit ^= 1;
        if (s->seg > s->data.nseg) {
            s->state = ST_DONE;
            s->t_end = now;
            tui_log(tm(_("{g}{b}✓ Transfer complete{0}: %d segments, %zu bytes in %.1f s {m}· %u "
                         "transmissions, %u retransmissions")),
                    s->data.nseg, s->data.len, (s->t_end - s->t_begin) / 1000.0, s->tx, s->retx);
        } else {
            s->state = ST_WAIT_CALL;
            s->next_send = now + (s->opt.delay_ms / 4 > 100 ? s->opt.delay_ms / 4 : 100);
        }
    } else {
        s->ack_dup++;
        snprintf(flash, sizeof(flash), "ACK %u ≠ %d", (unsigned)a->seq, s->bit);
        tui_channel_flash(TO_SENDER, FLASH_WARN, flash);
        tui_log(tm(_("{a}◀ ACK %u{0} received: {y}different bit (expected %d){0} → it acknowledges "
                     "the previous transfer, retransmitting")),
                (unsigned)a->seq, s->bit);
        s->loop_t = now;
        transmit(s, now);
    }
}

/* ------------------------------------------------------------------ */
/* Interface                                                          */
/* ------------------------------------------------------------------ */

static tstyle edge_style(const sender *s, int edge, long long now)
{
    if (s->edge == edge && now - s->edge_t < HIGHLIGHT_MS)
        return TS(C_INFO, C_DEFAULT, TA_BOLD);
    return TS(C_FAINT, C_DEFAULT, 0);
}

static void edge_h(int x0, int x1, int y, int dir, const char *label, tstyle st)
{
    if (x1 <= x0)
        return;
    tui_hline(x0, y, x1 - x0 + 1, st, "─");
    tui_put(dir > 0 ? x1 : x0, y, st, dir > 0 ? "▶" : "◀");
    tui_print_center(x0 + 1, y, x1 - x0 - 1, st, " %s ", label);
}

static void edge_v(int x, int y0, int y1, int dir, tstyle st)
{
    tui_vline(x, y0, y1 - y0 + 1, st, "│");
    tui_put(x, dir > 0 ? y1 : y0, st, dir > 0 ? "▼" : "▲");
}

/*
 *    ▐  WAIT DATA 0  ▌──── send DATA 0 ────▶▐  WAIT ACK 0  ▌ ↻
 *            ▲                                      │
 *      ACK 1 │                                      │ ACK 0
 *            │                                      ▼
 *  ↻ ▐  WAIT ACK 1  ▌◀──── send DATA 1 ────▐  WAIT DATA 1  ▌
 */
static void draw_fsm(const sender *s, int x, int y, int w, long long now)
{
    const int P = 19;
    int ix = x + 2, iw = w - 4;
    int gap = iw - 2 * P - 8;
    int left, lx, rx, cl, cr, r0 = y + 1, r4 = y + 5, active = -1;
    int looping = now - s->loop_t < HIGHLIGHT_MS;
    tstyle loop_on = TS(C_ERR, C_DEFAULT, TA_BOLD), loop_off = TS(C_FAINT, C_DEFAULT, 0);

    if (gap > 40)
        gap = 40;
    left = ix + (iw - (2 * P + gap)) / 2;
    lx = left;
    rx = left + P + gap;
    cl = lx + P / 2;
    cr = rx + P / 2;
    if (s->state == ST_WAIT_CALL)
        active = s->bit == 0 ? 0 : 2;
    else if (s->state == ST_WAIT_ACK)
        active = s->bit == 0 ? 1 : 3;

    tui_box(x, y, w, 8, TS(C_BORDER, C_DEFAULT, 0), _("Sender state machine (rdt 3.0)"));

    edge_h(lx + P, rx - 1, r0, +1, _("send DATA 0"), edge_style(s, EDGE_SEND0, now));
    edge_v(cr, r0 + 1, r4 - 1, +1, edge_style(s, EDGE_ACK0, now));
    tui_print(cr + 2, r0 + 2, P, edge_style(s, EDGE_ACK0, now), "ACK 0 ✓");
    edge_h(lx + P, rx - 1, r4, -1, _("send DATA 1"), edge_style(s, EDGE_SEND1, now));
    edge_v(cl, r0 + 1, r4 - 1, -1, edge_style(s, EDGE_ACK1, now));
    tui_print(cl - 9, r0 + 2, 8, edge_style(s, EDGE_ACK1, now), "ACK 1 ✓");

    tui_pill(lx, r0, P, _("WAIT DATA 0"), active == 0, C_DATA);
    tui_pill(rx, r0, P, _("WAIT ACK 0"), active == 1, C_WARN);
    tui_pill(rx, r4, P, _("WAIT DATA 1"), active == 2, C_DATA);
    tui_pill(lx, r4, P, _("WAIT ACK 1"), active == 3, C_WARN);
    tui_put(rx + P + 1, r0, looping && active == 1 ? loop_on : loop_off, "↻");
    tui_put(lx - 2, r4, looping && active == 3 ? loop_on : loop_off, "↻");

    if (s->state == ST_DONE)
        tui_print_center(cl + 2, r0 + 2, cr - cl - 3, TS(C_OK, C_DEFAULT, TA_BOLD), "%s", _("✓ transfer complete"));
    else if (s->state == ST_WAIT_ACK)
        tui_print_center(cl + 2, r0 + 2, cr - cl - 3, TS(C_TEXT, C_DEFAULT, 0),
                         tm(_("waiting for {a}{b}ACK %d{0}")), s->bit);
    else
        tui_print_center(cl + 2, r0 + 2, cr - cl - 3, TS(C_DIM, C_DEFAULT, 0),
                         "%s", _("waiting for data from the application"));

    tui_print(ix, y + 6, iw, TS(C_DIM, C_DEFAULT, 0),
              "%s", _("↻ waiting for ACK: timeout · ACK with the other bit · corrupted ACK  ⇒  retransmission"));
}

static tstyle seg_style(int seg, const void *ctx)
{
    const sender *s = ctx;

    if (s->state == ST_DONE || seg < s->seg)
        return TS(C_OK, C_DEFAULT, 0);
    if (seg == s->seg)
        return s->state == ST_WAIT_ACK ? TS(231, C_DATA_BG, TA_BOLD) : TS(C_DATA, C_DEFAULT, TA_BOLD);
    return TS(C_FAINT, C_DEFAULT, 0);
}

static void draw_transfer(const sender *s, int x, int y, int w, long long now)
{
    int ix = x + 2, iw = w - 4, lab = 12, bw, focus = 0, i;
    int done_segs = s->seg < s->data.nseg ? s->seg : s->data.nseg;
    tstyle dim = TS(C_DIM, C_DEFAULT, 0), txt = TS(C_TEXT, C_DEFAULT, 0);
    char segno[32], attempt[16];

    tui_box(x, y, w, 7, TS(C_BORDER, C_DEFAULT, 0), _("Transfer"));

    /* current bit, segment, attempts */
    if (s->state == ST_DONE)
        snprintf(segno, sizeof(segno), "—");
    else if (s->seg < s->data.nseg)
        snprintf(segno, sizeof(segno), "%d/%d", s->seg + 1, s->data.nseg);
    else
        snprintf(segno, sizeof(segno), "FIN");
    if (s->state == ST_WAIT_ACK)
        snprintf(attempt, sizeof(attempt), "%s%d", s->attempts > 1 ? T_WARN T_BOLD : "", s->attempts);
    else
        snprintf(attempt, sizeof(attempt), "—");
    tui_print(ix, y + 1, iw, txt,
              tm(_("{m}current bit{0}  %s{0}     {m}segment{0} %s     {m}attempt{0} %s")),
              s->state == ST_DONE ? T_CHIP_OFF " 0 " T_RESET " " T_CHIP_OFF " 1 "
              : s->bit == 0       ? T_CHIP_DATA " 0 " T_RESET " " T_CHIP_OFF " 1 "
                                  : T_CHIP_OFF " 0 " T_RESET " " T_CHIP_DATA " 1 ",
              segno, attempt);

    /* retransmission timer */
    bw = iw - lab - 16;
    tui_print(ix, y + 2, lab, dim, "timer");
    if (s->timer_on && s->state == ST_WAIT_ACK) {
        double el = (now - s->timer_start) / 1000.0, tot = s->opt.timeout_ms / 1000.0;
        double fr = el / tot;
        short col = fr < 0.5 ? C_OK : fr < 0.8 ? C_WARN : C_ERR;
        tui_bar(ix + lab, y + 2, bw, fr, col);
        tui_print(ix + lab + bw + 2, y + 2, 14, TS(col, C_DEFAULT, TA_BOLD), "%.1f / %.1f s", el > tot ? tot : el, tot);
    } else {
        tui_bar(ix + lab, y + 2, bw, 0.0, C_OK);
        tui_print(ix + lab + bw + 2, y + 2, 14, dim, "%s", _("■ stopped"));
    }

    /* confirmed segments */
    tui_print(ix, y + 3, lab, dim, "%s", _("confirmed"));
    tui_bar(ix + lab, y + 3, bw, s->data.nseg ? (double)done_segs / s->data.nseg : 0.0, C_ACK);
    tui_print(ix + lab + bw + 2, y + 3, 14, txt, "%d/%d", done_segs, s->data.nseg);

    /* text: confirmed (green), in flight (blue), still to send (grey) */
    tui_print(ix, y + 4, lab, dim, "%s", _("data"));
    for (i = 0; i < s->view.n; i++)
        if (s->view.g[i].seg == s->seg) {
            focus = i;
            break;
        }
    if (s->seg >= s->data.nseg)
        focus = s->view.n - 1;
    tui_draw_text(ix + lab, y + 4, iw - lab, 2, &s->view, focus, seg_style, s);
}

static void draw_stats(const sender *s, int x, int y, int w)
{
    int ix = x + 2, iw = w - 4;
    long long end = s->state == ST_DONE ? s->t_end : rdt_now_ms();
    int confirmed = s->seg;

    tui_box(x, y, w, 4, TS(C_BORDER, C_DEFAULT, 0), _("Statistics"));
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{d}{b}DATA{0}  {m}sent{0} %u  {m}retransmitted{0} %u  {t}timeouts{0} %u{m}  │  "
                   "{c}channel ▶{0} {m}lost{0} %u/%u  {m}corrupted{0} %u/%u")),
              s->tx, s->retx, s->timeouts, s->ch.n_lost, s->ch.n_sent, s->ch.n_corrupted, s->ch.n_sent);
    tui_print(ix, y + 2, iw, TS(C_TEXT, C_DEFAULT, 0),
              tm(_("{a}{b}ACK {0}  {g}✓{0} %u  {y}other bit{0} %u  {r}corrupted{0} %u  {m}out of "
                   "time{0} %u{m}  │  duration{0} %.1f s  {m}useful{0} %.0f%%")),
              s->ack_ok, s->ack_dup, s->ack_bad, s->ack_late,
              (end - s->t_begin) / 1000.0, s->tx ? 100.0 * confirmed / s->tx : 0.0);
}

static void render(sender *s, long long now)
{
    char l1[320], l2[320], note[32], foot[200];
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
    show_stats = H >= 19 + 4 + 5;
    show_fsm   = H >= 19 + 4 + 8 + 5;

    snprintf(l1, sizeof(l1),
             T_DATA "●" T_RESET " sender " T_BOLD "%s" T_RESET "   " T_DIM "──▶" T_RESET "   receiver " T_BOLD "%s",
             s->local_s, s->remote_s);
    snprintf(l2, sizeof(l2),
             _("timeout %.1f s · delay %.1f s · loss %.0f%% · corruption %.0f%% · %d×%d B · seed %u%s"),
             s->opt.timeout_ms / 1000.0, s->opt.delay_ms / 1000.0, s->opt.loss * 100, s->opt.corrupt * 100,
             s->data.nseg, s->opt.chunk, s->opt.seed,
             s->opt.timeout_ms <= 2 * s->opt.delay_ms ? tm(_("{y}  timeout ≤ RTT: premature timeouts!")) : "");
    tui_draw_header(y, W, PROTOCOL, ROLE_SENDER, l1, l2);
    y += 4;

    if (show_fsm) {
        draw_fsm(s, 0, y, W, now);
        y += 8;
    }
    draw_transfer(s, 0, y, W, now);
    y += 7;

    if (s->state == ST_DONE)
        snprintf(note, sizeof(note), "%s", tm(_("{g}✓ done")));
    else
        snprintf(note, sizeof(note), "bit " T_DATA T_BOLD "%d", s->bit);
    tui_draw_channel(0, y, W, &s->ch, ROLE_SENDER, now, note);
    y += 7;

    if (show_stats) {
        draw_stats(s, 0, y, W);
        y += 4;
    }
    tui_draw_log(0, y, W, H - 1 - y, _("Event log"));

    snprintf(foot, sizeof(foot), tm(_("{K} q {0} quit     stop-and-wait · sender · pid %d%s")),
             (int)getpid(), s->state == ST_DONE ? tm(_("{g}     ✓ transfer complete, press q to quit")) : "");
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
    ch_init(&s->ch, s->sock, TO_RECEIVER, s->opt.loss, s->opt.corrupt, s->opt.delay_ms);

    tui_init();
    s->state     = ST_WAIT_CALL;
    s->bit       = 0;
    s->edge      = EDGE_NONE;
    s->t_begin   = rdt_now_ms();
    s->next_send = s->t_begin + 800;
    s->loop_t    = s->edge_t = -HIGHLIGHT_MS;
    tui_log(tm(_("{i}● Sender ready{0}: %zu bytes in %d segments of at most %d bytes, to %s")),
            s->data.len, s->data.nseg, s->opt.chunk, s->remote_s);

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
        if (s->state == ST_WAIT_ACK && s->timer_on && now >= s->timer_deadline)
            on_timeout(s, now);
        if (s->state == ST_WAIT_CALL && now >= s->next_send)
            start_segment(s, now);
        ch_gc(&s->ch, now);
        render(s, now);

        if (s->state == ST_DONE && !tui_interactive())
            break;
        tui_wait(s->sock, 30);
    }
    tui_shutdown();

    if (s->state == ST_DONE)
        printf(_("Stop-and-wait: %d segments (%zu bytes) delivered in %.1f s · %u transmissions, %u "
                 "retransmissions, %u timeouts\n"), s->data.nseg, s->data.len,
               (s->t_end - s->t_begin) / 1000.0, s->tx, s->retx, s->timeouts);
    else
        printf(_("Stop-and-wait: interrupted after %d/%d segments confirmed\n"),
               s->seg < s->data.nseg ? s->seg : s->data.nseg, s->data.nseg);

    close(s->sock);
    rdt_free_data(&s->data);
    free(s->view.g);
    return EXIT_SUCCESS;
}
