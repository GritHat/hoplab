/*
 * stop_and_wait_recv.c - Receiver Stop-and-Wait (protocollo a bit alternato,
 *                        rdt 3.0) su UDP.
 *
 * Il receiver si aspetta un bit di sequenza alla volta (prima 0, poi 1, ...):
 *   - pacchetto integro con il bit atteso -> consegna i dati all'applicazione,
 *                                            invia ACK con lo stesso bit e
 *                                            passa ad attendere l'altro bit
 *   - pacchetto integro con l'altro bit   -> e' un duplicato (il sender non ha
 *                                            ricevuto il nostro ACK): non lo
 *                                            consegna di nuovo, ma ritrasmette
 *                                            l'ACK per quel bit
 *   - pacchetto corrotto                  -> invia l'ACK dell'ultimo bit ricevuto
 *                                            correttamente: al sender arriva un
 *                                            bit diverso da quello inviato e
 *                                            capisce di dover ritrasmettere
 *
 * Uso: ./stop_and_wait_recv [opzioni] [host] [porta]     (-h per l'aiuto)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "channel.h"
#include "rdt.h"
#include "tui.h"

#define PROTOCOL "STOP-AND-WAIT"
#define HIGHLIGHT_MS 900

static const rdt_protocol PROTO = { "stop-and-wait", NULL, 0, NULL };

/* Transizioni della macchina a stati (per evidenziare l'ultima percorsa) */
enum { EDGE_NONE = -1, EDGE_RECV0, EDGE_RECV1 };

typedef struct {
    rdt_options opt;
    channel     ch;
    int         sock;
    struct sockaddr_in local, peer;
    int         has_peer;
    char        local_s[64], peer_s[64];

    /* stato del protocollo */
    int         expected;          /* bit di sequenza atteso */
    int         done;              /* ricevuto il FIN */
    int         last_ack;          /* -1 = nessuno */

    /* dati consegnati all'applicazione */
    rdt_buffer  buf;
    int         nseg;
    tui_textbuf view;
    long long   last_deliver_t;

    /* statistiche */
    unsigned    rx, rx_new, rx_dup, rx_bad, acks, sessions;
    long long   t_first, t_done, t_last_pkt;

    /* evidenziazioni grafiche */
    int         edge;
    long long   edge_t, loop_t, render_now;
} receiver;

static receiver R;

/* ------------------------------------------------------------------ */
/* Protocollo                                                         */
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
    tui_log(T_ACK "▶ " T_BOLD "ACK %u" T_RESET " inviato " T_DIM "· %s", bit, why);
}

static void handle_packet(receiver *r, const ch_flight *f, long long now)
{
    const rdt_packet *p = &f->pkt;
    char d[256], flash[48];

    if (!r->has_peer || !rdt_addr_equal(&f->from, &r->peer)) {
        if (r->has_peer) {
            reset_session(r);
            tui_log_rule("nuova sessione");
        }
        r->peer = f->from;
        r->has_peer = 1;
        r->sessions++;
        r->t_first = now;
        rdt_addr_str(&r->peer, r->peer_s, sizeof(r->peer_s));
        tui_log(T_INFO "● Primo pacchetto dal sender %s", r->peer_s);
    }
    r->rx++;
    r->t_last_pkt = now;

    if (!f->valid) {
        r->rx_bad++;
        r->loop_t = now;
        tui_channel_flash(TO_RECEIVER, FLASH_ERR, "✗ checksum");
        tui_log(T_ERR "◀ pacchetto corrotto" T_RESET ": checksum errato " T_DIM "(si legge %s %u)" T_RESET
                " → scartato", rdt_type_name(p->type), (unsigned)p->seq);
        send_ack(r, (unsigned)(r->expected ^ 1),
                 &f->from, "bit dell'ultimo pacchetto corretto: il sender ritrasmetterà");
        return;
    }
    if (p->type != PKT_DATA && p->type != PKT_FIN) {
        tui_log(T_DIM "◀ %s %u inatteso dal sender, ignorato", rdt_type_name(p->type), (unsigned)p->seq);
        return;
    }

    tui_pkt_name(p, d, sizeof(d));
    if (p->seq == (uint32_t)r->expected) {
        r->rx_new++;
        snprintf(flash, sizeof(flash), "✓ %s %u", rdt_type_name(p->type), (unsigned)p->seq);
        tui_channel_flash(TO_RECEIVER, FLASH_OK, flash);
        if (p->type == PKT_DATA) {
            deliver(r, p, now);
            tui_log(T_DATA "◀ " T_RESET "%s ricevuto: " T_OK "✓ bit atteso" T_RESET " → consegnato all'applicazione", d);
        } else {
            r->done = 1;
            r->t_done = now;
            tui_log(T_FIN "◀ " T_RESET "%s ricevuto: " T_OK "✓ bit atteso" T_RESET " → fine dei dati", d);
        }
        r->edge = r->expected == 0 ? EDGE_RECV0 : EDGE_RECV1;
        r->edge_t = now;
        send_ack(r, p->seq, &f->from, "stesso bit del pacchetto ricevuto");
        r->expected ^= 1;
        if (r->done)
            tui_log(T_OK T_BOLD "✓ Trasferimento completato" T_RESET ": %d segmenti, %zu byte consegnati in %.1f s",
                    r->nseg, r->buf.len, (r->t_done - r->t_first) / 1000.0);
    } else {
        r->rx_dup++;
        r->loop_t = now;
        snprintf(flash, sizeof(flash), "dup %s %u", rdt_type_name(p->type), (unsigned)p->seq);
        tui_channel_flash(TO_RECEIVER, FLASH_WARN, flash);
        tui_log(T_WARN "◀ " T_RESET "%s " T_WARN "duplicato" T_RESET " (atteso bit %d): già consegnato, "
                "il mio ACK non è arrivato integro → non lo riconsegno", d, r->expected);
        send_ack(r, p->seq, &f->from, "ritrasmissione del riscontro");
    }
}

/* ------------------------------------------------------------------ */
/* Interfaccia                                                        */
/* ------------------------------------------------------------------ */

static tstyle edge_style(const receiver *r, int edge, long long now)
{
    if (r->edge == edge && now - r->edge_t < HIGHLIGHT_MS)
        return TS(C_INFO, C_DEFAULT, TA_BOLD);
    return TS(C_FAINT, C_DEFAULT, 0);
}

/*
 *              ╭────── DATA 0 ok · consegna · ACK 0 ──────╮
 *              │                                           ▼
 *  ↻ ▐ ATTESA DATI 0 ▌                           ▐ ATTESA DATI 1 ▌ ↻
 *              ▲                                           │
 *              ╰────── DATA 1 ok · consegna · ACK 1 ──────╯
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

    tui_box(x, y, w, 8, TS(C_BORDER, C_DEFAULT, 0), "Macchina a stati del receiver (rdt 3.0)");

    /* attesa 0 -> attesa 1 (sopra) */
    tui_put(cl, r0, e0, "╭");
    tui_hline(cl + 1, r0, cr - cl - 1, e0, "─");
    tui_put(cr, r0, e0, "╮");
    tui_put(cl, r0 + 1, e0, "│");
    tui_put(cr, r0 + 1, e0, "▼");
    tui_print_center(cl + 1, r0, cr - cl - 1, e0, " DATA 0 ok · consegna · ACK 0 ");

    /* attesa 1 -> attesa 0 (sotto) */
    tui_put(cr, r2 + 1, e1, "│");
    tui_put(cl, r2 + 1, e1, "▲");
    tui_put(cl, r4, e1, "╰");
    tui_hline(cl + 1, r4, cr - cl - 1, e1, "─");
    tui_put(cr, r4, e1, "╯");
    tui_print_center(cl + 1, r4, cr - cl - 1, e1, " DATA 1 ok · consegna · ACK 1 ");

    tui_pill(lx, r2, P, "ATTESA DATI 0", r->expected == 0, C_ACK);
    tui_pill(rx, r2, P, "ATTESA DATI 1", r->expected == 1, C_ACK);
    tui_put(lx - 2, r2, looping && r->expected == 0 ? loop_on : loop_off, "↻");
    tui_put(rx + P + 1, r2, looping && r->expected == 1 ? loop_on : loop_off, "↻");

    if (r->done)
        tui_print_center(lx + P + 1, r2, gap - 2, TS(C_OK, C_DEFAULT, TA_BOLD), "✓ FIN ricevuto");
    else
        tui_print_center(lx + P + 1, r2, gap - 2, TS(C_TEXT, C_DEFAULT, 0),
                         "atteso bit " T_ACK T_BOLD "%d", r->expected);

    tui_print(ix, y + 6, iw, TS(C_DIM, C_DEFAULT, 0),
              "↻ pacchetto corrotto o duplicato  ⇒  rinvia l'ultimo ACK (il bit dell'altro stato)");
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

    tui_box(x, y, w, 7, TS(C_BORDER, C_DEFAULT, 0), "Consegna all'applicazione");

    if (r->last_ack < 0)
        snprintf(ack, sizeof(ack), T_DIM "nessuno");
    else
        snprintf(ack, sizeof(ack), T_CHIP_ACK " ACK %d ", r->last_ack);
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              T_DIM "bit atteso" T_RESET "    %s" T_RESET "     " T_DIM "consegnati" T_RESET " %d segment%s, %zu byte"
              "     " T_DIM "ultimo ACK inviato" T_RESET " %s",
              r->expected == 0 ? T_CHIP_ACK " 0 " T_RESET " " T_CHIP_OFF " 1 "
                               : T_CHIP_OFF " 0 " T_RESET " " T_CHIP_ACK " 1 ",
              r->nseg, r->nseg == 1 ? "o" : "i", r->buf.len, ack);

    tui_print(ix, y + 2, lab, TS(C_DIM, C_DEFAULT, 0), "dati");
    if (r->view.n == 0)
        tui_print(ix + lab, y + 2, iw - lab, TS(C_FAINT, C_DEFAULT, 0), "(ancora nulla)");
    else
        tui_draw_text(ix + lab, y + 2, iw - lab, 4, &r->view, r->view.n - 1, seg_style, r);
}

static void draw_stats(const receiver *r, int x, int y, int w)
{
    int ix = x + 2, iw = w - 4;
    long long end = r->done ? r->t_done : r->has_peer ? rdt_now_ms() : r->t_first;

    tui_box(x, y, w, 4, TS(C_BORDER, C_DEFAULT, 0), "Statistiche");
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              T_DATA T_BOLD "DATA" T_RESET "  " T_DIM "ricevuti" T_RESET " %u  " T_OK "nuovi" T_RESET " %u  "
              T_WARN "duplicati" T_RESET " %u  " T_ERR "corrotti" T_RESET " %u" T_DIM "  │  durata" T_RESET " %.1f s",
              r->rx, r->rx_new, r->rx_dup, r->rx_bad, r->has_peer ? (end - r->t_first) / 1000.0 : 0.0);
    tui_print(ix, y + 2, iw, TS(C_TEXT, C_DEFAULT, 0),
              T_ACK T_BOLD "ACK " T_RESET "  " T_DIM "inviati" T_RESET " %u" T_DIM "  │  " T_CHAN "canale ◀" T_RESET " "
              T_DIM "persi" T_RESET " %u/%u  " T_DIM "corrotti" T_RESET " %u/%u",
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
                 T_ACK "●" T_RESET " in ascolto su " T_BOLD "%s" T_RESET "   " T_DIM "◀──" T_RESET "   sender " T_BOLD "%s",
                 r->local_s, r->peer_s);
    else
        snprintf(l1, sizeof(l1),
                 T_ACK "●" T_RESET " in ascolto su " T_BOLD "%s" T_RESET "   " T_DIM "in attesa del primo pacchetto…",
                 r->local_s);
    snprintf(l2, sizeof(l2), "ritardo %.1f s · perdita ACK %.0f%% · corruzione ACK %.0f%% · seed %u",
             r->opt.delay_ms / 1000.0, r->opt.loss * 100, r->opt.corrupt * 100, r->opt.seed);
    tui_draw_header(y, W, PROTOCOL, ROLE_RECEIVER, l1, l2);
    y += 4;

    if (show_fsm) {
        draw_fsm(r, 0, y, W, now);
        y += 8;
    }
    draw_delivery(r, 0, y, W);
    y += 7;

    snprintf(note, sizeof(note), "atteso " T_ACK T_BOLD "%d", r->expected);
    tui_draw_channel(0, y, W, &r->ch, ROLE_RECEIVER, now, note);
    y += 7;

    if (show_stats) {
        draw_stats(r, 0, y, W);
        y += 4;
    }
    tui_draw_log(0, y, W, H - 1 - y, "Registro eventi");

    snprintf(foot, sizeof(foot), T_KEY " q " T_RESET " esci     stop-and-wait · receiver · pid %d%s",
             (int)getpid(), r->done ? T_OK "     ✓ completato · in ascolto per FIN duplicati" : "");
    tui_draw_footer(H - 1, W, foot);
    tui_frame_end();
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    receiver *r = &R;
    int quit = 0;
    long long linger;

    rdt_parse_options(argc, argv, ROLE_RECEIVER, &PROTO, &r->opt);
    rdt_srand(r->opt.seed);
    r->sock = rdt_open_receiver(&r->opt, &r->local, r->local_s, sizeof(r->local_s));
    ch_init(&r->ch, r->sock, TO_SENDER, r->opt.loss, r->opt.corrupt, r->opt.delay_ms);
    reset_session(r);

    /* senza schermo (output rediretto) si termina un po' dopo il FIN, oppure
     * dopo un silenzio piu' lungo se il FIN non arriva */
    linger = 6LL * r->opt.delay_ms > 4000 ? 6LL * r->opt.delay_ms : 4000;

    tui_init();
    r->edge = EDGE_NONE;
    r->loop_t = r->edge_t = r->last_deliver_t = -HIGHLIGHT_MS * 10LL;
    tui_log(T_INFO "● Receiver in ascolto" T_RESET " su %s · atteso bit 0", r->local_s);

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
        printf("Stop-and-wait: ricevuti %d segmenti (%zu byte) da %s:\n", r->nseg, r->buf.len, r->peer_s);
        fwrite(r->buf.data, 1, r->buf.len, stdout);
        printf("\n");
    } else {
        printf("Stop-and-wait: interrotto dopo %d segmenti (%zu byte) consegnati\n", r->nseg, r->buf.len);
    }

    close(r->sock);
    free(r->buf.data);
    free(r->view.g);
    return EXIT_SUCCESS;
}
