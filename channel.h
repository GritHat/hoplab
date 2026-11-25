/*
 * channel.h - Unreliable channel simulated on top of a UDP socket.
 *
 * On localhost UDP practically never loses or corrupts anything, so the
 * channel does it on purpose: every outgoing packet can be dropped (loss)
 * or have one bit flipped (corruption) with the probabilities chosen on the
 * command line.
 *
 * The channel also models the propagation delay: a packet received from the
 * socket stays "in flight" for delay_ms before being delivered to the
 * protocol. The same packet is animated in flight by both processes
 * (outgoing by the one that sends it, incoming by the one that receives
 * it), so the two side-by-side windows show the same trip at the same time.
 */
#ifndef CHANNEL_H
#define CHANNEL_H

#include "rdt.h"

#define CH_MAX_FLIGHTS 64

/* Lane of the channel: towards the receiver (data) or towards the sender (acknowledgements) */
typedef enum { TO_RECEIVER = 0, TO_SENDER = 1 } ch_dir;

/* Fate of an outgoing packet, decided by the channel when it is sent */
typedef enum { FATE_OK, FATE_LOST, FATE_CORRUPT } ch_fate;

typedef struct {
    int        active;
    int        outbound;       /* 1 = sent by this process, 0 = incoming */
    ch_dir     dir;
    long long  t_start, t_end; /* departure and expected arrival (ms) */
    ch_fate    fate;           /* known only for outgoing packets */
    double     fate_pos;       /* point of the trip (0..1) where the loss/corruption happens */
    int        fate_reported;
    long long  fate_time;
    int        flipped_bit;    /* bit flipped by the corruption */
    rdt_packet pkt;            /* outgoing: original; incoming: as decoded */
    rdt_packet damaged;        /* outgoing: content after the corruption */
    int        valid;          /* incoming: correct checksum */
    struct sockaddr_in from;   /* incoming: sender address */
} ch_flight;

typedef struct {
    int        sock;
    ch_dir     out_dir;        /* lane on which the packets we send travel */
    double     loss, corrupt;
    int        delay_ms;
    ch_flight  fl[CH_MAX_FLIGHTS];
    unsigned   n_sent, n_lost, n_corrupted, n_recv;
} channel;

void ch_init(channel *ch, int sock, ch_dir out_dir, double loss, double corrupt, int delay_ms);

/* Sends a packet through the simulated channel and returns its fate */
ch_fate ch_send(channel *ch, const rdt_packet *p, const struct sockaddr_in *to);

/* Reads every datagram available on the socket and puts it "in flight" */
int ch_poll_socket(channel *ch);

/* Extracts the next incoming packet that has completed its trip */
int ch_pop_arrival(channel *ch, long long now, ch_flight *out);

/* Returns an outgoing packet that has just been lost/corrupted (once only), or NULL */
const ch_flight *ch_fate_event(channel *ch, long long now);

/* Removes the finished animations */
void ch_gc(channel *ch, long long now);

/* Progress 0..1 of a packet along the trip */
double ch_progress(const ch_flight *f, long long now);

#endif
