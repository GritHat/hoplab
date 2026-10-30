/*
 * tui.h - Interfaccia grafica da terminale per le demo RDT.
 *
 * Disegna su un buffer di celle e invia al terminale solo le celle cambiate
 * (niente sfarfallio). Usa colori ANSI a 256 e caratteri Unicode a una
 * colonna (box drawing, frecce, blocchi).
 *
 * Se stdout non e' un terminale (es. output rediretto su file) passa in
 * modalita' "testo": niente schermo, solo il registro eventi su stdout.
 *
 * Nei testi si possono cambiare colore e stile con i codici T_* qui sotto:
 *     tui_log(T_DATA "DATA 0" T_RESET " inviato");
 */
#ifndef TUI_H
#define TUI_H

#include <stdint.h>
#include <stddef.h>

#include "channel.h"

/* Colori della tavolozza a 256 colori */
#define C_DEFAULT  (-1)
#define C_TEXT     252
#define C_BRIGHT   255
#define C_DIM      244
#define C_FAINT    239
#define C_BORDER   240
#define C_DATA     75     /* blu: tutto cio' che parte dal sender */
#define C_DATA_BG  25
#define C_ACK      114    /* verde: tutto cio' che parte dal receiver */
#define C_ACK_BG   28
#define C_FIN      177
#define C_FIN_BG   97
#define C_OK       120
#define C_ERR      203
#define C_ERR_BG   160
#define C_WARN     221
#define C_WARN_BG  178
#define C_TIME     213
#define C_CHAN     209
#define C_INFO     117
#define C_PANEL    235    /* sfondo di barre e pillole spente */
#define C_SELECT   237    /* sfondo della riga evidenziata */

/* Attributi */
#define TA_BOLD    1u
#define TA_DIM     2u
#define TA_ITALIC  4u
#define TA_UNDER   8u
#define TA_REVERSE 16u

typedef struct {
    short         fg, bg;
    unsigned char attr;
} tstyle;

#define TS(fg, bg, attr) ((tstyle){ (short)(fg), (short)(bg), (unsigned char)(attr) })

/* Codici di markup utilizzabili dentro le stringhe */
#define T_RESET  "\001" "0"   /* torna allo stile di base */
#define T_BOLD   "\001" "b"
#define T_DIM    "\001" "m"
#define T_TEXT   "\001" "w"
#define T_DATA   "\001" "d"
#define T_ACK    "\001" "a"
#define T_FIN    "\001" "f"
#define T_OK     "\001" "g"
#define T_ERR    "\001" "r"
#define T_WARN   "\001" "y"
#define T_TIME   "\001" "t"
#define T_CHAN   "\001" "c"
#define T_INFO   "\001" "i"
#define T_CHIP_DATA "\001" "D"  /* "etichette" con sfondo colorato */
#define T_CHIP_ACK  "\001" "A"
#define T_CHIP_FIN  "\001" "F"
#define T_CHIP_ERR  "\001" "R"
#define T_CHIP_WARN "\001" "Y"
#define T_CHIP_OFF  "\001" "O"
#define T_KEY       "\001" "K"

/* Dimensioni minime dell'interfaccia di ogni programma */
#define TUI_MIN_W  72
#define TUI_MIN_H  24

/* ---- ciclo di vita ---- */
int  tui_init(void);           /* 1 = schermo interattivo, 0 = modalita' testo */
void tui_shutdown(void);
int  tui_interactive(void);
int  tui_should_quit(void);    /* Ctrl+C, SIGTERM, chiusura della finestra */
int  tui_key(void);            /* tasto premuto o -1 */
void tui_wait(int fd, int ms); /* attende dati su fd o sulla tastiera, al massimo ms */
long long tui_elapsed_ms(void);

/* ---- disegno (tra tui_frame_begin e tui_frame_end) ---- */
int  tui_frame_begin(void);    /* 0 se non c'e' nulla da disegnare (modalita' testo) */
void tui_frame_end(void);
int  tui_width(void);
int  tui_height(void);

void tui_put(int x, int y, tstyle st, const char *utf8);
int  tui_print(int x, int y, int maxw, tstyle base, const char *fmt, ...)
     __attribute__((format(printf, 5, 6)));
int  tui_print_center(int x, int y, int w, tstyle base, const char *fmt, ...)
     __attribute__((format(printf, 5, 6)));
int  tui_text_width(const char *markup);
void tui_fill(int x, int y, int w, int h, tstyle st);
void tui_hline(int x, int y, int w, tstyle st, const char *glyph);
void tui_vline(int x, int y, int h, tstyle st, const char *glyph);
void tui_box(int x, int y, int w, int h, tstyle border, const char *title);
void tui_bar(int x, int y, int w, double frac, short color);
void tui_pill(int x, int y, int w, const char *text, int active, short active_bg);
void tui_too_small(int minw, int minh);

/* ---- registro eventi ---- */
void tui_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void tui_log_rule(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void tui_draw_log(int x, int y, int w, int h, const char *title);

/* ---- widget specifici delle demo RDT ---- */

/* Copia in out al massimo maxcols caratteri stampabili del payload */
int  tui_payload(const uint8_t *data, size_t len, char *out, size_t outsz, int maxcols);

/* Nome colorato di un pacchetto per il registro, es. DATA 0 «Il proto» */
const char *tui_pkt_name(const rdt_packet *p, char *buf, size_t n);

/* Registra la perdita o la corruzione di un pacchetto in uscita */
void tui_log_fate(const ch_flight *f);

/* Testo dell'applicazione diviso in segmenti, per mostrarne l'avanzamento */
typedef struct { char ch[5]; int seg; } tui_glyph;
typedef struct { tui_glyph *g; int n, cap; } tui_textbuf;
typedef tstyle (*tui_seg_style)(int seg, const void *ctx);

void tui_text_append(tui_textbuf *t, const uint8_t *data, size_t len, int seg);
void tui_text_clear(tui_textbuf *t);
/* Disegna il testo su h righe tenendo visibile il carattere di indice focus */
void tui_draw_text(int x, int y, int w, int h, const tui_textbuf *t, int focus,
                   tui_seg_style style, const void *ctx);

/* Intestazione con titolo, ruolo e due righe di informazioni (h = 4) */
void tui_draw_header(int y, int w, const char *protocol, rdt_role role,
                     const char *line1, const char *line2);

/* Pannello del canale con i due host e i pacchetti in volo (h = 7) */
void tui_draw_channel(int x, int y, int w, const channel *ch, rdt_role self,
                      long long now, const char *self_note);

/* Esito di un arrivo, mostrato per un attimo all'estremita' della corsia */
enum { FLASH_OK, FLASH_WARN, FLASH_ERR };
void tui_channel_flash(ch_dir lane, int kind, const char *text);

void tui_draw_footer(int y, int w, const char *markup);

/* Regole del protocollo (evento -> azione); la regola active (o -1) e'
 * evidenziata. Altezza n + 2. */
typedef struct {
    const char *icon;        /* simbolo con il suo colore (markup) */
    const char *when;        /* evento */
    const char *then;        /* azione */
} tui_rule;

void tui_draw_rules(int x, int y, int w, const char *title, const tui_rule *rules, int n, int active);

/* ---- striscia dei numeri di sequenza (protocolli a finestra) ---- */

/* Stili delle celle */
#define SC_DONE     TS(151, 22, 0)                /* riscontrati / consegnati */
#define SC_FLIGHT   TS(231, C_DATA_BG, TA_BOLD)   /* inviati, in attesa di riscontro */
#define SC_FIN      TS(231, C_FIN_BG, TA_BOLD)    /* FIN */
#define SC_RETX     TS(16, C_WARN_BG, TA_BOLD)    /* da ritrasmettere */
#define SC_ACKED    TS(16, C_ACK, TA_BOLD)        /* riscontrati / bufferizzati fuori ordine */
#define SC_EXPECT   TS(16, C_WARN_BG, TA_BOLD)    /* receiver: il prossimo atteso */
#define SC_USABLE   TS(C_TEXT, 238, 0)            /* nella finestra, ancora liberi */
#define SC_UNUSABLE TS(C_FAINT, C_DEFAULT, 0)     /* fuori dalla finestra */

typedef struct {
    tstyle      st;          /* stile della cella */
    const char *below;       /* markup sotto la cella, o NULL */
    double      timer;       /* >= 0: avanzamento del timer della cella (barra sotto) */
} tui_seqcell;

typedef void (*tui_seqcell_fn)(long idx, tui_seqcell *c, const void *ctx);

typedef struct {
    tstyle      st;
    const char *label;
} tui_legend;

typedef struct {
    int             modulo;              /* numeri di sequenza 0..modulo-1 */
    long            count;               /* pacchetti esistenti 0..count-1, < 0 = senza fine */
    long            win_lo;              /* finestra [win_lo, win_lo + win_n) */
    int             win_n;
    const char     *win_label;
    long            shadow_lo;           /* seconda parentesi, tenue (o shadow_n = 0) */
    int             shadow_n;
    const char     *shadow_label;
    long            ptr_left, ptr_right; /* puntatori sopra le celle (< 0 = nessuno) */
    const char     *ptr_left_name, *ptr_right_name;
    short           accent;              /* colore di finestra e puntatori */
    tui_seqcell_fn  cell;
    const void     *ctx;
    const tui_legend *legend;
    int             nlegend;
} tui_seqstrip;

/* Celle numerate con i puntatori sopra e la finestra sotto (h = 7). Ogni
 * cella e' un pacchetto (indice assoluto) e mostra il suo numero di
 * sequenza, cioe' l'indice modulo 2^k. */
void tui_draw_seqstrip(int x, int y, int w, const char *title, const tui_seqstrip *s);

#endif
