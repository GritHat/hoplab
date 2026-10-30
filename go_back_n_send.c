/*
 * go_back_n_send.c - Sender Go-Back-N su UDP.
 *
 * Il sender puo' avere fino a N pacchetti inviati e non ancora riscontrati:
 * la finestra [base, base+N-1]. I numeri di sequenza sono presi modulo 2^k
 * (opzione -b) e un solo timer sorveglia il pacchetto piu' vecchio in volo.
 *   - dati dall'applicazione e nextseqnum < base+N
 *                          -> invia il pacchetto nextseqnum; se la finestra
 *                             era vuota avvia il timer
 *   - ACK n integro        -> riscontro cumulativo: sono arrivati tutti i
 *                             pacchetti fino a n, quindi base = n+1; il timer
 *                             riparte, o si ferma se non resta nulla in volo
 *   - timeout              -> si "torna indietro": ritrasmette TUTTI i
 *                             pacchetti da base a nextseqnum-1
 *   - ACK corrotto o duplicato -> ignorato (ci pensera' il timeout)
 * Alla fine un FIN, numerato come i dati, chiude il trasferimento.
 *
 * Per vedere i pacchetti in volo uno dietro l'altro il sender ne invia al
 * massimo uno ogni -i millisecondi (il tempo di trasmissione simulato).
 *
 * Uso: ./go_back_n_send [opzioni] [host] [porta]     (-h per l'aiuto)
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

static const rdt_protocol PROTO = {
    "go-back-N",
    "Go-back-N: con N pacchetti in volo il canale non resta mai vuoto; "
    "un solo timer e, se scade, si ritrasmette tutto a partire da base.",
    RDT_OPT_WINDOW | RDT_OPT_SEQBITS | RDT_OPT_INTERVAL,
    "Il receiver deve usare lo stesso -b."
};

/* Regole del sender, evidenziate quando scattano */
enum { RULE_NONE = -1, RULE_SEND, RULE_ACK, RULE_TIMEOUT, RULE_IGNORE };

static const tui_rule RULES[] = {
    { T_DATA "▶", "dati dall'app, nextseqnum < base+N", "invia DATA nextseqnum; il primo in volo avvia il timer" },
    { T_ACK "◀",  "ACK n integro (cumulativo)",         "base = n+1; riavvia il timer, fermo se nulla in volo" },
    { T_TIME "◷", "timeout (un solo timer, su base)",   "riavvia il timer e ritrasmette tutto da base in poi" },
    { T_ERR "✗",  "ACK corrotto o duplicato",           "ignorato: se serve, ci pensa il timeout" },
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
    int         base;              /* il piu' vecchio non riscontrato */
    int         nextseq;           /* il prossimo mai inviato */
    int         next_tx;           /* il prossimo da trasmettere (< nextseq: ritrasmissione) */
    long long   next_tx_time;      /* prossimo invio non prima di (ms) */
    int         timer_on;
    long long   timer_start, timer_deadline;
    int        *tx_count;          /* trasmissioni di ciascun pacchetto */
    int         done;
    long long   t_begin, t_end;

    /* statistiche */
    unsigned    tx, retx, timeouts, ack_new, ack_dup, ack_bad, ack_late;

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

static void start_timer(sender *s, long long now)
{
    s->timer_on       = 1;
    s->timer_start    = now;
    s->timer_deadline = now + s->opt.timeout_ms;
}

/* udt_send del pacchetto idx (nuovo se idx == nextseq, altrimenti ritrasmissione) */
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
        int started = s->base == s->nextseq;     /* finestra vuota: parte il timer */
        if (started)
            start_timer(s, now);
        s->nextseq++;
        set_rule(s, RULE_SEND, now);
        tui_log(T_DATA "▶ " T_RESET "%s inviato " T_DIM "· in volo %d/%d%s",
                d, s->nextseq - s->base, s->N, started ? " · timer avviato" : "");
    } else {
        s->retx++;
        tui_log(T_WARN "↻ " T_RESET "%s ritrasmesso " T_DIM "· tentativo %d", d, s->tx_count[idx]);
    }
}

/* Invia cio' che la finestra permette, al massimo un pacchetto ogni interval_ms */
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
    tui_log(T_TIME "◷ TIMEOUT" T_RESET ": nessun riscontro per " T_BOLD "seq %d" T_RESET
            " entro %.1f s → riavvio il timer e torno indietro",
            seq_of(s, s->base), s->opt.timeout_ms / 1000.0);
    tui_log_rule("go-back · ritrasmissione di %d pacchett%s, seq %d…%d",
                 n, n == 1 ? "o" : "i", seq_of(s, s->base), seq_of(s, s->nextseq - 1));
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
                T_DIM "(se serve, ritrasmetterà il timeout)");
        tui_channel_flash(TO_SENDER, FLASH_ERR, "✗ checksum");
        return;
    }

    /* distanza dell'ACK da base nello spazio circolare dei numeri di sequenza */
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
            tui_log(T_ACK "◀ ACK %u" T_RESET " ricevuto: " T_OK "✓ riscontra seq %u" T_RESET " → base = seq %d " T_DIM "· %s",
                    seq, seq, seq_of(s, s->base),
                    s->done ? "FIN confermato" : s->timer_on ? "timer riavviato" : "niente in volo, timer fermato");
        else
            tui_log(T_ACK "◀ ACK %u" T_RESET " ricevuto: " T_OK "✓ cumulativo, riscontra seq %d…%u" T_RESET
                    " (%d pacchetti) → base = seq %d " T_DIM "· %s",
                    seq, seq_of(s, old), seq, d + 1, seq_of(s, s->base),
                    s->done ? "FIN confermato" : s->timer_on ? "timer riavviato" : "niente in volo, timer fermato");
        if (s->done)
            tui_log(T_OK T_BOLD "✓ Trasferimento completato" T_RESET ": %d segmenti, %zu byte in %.1f s "
                    T_DIM "· %u trasmissioni, %u ritrasmissioni",
                    s->data.nseg, s->data.len, (s->t_end - s->t_begin) / 1000.0, s->tx, s->retx);
    } else {
        s->ack_dup++;
        set_rule(s, RULE_IGNORE, now);
        snprintf(flash, sizeof(flash), "dup ACK %u", seq);
        tui_channel_flash(TO_SENDER, FLASH_WARN, flash);
        tui_log(T_ACK "◀ ACK %u" T_RESET " " T_WARN "duplicato" T_RESET ": riscontra solo pacchetti già confermati "
                T_DIM "(base = seq %d)" T_RESET " → ignorato", seq, seq_of(s, s->base));
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
        if (idx >= s->next_tx) {             /* in coda per il go-back */
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
        { SC_DONE, "riscontrati" }, { SC_FLIGHT, "in volo" }, { SC_RETX, "da ritrasmettere" },
        { SC_USABLE, "utilizzabili" }, { SC_UNUSABLE, "non utilizzabili" },
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

    tui_box(x, y, w, 6, TS(C_BORDER, C_DEFAULT, 0), "Trasferimento");

    /* l'unico timer, quello del pacchetto base */
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
        tui_print(ix + lab + bw + 2, y + 1, 14, dim, "■ fermo");
    }

    tui_print(ix, y + 2, lab, dim, "confermati");
    tui_bar(ix + lab, y + 2, bw, s->data.nseg ? (double)done_segs / s->data.nseg : 0.0, C_ACK);
    tui_print(ix + lab + bw + 2, y + 2, 14, txt, "%d/%d", done_segs, s->data.nseg);

    /* testo: confermato (verde), in volo (blu), da ritrasmettere (giallo) */
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
              T_ERR "corrotti" T_RESET " %u  " T_DIM "fuori tempo" T_RESET " %u" T_DIM "  │  durata" T_RESET " %.1f s  "
              T_DIM "utili" T_RESET " %.0f%%",
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
             s->N > s->M - 1 ? T_WARN "   N > 2^k−1: un ACK vecchio può sembrare nuovo!" T_RESET : "",
             s->opt.timeout_ms <= 2 * s->opt.delay_ms ? T_WARN "   timeout ≤ RTT!" : "");
    snprintf(l2, sizeof(l2),
             "timeout %.1f s · ritardo %.1f s · perdita %.0f%% · corruzione %.0f%% · %d×%d B · seed %u",
             s->opt.timeout_ms / 1000.0, s->opt.delay_ms / 1000.0, s->opt.loss * 100, s->opt.corrupt * 100,
             s->data.nseg, s->opt.chunk, s->opt.seed);
    tui_draw_header(y, W, PROTOCOL, ROLE_SENDER, l1, l2);
    y += 4;

    if (show_rules) {
        tui_draw_rules(0, y, W, "Regole del sender Go-Back-N", RULES, 4,
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

    snprintf(foot, sizeof(foot), T_KEY " q " T_RESET " esci     go-back-N · sender · pid %d%s",
             (int)getpid(), s->done ? T_OK "     ✓ trasferimento completato, premi q per uscire" : "");
    tui_draw_footer(H - 1, W, foot);
    tui_frame_end();
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    sender *s = &S;
    int quit = 0, i;

    rdt_parse_options(argc, argv, ROLE_SENDER, &PROTO, &s->opt);
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
    if (s->N > s->M - 1)
        tui_log(T_WARN "N = %d con %d numeri di sequenza: se si perdono tutti gli ACK di una finestra, "
                "quello del pacchetto prima di base sembra nuovo (serve N ≤ 2^k − 1)", s->N, s->M);

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
        printf("Go-back-N: %d segmenti (%zu byte) consegnati in %.1f s · %u trasmissioni, "
               "%u ritrasmissioni, %u timeout\n", s->data.nseg, s->data.len,
               (s->t_end - s->t_begin) / 1000.0, s->tx, s->retx, s->timeouts);
    else
        printf("Go-back-N: interrotto dopo %d/%d segmenti confermati\n",
               s->base < s->data.nseg ? s->base : s->data.nseg, s->data.nseg);

    close(s->sock);
    rdt_free_data(&s->data);
    free(s->tx_count);
    free(s->view.g);
    return EXIT_SUCCESS;
}
