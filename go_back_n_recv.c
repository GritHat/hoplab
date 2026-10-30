/*
 * go_back_n_recv.c - Receiver Go-Back-N su UDP.
 *
 * Il receiver non ha buffer: accetta solo il pacchetto con il numero di
 * sequenza atteso (expectedseqnum, modulo 2^k) e scarta tutto il resto.
 *   - pacchetto integro con seq = expectedseqnum
 *                          -> consegna i dati all'applicazione, invia ACK seq
 *                             e passa ad attendere seq+1
 *   - pacchetto fuori ordine o duplicato
 *                          -> scartato; rinvia l'ACK dell'ultimo pacchetto
 *                             ricevuto in ordine (riscontro cumulativo)
 *   - pacchetto corrotto   -> come sopra
 * Prima di aver ricevuto qualcosa in ordine l'"ultimo riscontrato" e' il
 * numero che precede 0, cioe' 2^k-1: il sender lo riconosce come vecchio.
 *
 * Uso: ./go_back_n_recv [opzioni] [host] [porta]     (-h per l'aiuto)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "channel.h"
#include "rdt.h"
#include "tui.h"

#define PROTOCOL "GO-BACK-N"
#define HIGHLIGHT_MS 900
#define MARK_MS 1500

static const rdt_protocol PROTO = {
    "go-back-N", NULL, RDT_OPT_SEQBITS,
    "Il sender deve usare lo stesso -b; la finestra del receiver Go-Back-N è sempre 1."
};

enum { RULE_NONE = -1, RULE_OK, RULE_OOO, RULE_BAD };

static const tui_rule RULES[] = {
    { T_DATA "◀", "DATA n integro, n = expectedseqnum", "consegna all'app, invia ACK n, expectedseqnum + 1" },
    { T_WARN "◀", "DATA n fuori ordine o duplicato",    "scartato (niente buffer); rinvia ACK dell'ultimo in ordine" },
    { T_ERR "✗",  "pacchetto corrotto",                 "scartato; rinvia ACK dell'ultimo in ordine" },
};

typedef struct {
    rdt_options opt;
    channel     ch;
    int         sock;
    struct sockaddr_in local, peer;
    int         has_peer;
    char        local_s[64], peer_s[64];
    int         M;                 /* numeri di sequenza 0..M-1 */

    /* stato del protocollo */
    int         expected;          /* expectedseqnum */
    int         delivered;         /* pacchetti consegnati = indice assoluto di expectedseqnum */
    int         done;              /* FIN consegnato */
    int         last_ack;          /* -1 = nessuno */

    /* dati consegnati all'applicazione */
    rdt_buffer  buf;
    int         nseg;
    tui_textbuf view;
    long long   last_deliver_t;

    /* cella su cui e' arrivato l'ultimo pacchetto scartato */
    long        mark_idx;
    long long   mark_t;

    /* statistiche */
    unsigned    rx, rx_new, rx_ooo, rx_bad, acks, sessions;
    long long   t_first, t_done, t_last_pkt;

    /* evidenziazioni grafiche */
    int         rule;
    long long   rule_t, render_now;
} receiver;

static receiver R;

/* ------------------------------------------------------------------ */
/* Protocollo                                                         */
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
    tui_log(T_ACK "▶ " T_BOLD "ACK %u" T_RESET " inviato " T_DIM "· %s", seq, why);
}

/* ACK cumulativo dell'ultimo pacchetto ricevuto in ordine */
static void resend_last_ack(receiver *r, const struct sockaddr_in *to)
{
    unsigned last = (unsigned)((r->expected + r->M - 1) % r->M);

    send_ack(r, last, to, r->delivered ? "ultimo ricevuto in ordine"
                                       : "nessuno ancora in ordine: il numero prima di 0");
}

static void handle_packet(receiver *r, const ch_flight *f, long long now)
{
    const rdt_packet *p = &f->pkt;
    char d[256], flash[48];
    unsigned seq;

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
        set_rule(r, RULE_BAD, now);
        tui_channel_flash(TO_RECEIVER, FLASH_ERR, "✗ checksum");
        tui_log(T_ERR "◀ pacchetto corrotto" T_RESET ": checksum errato " T_DIM "(si legge %s %u)" T_RESET
                " → scartato", rdt_type_name(p->type), (unsigned)p->seq);
        resend_last_ack(r, &f->from);
        return;
    }
    if (p->type != PKT_DATA && p->type != PKT_FIN) {
        tui_log(T_DIM "◀ %s %u inatteso dal sender, ignorato", rdt_type_name(p->type), (unsigned)p->seq);
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
            tui_log(T_DATA "◀ " T_RESET "%s ricevuto: " T_OK "✓ in ordine" T_RESET " → consegnato all'applicazione", d);
        } else {
            r->done = 1;
            r->t_done = now;
            r->last_deliver_t = now;
            tui_log(T_FIN "◀ " T_RESET "%s ricevuto: " T_OK "✓ in ordine" T_RESET " → fine dei dati", d);
        }
        send_ack(r, seq, &f->from, "riscontra tutto fino a questo pacchetto");
        r->expected = (r->expected + 1) % r->M;
        r->delivered++;
        if (r->done)
            tui_log(T_OK T_BOLD "✓ Trasferimento completato" T_RESET ": %d segmenti, %zu byte consegnati in %.1f s",
                    r->nseg, r->buf.len, (r->t_done - r->t_first) / 1000.0);
    } else {
        /* distanza dall'atteso: piccola = in anticipo, grande = gia' passato */
        unsigned dist = (seq + (unsigned)r->M - (unsigned)r->expected) % (unsigned)r->M;

        r->rx_ooo++;
        set_rule(r, RULE_OOO, now);
        r->mark_idx = dist < (unsigned)r->M / 2 ? r->delivered + (long)dist
                                                : r->delivered - (long)(r->M - dist);
        r->mark_t = now;
        snprintf(flash, sizeof(flash), "scarto %u", seq);
        tui_channel_flash(TO_RECEIVER, FLASH_WARN, flash);
        tui_log(T_WARN "◀ " T_RESET "%s " T_WARN "fuori ordine" T_RESET " (atteso seq %d) → scartato", d, r->expected);
        resend_last_ack(r, &f->from);
    }
}

/* ------------------------------------------------------------------ */
/* Interfaccia                                                        */
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
        { SC_DONE, "consegnati" }, { SC_EXPECT, "atteso (expectedseqnum)" },
        { SC_UNUSABLE, "fuori ordine: scartati se arrivano" },
    };
    char title[160];
    tui_seqstrip st;

    snprintf(title, sizeof(title), "Finestra del receiver · seq 0…%d (%d bit) · nessun buffer",
             r->M - 1, r->opt.seq_bits);
    memset(&st, 0, sizeof(st));
    st.modulo         = r->M;
    st.count          = r->done ? r->delivered : -1;
    st.win_lo         = r->delivered;
    st.win_n          = r->done ? 0 : 1;
    st.win_label      = "finestra 1";
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

    tui_box(x, y, w, 7, TS(C_BORDER, C_DEFAULT, 0), "Consegna all'applicazione");

    if (r->last_ack < 0)
        snprintf(ack, sizeof(ack), T_DIM "nessuno");
    else
        snprintf(ack, sizeof(ack), T_CHIP_ACK " ACK %d ", r->last_ack);
    if (r->done)
        snprintf(expect, sizeof(expect), T_OK "✓ FIN ricevuto");
    else
        snprintf(expect, sizeof(expect), T_CHIP_WARN " %d ", r->expected);
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              T_DIM "expectedseqnum" T_RESET " %s" T_RESET "     " T_DIM "consegnati" T_RESET " %d segment%s, %zu byte"
              "     " T_DIM "ultimo ACK inviato" T_RESET " %s",
              expect, r->nseg, r->nseg == 1 ? "o" : "i", r->buf.len, ack);

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
              T_DATA T_BOLD "DATA" T_RESET "  " T_DIM "ricevuti" T_RESET " %u  " T_OK "in ordine" T_RESET " %u  "
              T_WARN "fuori ordine (scartati)" T_RESET " %u  " T_ERR "corrotti" T_RESET " %u"
              T_DIM "  │  durata" T_RESET " %.1f s",
              r->rx, r->rx_new, r->rx_ooo, r->rx_bad, r->has_peer ? (end - r->t_first) / 1000.0 : 0.0);
    tui_print(ix, y + 2, iw, TS(C_TEXT, C_DEFAULT, 0),
              T_ACK T_BOLD "ACK " T_RESET "  " T_DIM "inviati" T_RESET " %u" T_DIM "  │  " T_CHAN "canale ◀" T_RESET " "
              T_DIM "persi" T_RESET " %u/%u  " T_DIM "corrotti" T_RESET " %u/%u",
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
                 T_ACK "●" T_RESET " in ascolto su " T_BOLD "%s" T_RESET "   " T_DIM "◀──" T_RESET "   sender " T_BOLD "%s",
                 r->local_s, r->peer_s);
    else
        snprintf(l1, sizeof(l1),
                 T_ACK "●" T_RESET " in ascolto su " T_BOLD "%s" T_RESET "   " T_DIM "in attesa del primo pacchetto…",
                 r->local_s);
    snprintf(l2, sizeof(l2), "seq 0…%d · ritardo %.1f s · perdita ACK %.0f%% · corruzione ACK %.0f%% · seed %u",
             r->M - 1, r->opt.delay_ms / 1000.0, r->opt.loss * 100, r->opt.corrupt * 100, r->opt.seed);
    tui_draw_header(y, W, PROTOCOL, ROLE_RECEIVER, l1, l2);
    y += 4;

    if (show_rules) {
        tui_draw_rules(0, y, W, "Regole del receiver Go-Back-N", RULES, 3,
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
        snprintf(note, sizeof(note), T_OK "✓ fine");
    else
        snprintf(note, sizeof(note), "atteso " T_ACK T_BOLD "%d", r->expected);
    tui_draw_channel(0, y, W, &r->ch, ROLE_RECEIVER, now, note);
    y += 7;

    if (show_stats) {
        draw_stats(r, 0, y, W);
        y += 4;
    }
    tui_draw_log(0, y, W, H - 1 - y, "Registro eventi");

    snprintf(foot, sizeof(foot), T_KEY " q " T_RESET " esci     go-back-N · receiver · pid %d%s",
             (int)getpid(), r->done ? T_OK "     ✓ completato · resta in ascolto" : "");
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
    r->M = 1 << r->opt.seq_bits;
    ch_init(&r->ch, r->sock, TO_SENDER, r->opt.loss, r->opt.corrupt, r->opt.delay_ms);
    reset_session(r);

    /* senza schermo (output rediretto) si termina un po' dopo il FIN, oppure
     * dopo un silenzio piu' lungo se il FIN non arriva */
    linger = 6LL * r->opt.delay_ms > 4000 ? 6LL * r->opt.delay_ms : 4000;

    tui_init();
    r->rule = RULE_NONE;
    r->rule_t = r->last_deliver_t = r->mark_t = -MARK_MS * 10LL;
    tui_log(T_INFO "● Receiver in ascolto" T_RESET " su %s · seq 0…%d · atteso seq 0", r->local_s, r->M - 1);

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
        printf("Go-back-N: ricevuti %d segmenti (%zu byte) da %s:\n", r->nseg, r->buf.len, r->peer_s);
        fwrite(r->buf.data, 1, r->buf.len, stdout);
        printf("\n");
    } else {
        printf("Go-back-N: interrotto dopo %d segmenti (%zu byte) consegnati\n", r->nseg, r->buf.len);
    }

    close(r->sock);
    free(r->buf.data);
    free(r->view.g);
    return EXIT_SUCCESS;
}
