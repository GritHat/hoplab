/*
 * selective_repeat_send.c - Sender Selective Repeat su UDP.
 *
 * Come in Go-Back-N il sender puo' avere fino a N pacchetti in volo (la
 * finestra [send_base, send_base+N-1], numeri di sequenza modulo 2^k), ma
 * ogni pacchetto ha il SUO timer e il receiver riscontra i pacchetti uno per
 * uno, anche fuori ordine:
 *   - dati dall'applicazione e nextseqnum < send_base+N
 *                          -> invia il pacchetto nextseqnum e ne avvia il timer
 *   - ACK n nella finestra -> segna n come riscontrato e ne ferma il timer; se
 *                             n = send_base la finestra scorre fino al primo
 *                             pacchetto non ancora riscontrato
 *   - timeout del pacchetto n
 *                          -> ritrasmette SOLO n e ne riavvia il timer
 *   - ACK corrotto, duplicato o fuori finestra -> ignorato
 * Alla fine un FIN, numerato come i dati, chiude il trasferimento.
 *
 * Perche' il receiver non scambi un pacchetto ritrasmesso per uno nuovo serve
 * N <= 2^(k-1): con una finestra piu' grande l'intestazione lo segnala.
 *
 * Uso: ./selective_repeat_send [opzioni] [host] [porta]     (-h per l'aiuto)
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

static const rdt_protocol PROTO = {
    "selective repeat",
    "Selective repeat: ogni pacchetto ha il suo timer, il receiver tiene da parte "
    "quelli fuori ordine e si ritrasmette solo ciò che è andato perso.",
    RDT_OPT_WINDOW | RDT_OPT_SEQBITS | RDT_OPT_INTERVAL,
    "Il receiver deve usare gli stessi -w e -b."
};

enum { RULE_NONE = -1, RULE_SEND, RULE_ACK, RULE_TIMEOUT, RULE_IGNORE };

static const tui_rule RULES[] = {
    { T_DATA "▶", "dati dall'app, nextseqnum < send_base+N", "invia DATA nextseqnum e avvia il suo timer" },
    { T_ACK "◀",  "ACK n nella finestra",                    "ferma il timer di n; se n = send_base, scorre" },
    { T_TIME "◷", "timeout del pacchetto n",                 "ritrasmette solo n e ne riavvia il timer" },
    { T_ERR "✗",  "ACK corrotto, duplicato o fuori finestra", "ignorato" },
};

typedef struct {
    rdt_options opt;
    channel     ch;
    int         sock;
    struct sockaddr_in dst;
    char        local_s[64], remote_s[64];

    rdt_data    data;              /* segmenti 0..nseg-1; il FIN e' il pacchetto nseg */
    tui_textbuf view;
    int         total;             /* pacchetti da consegnare, FIN compreso */
    int         M, N;              /* numeri di sequenza 0..M-1, finestra N */

    /* stato del protocollo: indici assoluti dei pacchetti, seq = indice mod M */
    int         base;              /* send_base: il piu' vecchio non riscontrato */
    int         nextseq;           /* il prossimo mai inviato */
    long long   next_tx_time;      /* prossimo invio non prima di (ms) */
    unsigned char *acked;          /* per pacchetto: riscontrato */
    unsigned char *timer_on;       /* per pacchetto: timer attivo */
    unsigned char *pending;        /* per pacchetto: timer scaduto, da ritrasmettere */
    long long  *timer_start;
    int        *tx_count;
    int         done;
    long long   t_begin, t_end;

    /* statistiche */
    unsigned    tx, retx, timeouts, ack_new, ack_dup, ack_out, ack_bad, ack_late;

    /* evidenziazioni grafiche */
    int         rule;
    long long   rule_t, render_now;
} sender;

static sender S;

/* ------------------------------------------------------------------ */
/* Protocollo                                                         */
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

/* udt_send del pacchetto idx e avvio del suo timer */
static void transmit(sender *s, int idx, long long now)
{
    rdt_packet p;
    char d[256];

    rdt_make_packet(&s->data, idx, (uint32_t)seq_of(s, idx), &p);
    ch_send(&s->ch, &p, &s->dst);
    s->tx_count[idx]++;
    s->tx++;
    s->timer_on[idx]    = 1;
    s->timer_start[idx] = now;
    s->pending[idx]     = 0;
    tui_pkt_name(&p, d, sizeof(d));

    if (idx == s->nextseq) {
        s->nextseq++;
        set_rule(s, RULE_SEND, now);
        tui_log(T_DATA "▶ " T_RESET "%s inviato " T_DIM "· suo timer avviato · finestra %d/%d",
                d, s->nextseq - s->base, s->N);
    } else {
        s->retx++;
        tui_log(T_WARN "↻ " T_RESET "%s ritrasmesso " T_DIM "· tentativo %d · suo timer riavviato",
                d, s->tx_count[idx]);
    }
}

/* Scadenza dei timer: il pacchetto va in coda per la ritrasmissione */
static void check_timers(sender *s, long long now)
{
    int i;

    for (i = s->base; i < s->nextseq; i++)
        if (s->timer_on[i] && now - s->timer_start[i] >= s->opt.timeout_ms) {
            s->timer_on[i] = 0;
            s->pending[i]  = 1;
            s->timeouts++;
            set_rule(s, RULE_TIMEOUT, now);
            tui_log(T_TIME "◷ TIMEOUT" T_RESET " di " T_BOLD "seq %d" T_RESET ": nessun ACK entro %.1f s "
                    "→ ritrasmetto solo questo pacchetto", seq_of(s, i), s->opt.timeout_ms / 1000.0);
        }
}

/* Invia al massimo un pacchetto ogni interval_ms: prima le ritrasmissioni */
static void pump(sender *s, long long now)
{
    while (!s->done && now >= s->next_tx_time) {
        int i, idx = -1;

        for (i = s->base; i < s->nextseq && idx < 0; i++)
            if (s->pending[i])
                idx = i;
        if (idx < 0 && s->nextseq < s->base + s->N && s->nextseq < s->total)
            idx = s->nextseq;
        if (idx < 0)
            break;
        transmit(s, idx, now);
        s->next_tx_time = now + s->opt.interval_ms;
    }
}

static void handle_ack(sender *s, const ch_flight *f, long long now)
{
    const rdt_packet *a = &f->pkt;
    unsigned seq;
    int d;
    char flash[48];

    if (!rdt_addr_equal(&f->from, &s->dst)) {
        char from[64];
        rdt_addr_str(&f->from, from, sizeof(from));
        tui_log(T_DIM "◀ pacchetto da %s ignorato: non è il receiver", from);
        return;
    }
    if (s->done) {
        s->ack_late++;
        tui_log(T_DIM "◀ %s %u fuori tempo: trasferimento già concluso, ignorato",
                f->valid ? rdt_type_name(a->type) : "pacchetto", (unsigned)a->seq);
        tui_channel_flash(TO_SENDER, FLASH_WARN, "ignorato");
        return;
    }
    if (!f->valid || a->type != PKT_ACK) {
        s->ack_bad++;
        set_rule(s, RULE_IGNORE, now);
        tui_log(T_ERR "◀ ACK corrotto" T_RESET ": checksum errato → ignorato "
                T_DIM "(il timer del pacchetto farà il resto)");
        tui_channel_flash(TO_SENDER, FLASH_ERR, "✗ checksum");
        return;
    }

    seq = a->seq % (unsigned)s->M;
    d = (int)((seq + (unsigned)s->M - (unsigned)seq_of(s, s->base)) % (unsigned)s->M);

    if (d >= s->nextseq - s->base) {
        s->ack_out++;
        set_rule(s, RULE_IGNORE, now);
        snprintf(flash, sizeof(flash), "ACK %u vecchio", seq);
        tui_channel_flash(TO_SENDER, FLASH_WARN, flash);
        tui_log(T_ACK "◀ ACK %u" T_RESET " " T_WARN "fuori finestra" T_RESET ": riscontra un pacchetto già "
                "uscito dalla finestra " T_DIM "(send_base = seq %d)" T_RESET " → ignorato", seq, seq_of(s, s->base));
        return;
    }

    {
        int i = s->base + d;

        if (s->acked[i]) {
            s->ack_dup++;
            set_rule(s, RULE_IGNORE, now);
            snprintf(flash, sizeof(flash), "dup ACK %u", seq);
            tui_channel_flash(TO_SENDER, FLASH_WARN, flash);
            tui_log(T_ACK "◀ ACK %u" T_RESET " " T_WARN "duplicato" T_RESET ": seq %u già riscontrato → ignorato", seq, seq);
            return;
        }
        s->acked[i]    = 1;
        s->timer_on[i] = 0;
        s->pending[i]  = 0;
        s->ack_new++;
        set_rule(s, RULE_ACK, now);
        snprintf(flash, sizeof(flash), "✓ ACK %u", seq);
        tui_channel_flash(TO_SENDER, FLASH_OK, flash);

        if (i == s->base) {
            int old = s->base;
            while (s->base < s->nextseq && s->acked[s->base])
                s->base++;
            if (s->base >= s->total) {
                s->done = 1;
                s->t_end = now;
            }
            if (s->done)
                tui_log(T_ACK "◀ ACK %u" T_RESET " ricevuto: " T_OK "✓ timer fermato" T_RESET " → FIN confermato", seq);
            else
                tui_log(T_ACK "◀ ACK %u" T_RESET " ricevuto: " T_OK "✓ timer fermato" T_RESET
                        " → la finestra scorre di %d, send_base = seq %d" T_DIM "%s",
                        seq, s->base - old, seq_of(s, s->base),
                        s->base - old > 1 ? " (i successivi erano già riscontrati)" : "");
        } else {
            tui_log(T_ACK "◀ ACK %u" T_RESET " ricevuto: " T_OK "✓ riscontrato fuori ordine" T_RESET
                    ", timer fermato " T_DIM "· send_base resta seq %d, in attesa del suo ACK",
                    seq, seq_of(s, s->base));
        }
        if (s->done)
            tui_log(T_OK T_BOLD "✓ Trasferimento completato" T_RESET ": %d segmenti, %zu byte in %.1f s "
                    T_DIM "· %u trasmissioni, %u ritrasmissioni",
                    s->data.nseg, s->data.len, (s->t_end - s->t_begin) / 1000.0, s->tx, s->retx);
    }
}

/* ------------------------------------------------------------------ */
/* Interfaccia                                                        */
/* ------------------------------------------------------------------ */

static void cell_info(long idx, tui_seqcell *c, const void *ctx)
{
    const sender *s = ctx;
    int fin = idx == s->total - 1;

    c->below = fin ? T_FIN "FIN" : NULL;
    if (idx < s->base) {
        c->st = SC_DONE;
    } else if (idx < s->nextseq) {
        if (s->acked[idx]) {
            c->st = SC_ACKED;
            c->below = T_OK "✓";
        } else if (s->pending[idx]) {
            c->st = SC_RETX;
            c->below = T_WARN "↻";
        } else {
            c->st = fin ? SC_FIN : SC_FLIGHT;
            if (s->timer_on[idx])
                c->timer = (double)(s->render_now - s->timer_start[idx]) / s->opt.timeout_ms;
        }
    } else if (idx < s->base + s->N) {
        c->st = SC_USABLE;
    } else {
        c->st = SC_UNUSABLE;
    }
}

static void draw_window(const sender *s, int x, int y, int w)
{
    const tui_legend legend[] = {
        { SC_DONE, "riscontrati" }, { SC_ACKED, "ACK fuori ordine" }, { SC_FLIGHT, "in volo + timer" },
        { SC_RETX, "da ritrasmettere" }, { SC_USABLE, "utilizzabili" }, { SC_UNUSABLE, "non utilizzabili" },
    };
    char title[160], wl[32];
    tui_seqstrip st;

    snprintf(title, sizeof(title), "Finestra del sender · N = %d · seq 0…%d (%d bit) · un invio ogni %.2f s",
             s->N, s->M - 1, s->opt.seq_bits, s->opt.interval_ms / 1000.0);
    snprintf(wl, sizeof(wl), "finestra N = %d", s->N);
    memset(&st, 0, sizeof(st));
    st.modulo         = s->M;
    st.count          = s->total;
    st.win_lo         = s->base;
    st.win_n          = s->done ? 0 : s->N;
    st.win_label      = wl;
    st.ptr_left       = s->done ? -1 : s->base;
    st.ptr_left_name  = "send_base";
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
        return s->acked[seg]   ? TS(16, C_ACK, 0)
             : s->pending[seg] ? TS(16, C_WARN_BG, 0)
                               : TS(231, C_DATA_BG, TA_BOLD);
    if (seg < s->base + s->N)
        return TS(C_TEXT, C_DEFAULT, 0);
    return TS(C_FAINT, C_DEFAULT, 0);
}

static void draw_transfer(const sender *s, int x, int y, int w, long long now)
{
    int ix = x + 2, iw = w - 4, lab = 12, bw = iw - lab - 16, focus, i;
    int done_segs = s->base < s->data.nseg ? s->base : s->data.nseg;
    int flying = 0, early = 0, retx = 0, next = -1;
    long long next_left = 0;
    tstyle dim = TS(C_DIM, C_DEFAULT, 0), txt = TS(C_TEXT, C_DEFAULT, 0);
    char nt[96];

    tui_box(x, y, w, 6, TS(C_BORDER, C_DEFAULT, 0), "Trasferimento");

    /* un timer per pacchetto: si mostra quello che scade per primo */
    for (i = s->base; i < s->nextseq; i++) {
        if (s->acked[i])
            early++;
        else if (s->pending[i])
            retx++;
        else
            flying++;
        if (s->timer_on[i]) {
            long long left = s->timer_start[i] + s->opt.timeout_ms - now;
            if (next < 0 || left < next_left) {
                next = i;
                next_left = left;
            }
        }
    }
    if (next >= 0)
        snprintf(nt, sizeof(nt), T_DIM "prossimo timeout" T_RESET " " T_DATA "seq %d" T_RESET " tra %.1f s",
                 seq_of(s, next), next_left > 0 ? next_left / 1000.0 : 0.0);
    else
        snprintf(nt, sizeof(nt), T_DIM "nessun timer attivo");
    tui_print(ix, y + 1, lab, dim, "timer");
    tui_print(ix + lab, y + 1, iw - lab, txt,
              T_DATA "%d in volo" T_RESET T_DIM " · " T_RESET T_OK "%d ACK fuori ordine" T_RESET T_DIM " · " T_RESET
              T_WARN "%d da ritrasmettere" T_RESET "   %s", flying, early, retx, nt);

    tui_print(ix, y + 2, lab, dim, "confermati");
    tui_bar(ix + lab, y + 2, bw, s->data.nseg ? (double)done_segs / s->data.nseg : 0.0, C_ACK);
    tui_print(ix + lab + bw + 2, y + 2, 14, txt, "%d/%d", done_segs, s->data.nseg);

    tui_print(ix, y + 3, lab, dim, "dati");
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

    tui_box(x, y, w, 4, TS(C_BORDER, C_DEFAULT, 0), "Statistiche");
    tui_print(ix, y + 1, iw, TS(C_TEXT, C_DEFAULT, 0),
              T_DATA T_BOLD "DATA" T_RESET "  " T_DIM "trasmessi" T_RESET " %u  " T_DIM "ritrasmessi" T_RESET " %u  "
              T_TIME "timeout" T_RESET " %u" T_DIM "  │  " T_CHAN "canale ▶" T_RESET " " T_DIM "persi" T_RESET " %u/%u  "
              T_DIM "corrotti" T_RESET " %u/%u",
              s->tx, s->retx, s->timeouts, s->ch.n_lost, s->ch.n_sent, s->ch.n_corrupted, s->ch.n_sent);
    tui_print(ix, y + 2, iw, TS(C_TEXT, C_DEFAULT, 0),
              T_ACK T_BOLD "ACK " T_RESET "  " T_OK "nuovi" T_RESET " %u  " T_WARN "duplicati" T_RESET " %u  "
              T_WARN "fuori finestra" T_RESET " %u  " T_ERR "corrotti" T_RESET " %u"
              T_DIM "  │  durata" T_RESET " %.1f s  " T_DIM "utili" T_RESET " %.0f%%",
              s->ack_new, s->ack_dup, s->ack_out, s->ack_bad,
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
             s->N > s->M / 2 ? T_WARN "   N > 2^(k−1): il receiver può scambiare vecchi per nuovi!" T_RESET : "",
             s->opt.timeout_ms <= 2 * s->opt.delay_ms ? T_WARN "   timeout ≤ RTT!" : "");
    snprintf(l2, sizeof(l2),
             "timeout %.1f s · ritardo %.1f s · perdita %.0f%% · corruzione %.0f%% · %d×%d B · seed %u",
             s->opt.timeout_ms / 1000.0, s->opt.delay_ms / 1000.0, s->opt.loss * 100, s->opt.corrupt * 100,
             s->data.nseg, s->opt.chunk, s->opt.seed);
    tui_draw_header(y, W, PROTOCOL, ROLE_SENDER, l1, l2);
    y += 4;

    if (show_rules) {
        tui_draw_rules(0, y, W, "Regole del sender Selective Repeat", RULES, 4,
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
        snprintf(note, sizeof(note), T_OK "✓ fine");
    else
        snprintf(note, sizeof(note), "base " T_DATA T_BOLD "%d", seq_of(s, s->base));
    tui_draw_channel(0, y, W, &s->ch, ROLE_SENDER, now, note);
    y += 7;

    if (show_stats) {
        draw_stats(s, 0, y, W);
        y += 4;
    }
    tui_draw_log(0, y, W, H - 1 - y, "Registro eventi");

    snprintf(foot, sizeof(foot), T_KEY " q " T_RESET " esci     selective repeat · sender · pid %d%s",
             (int)getpid(), s->done ? T_OK "     ✓ trasferimento completato, premi q per uscire" : "");
    tui_draw_footer(H - 1, W, foot);
    tui_frame_end();
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    sender *s = &S;
    int quit = 0, i;
    size_t n;

    rdt_parse_options(argc, argv, ROLE_SENDER, &PROTO, &s->opt);
    rdt_srand(s->opt.seed);
    s->sock = rdt_open_sender(&s->opt, &s->dst, s->local_s, s->remote_s, sizeof(s->local_s));

    rdt_load_data(&s->opt, &s->data);
    for (i = 0; i < s->data.nseg; i++)
        tui_text_append(&s->view, s->data.text + s->data.off[i], s->data.seg_len[i], i);
    s->total = s->data.nseg + 1;
    s->M     = 1 << s->opt.seq_bits;
    s->N     = s->opt.window;
    n = (size_t)s->total;
    s->acked       = calloc(n, 1);
    s->timer_on    = calloc(n, 1);
    s->pending     = calloc(n, 1);
    s->timer_start = calloc(n, sizeof(*s->timer_start));
    s->tx_count    = calloc(n, sizeof(*s->tx_count));
    if (!s->acked || !s->timer_on || !s->pending || !s->timer_start || !s->tx_count) {
        fprintf(stderr, "memoria esaurita\n");
        return EXIT_FAILURE;
    }
    ch_init(&s->ch, s->sock, TO_RECEIVER, s->opt.loss, s->opt.corrupt, s->opt.delay_ms);

    tui_init();
    s->rule         = RULE_NONE;
    s->rule_t       = -HIGHLIGHT_MS;
    s->t_begin      = rdt_now_ms();
    s->next_tx_time = s->t_begin + 800;
    tui_log(T_INFO "● Sender pronto" T_RESET ": %zu byte in %d segmenti + FIN, finestra N = %d, seq 0…%d, verso %s",
            s->data.len, s->data.nseg, s->N, s->M - 1, s->remote_s);
    if (s->N > s->M / 2)
        tui_log(T_WARN "N = %d con %d numeri di sequenza: le finestre di sender e receiver possono "
                "sovrapporsi e un pacchetto ritrasmesso sembrare nuovo (serve N ≤ 2^(k−1))", s->N, s->M);

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
        if (!s->done)
            check_timers(s, now);
        pump(s, now);
        ch_gc(&s->ch, now);
        render(s, now);

        if (s->done && !tui_interactive())
            break;
        tui_wait(s->sock, 30);
    }
    tui_shutdown();

    if (s->done)
        printf("Selective repeat: %d segmenti (%zu byte) consegnati in %.1f s · %u trasmissioni, "
               "%u ritrasmissioni, %u timeout\n", s->data.nseg, s->data.len,
               (s->t_end - s->t_begin) / 1000.0, s->tx, s->retx, s->timeouts);
    else
        printf("Selective repeat: interrotto dopo %d/%d segmenti confermati\n",
               s->base < s->data.nseg ? s->base : s->data.nseg, s->data.nseg);

    close(s->sock);
    rdt_free_data(&s->data);
    free(s->acked);
    free(s->timer_on);
    free(s->pending);
    free(s->timer_start);
    free(s->tx_count);
    free(s->view.g);
    return EXIT_SUCCESS;
}
