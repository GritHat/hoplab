/*
 * tui.h - Terminal user interface for the RDT demos (and for netlab).
 *
 * It draws on a cell buffer and sends only the changed cells to the terminal
 * (no flicker). It uses 256 ANSI colours and one-column Unicode characters
 * (box drawing, arrows, blocks).
 *
 * If stdout is not a terminal (e.g. output redirected to a file) it switches
 * to "text" mode: no screen, only the event log on stdout.
 *
 * Inside texts, colour and style can be changed with the T_* codes below:
 *     tui_log(T_DATA "DATA 0" T_RESET " sent");
 */
#ifndef TUI_H
#define TUI_H

#include <stdint.h>
#include <stddef.h>

#include "channel.h"

/* Colours of the 256-colour palette */
#define C_DEFAULT  (-1)
#define C_TEXT     252
#define C_BRIGHT   255
#define C_DIM      244
#define C_FAINT    239
#define C_BORDER   240
#define C_DATA     75     /* blue: everything that leaves the sender */
#define C_DATA_BG  25
#define C_ACK      114    /* green: everything that leaves the receiver */
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
#define C_PANEL    235    /* background of bars and inactive pills */
#define C_SELECT   237    /* background of the highlighted row */

/* Attributes */
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

/* Markup codes usable inside strings */
#define T_RESET  "\001" "0"   /* back to the base style */
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
#define T_CHIP_DATA "\001" "D"  /* "chips" with a coloured background */
#define T_CHIP_ACK  "\001" "A"
#define T_CHIP_FIN  "\001" "F"
#define T_CHIP_ERR  "\001" "R"
#define T_CHIP_WARN "\001" "Y"
#define T_CHIP_OFF  "\001" "O"
#define T_KEY       "\001" "K"

/* Minimum size of the interface of each program */
#define TUI_MIN_W  72
#define TUI_MIN_H  24

/* ---- life cycle ---- */
int  tui_init(void);           /* 1 = interactive screen, 0 = text mode */
void tui_shutdown(void);
int  tui_interactive(void);
int  tui_should_quit(void);    /* Ctrl+C, SIGTERM, window closed */
int  tui_key(void);            /* key pressed or -1 */
void tui_wait(int fd, int ms); /* waits for data on fd or the keyboard, at most ms */
long long tui_elapsed_ms(void);

/* ---- drawing (between tui_frame_begin and tui_frame_end) ---- */
int  tui_frame_begin(void);    /* 0 if there is nothing to draw (text mode) */
void tui_frame_end(void);
int  tui_width(void);
int  tui_height(void);

void tui_put(int x, int y, tstyle st, const char *utf8);
int  tui_print(int x, int y, int maxw, tstyle base, const char *fmt, ...)
     __attribute__((format(printf, 5, 6)));
int  tui_print_center(int x, int y, int w, tstyle base, const char *fmt, ...)
     __attribute__((format(printf, 5, 6)));
int  tui_text_width(const char *markup);
/* Text with markup that wraps on words and on \n; draws at most maxh rows
 * (maxh = 0: only measure) and returns the rows needed */
int  tui_print_wrapped(int x, int y, int w, int maxh, tstyle base, const char *text);
void tui_fill(int x, int y, int w, int h, tstyle st);
void tui_hline(int x, int y, int w, tstyle st, const char *glyph);
void tui_vline(int x, int y, int h, tstyle st, const char *glyph);
void tui_box(int x, int y, int w, int h, tstyle border, const char *title);
void tui_bar(int x, int y, int w, double frac, short color);
void tui_pill(int x, int y, int w, const char *text, int active, short active_bg);
void tui_too_small(int minw, int minh);

/* ---- event log ---- */
void tui_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void tui_log_rule(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void tui_draw_log(int x, int y, int w, int h, const char *title);

/* ---- widgets specific to the RDT demos ---- */

/* Copies into out at most maxcols printable characters of the payload */
int  tui_payload(const uint8_t *data, size_t len, char *out, size_t outsz, int maxcols);

/* Coloured name of a packet for the log, e.g. DATA 0 «The proto» */
const char *tui_pkt_name(const rdt_packet *p, char *buf, size_t n);

/* Logs the loss or the corruption of an outgoing packet */
void tui_log_fate(const ch_flight *f);

/* Application text split into segments, to show its progress */
typedef struct { char ch[5]; int seg; } tui_glyph;
typedef struct { tui_glyph *g; int n, cap; } tui_textbuf;
typedef tstyle (*tui_seg_style)(int seg, const void *ctx);

void tui_text_append(tui_textbuf *t, const uint8_t *data, size_t len, int seg);
void tui_text_clear(tui_textbuf *t);
/* Draws the text on h rows keeping the character with index focus visible */
void tui_draw_text(int x, int y, int w, int h, const tui_textbuf *t, int focus,
                   tui_seg_style style, const void *ctx);

/* Header with title, role and two lines of information (h = 4) */
void tui_draw_header(int y, int w, const char *protocol, rdt_role role,
                     const char *line1, const char *line2);

/* Channel panel with the two hosts and the packets in flight (h = 7) */
void tui_draw_channel(int x, int y, int w, const channel *ch, rdt_role self,
                      long long now, const char *self_note);

/* Outcome of an arrival, shown for a moment at the end of the lane */
enum { FLASH_OK, FLASH_WARN, FLASH_ERR };
void tui_channel_flash(ch_dir lane, int kind, const char *text);

void tui_draw_footer(int y, int w, const char *markup);

/* Protocol rules (event -> action); the rule `active` (or -1) is
 * highlighted. Height n + 2. */
typedef struct {
    const char *icon;        /* symbol with its colour (markup) */
    const char *when;        /* event */
    const char *then;        /* action */
} tui_rule;

void tui_draw_rules(int x, int y, int w, const char *title, const tui_rule *rules, int n, int active);

/* ---- sequence number strip (windowed protocols) ---- */

/* Cell styles */
#define SC_DONE     TS(151, 22, 0)                /* acknowledged / delivered */
#define SC_FLIGHT   TS(231, C_DATA_BG, TA_BOLD)   /* sent, waiting for the acknowledgement */
#define SC_FIN      TS(231, C_FIN_BG, TA_BOLD)    /* FIN */
#define SC_RETX     TS(16, C_WARN_BG, TA_BOLD)    /* to be retransmitted */
#define SC_ACKED    TS(16, C_ACK, TA_BOLD)        /* acknowledged / buffered out of order */
#define SC_EXPECT   TS(16, C_WARN_BG, TA_BOLD)    /* receiver: the next one expected */
#define SC_USABLE   TS(C_TEXT, 238, 0)            /* in the window, still free */
#define SC_UNUSABLE TS(C_FAINT, C_DEFAULT, 0)     /* outside the window */

typedef struct {
    tstyle      st;          /* style of the cell */
    const char *below;       /* markup below the cell, or NULL */
    double      timer;       /* >= 0: progress of the cell's timer (bar below) */
} tui_seqcell;

typedef void (*tui_seqcell_fn)(long idx, tui_seqcell *c, const void *ctx);

typedef struct {
    tstyle      st;
    const char *label;
} tui_legend;

typedef struct {
    int             modulo;              /* sequence numbers 0..modulo-1 */
    long            count;               /* existing packets 0..count-1, < 0 = endless */
    long            win_lo;              /* window [win_lo, win_lo + win_n) */
    int             win_n;
    const char     *win_label;
    long            shadow_lo;           /* second bracket, faint (or shadow_n = 0) */
    int             shadow_n;
    const char     *shadow_label;
    long            ptr_left, ptr_right; /* pointers above the cells (< 0 = none) */
    const char     *ptr_left_name, *ptr_right_name;
    short           accent;              /* colour of window and pointers */
    tui_seqcell_fn  cell;
    const void     *ctx;
    const tui_legend *legend;
    int             nlegend;
} tui_seqstrip;

/* Numbered cells with the pointers above and the window below (h = 7). Each
 * cell is a packet (absolute index) and shows its sequence number, that is
 * the index modulo 2^k. */
void tui_draw_seqstrip(int x, int y, int w, const char *title, const tui_seqstrip *s);

#endif
