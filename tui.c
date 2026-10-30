/*
 * tui.c - Grafica da terminale: buffer di celle, testo colorato, pannelli,
 *         registro eventi e widget del canale.
 *
 * Due varianti, scelte con variabili d'ambiente:
 *   RDT_PANE=left|right  il programma disegna solo in una meta' del terminale,
 *                        che condivide con l'altro programma (modalita' "split"
 *                        di demo.sh, senza tmux): left = sinistra, right =
 *                        destra; se la finestra e' stretta e alta, sopra e sotto
 *   RDT_BASIC=1|0        16 colori e soli caratteri della code page 437, per
 *                        la console testuale di Linux (attiva da sola con
 *                        TERM=linux; 0 la disattiva)
 */
#include "tui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    char   ch[5];                   /* un carattere UTF-8 */
    tstyle st;
} tcell;

enum { MODE_OFF, MODE_SCREEN, MODE_TEXT };
enum { PANE_NONE, PANE_LEFT, PANE_RIGHT };

static int    g_mode = MODE_OFF;
static int    g_w, g_h;             /* area di disegno */
static int    g_bw, g_bh;           /* buffer: l'area piu' l'eventuale riga/colonna di separazione */
static int    g_x0, g_y0;           /* punto dello schermo da cui parte il buffer */
static int    g_pane = PANE_NONE;
static int    g_basic;              /* console di Linux: 16 colori, code page 437 */
static tcell *g_cur, *g_prev;       /* fotogramma in costruzione / gia' a schermo */
static int    g_full_redraw;
static int    g_title_set;
static long long g_t0;

static struct termios g_term_orig;
static int            g_term_saved;

static volatile sig_atomic_t g_winch, g_quit;

/* ------------------------------------------------------------------ */
/* Output bufferizzato: un fotogramma = una sola write()              */
/* ------------------------------------------------------------------ */

static char  *g_out;
static size_t g_out_len, g_out_cap;

static void out_add(const char *s, size_t n)
{
    if (g_out_len + n > g_out_cap) {
        size_t cap = g_out_cap ? g_out_cap : 65536;
        char *p;
        while (cap < g_out_len + n)
            cap *= 2;
        p = realloc(g_out, cap);
        if (!p)
            return;
        g_out = p;
        g_out_cap = cap;
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
}

static void out_str(const char *s)
{
    out_add(s, strlen(s));
}

/* Una write() su un terminale non si mescola con quelle di altri processi,
 * purche' un segnale non la spezzi: in modalita' split l'altro programma
 * scrive sullo stesso terminale, quindi i segnali restano bloccati. */
static void out_flush(void)
{
    sigset_t block, old;
    size_t off = 0;

    sigemptyset(&block);
    sigaddset(&block, SIGWINCH);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    sigprocmask(SIG_BLOCK, &block, &old);
    while (off < g_out_len) {
        ssize_t n = write(STDOUT_FILENO, g_out + off, g_out_len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)n;
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    g_out_len = 0;
}

/* ------------------------------------------------------------------ */
/* Ciclo di vita                                                      */
/* ------------------------------------------------------------------ */

static void on_winch(int sig) { (void)sig; g_winch = 1; }
static void on_quit(int sig)  { (void)sig; g_quit = 1; }

static void resize(void)
{
    struct winsize ws;
    int w = 80, h = 24;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        w = ws.ws_col;
        h = ws.ws_row;
    }
    if (w > 1000) w = 1000;
    if (h > 500)  h = 500;

    /* meta' schermo: affiancati se c'e' spazio in larghezza, altrimenti uno
     * sopra l'altro. Una colonna (o riga) vuota separa le due aree: la
     * disegna il primo programma, cosi' dopo un ridimensionamento non resta
     * nulla di vecchio. Entrambi decidono dalle stesse dimensioni. */
    g_x0 = g_y0 = 0;
    g_bw = w;
    g_bh = h;
    if (g_pane != PANE_NONE) {
        int first = g_pane == PANE_LEFT;
        if (w >= 2 * TUI_MIN_W + 1 || h < 2 * TUI_MIN_H + 1) {
            int sep = (w - 1) / 2;
            if (first) {
                g_bw = sep + 1;
                w = sep;
            } else {
                g_x0 = sep + 1;
                w = w - sep - 1;
                g_bw = w;
            }
        } else {
            int sep = (h - 1) / 2;
            if (first) {
                g_bh = sep + 1;
                h = sep;
            } else {
                g_y0 = sep + 1;
                h = h - sep - 1;
                g_bh = h;
            }
        }
    }

    free(g_cur);
    free(g_prev);
    g_cur  = calloc((size_t)g_bw * (size_t)g_bh + 1, sizeof(tcell));
    g_prev = calloc((size_t)g_bw * (size_t)g_bh + 1, sizeof(tcell));
    if (!g_cur || !g_prev) {
        tui_shutdown();
        fprintf(stderr, "memoria esaurita\n");
        exit(EXIT_FAILURE);
    }
    g_w = w;
    g_h = h;
    g_full_redraw = 1;
}

int tui_init(void)
{
    const char *pane = getenv("RDT_PANE"), *basic = getenv("RDT_BASIC"), *term = getenv("TERM");
    struct sigaction sa;

    g_t0 = rdt_now_ms();
    g_pane = pane && strcmp(pane, "left") == 0  ? PANE_LEFT
           : pane && strcmp(pane, "right") == 0 ? PANE_RIGHT : PANE_NONE;
    if (basic && *basic)
        g_basic = strcmp(basic, "0") != 0;
    else
        g_basic = term && strncmp(term, "linux", 5) == 0;

    /* niente SA_RESTART: poll() si interrompe subito e il ciclo reagisce */
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_quit;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (!isatty(STDOUT_FILENO)) {
        g_mode = MODE_TEXT;
        return 0;
    }

    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);

    g_mode = MODE_SCREEN;
    atexit(tui_shutdown);

    /* a meta' schermo terminale e tastiera sono di chi ha diviso lo schermo */
    if (g_pane == PANE_NONE) {
        /* tastiera senza eco e senza attendere Invio */
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_term_orig) == 0) {
            struct termios t = g_term_orig;
            t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
            t.c_cc[VMIN]  = 0;
            t.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
            g_term_saved = 1;
        }
        /* schermo alternativo, cursore nascosto, niente a-capo automatico */
        out_str("\033[?1049h\033[?25l\033[?7l\033[2J");
        out_flush();
    }
    resize();
    return 1;
}

void tui_shutdown(void)
{
    if (g_mode == MODE_SCREEN && g_pane != PANE_NONE) {
        /* il riepilogo finale non deve finire sopra l'altra meta' dello
         * schermo: va nel file indicato da demo.sh (RDT_PANE_LOG) */
        const char *log = getenv("RDT_PANE_LOG");
        int fd;

        out_str("\033[0m");
        out_flush();
        fflush(stdout);
        fd = open(log && *log ? log : "/dev/null", O_WRONLY | O_APPEND | O_CREAT, 0600);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
    } else if (g_mode == MODE_SCREEN) {
        out_str("\033[0m\033[?7h\033[?25h\033[?1049l");
        if (g_basic)
            out_str("\033[2J\033[H");     /* la console di Linux non ha lo schermo alternativo */
        out_flush();
        if (g_term_saved)
            tcsetattr(STDIN_FILENO, TCSANOW, &g_term_orig);
        g_term_saved = 0;
    }
    g_mode = MODE_OFF;
}

int tui_interactive(void)  { return g_mode == MODE_SCREEN; }
int tui_should_quit(void)  { return g_quit; }
int tui_width(void)        { return g_w; }
int tui_height(void)       { return g_h; }

long long tui_elapsed_ms(void)
{
    return rdt_now_ms() - g_t0;
}

int tui_key(void)
{
    unsigned char c;

    if (g_mode != MODE_SCREEN || !g_term_saved)
        return -1;
    return read(STDIN_FILENO, &c, 1) == 1 ? c : -1;
}

void tui_wait(int fd, int ms)
{
    struct pollfd p[2];
    nfds_t n = 0;

    if (fd >= 0) {
        p[n].fd = fd;
        p[n].events = POLLIN;
        n++;
    }
    if (g_mode == MODE_SCREEN && g_term_saved) {
        p[n].fd = STDIN_FILENO;
        p[n].events = POLLIN;
        n++;
    }
    poll(p, n, ms);
}

/* ------------------------------------------------------------------ */
/* Celle e fotogrammi                                                 */
/* ------------------------------------------------------------------ */

static int style_eq(tstyle a, tstyle b)
{
    return a.fg == b.fg && a.bg == b.bg && a.attr == b.attr;
}

/* Lunghezza in byte del carattere UTF-8 che inizia in s */
static int cp_len(const char *s)
{
    unsigned char c = (unsigned char)*s;
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    int i;

    for (i = 1; i < n; i++)
        if ((s[i] & 0xC0) != 0x80)
            return 1;
    return n;
}

static void set_cell(int x, int y, const char *s, int n, tstyle st)
{
    tcell *c;

    if (x < 0 || y < 0 || x >= g_w || y >= g_h || n <= 0 || n > 4)
        return;
    c = &g_cur[y * g_bw + x];
    memcpy(c->ch, s, (size_t)n);
    c->ch[n] = '\0';
    c->st = st;
}

int tui_frame_begin(void)
{
    int i;

    if (g_mode != MODE_SCREEN)
        return 0;
    if (g_winch) {
        g_winch = 0;
        resize();
        /* a meta' schermo non si cancella tutto: si ridisegna la propria area */
        if (g_pane == PANE_NONE)
            out_str("\033[0m\033[2J");
    }
    for (i = 0; i < g_bw * g_bh; i++) {
        g_cur[i].ch[0] = ' ';
        g_cur[i].ch[1] = '\0';
        g_cur[i].st = TS(C_DEFAULT, C_DEFAULT, 0);
    }
    return 1;
}

/* ---- console di Linux ---- */

/* Caratteri sostituiti con altri presenti nei font della console (code
 * page 437); gli altri, come le lettere accentate, restano come sono. */
static const char *const BASIC_GLYPHS[][2] = {
    { "╭", "┌" }, { "╮", "┐" }, { "╰", "└" }, { "╯", "┘" },
    { "┃", "║" }, { "━", "═" }, { "┗", "╚" }, { "┛", "╝" }, { "┄", "·" },
    { "▶", "►" }, { "◀", "◄" }, { "⇒", "→" }, { "↻", "»" }, { "↵", "¶" },
    { "✓", "√" }, { "✗", "x" }, { "×", "x" }, { "↯", "!" }, { "≠", "#" },
    { "◷", "○" }, { "●", "•" }, { "◉", "◙" }, { "◆", "♦" },
    { "…", "~" }, { "−", "-" }, { "—", "-" },
    { "▏", " " }, { "▎", " " }, { "▍", "▌" }, { "▋", "▌" }, { "▊", "█" }, { "▉", "█" },
};

static const char *basic_glyph(const char *ch)
{
    size_t i;

    if ((unsigned char)ch[0] < 0x80)
        return ch;
    for (i = 0; i < sizeof(BASIC_GLYPHS) / sizeof(BASIC_GLYPHS[0]); i++)
        if (strcmp(ch, BASIC_GLYPHS[i][0]) == 0)
            return BASIC_GLYPHS[i][1];
    return ch;
}

/* Colore della tavolozza a 256 -> uno dei 16 della console (i fondi solo 0..7).
 * I colori dell'interfaccia hanno una corrispondenza scelta a mano, perche'
 * restino distinguibili; gli altri vanno al piu' vicino. */
static int color16(int c, int bg)
{
    static const short FG[][2] = {
        { 16, 0 }, { 231, 15 }, { 255, 15 }, { 252, 7 }, { 250, 7 }, { 151, 15 },
        { 244, 8 }, { 240, 8 }, { 239, 8 }, { 75, 12 }, { 114, 10 },
    };
    static const short BG[][2] = {
        { 22, 2 }, { 25, 4 }, { 28, 2 }, { 30, 6 }, { 97, 5 }, { 114, 6 }, { 75, 4 },
        { 160, 1 }, { 178, 3 }, { 221, 3 }, { 120, 2 }, { 203, 1 }, { 117, 6 }, { 177, 5 },
        { 235, 0 }, { 236, 0 }, { 237, 0 }, { 238, 7 }, { 250, 7 },
    };
    static const unsigned char VGA[16][3] = {
        {0,0,0}, {170,0,0}, {0,170,0}, {170,85,0}, {0,0,170}, {170,0,170}, {0,170,170}, {170,170,170},
        {85,85,85}, {255,85,85}, {85,255,85}, {255,255,85}, {85,85,255}, {255,85,255}, {85,255,255}, {255,255,255},
    };
    int r, g, b, i, best = 0, nmax = bg ? 8 : 16;
    long bestd = -1;

    for (i = 0; i < (bg ? (int)(sizeof(BG) / sizeof(BG[0])) : (int)(sizeof(FG) / sizeof(FG[0]))); i++)
        if ((bg ? BG[i][0] : FG[i][0]) == c)
            return bg ? BG[i][1] : FG[i][1];
    if (c < 16)
        return bg ? c & 7 : c;
    if (c < 232) {
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        r = lv[(c - 16) / 36];
        g = lv[(c - 16) / 6 % 6];
        b = lv[(c - 16) % 6];
    } else {
        r = g = b = 8 + 10 * (c - 232);
    }
    for (i = 0; i < nmax; i++) {
        long d = (long)(r - VGA[i][0]) * (r - VGA[i][0]) + (long)(g - VGA[i][1]) * (g - VGA[i][1]) +
                 (long)(b - VGA[i][2]) * (b - VGA[i][2]);
        if (bestd < 0 || d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

static void emit_style_basic(tstyle s, char *b, size_t size, int n)
{
    int fg = s.fg >= 0 ? color16(s.fg, 0) : -1;
    int bg = s.bg >= 0 ? color16(s.bg, 1) : -1;

    /* testo poco leggibile sul suo fondo: nero sui fondi chiari, bianco sugli scuri */
    if (bg >= 0 && fg >= 0) {
        int light = bg == 2 || bg == 3 || bg == 6 || bg == 7;
        if (fg == bg || (bg == 7 && fg == 15))
            fg = light ? 0 : 15;
        else if (!light && fg == 0)
            fg = 15;
    }
    if (s.attr & TA_BOLD)    n += snprintf(b + n, size - (size_t)n, ";1");
    if (s.attr & TA_REVERSE) n += snprintf(b + n, size - (size_t)n, ";7");
    if (fg >= 0)             n += snprintf(b + n, size - (size_t)n, ";%d", fg < 8 ? 30 + fg : 90 + fg - 8);
    if (bg >= 0)             n += snprintf(b + n, size - (size_t)n, ";%d", 40 + bg);
    out_add(b, (size_t)n);
    out_add("m", 1);
}

static void emit_style(tstyle s)
{
    char b[64];
    int n = snprintf(b, sizeof(b), "\033[0");

    if (g_basic) {
        emit_style_basic(s, b, sizeof(b), n);
        return;
    }
    if (s.attr & TA_BOLD)    n += snprintf(b + n, sizeof(b) - (size_t)n, ";1");
    if (s.attr & TA_DIM)     n += snprintf(b + n, sizeof(b) - (size_t)n, ";2");
    if (s.attr & TA_ITALIC)  n += snprintf(b + n, sizeof(b) - (size_t)n, ";3");
    if (s.attr & TA_UNDER)   n += snprintf(b + n, sizeof(b) - (size_t)n, ";4");
    if (s.attr & TA_REVERSE) n += snprintf(b + n, sizeof(b) - (size_t)n, ";7");
    if (s.fg >= 0)           n += snprintf(b + n, sizeof(b) - (size_t)n, ";38;5;%d", s.fg);
    if (s.bg >= 0)           n += snprintf(b + n, sizeof(b) - (size_t)n, ";48;5;%d", s.bg);
    out_add(b, (size_t)n);
    out_add("m", 1);
}

void tui_frame_end(void)
{
    tstyle cur = TS(-2, -2, 255);
    int x, y, cx = -1, cy = -1, any = 0;
    char b[32];

    if (g_mode != MODE_SCREEN)
        return;

    for (y = 0; y < g_bh; y++) {
        for (x = 0; x < g_bw; x++) {
            tcell *c = &g_cur[y * g_bw + x];
            tcell *p = &g_prev[y * g_bw + x];

            if (!g_full_redraw && style_eq(c->st, p->st) && strcmp(c->ch, p->ch) == 0)
                continue;
            if (!any) {
                out_str("\033[?2026h");          /* aggiornamento sincronizzato */
                any = 1;
            }
            if (y != cy || x != cx) {
                snprintf(b, sizeof(b), "\033[%d;%dH", g_y0 + y + 1, g_x0 + x + 1);
                out_str(b);
            }
            if (!style_eq(c->st, cur)) {
                emit_style(c->st);
                cur = c->st;
            }
            out_str(g_basic ? basic_glyph(c->ch) : c->ch);
            *p = *c;
            cx = x + 1;
            cy = y;
        }
    }
    if (any) {
        out_str("\033[0m\033[?2026l");
        out_flush();
    }
    g_full_redraw = 0;
}

/* ------------------------------------------------------------------ */
/* Testo con markup                                                   */
/* ------------------------------------------------------------------ */

static tstyle markup_style(char code, tstyle cur, tstyle base)
{
    switch (code) {
    case '0': return base;
    case 'b': cur.attr |= TA_BOLD; break;
    case 'm': cur.fg = C_DIM;   break;
    case 'w': cur.fg = C_TEXT;  break;
    case 'd': cur.fg = C_DATA;  break;
    case 'a': cur.fg = C_ACK;   break;
    case 'f': cur.fg = C_FIN;   break;
    case 'g': cur.fg = C_OK;    break;
    case 'r': cur.fg = C_ERR;   break;
    case 'y': cur.fg = C_WARN;  break;
    case 't': cur.fg = C_TIME;  break;
    case 'c': cur.fg = C_CHAN;  break;
    case 'i': cur.fg = C_INFO;  break;
    case 'D': cur = TS(231, C_DATA_BG, TA_BOLD); break;
    case 'A': cur = TS(231, C_ACK_BG, TA_BOLD);  break;
    case 'F': cur = TS(231, C_FIN_BG, TA_BOLD);  break;
    case 'R': cur = TS(231, C_ERR_BG, TA_BOLD);  break;
    case 'Y': cur = TS(16, C_WARN_BG, TA_BOLD);  break;
    case 'O': cur = TS(C_DIM, 237, 0);           break;
    case 'K': cur = TS(16, 250, TA_BOLD);        break;
    default: break;
    }
    return cur;
}

int tui_text_width(const char *s)
{
    int w = 0;

    while (*s) {
        if (*s == '\001') {
            if (!s[1])
                break;
            s += 2;
            continue;
        }
        s += cp_len(s);
        w++;
    }
    return w;
}

/* Disegna testo con markup su una riga; se supera maxw lo tronca con "…" */
static int draw_markup(int x, int y, int maxw, tstyle base, const char *s)
{
    int total = tui_text_width(s);
    int cut = maxw >= 0 && total > maxw;
    int limit = cut ? maxw - 1 : total;
    tstyle cur = base;
    int col = 0;

    while (*s && col < limit) {
        int n;
        if (*s == '\001') {
            if (!s[1])
                break;
            cur = markup_style(s[1], cur, base);
            s += 2;
            continue;
        }
        n = cp_len(s);
        set_cell(x + col, y, s, n, cur);
        s += n;
        col++;
    }
    if (cut && maxw > 0) {
        set_cell(x + col, y, "…", 3, cur);
        col++;
    }
    return col;
}

/* Larghezza della parola che inizia in s (fino al prossimo spazio) */
static int word_width(const char *s)
{
    int w = 0;

    while (*s && *s != ' ') {
        if (*s == '\001') {
            if (!s[1])
                break;
            s += 2;
            continue;
        }
        s += cp_len(s);
        w++;
    }
    return w;
}

/* Testo con markup che va a capo sulle parole; ritorna le righe usate */
static int draw_wrapped(int x, int y, int w, tstyle base, const char *s, int draw)
{
    tstyle cur = base;
    int line = 0, col = 0;

    if (w <= 0)
        return 1;
    while (*s) {
        int n;
        if (*s == '\001') {
            if (!s[1])
                break;
            cur = markup_style(s[1], cur, base);
            s += 2;
            continue;
        }
        if (*s == ' ' && col > 0) {
            int ww = word_width(s + 1);
            if (col + 1 + ww > w && ww <= w) {
                line++;
                col = 0;
                s++;
                continue;
            }
        }
        if (col >= w) {
            line++;
            col = 0;
            if (*s == ' ') {
                s++;
                continue;
            }
        }
        n = cp_len(s);
        if (draw)
            set_cell(x + col, y + line, s, n, cur);
        s += n;
        col++;
    }
    return line + 1;
}

void tui_put(int x, int y, tstyle st, const char *s)
{
    while (*s) {
        int n = cp_len(s);
        set_cell(x++, y, s, n, st);
        s += n;
    }
}

int tui_print(int x, int y, int maxw, tstyle base, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return draw_markup(x, y, maxw, base, buf);
}

int tui_print_center(int x, int y, int w, tstyle base, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    int tw;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tw = tui_text_width(buf);
    if (tw < w)
        x += (w - tw) / 2;
    return draw_markup(x, y, w, base, buf);
}

/* ------------------------------------------------------------------ */
/* Primitive grafiche                                                 */
/* ------------------------------------------------------------------ */

void tui_fill(int x, int y, int w, int h, tstyle st)
{
    int i, j;

    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            set_cell(x + i, y + j, " ", 1, st);
}

void tui_hline(int x, int y, int w, tstyle st, const char *glyph)
{
    int n = cp_len(glyph), i;

    for (i = 0; i < w; i++)
        set_cell(x + i, y, glyph, n, st);
}

void tui_vline(int x, int y, int h, tstyle st, const char *glyph)
{
    int n = cp_len(glyph), i;

    for (i = 0; i < h; i++)
        set_cell(x, y + i, glyph, n, st);
}

void tui_box(int x, int y, int w, int h, tstyle border, const char *title)
{
    if (w < 2 || h < 2)
        return;
    tui_put(x, y, border, "╭");
    tui_hline(x + 1, y, w - 2, border, "─");
    tui_put(x + w - 1, y, border, "╮");
    tui_vline(x, y + 1, h - 2, border, "│");
    tui_vline(x + w - 1, y + 1, h - 2, border, "│");
    tui_put(x, y + h - 1, border, "╰");
    tui_hline(x + 1, y + h - 1, w - 2, border, "─");
    tui_put(x + w - 1, y + h - 1, border, "╯");
    if (title && *title)
        tui_print(x + 2, y, w - 4, TS(C_TEXT, C_DEFAULT, TA_BOLD), " %s ", title);
}

void tui_bar(int x, int y, int w, double frac, short color)
{
    static const char *eighths[] = { "", "▏", "▎", "▍", "▌", "▋", "▊", "▉" };
    double cells;
    int i, full, part;

    if (w <= 0)
        return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    cells = frac * w;
    full  = (int)cells;
    part  = (int)((cells - full) * 8.0);
    for (i = 0; i < w; i++) {
        if (i < full)
            set_cell(x + i, y, "█", 3, TS(color, C_PANEL, 0));
        else if (i == full && part > 0)
            set_cell(x + i, y, eighths[part], 3, TS(color, C_PANEL, 0));
        else if (g_basic)
            set_cell(x + i, y, "░", 3, TS(C_FAINT, C_PANEL, 0));   /* il fondo grigio non c'e' */
        else
            set_cell(x + i, y, " ", 1, TS(C_DEFAULT, C_PANEL, 0));
    }
}

void tui_pill(int x, int y, int w, const char *text, int active, short active_bg)
{
    short  bg    = active ? active_bg : C_PANEL;
    tstyle inner = active ? TS(16, bg, TA_BOLD) : TS(C_DIM, bg, 0);

    if (w < 3)
        return;
    tui_put(x, y, TS(bg, C_DEFAULT, 0), "▐");
    tui_fill(x + 1, y, w - 2, 1, inner);
    tui_print_center(x + 1, y, w - 2, inner, "%s", text);
    tui_put(x + w - 1, y, TS(bg, C_DEFAULT, 0), "▌");
}

void tui_too_small(int minw, int minh)
{
    tui_print_center(0, g_h / 2 - 1, g_w, TS(C_WARN, C_DEFAULT, TA_BOLD), "Finestra troppo piccola");
    tui_print_center(0, g_h / 2 + 1, g_w, TS(C_DIM, C_DEFAULT, 0),
                     "servono almeno %dx%d caratteri (ora %dx%d)", minw, minh, g_w, g_h);
    if (g_pane != PANE_NONE)
        tui_print_center(0, g_h / 2 + 2, g_w, TS(C_DIM, C_DEFAULT, 0),
                         "ingrandisci la finestra: in tutto %d colonne (o %d righe)",
                         2 * minw + 1, 2 * minh + 1);
}

/* ------------------------------------------------------------------ */
/* Registro eventi                                                    */
/* ------------------------------------------------------------------ */

#define LOG_MAX 300
#define LOG_LEN 400

typedef struct {
    long long t;
    int       rule;                 /* 1 = riga separatrice */
    char      text[LOG_LEN];
} log_entry;

static log_entry g_log[LOG_MAX];
static int       g_log_count, g_log_next;

static void fmt_time(long long ms, char *b, size_t n)
{
    snprintf(b, n, "%02lld:%02lld.%lld", ms / 60000, ms / 1000 % 60, ms / 100 % 10);
}

static void print_plain(const log_entry *e)
{
    char t[32];
    const char *s;

    fmt_time(e->t, t, sizeof(t));
    printf(e->rule ? "[%s] ---- " : "[%s] ", t);
    for (s = e->text; *s; s++) {
        if (*s == '\001') {
            if (!s[1])
                break;
            s++;
            continue;
        }
        putchar(*s);
    }
    printf(e->rule ? " ----\n" : "\n");
    fflush(stdout);
}

static void log_store(int rule, const char *fmt, va_list ap)
{
    log_entry *e = &g_log[g_log_next];

    e->t = tui_elapsed_ms();
    e->rule = rule;
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    g_log_next = (g_log_next + 1) % LOG_MAX;
    if (g_log_count < LOG_MAX)
        g_log_count++;
    if (g_mode != MODE_SCREEN)
        print_plain(e);
}

void tui_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_store(0, fmt, ap);
    va_end(ap);
}

void tui_log_rule(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_store(1, fmt, ap);
    va_end(ap);
}

void tui_draw_log(int x, int y, int w, int h, const char *title)
{
    const int tw = 9;               /* "mm:ss.d  " */
    int ix = x + 2, iw = w - 4;
    int top = y + 1, row = y + h - 2;
    int k;

    tui_box(x, y, w, h, TS(C_BORDER, C_DEFAULT, 0), title);
    if (iw <= tw + 4)
        return;

    /* dal piu' recente (in basso) verso l'alto */
    for (k = 0; k < g_log_count && row >= top; k++) {
        const log_entry *e = &g_log[(g_log_next - 1 - k + LOG_MAX) % LOG_MAX];
        tstyle base = TS(C_TEXT, C_DEFAULT, 0);
        char t[32];
        int lines, start;

        if (e->rule) {
            tui_hline(ix, row, iw, TS(C_FAINT, C_DEFAULT, 0), "─");
            tui_print_center(ix, row, iw, TS(C_DIM, C_DEFAULT, 0), " %s ", e->text);
            row--;
            continue;
        }
        lines = draw_wrapped(ix + tw, 0, iw - tw, base, e->text, 0);
        start = row - lines + 1;
        if (start < top)
            break;
        fmt_time(e->t, t, sizeof(t));
        tui_put(ix, start, TS(C_FAINT, C_DEFAULT, 0), t);
        draw_wrapped(ix + tw, start, iw - tw, base, e->text, 1);
        row = start - 1;
    }
}

/* ------------------------------------------------------------------ */
/* Widget RDT                                                         */
/* ------------------------------------------------------------------ */

/* Caratteri non ASCII ammessi: solo alfabeti a una colonna (accenti ecc.) */
static int narrow_cp(unsigned cp)
{
    return (cp >= 0xA0 && cp <= 0x24F && cp != 0xAD) ||
           (cp >= 0x370 && cp <= 0x52F) ||
           (cp >= 0x2010 && cp <= 0x2027) ||
           cp == 0x20AC;
}

int tui_payload(const uint8_t *d, size_t len, char *out, size_t outsz, int maxcols)
{
    size_t i = 0, o = 0, last = 0;
    int cols = 0;

    if (outsz < 8) {
        if (outsz)
            out[0] = '\0';
        return 0;
    }
    while (i < len && cols < maxcols) {
        unsigned c = d[i];
        char tmp[5];
        size_t n = 0, adv = 1;
        const char *rep = NULL;

        if (c >= 0x20 && c < 0x7F) {
            tmp[0] = (char)c;
            n = 1;
        } else if (c == '\n') {
            rep = "↵";
        } else if (c == '\t') {
            tmp[0] = ' ';
            n = 1;
        } else if (c >= 0xC0) {
            size_t need = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            unsigned cp = need == 2 ? (c & 0x1F) : need == 3 ? (c & 0x0F) : (c & 0x07);
            int ok = need > 0 && i + need <= len;
            size_t k;
            for (k = 1; ok && k < need; k++) {
                if ((d[i + k] & 0xC0) != 0x80)
                    ok = 0;
                else
                    cp = cp << 6 | (d[i + k] & 0x3F);
            }
            if (ok && narrow_cp(cp)) {
                memcpy(tmp, d + i, need);
                n = adv = need;
            } else {
                rep = "·";
            }
        } else {
            rep = "·";                  /* byte di controllo o UTF-8 non valido */
        }
        if (rep) {
            n = strlen(rep);
            memcpy(tmp, rep, n);
        }
        if (o + n + 4 > outsz)
            break;
        last = o;
        memcpy(out + o, tmp, n);
        o += n;
        i += adv;
        cols++;
    }
    if (i < len && cols > 0) {          /* troncato: l'ultimo carattere diventa "…" */
        o = last;
        memcpy(out + o, "…", 3);
        o += 3;
    }
    out[o] = '\0';
    return cols;
}

const char *tui_pkt_name(const rdt_packet *p, char *buf, size_t n)
{
    char pay[96];

    switch (p->type) {
    case PKT_DATA:
        tui_payload(p->data, p->len, pay, sizeof(pay), 16);
        snprintf(buf, n, T_DATA T_BOLD "DATA %u" T_RESET " " T_DIM "«" T_RESET "%s" T_DIM "»" T_RESET,
                 (unsigned)p->seq, pay);
        break;
    case PKT_ACK:
        snprintf(buf, n, T_ACK T_BOLD "ACK %u" T_RESET, (unsigned)p->seq);
        break;
    case PKT_FIN:
        snprintf(buf, n, T_FIN T_BOLD "FIN %u" T_RESET, (unsigned)p->seq);
        break;
    default:
        snprintf(buf, n, T_WARN T_BOLD "??? %u" T_RESET, (unsigned)p->seq);
        break;
    }
    return buf;
}

void tui_log_fate(const ch_flight *f)
{
    char d[256];

    tui_pkt_name(&f->pkt, d, sizeof(d));
    if (f->fate == FATE_LOST)
        tui_log(T_CHAN "✗ canale" T_RESET ": %s perso lungo il percorso", d);
    else if (f->fate == FATE_CORRUPT)
        tui_log(T_CHAN "↯ canale" T_RESET ": %s corrotto " T_DIM "(invertito il bit %d di %d)",
                d, f->flipped_bit, (RDT_HEADER_SIZE + f->pkt.len) * 8);
}

void tui_text_append(tui_textbuf *t, const uint8_t *data, size_t len, int seg)
{
    size_t outsz = len * 4 + 8;
    char *buf = malloc(outsz);
    const char *s;

    if (!buf)
        return;
    tui_payload(data, len, buf, outsz, (int)len + 1);
    for (s = buf; *s; ) {
        int n = cp_len(s);
        if (t->n == t->cap) {
            int cap = t->cap ? t->cap * 2 : 256;
            tui_glyph *g = realloc(t->g, (size_t)cap * sizeof(*g));
            if (!g)
                break;
            t->g = g;
            t->cap = cap;
        }
        memcpy(t->g[t->n].ch, s, (size_t)n);
        t->g[t->n].ch[n] = '\0';
        t->g[t->n].seg = seg;
        t->n++;
        s += n;
    }
    free(buf);
}

void tui_text_clear(tui_textbuf *t)
{
    t->n = 0;
}

static int is_space(const tui_glyph *g)
{
    return g->ch[0] == ' ' && g->ch[1] == '\0';
}

/* Dispone i caratteri da start in poi andando a capo sulle parole.
 * Ritorna 1 se il carattere focus rientra nelle h righe. */
static int text_layout(int x, int y, int w, int h, const tui_textbuf *t, int start, int focus,
                       tui_seg_style style, const void *ctx, int draw)
{
    int i, col = 0, row = 0, seen = 0;

    for (i = start; i < t->n && row < h; i++) {
        const tui_glyph *g = &t->g[i];

        if (is_space(g) && i > start) {
            int j, ww = 0;
            if (col == 0)
                continue;                   /* niente spazi a inizio riga */
            for (j = i + 1; j < t->n && !is_space(&t->g[j]); j++)
                ww++;
            if (col + 1 + ww > w && ww <= w) {
                col = 0;
                row++;
                continue;
            }
        }
        if (col >= w) {
            col = 0;
            if (++row >= h)
                break;
        }
        if (draw) {
            if (i == start && start > 0)
                tui_put(x + col, y + row, TS(C_DIM, C_DEFAULT, 0), "…");
            else
                tui_put(x + col, y + row, style(g->seg, ctx), g->ch);
        }
        if (i == focus)
            seen = 1;
        col++;
    }
    return seen;
}

void tui_draw_text(int x, int y, int w, int h, const tui_textbuf *t, int focus,
                   tui_seg_style style, const void *ctx)
{
    int cap = w * h, start = 0, step = w / 2 > 0 ? w / 2 : 1;

    if (w <= 0 || h <= 0)
        return;
    if (t->n > cap) {
        start = focus - cap / 3;
        if (start > t->n - cap)
            start = t->n - cap;
        if (start < 0)
            start = 0;
    }
    /* andando a capo sulle parole ci stanno meno caratteri: si scorre finche'
     * il carattere da tenere in vista non entra nel riquadro */
    while (start < focus && !text_layout(x, y, w, h, t, start, focus, style, ctx, 0))
        start += step;
    if (start > focus)
        start = focus;
    text_layout(x, y, w, h, t, start, focus, style, ctx, 1);
}

void tui_draw_header(int y, int w, const char *protocol, rdt_role role,
                     const char *line1, const char *line2)
{
    int sender = role == ROLE_SENDER;
    short acc = sender ? C_DATA : C_ACK;
    const char *name = sender ? "SENDER" : "RECEIVER";
    int cw = (int)strlen(name) + 4;

    /* titolo della finestra: a meta' schermo lo sceglie demo.sh, la console non ce l'ha */
    if (!g_title_set && g_pane == PANE_NONE && !g_basic) {
        char t[128];
        snprintf(t, sizeof(t), "\033]0;RDT %s - %s\007", protocol, name);
        out_str(t);
        g_title_set = 1;
    }
    tui_box(0, y, w, 4, TS(acc, C_DEFAULT, 0), NULL);
    tui_print(2, y, w - cw - 6, TS(C_BRIGHT, C_DEFAULT, TA_BOLD), " ◆ RDT · %s ", protocol);
    tui_print(w - 3 - cw, y, cw, TS(16, acc, TA_BOLD), "  %s  ", name);
    tui_print(2, y + 1, w - 4, TS(C_TEXT, C_DEFAULT, 0), "%s", line1);
    tui_print(2, y + 2, w - 4, TS(C_DIM, C_DEFAULT, 0), "%s", line2);
}

void tui_draw_footer(int y, int w, const char *markup)
{
    tui_fill(0, y, w, 1, TS(C_DIM, C_PANEL, 0));
    tui_print(1, y, w - 2, TS(C_DIM, C_PANEL, 0), "%s", markup);
}

/* ---- canale ---- */

typedef struct {
    char      text[48];
    int       kind;
    long long until;
} flash_t;

static flash_t g_flash[2];

void tui_channel_flash(ch_dir lane, int kind, const char *text)
{
    flash_t *f = &g_flash[lane == TO_SENDER];

    snprintf(f->text, sizeof(f->text), "%s", text);
    f->kind  = kind;
    f->until = rdt_now_ms() + 1100;
}

static tstyle host_border(int self, short acc)
{
    return self ? TS(acc, C_DEFAULT, TA_BOLD) : TS(C_FAINT, C_DEFAULT, 0);
}

static void host_box(int x, int y, int w, const char *name, int self, short acc, const char *note)
{
    tui_box(x, y, w, 5, host_border(self, acc), NULL);
    tui_print_center(x + 1, y + 1, w - 2,
                     self ? TS(acc, C_DEFAULT, TA_BOLD) : TS(C_DIM, C_DEFAULT, TA_BOLD), "%s", name);
    tui_print_center(x + 1, y + 2, w - 2,
                     self ? TS(C_TEXT, C_DEFAULT, 0) : TS(C_FAINT, C_DEFAULT, 0),
                     "%s", self ? "◉ questo" : "remoto");
    if (self && note)
        tui_print_center(x + 1, y + 3, w - 2, TS(C_DIM, C_DEFAULT, 0), "%s", note);
}

static tstyle chip_style(int type)
{
    switch (type) {
    case PKT_DATA: return TS(231, C_DATA_BG, TA_BOLD);
    case PKT_ACK:  return TS(231, C_ACK_BG, TA_BOLD);
    case PKT_FIN:  return TS(231, C_FIN_BG, TA_BOLD);
    default:       return TS(16, C_WARN_BG, TA_BOLD);
    }
}

static void chip_label(const rdt_packet *p, int maxpay, int damaged, char *buf, size_t n)
{
    const char *mark = damaged ? "↯ " : "";
    char pay[96];

    if (p->type == PKT_DATA && maxpay > 0 && p->len > 0) {
        tui_payload(p->data, p->len, pay, sizeof(pay), maxpay);
        snprintf(buf, n, " %sDATA %u │ %s ", mark, (unsigned)p->seq, pay);
    } else {
        snprintf(buf, n, " %s%s %u ", mark, rdt_type_name(p->type), (unsigned)p->seq);
    }
}

/* Formati dell'etichetta di un pacchetto in volo, dal piu' ricco al piu' corto:
 * con piu' pacchetti nella stessa corsia si usa il primo che non si sovrappone. */
enum { LBL_FULL, LBL_SHORT, LBL_TINY };

static int flight_lost(const ch_flight *f)
{
    return f->outbound && f->fate == FATE_LOST && f->fate_reported;
}

static void flight_label(const ch_flight *f, int variant, int maxpay, long long now,
                         char *buf, size_t n, tstyle *st)
{
    int damaged = f->outbound && f->fate == FATE_CORRUPT && f->fate_reported;
    const rdt_packet *p = damaged ? &f->damaged : &f->pkt;
    unsigned seq = (unsigned)p->seq;

    if (flight_lost(f)) {
        *st = now - f->fate_time < 700 ? TS(231, C_ERR_BG, TA_BOLD) : TS(C_ERR, C_DEFAULT, TA_BOLD);
        if (variant == LBL_FULL)
            snprintf(buf, n, " ✗ %s %u perso ", rdt_type_name(p->type), seq);
        else if (variant == LBL_SHORT)
            snprintf(buf, n, " ✗ %s %u ", rdt_type_name(p->type), seq);
        else
            snprintf(buf, n, "✗%u ", seq);
        return;
    }
    *st = damaged ? TS(231, C_ERR_BG, TA_BOLD) : chip_style(p->type);
    if (variant == LBL_FULL)
        chip_label(p, maxpay, damaged, buf, n);
    else if (variant == LBL_SHORT)
        snprintf(buf, n, " %s%s %u ", damaged ? "↯ " : "", rdt_type_name(p->type), seq);
    else
        snprintf(buf, n, damaged ? "↯%u " : " %u ", seq);
}

/* Colonna di partenza dell'etichetta lungo la corsia */
static int flight_pos(const ch_flight *f, int cw, int lx0, int lx1, int L, long long now)
{
    double p = flight_lost(f) ? f->fate_pos : ch_progress(f, now);
    int span = L - cw;

    return f->dir == TO_RECEIVER ? lx0 + (int)(p * span + 0.5)
                                 : lx1 - cw + 1 - (int)(p * span + 0.5);
}

/* Primo formato con cui i pacchetti in movimento sulla corsia non si toccano */
static int lane_variant(const ch_flight *const *order, int n, ch_dir lane, int maxpay,
                        int lx0, int lx1, int L, long long now)
{
    int v;

    for (v = LBL_FULL; v < LBL_TINY; v++) {
        int pos[CH_MAX_FLIGHTS], wid[CH_MAX_FLIGHTS], m = 0, i, j, ok = 1;

        for (i = 0; i < n; i++) {
            char label[160];
            tstyle st;
            int cw;
            if (order[i]->dir != lane || flight_lost(order[i]))
                continue;
            flight_label(order[i], v, maxpay, now, label, sizeof(label), &st);
            cw = tui_text_width(label);
            if (cw > L)
                cw = L;
            pos[m] = flight_pos(order[i], cw, lx0, lx1, L, now);
            wid[m] = cw;
            /* inserimento ordinato per colonna */
            for (j = m; j > 0 && pos[j - 1] > pos[j]; j--) {
                int tp = pos[j], tw = wid[j];
                pos[j] = pos[j - 1]; wid[j] = wid[j - 1];
                pos[j - 1] = tp;     wid[j - 1] = tw;
            }
            m++;
        }
        for (i = 0; i + 1 < m && ok; i++)
            if (pos[i] + wid[i] + 1 > pos[i + 1])     /* almeno una colonna libera fra due */
                ok = 0;
        if (ok)
            return v;
    }
    return LBL_TINY;
}

static int cmp_flight(const void *a, const void *b)
{
    const ch_flight *fa = *(const ch_flight *const *)a;
    const ch_flight *fb = *(const ch_flight *const *)b;

    return (fa->t_start > fb->t_start) - (fa->t_start < fb->t_start);
}

void tui_draw_channel(int x, int y, int w, const channel *ch, rdt_role self,
                      long long now, const char *self_note)
{
    const ch_flight *order[CH_MAX_FLIGHTS];
    int ix = x + 1, iw = w - 2;
    int hb = iw >= 72 ? 12 : 10;                 /* larghezza dei box degli host */
    int lbx = ix + 1, rbx = ix + iw - 1 - hb;
    int lx0 = lbx + hb, lx1 = rbx - 1;           /* colonne della corsia */
    int L = lx1 - lx0 + 1;
    int r_top = y + 1, r_data = y + 2, r_mid = y + 3, r_ack = y + 4, r_bot = y + 5;
    int is_s = self == ROLE_SENDER;
    int maxpay = L >= 56 ? 10 : L >= 40 ? 6 : 0;
    int variant[2];
    int i, n = 0, lane;

    tui_box(x, y, w, 7, TS(C_BORDER, C_DEFAULT, 0), "Canale UDP inaffidabile (simulato)");
    host_box(lbx, r_top, hb, "SENDER", is_s, C_DATA, self_note);
    host_box(rbx, r_top, hb, "RECEIVER", !is_s, C_ACK, self_note);
    if (L < 12)
        return;

    /* corsie e frecce */
    tui_hline(lx0, r_data, L, TS(C_FAINT, C_DEFAULT, 0), "┄");
    tui_hline(lx0, r_ack, L, TS(C_FAINT, C_DEFAULT, 0), "┄");
    tui_put(lx1, r_data, TS(C_DATA, C_DEFAULT, 0), "▶");
    tui_put(lx0, r_ack, TS(C_ACK, C_DEFAULT, 0), "◀");
    tui_put(lbx + hb - 1, r_data, host_border(is_s, C_DATA), "├");
    tui_put(lbx + hb - 1, r_ack, host_border(is_s, C_DATA), "├");
    tui_put(rbx, r_data, host_border(!is_s, C_ACK), "┤");
    tui_put(rbx, r_ack, host_border(!is_s, C_ACK), "┤");

    tui_print_center(lx0, r_top, L, TS(C_FAINT, C_DEFAULT, 0), "dati  " T_DATA "DATA ──▶");
    tui_print_center(lx0, r_bot, L, TS(C_FAINT, C_DEFAULT, 0), T_ACK "◀── ACK" T_RESET "  riscontri");
    tui_print_center(lx0, r_mid, L, TS(C_FAINT, C_DEFAULT, 0), "UDP · ritardo %.1f s", ch->delay_ms / 1000.0);

    /* pacchetti in volo, dal piu' vecchio al piu' recente */
    for (i = 0; i < CH_MAX_FLIGHTS; i++)
        if (ch->fl[i].active)
            order[n++] = &ch->fl[i];
    qsort(order, (size_t)n, sizeof(order[0]), cmp_flight);
    variant[TO_RECEIVER] = lane_variant(order, n, TO_RECEIVER, maxpay, lx0, lx1, L, now);
    variant[TO_SENDER]   = lane_variant(order, n, TO_SENDER, maxpay, lx0, lx1, L, now);

    /* etichette e posizioni; i segni delle perdite restano fermi dove il
     * pacchetto si e' perso e spariscono mentre un altro ci passa sopra */
    {
        static char label[CH_MAX_FLIGHTS][160];
        tstyle st[CH_MAX_FLIGHTS];
        int pos[CH_MAX_FLIGHTS], cw[CH_MAX_FLIGHTS], j;

        for (i = 0; i < n; i++) {
            flight_label(order[i], variant[order[i]->dir], maxpay, now, label[i], sizeof(label[i]), &st[i]);
            cw[i] = tui_text_width(label[i]);
            if (cw[i] > L)
                cw[i] = L;
            pos[i] = flight_pos(order[i], cw[i], lx0, lx1, L, now);
        }
        for (i = 0; i < n; i++) {
            int hidden = 0;
            if (!flight_lost(order[i]))
                continue;
            for (j = 0; j < n && !hidden; j++)
                if (!flight_lost(order[j]) && order[j]->dir == order[i]->dir &&
                    pos[j] < pos[i] + cw[i] + 1 && pos[i] < pos[j] + cw[j] + 1)
                    hidden = 1;
            if (!hidden)
                tui_print(pos[i], order[i]->dir == TO_RECEIVER ? r_data : r_ack, cw[i], st[i], "%s", label[i]);
        }
        for (i = 0; i < n; i++)
            if (!flight_lost(order[i]))
                tui_print(pos[i], order[i]->dir == TO_RECEIVER ? r_data : r_ack, cw[i], st[i], "%s", label[i]);
    }

    /* esito degli arrivi all'estremita' della corsia */
    for (lane = 0; lane < 2; lane++) {
        const flash_t *fl = &g_flash[lane];
        int row = lane == 0 ? r_data : r_ack;
        char t[160];
        int tw;
        tstyle st;

        if (now >= fl->until)
            continue;
        st = fl->kind == FLASH_OK   ? TS(16, C_OK, TA_BOLD)
           : fl->kind == FLASH_WARN ? TS(16, C_WARN_BG, TA_BOLD)
                                    : TS(231, C_ERR_BG, TA_BOLD);
        snprintf(t, sizeof(t), " %s ", fl->text);
        tw = tui_text_width(t);
        tui_print(lane == 0 ? lx1 - tw + 1 : lx0, row, L, st, "%s", t);
    }
}

/* ---- regole del protocollo ---- */

void tui_draw_rules(int x, int y, int w, const char *title, const tui_rule *rules, int n, int active)
{
    int ix = x + 2, iw = w - 4, wcol = 0, i;

    tui_box(x, y, w, n + 2, TS(C_BORDER, C_DEFAULT, 0), title);
    for (i = 0; i < n; i++) {
        int tw = tui_text_width(rules[i].when);
        if (tw > wcol)
            wcol = tw;
    }
    wcol += 2;
    if (wcol > iw / 2)
        wcol = iw / 2;

    for (i = 0; i < n; i++) {
        int row = y + 1 + i, on = i == active;
        tstyle when = on ? TS(C_BRIGHT, C_SELECT, TA_BOLD) : TS(C_TEXT, C_DEFAULT, 0);
        tstyle then = on ? TS(C_BRIGHT, C_SELECT, 0) : TS(C_DIM, C_DEFAULT, 0);

        if (on)
            tui_fill(ix - 1, row, iw + 2, 1, TS(C_DEFAULT, C_SELECT, 0));
        tui_print(ix, row, 2, then, "%s", rules[i].icon);
        tui_print(ix + 2, row, wcol, when, "%s", rules[i].when);
        tui_print(ix + 2 + wcol, row, 2, then, "→");
        tui_print(ix + 4 + wcol, row, iw - 4 - wcol, then, "%s", rules[i].then);
    }
}

/* ---- striscia dei numeri di sequenza ---- */

/* Puntatore sopra una cella: "nome ▼" a sinistra della freccia o "▼ nome" a
 * destra; se non c'e' spazio dalla parte preferita si usa l'altra. */
static void seq_pointer(int col, int row, int lo, int hi, const char *name, int left, tstyle st)
{
    int tw = tui_text_width(name) + 2;

    if (left && col - tw + 1 < lo)
        left = 0;
    else if (!left && col + tw - 1 > hi)
        left = 1;
    if (left)
        tui_print(col - tw + 1, row, tw, st, "%s ▼", name);
    else
        tui_print(col, row, tw, st, "▼ %s", name);
}

/* Parentesi sotto le celle [lo, lo+n): lati verticali sulla riga delle celle
 * e di cio' che sta sotto, base con l'etichetta. */
static void seq_bracket(int cx0, int pitch, int cw, long first, long lo, int n, int ytop,
                        int xmax, int heavy, tstyle st, const char *label)
{
    int lc, rc, clipped, inner, tw;

    if (n <= 0)
        return;
    if (lo < first) {
        n -= (int)(first - lo);
        lo = first;
        if (n <= 0)
            return;
    }
    lc = cx0 + (int)(lo - first) * pitch - 1;
    rc = cx0 + (int)(lo + n - 1 - first) * pitch + cw;
    if (lc > xmax)
        return;
    clipped = rc > xmax;
    if (clipped)
        rc = xmax + 1;

    tui_vline(lc, ytop, 2, st, heavy ? "┃" : "│");
    tui_put(lc, ytop + 2, st, heavy ? "┗" : "╰");
    tui_hline(lc + 1, ytop + 2, rc - lc - 1, st, heavy ? "━" : "─");
    if (!clipped) {
        tui_vline(rc, ytop, 2, st, heavy ? "┃" : "│");
        tui_put(rc, ytop + 2, st, heavy ? "┛" : "╯");
    }
    if (!label)
        return;
    inner = rc - lc - 1;
    tw = tui_text_width(label) + 2;
    if (tw <= inner)
        tui_print_center(lc + 1, ytop + 2, inner, st, " %s ", label);
    else if (!clipped && rc + 2 + tw - 2 <= xmax + 1)
        tui_print(rc + 2, ytop + 2, xmax - rc - 1, st, "%s", label);
}

void tui_draw_seqstrip(int x, int y, int w, const char *title, const tui_seqstrip *s)
{
    int ix = x + 2, iw = w - 4, xmax = ix + iw - 1;
    int digits = 1, cw, pitch, cx0, ncells, k, m;
    long first, last, margin;
    tstyle acc = TS(s->accent, C_DEFAULT, TA_BOLD);
    tstyle dim = TS(C_DIM, C_DEFAULT, 0);

    tui_box(x, y, w, 7, TS(C_BORDER, C_DEFAULT, 0), title);
    for (m = s->modulo - 1; m >= 10; m /= 10)
        digits++;
    cw = digits + 2;
    pitch = cw + 1;
    cx0 = ix + 2;
    ncells = (iw - 4) / pitch;
    if (ncells < 1)
        return;

    /* un po' di storia a sinistra della finestra, il resto a destra */
    margin = (ncells - s->win_n) / 3;
    if (s->shadow_n > 0 && margin < s->shadow_n)
        margin = s->shadow_n;
    if (margin > ncells - s->win_n)
        margin = ncells - s->win_n;
    if (margin < 1)
        margin = 1;
    first = s->win_lo - margin;
    if (s->count >= 0 && first > s->count - ncells)
        first = s->count - ncells;
    if (first < 0)
        first = 0;
    last = first + ncells;
    if (s->count >= 0 && last > s->count)
        last = s->count;

    if (first > 0)
        tui_put(ix, y + 2, dim, "…");
    if (s->count < 0 || last < s->count)
        tui_put(cx0 + (int)(last - first) * pitch, y + 2, dim, "…");

    /* celle */
    for (k = 0; first + k < last; k++) {
        long idx = first + k;
        int cx = cx0 + k * pitch;
        tui_seqcell c;

        c.st = SC_UNUSABLE;
        c.below = NULL;
        c.timer = -1.0;
        s->cell(idx, &c, s->ctx);
        tui_fill(cx, y + 2, cw, 1, c.st);
        tui_print_center(cx, y + 2, cw, c.st, "%ld", idx % s->modulo);
        if (c.timer >= 0.0) {
            double t = c.timer > 1.0 ? 1.0 : c.timer;
            tui_bar(cx, y + 3, cw, t, t < 0.5 ? C_OK : t < 0.8 ? C_WARN : C_ERR);
        } else if (c.below) {
            tui_print_center(cx, y + 3, cw, dim, "%s", c.below);
        }
    }

    /* parentesi: prima quella tenue, poi la finestra che ne copre il bordo comune */
    seq_bracket(cx0, pitch, cw, first, s->shadow_lo, s->shadow_n, y + 2, xmax, 0,
                TS(C_DIM, C_DEFAULT, 0), s->shadow_label);
    seq_bracket(cx0, pitch, cw, first, s->win_lo, s->win_n, y + 2, xmax, 1, acc, s->win_label);

    /* puntatori */
    if (s->ptr_left >= first && s->ptr_left <= last)
        seq_pointer(cx0 + (int)(s->ptr_left - first) * pitch + cw / 2, y + 1, ix, xmax,
                    s->ptr_left_name, 1, acc);
    if (s->ptr_right >= first && s->ptr_right <= last)
        seq_pointer(cx0 + (int)(s->ptr_right - first) * pitch + cw / 2, y + 1, ix, xmax,
                    s->ptr_right_name, 0, acc);

    /* legenda: quelle che non ci stanno si perdono dalla fine */
    {
        int lx = ix;
        for (k = 0; k < s->nlegend; k++) {
            const tui_legend *l = &s->legend[k];
            int need = 3 + tui_text_width(l->label);
            if (lx + need - 1 > xmax)
                break;
            if (l->st.bg < 0)
                tui_put(lx, y + 5, l->st, "··");
            else
                tui_fill(lx, y + 5, 2, 1, l->st);
            tui_print(lx + 3, y + 5, need - 3, dim, "%s", l->label);
            lx += need + 3;
        }
    }
}
