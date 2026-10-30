/*
 * selective_repeat_recv.c - Receiver Selective Repeat su UDP.
 *
 * Il receiver ha una finestra di N posti [rcv_base, rcv_base+N-1] (numeri di
 * sequenza modulo 2^k) e riscontra ogni pacchetto singolarmente:
 *   - DATA n integro dentro la finestra
 *                          -> invia ACK n; se non l'aveva gia' lo mette nel
 *                             buffer; se n = rcv_base consegna all'applicazione
 *                             n e tutti i pacchetti consecutivi gia' nel buffer,
 *                             e la finestra scorre
 *   - DATA n integro in [rcv_base-N, rcv_base-1]
 *                          -> gia' consegnato, ma il sender non ha avuto l'ACK
 *                             (altrimenti non lo ritrasmetterebbe): rinvia ACK n
 *   - altrimenti, o pacchetto corrotto -> ignorato, senza ACK
 *
 * Uso: ./selective_repeat_recv [opzioni] [host] [porta]     (-h per l'aiuto)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "channel.h"
#include "rdt.h"
#include "tui.h"

#define PROTOCOL "SELECTIVE REPEAT"
#define HIGHLIGHT_MS 900
#define MARK_MS 1500

static const rdt_protocol PROTO = {
    "selective repeat", NULL, RDT_OPT_WINDOW | RDT_OPT_SEQBITS,
    "Il sender deve usare gli stessi -w e -b."
};

enum { RULE_NONE = -1, RULE_WINDOW, RULE_OLD, RULE_IGNORE };

static const tui_rule RULES[] = {
    { T_DATA "◀", "DATA n in [rcv_base, rcv_base+N−1]", "ACK n, nel buffer; se n = rcv_base consegna e scorre" },
    { T_WARN "◀", "DATA n in [rcv_base−N, rcv_base−1]", "già consegnato: rinvia ACK n (il precedente si è perso)" },
    { T_ERR "✗",  "corrotto o fuori da entrambe",       "ignorato, nessun ACK: il sender ritrasmetterà al timeout" },
};

typedef struct {
    rdt_options opt;
    channel     ch;
    int         sock;
    struct sockaddr_in local, peer;
    int         has_peer;
    char        local_s[64], peer_s[64];
    int         M, N;              /* numeri di sequenza 0..M-1, finestra N */

    /* stato del protocollo */
    int         rcv_base;          /* il prossimo da consegnare (mod M) */
    int         delivered;         /* pacchetti consegnati = indice assoluto di rcv_base */
    int         done;              /* FIN consegnato */
    unsigned char *have;           /* [M] pacchetto nel buffer, per numero di sequenza */
    rdt_packet *slot;              /* [M] */
    int         nbuf;
    int         last_ack;          /* -1 = nessuno */

    /* dati consegnati all'applicazione */
    rdt_buffer  buf;
    int         nseg, batch_first; /* segmenti consegnati; primo dell'ultima consegna */
    tui_textbuf view;
    long long   last_deliver_t;

    /* cella su cui e' arrivato l'ultimo pacchetto gia' consegnato */
    long        mark_idx;
    long long   mark_t;

    /* statistiche */
    unsigned    rx, rx_new, rx_buf, rx_dup, rx_old, rx_ign, rx_bad, acks, sessions;
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
    tui_log(T_ACK "▶ " T_BOLD "ACK %u" T_RESET " inviato " T_DIM "· %s", seq, why);
}

/* Consegna rcv_base e i successivi gia' nel buffer; ritorna quanti */
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
        set_rule(r, RULE_IGNORE, now);
        tui_channel_flash(TO_RECEIVER, FLASH_ERR, "✗ checksum");
        tui_log(T_ERR "◀ pacchetto corrotto" T_RESET ": checksum errato " T_DIM "(si legge %s %u)" T_RESET
                " → scartato, nessun ACK " T_DIM "(il sender ritrasmetterà al timeout)",
                rdt_type_name(p->type), (unsigned)p->seq);
        return;
    }
    if (p->type != PKT_DATA && p->type != PKT_FIN) {
        tui_log(T_DIM "◀ %s %u inatteso dal sender, ignorato", rdt_type_name(p->type), (unsigned)p->seq);
        return;
    }

    tui_pkt_name(p, d, sizeof(d));
    seq  = p->seq % (unsigned)r->M;
    dist = (seq + (unsigned)r->M - (unsigned)r->rcv_base) % (unsigned)r->M;

    if (!r->done && dist < (unsigned)r->N) {
        /* dentro la finestra */
        set_rule(r, RULE_WINDOW, now);
        if (r->have[seq]) {
            r->rx_dup++;
            snprintf(flash, sizeof(flash), "dup %u", seq);
            tui_channel_flash(TO_RECEIVER, FLASH_WARN, flash);
            tui_log(T_WARN "◀ " T_RESET "%s " T_WARN "duplicato" T_RESET ": è già nel buffer → solo ACK", d);
            send_ack(r, seq, &f->from, "ripetuto, il precedente non è arrivato");
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
                tui_log(T_DATA "◀ " T_RESET "%s ricevuto: " T_OK "✓ è rcv_base" T_RESET " → %s, rcv_base = seq %d",
                        d, r->done ? "fine dei dati" : "consegnato", r->rcv_base);
            else
                tui_log(T_DATA "◀ " T_RESET "%s ricevuto: " T_OK "✓ è rcv_base" T_RESET
                        " → consegnati %d pacchetti (seq %d…%d, %d dal buffer), rcv_base = seq %d",
                        d, k, first % r->M, (first + k - 1) % r->M, k - 1, r->rcv_base);
        } else {
            r->rx_buf++;
            snprintf(flash, sizeof(flash), "buffer %u", seq);
            tui_channel_flash(TO_RECEIVER, FLASH_OK, flash);
            tui_log(T_DATA "◀ " T_RESET "%s ricevuto " T_INFO "fuori ordine" T_RESET " → nel buffer "
                    T_DIM "(manca seq %d, rcv_base)", d, r->rcv_base);
        }
        send_ack(r, seq, &f->from, "riscontro di questo pacchetto");
        if (r->done)
            tui_log(T_OK T_BOLD "✓ Trasferimento completato" T_RESET ": %d segmenti, %zu byte consegnati in %.1f s",
                    r->nseg, r->buf.len, (r->t_done - r->t_first) / 1000.0);
    } else if (dist >= (unsigned)(r->M - r->N)) {
        /* nella finestra precedente: gia' consegnato */
        r->rx_old++;
        set_rule(r, RULE_OLD, now);
        r->mark_idx = r->delivered - (long)(r->M - dist);
        r->mark_t = now;
        snprintf(flash, sizeof(flash), "già %u", seq);
        tui_channel_flash(TO_RECEIVER, FLASH_WARN, flash);
        tui_log(T_WARN "◀ " T_RESET "%s " T_WARN "già consegnato" T_RESET
                ": il sender non ha avuto l'ACK → lo rinvio, senza riconsegnare", d);
        send_ack(r, seq, &f->from, "ripetuto per un pacchetto già consegnato");
    } else {
        r->rx_ign++;
        set_rule(r, RULE_IGNORE, now);
        tui_channel_flash(TO_RECEIVER, FLASH_WARN, "ignorato");
        tui_log(T_DIM "◀ %s fuori da entrambe le finestre (rcv_base = seq %d) → ignorato", d, r->rcv_base);
    }
}

/* ------------------------------------------------------------------ */
/* Interfaccia                                                        */
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
        { SC_DONE, "consegnati" }, { SC_ACKED, "nel buffer (riscontrati)" }, { SC_EXPECT, "atteso" },
        { SC_USABLE, "accettabili" }, { SC_UNUSABLE, "non utilizzabili" },
    };
    char title[160], wl[32];
    tui_seqstrip st;

    snprintf(title, sizeof(title), "Finestra del receiver · N = %d · seq 0…%d (%d bit) · buffer %d/%d",
             r->N, r->M - 1, r->opt.seq_bits, r->nbuf, r->N);
    snprintf(wl, sizeof(wl), "finestra N = %d", r->N);
    memset(&st, 0, sizeof(st));
    st.modulo        = r->M;
    st.count         = r->done ? r->delivered : -1;
    st.win_lo        = r->delivered;
    st.win_n         = r->done ? 0 : r->N;
    st.win_label     = wl;
    st.shadow_lo     = r->delivered - r->N;
    st.shadow_n      = r->N;
    st.shadow_label  = "rinvio ACK";
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

    tui_box(x, y, w, 7, TS(C_BORDER, C_DEFAULT, 0), "Consegna all'applicazione");

    if (r->last_ack < 0)
        snprintf(ack, sizeof(ack), T_DIM "nessuno");
    else
        snprintf(ack, sizeof(ack), T_CHIP_ACK " ACK %d ", r->last_ack);
    if (r->done)
        snprintf(base, sizeof(base), T_OK "✓ FIN ricevuto");
    else
        snprintf(base, sizeof(base), T_CHIP_WARN " %d ", r->rcv_base);
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              T_DIM "rcv_base" T_RESET " %s" T_RESET "    " T_DIM "consegnati" T_RESET " %d segment%s, %zu byte"
              "    " T_DIM "nel buffer" T_RESET " %d    " T_DIM "ultimo ACK" T_RESET " %s",
              base, r->nseg, r->nseg == 1 ? "o" : "i", r->buf.len, r->nbuf, ack);

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
              T_INFO "nel buffer" T_RESET " %u  " T_WARN "duplicati" T_RESET " %u  " T_WARN "già consegnati" T_RESET " %u  "
              T_ERR "corrotti" T_RESET " %u  " T_DIM "ignorati" T_RESET " %u",
              r->rx, r->rx_new, r->rx_buf, r->rx_dup, r->rx_old, r->rx_bad, r->rx_ign);
    tui_print(ix, y + 2, iw, TS(C_TEXT, C_DEFAULT, 0),
              T_ACK T_BOLD "ACK " T_RESET "  " T_DIM "inviati" T_RESET " %u" T_DIM "  │  " T_CHAN "canale ◀" T_RESET " "
              T_DIM "persi" T_RESET " %u/%u  " T_DIM "corrotti" T_RESET " %u/%u" T_DIM "  │  durata" T_RESET " %.1f s",
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
                 T_ACK "●" T_RESET " in ascolto su " T_BOLD "%s" T_RESET "   " T_DIM "◀──" T_RESET "   sender " T_BOLD "%s" T_RESET "%s",
                 r->local_s, r->peer_s, r->N > r->M / 2 ? T_WARN "   N > 2^(k−1): vecchi e nuovi si confondono!" : "");
    else
        snprintf(l1, sizeof(l1),
                 T_ACK "●" T_RESET " in ascolto su " T_BOLD "%s" T_RESET "   " T_DIM "in attesa del primo pacchetto…",
                 r->local_s);
    snprintf(l2, sizeof(l2), "ritardo %.1f s · perdita ACK %.0f%% · corruzione ACK %.0f%% · seed %u",
             r->opt.delay_ms / 1000.0, r->opt.loss * 100, r->opt.corrupt * 100, r->opt.seed);
    tui_draw_header(y, W, PROTOCOL, ROLE_RECEIVER, l1, l2);
    y += 4;

    if (show_rules) {
        tui_draw_rules(0, y, W, "Regole del receiver Selective Repeat", RULES, 3,
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
        snprintf(note, sizeof(note), "base " T_ACK T_BOLD "%d", r->rcv_base);
    tui_draw_channel(0, y, W, &r->ch, ROLE_RECEIVER, now, note);
    y += 7;

    if (show_stats) {
        draw_stats(r, 0, y, W);
        y += 4;
    }
    tui_draw_log(0, y, W, H - 1 - y, "Registro eventi");

    snprintf(foot, sizeof(foot), T_KEY " q " T_RESET " esci     selective repeat · receiver · pid %d%s",
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
    r->M    = 1 << r->opt.seq_bits;
    r->N    = r->opt.window;
    r->have = calloc((size_t)r->M, 1);
    r->slot = calloc((size_t)r->M, sizeof(*r->slot));
    if (!r->have || !r->slot) {
        fprintf(stderr, "memoria esaurita\n");
        return EXIT_FAILURE;
    }
    ch_init(&r->ch, r->sock, TO_SENDER, r->opt.loss, r->opt.corrupt, r->opt.delay_ms);
    reset_session(r);

    /* senza schermo (output rediretto) si termina un po' dopo il FIN, oppure
     * dopo un silenzio piu' lungo se il FIN non arriva */
    linger = 6LL * r->opt.delay_ms > 4000 ? 6LL * r->opt.delay_ms : 4000;

    tui_init();
    r->rule = RULE_NONE;
    r->rule_t = r->last_deliver_t = r->mark_t = -MARK_MS * 10LL;
    tui_log(T_INFO "● Receiver in ascolto" T_RESET " su %s · finestra N = %d · seq 0…%d",
            r->local_s, r->N, r->M - 1);
    if (r->N > r->M / 2)
        tui_log(T_WARN "N = %d con %d numeri di sequenza: un pacchetto ritrasmesso può cadere nella "
                "finestra nuova ed essere consegnato due volte (serve N ≤ 2^(k−1))", r->N, r->M);

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
        printf("Selective repeat: ricevuti %d segmenti (%zu byte) da %s:\n", r->nseg, r->buf.len, r->peer_s);
        fwrite(r->buf.data, 1, r->buf.len, stdout);
        printf("\n");
    } else {
        printf("Selective repeat: interrotto dopo %d segmenti (%zu byte) consegnati\n", r->nseg, r->buf.len);
    }

    close(r->sock);
    free(r->buf.data);
    free(r->have);
    free(r->slot);
    free(r->view.g);
    return EXIT_SUCCESS;
}
