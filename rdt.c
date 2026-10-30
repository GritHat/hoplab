/*
 * rdt.c - Pacchetti, checksum, opzioni, dati, socket e utilita' comuni.
 */
#include "rdt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Pacchetti                                                          */
/* ------------------------------------------------------------------ */

uint16_t rdt_checksum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)buf[i] << 8 | buf[i + 1];
    if (len & 1)
        sum += (uint32_t)buf[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

size_t rdt_encode(const rdt_packet *p, uint8_t *buf)
{
    uint16_t len = p->len > RDT_MAX_PAYLOAD ? RDT_MAX_PAYLOAD : p->len;
    uint16_t cs;

    buf[0]  = p->type;
    buf[1]  = 0;
    buf[2]  = (uint8_t)(len >> 8);
    buf[3]  = (uint8_t)len;
    buf[4]  = (uint8_t)(p->seq >> 24);
    buf[5]  = (uint8_t)(p->seq >> 16);
    buf[6]  = (uint8_t)(p->seq >> 8);
    buf[7]  = (uint8_t)p->seq;
    buf[8]  = 0;                      /* checksum calcolato con il campo a zero */
    buf[9]  = 0;
    buf[10] = 0;
    buf[11] = 0;
    memcpy(buf + RDT_HEADER_SIZE, p->data, len);

    cs = rdt_checksum(buf, RDT_HEADER_SIZE + (size_t)len);
    buf[8] = (uint8_t)(cs >> 8);
    buf[9] = (uint8_t)cs;
    return RDT_HEADER_SIZE + (size_t)len;
}

int rdt_decode(const uint8_t *buf, size_t len, rdt_packet *p)
{
    size_t avail;
    int ok;

    memset(p, 0, sizeof(*p));
    if (len < RDT_HEADER_SIZE)
        return 0;

    p->type     = buf[0];
    p->len      = (uint16_t)(buf[2] << 8 | buf[3]);
    p->seq      = (uint32_t)buf[4] << 24 | (uint32_t)buf[5] << 16 |
                  (uint32_t)buf[6] << 8 | buf[7];
    p->checksum = (uint16_t)(buf[8] << 8 | buf[9]);

    avail = len - RDT_HEADER_SIZE;
    if (avail > RDT_MAX_PAYLOAD)
        avail = RDT_MAX_PAYLOAD;
    memcpy(p->data, buf + RDT_HEADER_SIZE, avail);

    /* Sommando anche il checksum memorizzato si deve ottenere 0xFFFF,
     * cioe' rdt_checksum() sull'intero datagramma deve valere 0. */
    ok = rdt_checksum(buf, len) == 0 &&
         p->len == len - RDT_HEADER_SIZE &&
         (p->type == PKT_DATA || p->type == PKT_ACK || p->type == PKT_FIN);

    /* un campo lunghezza corrotto non deve far leggere oltre i dati presenti */
    if (p->len > avail)
        p->len = (uint16_t)avail;
    return ok;
}

const char *rdt_type_name(int type)
{
    switch (type) {
    case PKT_DATA: return "DATA";
    case PKT_ACK:  return "ACK";
    case PKT_FIN:  return "FIN";
    default:       return "???";
    }
}

/* ------------------------------------------------------------------ */
/* Opzioni                                                            */
/* ------------------------------------------------------------------ */

static void usage(const char *prog, rdt_role role, const rdt_protocol *proto)
{
    int s = role == ROLE_SENDER;
    unsigned x = proto->opts;

    printf("Uso: %s [opzioni] [host] [porta]\n\n", prog);
    printf("%s %s su UDP con canale inaffidabile simulato.\n\n",
           s ? "Sender" : "Receiver", proto->name);
    if (s) {
        printf("  -a, --host HOST      receiver da contattare            (default %s)\n", RDT_DEFAULT_HOST);
        printf("  -p, --port PORTA     porta UDP del receiver            (default %d)\n", RDT_DEFAULT_PORT);
    } else {
        printf("  -a, --host HOST      indirizzo su cui ascoltare        (default %s,\n", RDT_DEFAULT_HOST);
        printf("                       0.0.0.0 = tutte le interfacce)\n");
        printf("  -p, --port PORTA     porta UDP di ascolto              (default %d)\n", RDT_DEFAULT_PORT);
    }
    printf("  -l, --loss P         perdita %-26s(default %.0f%%)\n",
           s ? "dei DATA inviati" : "degli ACK inviati", RDT_DEFAULT_LOSS * 100);
    printf("  -c, --corrupt P      corruzione %-23s(default %.0f%%)\n",
           s ? "dei DATA" : "degli ACK", RDT_DEFAULT_CORRUPT * 100);
    printf("                       P è una probabilità: 0.2 oppure 20%%\n");
    printf("  -d, --delay MS       ritardo di propagazione simulato  (default %d ms)\n", RDT_DEFAULT_DELAY_MS);
    if (s)
        printf("  -t, --timeout MS     timeout di ritrasmissione         (default 2.5*ritardo + 500)\n");
    if (x & RDT_OPT_WINDOW)
        printf("  -w, --window N       dimensione della finestra         (default %d, max %d)\n",
               RDT_DEFAULT_WINDOW, RDT_MAX_WINDOW);
    if (x & RDT_OPT_SEQBITS)
        printf("  -b, --bits K         bit del numero di sequenza        (default %d: numeri 0…%d)\n",
               RDT_DEFAULT_SEQBITS, (1 << RDT_DEFAULT_SEQBITS) - 1);
    if (x & RDT_OPT_INTERVAL)
        printf("  -i, --interval MS    intervallo fra due invii          (default ritardo/4)\n");
    if (s) {
        printf("  -m, --message TESTO  testo da trasmettere\n");
        printf("  -f, --file FILE      trasmette il contenuto di un file\n");
        printf("  -s, --size N         byte di payload per pacchetto     (default %d)\n", RDT_DEFAULT_CHUNK);
    }
    printf("  -S, --seed N         seme casuale, per ripetere lo stesso scenario\n");
    printf("  -h, --help           mostra questo aiuto\n\n");
    if (proto->note)
        printf("%s\n\n", proto->note);
    printf("Esempi:\n");
    printf("  %s\n", prog);
    printf("  %s 192.168.1.20 9000\n", prog);
    if (s && (x & RDT_OPT_WINDOW))
        printf("  %s -w 6 -b 4 -l 0.3 -m \"ciao mondo\"\n", prog);
    else if (s)
        printf("  %s -l 0.3 -c 0 -d 600 -m \"ciao mondo\"\n", prog);
    else
        printf("  %s -a 0.0.0.0 -l 25%% -c 0\n", prog);
}

static double parse_prob(const char *s, const char *name)
{
    char *end;
    double v = strtod(s, &end);

    if (end == s) {
        fprintf(stderr, "Valore non valido per %s: %s\n", name, s);
        exit(EXIT_FAILURE);
    }
    if (*end == '%' || v > 1.0)
        v /= 100.0;
    if (v < 0.0 || v > 1.0) {
        fprintf(stderr, "%s deve essere compreso tra 0 e 1 (oppure 0%% e 100%%)\n", name);
        exit(EXIT_FAILURE);
    }
    return v;
}

static int parse_int(const char *s, const char *name, int min, int max)
{
    char *end;
    long v = strtol(s, &end, 10);

    if (end == s || *end != '\0' || v < min || v > max) {
        fprintf(stderr, "Valore non valido per %s: %s (ammesso %d..%d)\n", name, s, min, max);
        exit(EXIT_FAILURE);
    }
    return (int)v;
}

void rdt_parse_options(int argc, char **argv, rdt_role role,
                       const rdt_protocol *proto, rdt_options *o)
{
    static const struct option longopts[] = {
        { "host",     required_argument, NULL, 'a' },
        { "port",     required_argument, NULL, 'p' },
        { "loss",     required_argument, NULL, 'l' },
        { "corrupt",  required_argument, NULL, 'c' },
        { "delay",    required_argument, NULL, 'd' },
        { "timeout",  required_argument, NULL, 't' },
        { "window",   required_argument, NULL, 'w' },
        { "bits",     required_argument, NULL, 'b' },
        { "interval", required_argument, NULL, 'i' },
        { "message",  required_argument, NULL, 'm' },
        { "file",     required_argument, NULL, 'f' },
        { "size",     required_argument, NULL, 's' },
        { "seed",     required_argument, NULL, 'S' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    char optstring[64] = ":a:p:l:c:d:S:h";   /* ':' iniziale: errori gestiti qui */
    int interval_set = 0, c, li = 0;

    if (role == ROLE_SENDER)
        strcat(optstring, "t:m:f:s:");
    if (proto->opts & RDT_OPT_WINDOW)
        strcat(optstring, "w:");
    if (proto->opts & RDT_OPT_SEQBITS)
        strcat(optstring, "b:");
    if (proto->opts & RDT_OPT_INTERVAL)
        strcat(optstring, "i:");

    memset(o, 0, sizeof(*o));
    o->host     = RDT_DEFAULT_HOST;
    o->port     = RDT_DEFAULT_PORT;
    o->loss     = RDT_DEFAULT_LOSS;
    o->corrupt  = RDT_DEFAULT_CORRUPT;
    o->delay_ms = RDT_DEFAULT_DELAY_MS;
    o->chunk    = RDT_DEFAULT_CHUNK;
    o->window   = RDT_DEFAULT_WINDOW;
    o->seq_bits = RDT_DEFAULT_SEQBITS;
    o->message  = proto->message ? proto->message : "";
    o->seed     = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);

    opterr = 0;
    while ((c = getopt_long(argc, argv, optstring, longopts, &li)) != -1) {
        if (c == '?') {
            if (optopt)
                fprintf(stderr, "Opzione sconosciuta: -%c\n\n", optopt);
            else
                fprintf(stderr, "Opzione sconosciuta: %s\n\n", argv[optind - 1]);
            usage(argv[0], role, proto);
            exit(EXIT_FAILURE);
        }
        if (c == ':') {
            fprintf(stderr, "L'opzione %s richiede un argomento\n\n", argv[optind - 1]);
            usage(argv[0], role, proto);
            exit(EXIT_FAILURE);
        }
        /* opzione lunga esistente ma non prevista per questo programma */
        if (!strchr(optstring + 1, c)) {
            fprintf(stderr, "L'opzione --%s non è prevista per il %s %s\n\n", longopts[li].name,
                    role == ROLE_SENDER ? "sender" : "receiver", proto->name);
            usage(argv[0], role, proto);
            exit(EXIT_FAILURE);
        }
        switch (c) {
        case 'a': o->host = optarg; break;
        case 'p': o->port = parse_int(optarg, "--port", 1, 65535); break;
        case 'l': o->loss = parse_prob(optarg, "--loss"); break;
        case 'c': o->corrupt = parse_prob(optarg, "--corrupt"); break;
        case 'd': o->delay_ms = parse_int(optarg, "--delay", 0, 60000); break;
        case 't': o->timeout_ms = parse_int(optarg, "--timeout", 10, 600000); o->timeout_set = 1; break;
        case 'w': o->window = parse_int(optarg, "--window", 1, RDT_MAX_WINDOW); break;
        case 'b': o->seq_bits = parse_int(optarg, "--bits", 1, RDT_MAX_SEQBITS); break;
        case 'i': o->interval_ms = parse_int(optarg, "--interval", 0, 60000); interval_set = 1; break;
        case 'm': o->message = optarg; o->file = NULL; break;
        case 'f': o->file = optarg; break;
        case 's': o->chunk = parse_int(optarg, "--size", 1, RDT_MAX_PAYLOAD); break;
        case 'S': o->seed = (unsigned)parse_int(optarg, "--seed", 0, 2147483647); break;
        case 'h': usage(argv[0], role, proto); exit(EXIT_SUCCESS);
        default:  usage(argv[0], role, proto); exit(EXIT_FAILURE);
        }
    }

    /* Argomenti posizionali: [host] [porta] */
    if (optind < argc)
        o->host = argv[optind++];
    if (optind < argc)
        o->port = parse_int(argv[optind++], "porta", 1, 65535);
    if (optind < argc) {
        fprintf(stderr, "Argomento inatteso: %s\n\n", argv[optind]);
        usage(argv[0], role, proto);
        exit(EXIT_FAILURE);
    }

    /* Con piu' pacchetti in finestra che numeri di sequenza due pacchetti
     * in volo avrebbero lo stesso numero: non ha senso nemmeno come esperimento. */
    if ((proto->opts & RDT_OPT_WINDOW) && (proto->opts & RDT_OPT_SEQBITS) &&
        o->window > 1 << o->seq_bits) {
        fprintf(stderr, "La finestra (%d) supera i numeri di sequenza disponibili: con -b %d sono %d\n",
                o->window, o->seq_bits, 1 << o->seq_bits);
        exit(EXIT_FAILURE);
    }

    if (!o->timeout_set)
        o->timeout_ms = o->delay_ms * 5 / 2 + 500;
    if (!interval_set)
        o->interval_ms = o->delay_ms / 4;
}

/* ------------------------------------------------------------------ */
/* Dati dell'applicazione                                             */
/* ------------------------------------------------------------------ */

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);

    if (!q) {
        fprintf(stderr, "memoria esaurita\n");
        exit(EXIT_FAILURE);
    }
    return q;
}

void rdt_load_data(const rdt_options *o, rdt_data *d)
{
    size_t cap = 0, off = 0;
    int n = 0;

    memset(d, 0, sizeof(*d));
    if (o->file) {
        FILE *f = fopen(o->file, "rb");
        size_t r;
        if (!f) {
            fprintf(stderr, "Impossibile aprire %s: %s\n", o->file, strerror(errno));
            exit(EXIT_FAILURE);
        }
        do {
            if (d->len == cap) {
                cap = cap ? cap * 2 : 4096;
                d->text = xrealloc(d->text, cap);
            }
            r = fread(d->text + d->len, 1, cap - d->len, f);
            d->len += r;
        } while (r > 0);
        fclose(f);
    } else {
        d->len = strlen(o->message);
        d->text = xrealloc(NULL, d->len + 1);
        memcpy(d->text, o->message, d->len);
    }
    if (d->len == 0) {
        fprintf(stderr, "Nessun dato da inviare\n");
        exit(EXIT_FAILURE);
    }

    /* Segmenti di al massimo o->chunk byte, senza spezzare i caratteri UTF-8 */
    d->off     = xrealloc(NULL, (d->len + 1) * sizeof(*d->off));
    d->seg_len = xrealloc(NULL, (d->len + 1) * sizeof(*d->seg_len));
    while (off < d->len) {
        size_t len = d->len - off;
        if (len > (size_t)o->chunk)
            len = (size_t)o->chunk;
        while (len > 0 && off + len < d->len && (d->text[off + len] & 0xC0) == 0x80)
            len--;
        if (len == 0) {                /* carattere piu' lungo di un segmento */
            len = 1;
            while (off + len < d->len && (d->text[off + len] & 0xC0) == 0x80)
                len++;
        }
        d->off[n]     = off;
        d->seg_len[n] = (uint16_t)len;
        off += len;
        n++;
    }
    d->nseg = n;
}

void rdt_free_data(rdt_data *d)
{
    free(d->text);
    free(d->off);
    free(d->seg_len);
    memset(d, 0, sizeof(*d));
}

void rdt_make_packet(const rdt_data *d, int idx, uint32_t seq, rdt_packet *p)
{
    memset(p, 0, sizeof(*p));
    if (idx < d->nseg) {
        p->type = PKT_DATA;
        p->len  = d->seg_len[idx];
        memcpy(p->data, d->text + d->off[idx], p->len);
    } else {
        p->type = PKT_FIN;
    }
    p->seq = seq;
}

int rdt_buffer_append(rdt_buffer *b, const void *data, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        uint8_t *p;
        while (cap < b->len + n)
            cap *= 2;
        p = realloc(b->data, cap);
        if (!p)
            return -1;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rete                                                               */
/* ------------------------------------------------------------------ */

int rdt_resolve(const char *host, int port, struct sockaddr_in *out)
{
    struct addrinfo hints, *res;

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1)
        return 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return -1;
    out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

int rdt_udp_socket(const struct sockaddr_in *bind_addr)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int flags;

    if (fd < 0)
        return -1;
    if (bind_addr && bind(fd, (const struct sockaddr *)bind_addr, sizeof(*bind_addr)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    /* Non bloccante: il ciclo principale usa poll() per gestire anche timer e animazioni */
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

void rdt_addr_str(const struct sockaddr_in *a, char *buf, size_t size)
{
    char ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(buf, size, "%s:%u", ip, ntohs(a->sin_port));
}

int rdt_addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

int rdt_open_sender(const rdt_options *o, struct sockaddr_in *dst,
                    char *local_s, char *remote_s, size_t size)
{
    struct sockaddr_in any, local;
    socklen_t len = sizeof(local);
    int fd;

    if (rdt_resolve(o->host, o->port, dst) < 0) {
        fprintf(stderr, "Indirizzo non valido o non risolvibile: %s\n", o->host);
        exit(EXIT_FAILURE);
    }
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    fd = rdt_udp_socket(&any);
    if (fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    getsockname(fd, (struct sockaddr *)&local, &len);
    snprintf(local_s, size, "*:%u", ntohs(local.sin_port));
    rdt_addr_str(dst, remote_s, size);
    return fd;
}

int rdt_open_receiver(const rdt_options *o, struct sockaddr_in *local,
                      char *local_s, size_t size)
{
    int fd;

    if (rdt_resolve(o->host, o->port, local) < 0) {
        fprintf(stderr, "Indirizzo non valido o non risolvibile: %s\n", o->host);
        exit(EXIT_FAILURE);
    }
    fd = rdt_udp_socket(local);
    if (fd < 0) {
        fprintf(stderr, "Impossibile usare %s:%d: %s\n", o->host, o->port, strerror(errno));
        if (errno == EADDRINUSE)
            fprintf(stderr, "La porta è già occupata: c'è già un receiver in esecuzione? "
                            "Prova con un'altra porta (-p).\n");
        exit(EXIT_FAILURE);
    }
    rdt_addr_str(local, local_s, size);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Tempo e numeri casuali                                             */
/* ------------------------------------------------------------------ */

long long rdt_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* xorshift64*: piccolo, veloce e con il seme sotto controllo (scenari ripetibili) */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

void rdt_srand(unsigned seed)
{
    uint64_t z = (uint64_t)seed + 0x9E3779B97F4A7C15ULL;   /* splitmix64 */

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    rng_state = (z ^ (z >> 31)) | 1;
}

static uint64_t rng_next(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

double rdt_rand01(void)
{
    return (double)(rng_next() >> 11) * (1.0 / 9007199254740992.0);
}

unsigned rdt_rand(unsigned n)
{
    return n ? (unsigned)(rng_next() % n) : 0;
}
