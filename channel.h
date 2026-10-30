/*
 * channel.h - Canale inaffidabile simulato sopra un socket UDP.
 *
 * Su localhost UDP praticamente non perde ne' corrompe nulla, quindi il
 * canale lo fa apposta: ogni pacchetto in uscita puo' essere scartato
 * (perdita) o avere un bit invertito (corruzione) con le probabilita'
 * scelte da riga di comando.
 *
 * Il canale modella anche il ritardo di propagazione: un pacchetto ricevuto
 * dal socket resta "in volo" per delay_ms prima di essere consegnato al
 * protocollo. Lo stesso pacchetto e' animato in volo da entrambi i processi
 * (in uscita da chi lo invia, in arrivo da chi lo riceve), cosi' le due
 * finestre affiancate mostrano lo stesso viaggio in contemporanea.
 */
#ifndef CHANNEL_H
#define CHANNEL_H

#include "rdt.h"

#define CH_MAX_FLIGHTS 64

/* Corsia del canale: verso il receiver (dati) o verso il sender (riscontri) */
typedef enum { TO_RECEIVER = 0, TO_SENDER = 1 } ch_dir;

/* Destino di un pacchetto in uscita, deciso dal canale al momento dell'invio */
typedef enum { FATE_OK, FATE_LOST, FATE_CORRUPT } ch_fate;

typedef struct {
    int        active;
    int        outbound;       /* 1 = inviato da questo processo, 0 = in arrivo */
    ch_dir     dir;
    long long  t_start, t_end; /* partenza e arrivo previsti (ms) */
    ch_fate    fate;           /* noto solo per i pacchetti in uscita */
    double     fate_pos;       /* punto del tragitto (0..1) in cui avviene perdita/corruzione */
    int        fate_reported;
    long long  fate_time;
    int        flipped_bit;    /* bit invertito dalla corruzione */
    rdt_packet pkt;            /* in uscita: originale; in arrivo: come decodificato */
    rdt_packet damaged;        /* in uscita: contenuto dopo la corruzione */
    int        valid;          /* in arrivo: checksum corretto */
    struct sockaddr_in from;   /* in arrivo: mittente */
} ch_flight;

typedef struct {
    int        sock;
    ch_dir     out_dir;        /* corsia su cui viaggiano i pacchetti che inviamo */
    double     loss, corrupt;
    int        delay_ms;
    ch_flight  fl[CH_MAX_FLIGHTS];
    unsigned   n_sent, n_lost, n_corrupted, n_recv;
} channel;

void ch_init(channel *ch, int sock, ch_dir out_dir, double loss, double corrupt, int delay_ms);

/* Invia un pacchetto attraverso il canale simulato e ne restituisce il destino */
ch_fate ch_send(channel *ch, const rdt_packet *p, const struct sockaddr_in *to);

/* Legge tutti i datagrammi disponibili sul socket e li mette "in volo" */
int ch_poll_socket(channel *ch);

/* Estrae il prossimo pacchetto in arrivo che ha completato il tragitto */
int ch_pop_arrival(channel *ch, long long now, ch_flight *out);

/* Restituisce un pacchetto in uscita appena perso/corrotto (una volta sola), o NULL */
const ch_flight *ch_fate_event(channel *ch, long long now);

/* Rimuove le animazioni concluse */
void ch_gc(channel *ch, long long now);

/* Avanzamento 0..1 di un pacchetto lungo il tragitto */
double ch_progress(const ch_flight *f, long long now);

#endif
