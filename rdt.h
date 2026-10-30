/*
 * rdt.h - Definizioni comuni ai protocolli di trasferimento affidabile
 *         (stop-and-wait, go-back-N, selective repeat) costruiti su UDP.
 *
 * Il numero di sequenza viaggia in un campo a 32 bit, ma i protocolli a
 * finestra usano solo i valori 0..2^k-1 (k = opzione -b), come nei libri.
 *
 * Formato del pacchetto sul filo (big endian, 12 byte di intestazione):
 *
 *    0      1      2             4                           8            10           12
 *    +------+------+-------------+---------------------------+------------+------------+---------
 *    | tipo | flag | lunghezza   | numero di sequenza        | checksum   | riservato  | payload
 *    +------+------+-------------+---------------------------+------------+------------+---------
 *
 * Il checksum e' quello di Internet (complemento a uno della somma a 16 bit
 * in complemento a uno) calcolato su intestazione e payload: rileva sempre
 * l'inversione di un singolo bit.
 */
#ifndef RDT_H
#define RDT_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#define RDT_DEFAULT_HOST     "127.0.0.1"
#define RDT_DEFAULT_PORT     8142
#define RDT_DEFAULT_LOSS     0.15
#define RDT_DEFAULT_CORRUPT  0.10
#define RDT_DEFAULT_DELAY_MS 1000
#define RDT_DEFAULT_CHUNK    10
#define RDT_DEFAULT_WINDOW   4
#define RDT_DEFAULT_SEQBITS  3       /* numeri di sequenza 0..7 */

#define RDT_MAX_WINDOW       32
#define RDT_MAX_SEQBITS      8

#define RDT_HEADER_SIZE      12
#define RDT_MAX_PAYLOAD      1024
#define RDT_MAX_DATAGRAM     (RDT_HEADER_SIZE + RDT_MAX_PAYLOAD)

/* Tipi di pacchetto */
enum { PKT_DATA = 1, PKT_ACK = 2, PKT_FIN = 3 };

typedef struct {
    uint8_t  type;
    uint16_t len;                    /* byte di payload */
    uint32_t seq;                    /* numero di sequenza (o di riscontro) */
    uint16_t checksum;               /* valore letto dal filo */
    uint8_t  data[RDT_MAX_PAYLOAD];
} rdt_packet;

/* Checksum di Internet su un buffer */
uint16_t rdt_checksum(const uint8_t *buf, size_t len);

/* Serializza il pacchetto in buf calcolando il checksum; ritorna i byte scritti */
size_t rdt_encode(const rdt_packet *p, uint8_t *buf);

/* Deserializza buf in p. Ritorna 1 se il pacchetto e' integro, 0 se corrotto
 * (in quel caso p contiene comunque i campi letti, utili solo per mostrarli). */
int rdt_decode(const uint8_t *buf, size_t len, rdt_packet *p);

const char *rdt_type_name(int type);

/* ------------------------------------------------------------------ */
/* Opzioni da riga di comando                                         */
/* ------------------------------------------------------------------ */

typedef enum { ROLE_SENDER, ROLE_RECEIVER } rdt_role;

/* Opzioni accettate oltre a quelle comuni */
#define RDT_OPT_WINDOW   1u          /* -w: dimensione della finestra */
#define RDT_OPT_SEQBITS  2u          /* -b: bit del numero di sequenza */
#define RDT_OPT_INTERVAL 4u          /* -i: intervallo fra due invii (sender) */

/* Descrizione del programma, per l'aiuto e le opzioni accettate */
typedef struct {
    const char *name;                /* es. "go-back-N" */
    const char *message;             /* sender: testo inviato di default */
    unsigned    opts;                /* RDT_OPT_* */
    const char *note;                /* riga aggiuntiva dell'aiuto, o NULL */
} rdt_protocol;

typedef struct {
    const char *host;        /* sender: receiver da contattare; receiver: indirizzo di ascolto */
    int         port;
    double      loss;        /* probabilita' che il canale perda un pacchetto in uscita */
    double      corrupt;     /* probabilita' che il canale inverta un bit di un pacchetto in uscita */
    int         delay_ms;    /* ritardo di propagazione simulato (una direzione) */
    int         timeout_ms;  /* solo sender */
    int         timeout_set; /* 1 se il timeout e' stato scelto dall'utente */
    unsigned    seed;
    const char *message;     /* solo sender: testo da inviare */
    const char *file;        /* solo sender: file da inviare */
    int         chunk;       /* solo sender: byte di payload per pacchetto */
    int         window;      /* finestra N (protocolli a finestra) */
    int         seq_bits;    /* numeri di sequenza 0..2^seq_bits-1 */
    int         interval_ms; /* sender a finestra: intervallo minimo fra due invii */
} rdt_options;

/* Legge le opzioni; in caso di errore o -h stampa l'aiuto ed esce. */
void rdt_parse_options(int argc, char **argv, rdt_role role,
                       const rdt_protocol *proto, rdt_options *o);

/* ------------------------------------------------------------------ */
/* Dati dell'applicazione                                             */
/* ------------------------------------------------------------------ */

/* Testo da inviare diviso in segmenti di al piu' opt.chunk byte, senza
 * spezzare i caratteri UTF-8. Il FIN e' il pacchetto di indice nseg. */
typedef struct {
    uint8_t  *text;
    size_t    len;
    size_t   *off;           /* inizio di ciascun segmento */
    uint16_t *seg_len;       /* lunghezza di ciascun segmento */
    int       nseg;
} rdt_data;

void rdt_load_data(const rdt_options *o, rdt_data *d);    /* esce in caso di errore */
void rdt_free_data(rdt_data *d);

/* Pacchetto DATA del segmento idx, oppure FIN se idx == nseg */
void rdt_make_packet(const rdt_data *d, int idx, uint32_t seq, rdt_packet *p);

/* Buffer dei dati consegnati all'applicazione (receiver) */
typedef struct {
    uint8_t *data;
    size_t   len, cap;
} rdt_buffer;

int rdt_buffer_append(rdt_buffer *b, const void *data, size_t n);   /* 0 = ok */

/* ------------------------------------------------------------------ */
/* Rete                                                               */
/* ------------------------------------------------------------------ */

/* Risolve un nome o un indirizzo IPv4; ritorna 0 se ok, -1 altrimenti */
int  rdt_resolve(const char *host, int port, struct sockaddr_in *out);

/* Crea un socket UDP non bloccante; se bind_addr != NULL lo associa a quell'indirizzo */
int  rdt_udp_socket(const struct sockaddr_in *bind_addr);

void rdt_addr_str(const struct sockaddr_in *a, char *buf, size_t size);
int  rdt_addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b);

/* Sender: risolve il receiver e apre un socket su una porta effimera
 * (scelta subito, per poterla mostrare). Esce in caso di errore. */
int  rdt_open_sender(const rdt_options *o, struct sockaddr_in *dst,
                     char *local_s, char *remote_s, size_t size);

/* Receiver: apre il socket di ascolto su opt.host:opt.port. Esce in caso di errore. */
int  rdt_open_receiver(const rdt_options *o, struct sockaddr_in *local,
                       char *local_s, size_t size);

/* ------------------------------------------------------------------ */
/* Tempo e numeri casuali                                             */
/* ------------------------------------------------------------------ */

long long rdt_now_ms(void);          /* orologio monotono in millisecondi */
void      rdt_srand(unsigned seed);
double    rdt_rand01(void);          /* uniforme in [0, 1) */
unsigned  rdt_rand(unsigned n);      /* uniforme in [0, n) */

#endif
