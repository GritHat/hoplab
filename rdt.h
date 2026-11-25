/*
 * rdt.h - Definitions shared by the reliable data transfer protocols
 *         (stop-and-wait, go-back-N, selective repeat) built on UDP.
 *
 * The sequence number travels in a 32-bit field, but the windowed protocols
 * only use the values 0..2^k-1 (k = option -b), as in the textbooks.
 *
 * Packet format on the wire (big endian, 12-byte header):
 *
 *    0      1      2             4                           8            10           12
 *    +------+------+-------------+---------------------------+------------+------------+---------
 *    | type | flag | length      | sequence number           | checksum   | reserved   | payload
 *    +------+------+-------------+---------------------------+------------+------------+---------
 *
 * The checksum is the Internet one (one's complement of the 16-bit one's
 * complement sum) computed over header and payload: it always detects the
 * flip of a single bit.
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
#define RDT_DEFAULT_SEQBITS  3       /* sequence numbers 0..7 */

#define RDT_MAX_WINDOW       32
#define RDT_MAX_SEQBITS      8

#define RDT_HEADER_SIZE      12
#define RDT_MAX_PAYLOAD      1024
#define RDT_MAX_DATAGRAM     (RDT_HEADER_SIZE + RDT_MAX_PAYLOAD)

/* Packet types */
enum { PKT_DATA = 1, PKT_ACK = 2, PKT_FIN = 3 };

typedef struct {
    uint8_t  type;
    uint16_t len;                    /* payload bytes */
    uint32_t seq;                    /* sequence (or acknowledgement) number */
    uint16_t checksum;               /* value read from the wire */
    uint8_t  data[RDT_MAX_PAYLOAD];
} rdt_packet;

/* Internet checksum of a buffer */
uint16_t rdt_checksum(const uint8_t *buf, size_t len);

/* Serializes the packet into buf computing the checksum; returns the bytes written */
size_t rdt_encode(const rdt_packet *p, uint8_t *buf);

/* Deserializes buf into p. Returns 1 if the packet is intact, 0 if corrupted
 * (in that case p still holds the fields read, useful only to show them). */
int rdt_decode(const uint8_t *buf, size_t len, rdt_packet *p);

const char *rdt_type_name(int type);

/* ------------------------------------------------------------------ */
/* Command-line options                                               */
/* ------------------------------------------------------------------ */

typedef enum { ROLE_SENDER, ROLE_RECEIVER } rdt_role;

/* Options accepted in addition to the common ones */
#define RDT_OPT_WINDOW   1u          /* -w: window size */
#define RDT_OPT_SEQBITS  2u          /* -b: bits of the sequence number */
#define RDT_OPT_INTERVAL 4u          /* -i: interval between two transmissions (sender) */

/* Description of the program, for the help text and the accepted options */
typedef struct {
    const char *name;                /* e.g. "go-back-N" */
    const char *message;             /* sender: text sent by default */
    unsigned    opts;                /* RDT_OPT_* */
    const char *note;                /* extra line of the help text, or NULL */
} rdt_protocol;

typedef struct {
    const char *host;        /* sender: receiver to contact; receiver: address to listen on */
    int         port;
    double      loss;        /* probability that the channel loses an outgoing packet */
    double      corrupt;     /* probability that the channel flips a bit of an outgoing packet */
    int         delay_ms;    /* simulated propagation delay (one way) */
    int         timeout_ms;  /* sender only */
    int         timeout_set; /* 1 if the timeout was chosen by the user */
    unsigned    seed;
    const char *message;     /* sender only: text to send */
    const char *file;        /* sender only: file to send */
    int         chunk;       /* sender only: payload bytes per packet */
    int         window;      /* window N (windowed protocols) */
    int         seq_bits;    /* sequence numbers 0..2^seq_bits-1 */
    int         interval_ms; /* windowed sender: minimum interval between two transmissions */
} rdt_options;

/* Reads the options (and loads the message catalog of the user's language);
 * on error or -h prints the help text and exits. */
void rdt_parse_options(int argc, char **argv, rdt_role role,
                       const rdt_protocol *proto, rdt_options *o);

/* ------------------------------------------------------------------ */
/* Application data                                                   */
/* ------------------------------------------------------------------ */

/* Text to send split into segments of at most opt.chunk bytes, without
 * breaking UTF-8 characters. The FIN is the packet with index nseg. */
typedef struct {
    uint8_t  *text;
    size_t    len;
    size_t   *off;           /* start of each segment */
    uint16_t *seg_len;       /* length of each segment */
    int       nseg;
} rdt_data;

void rdt_load_data(const rdt_options *o, rdt_data *d);    /* exits on error */
void rdt_free_data(rdt_data *d);

/* DATA packet of segment idx, or FIN if idx == nseg */
void rdt_make_packet(const rdt_data *d, int idx, uint32_t seq, rdt_packet *p);

/* Buffer of the data delivered to the application (receiver) */
typedef struct {
    uint8_t *data;
    size_t   len, cap;
} rdt_buffer;

int rdt_buffer_append(rdt_buffer *b, const void *data, size_t n);   /* 0 = ok */

/* ------------------------------------------------------------------ */
/* Network                                                            */
/* ------------------------------------------------------------------ */

/* Resolves a name or an IPv4 address; returns 0 if ok, -1 otherwise */
int  rdt_resolve(const char *host, int port, struct sockaddr_in *out);

/* Creates a non-blocking UDP socket; if bind_addr != NULL binds it to that address */
int  rdt_udp_socket(const struct sockaddr_in *bind_addr);

void rdt_addr_str(const struct sockaddr_in *a, char *buf, size_t size);
int  rdt_addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b);

/* Sender: resolves the receiver and opens a socket on an ephemeral port
 * (chosen right away, so that it can be shown). Exits on error. */
int  rdt_open_sender(const rdt_options *o, struct sockaddr_in *dst,
                     char *local_s, char *remote_s, size_t size);

/* Receiver: opens the listening socket on opt.host:opt.port. Exits on error. */
int  rdt_open_receiver(const rdt_options *o, struct sockaddr_in *local,
                       char *local_s, size_t size);

/* ------------------------------------------------------------------ */
/* Time and random numbers                                            */
/* ------------------------------------------------------------------ */

long long rdt_now_ms(void);          /* monotonic clock in milliseconds */
void      rdt_srand(unsigned seed);
double    rdt_rand01(void);          /* uniform in [0, 1) */
unsigned  rdt_rand(unsigned n);      /* uniform in [0, n) */

#endif
