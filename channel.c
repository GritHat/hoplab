/*
 * channel.c - Canale inaffidabile simulato (perdita, corruzione, ritardo).
 */
#include "channel.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

void ch_init(channel *ch, int sock, ch_dir out_dir, double loss, double corrupt, int delay_ms)
{
    memset(ch, 0, sizeof(*ch));
    ch->sock     = sock;
    ch->out_dir  = out_dir;
    ch->loss     = loss;
    ch->corrupt  = corrupt;
    ch->delay_ms = delay_ms;
}

/* Trova un posto libero; se non c'e' sacrifica l'animazione in uscita piu'
 * vecchia (mai un pacchetto in arrivo, che deve ancora essere consegnato). */
static ch_flight *alloc_flight(channel *ch)
{
    ch_flight *oldest = NULL;
    int i;

    for (i = 0; i < CH_MAX_FLIGHTS; i++)
        if (!ch->fl[i].active)
            return &ch->fl[i];
    for (i = 0; i < CH_MAX_FLIGHTS; i++)
        if (ch->fl[i].outbound && (!oldest || ch->fl[i].t_start < oldest->t_start))
            oldest = &ch->fl[i];
    return oldest;
}

ch_fate ch_send(channel *ch, const rdt_packet *p, const struct sockaddr_in *to)
{
    uint8_t buf[RDT_MAX_DATAGRAM];
    size_t n = rdt_encode(p, buf);
    long long now = rdt_now_ms();
    ch_fate fate = FATE_OK;
    int bit = -1;
    ch_flight *f;

    ch->n_sent++;
    if (rdt_rand01() < ch->loss) {
        fate = FATE_LOST;
        ch->n_lost++;
    } else if (rdt_rand01() < ch->corrupt) {
        /* inverte un bit a caso fra intestazione e payload */
        fate = FATE_CORRUPT;
        ch->n_corrupted++;
        bit = (int)rdt_rand((unsigned)(n * 8));
        buf[bit / 8] ^= (uint8_t)(0x80 >> (bit % 8));
    }

    if (fate != FATE_LOST)
        sendto(ch->sock, buf, n, 0, (const struct sockaddr *)to, sizeof(*to));

    f = alloc_flight(ch);
    if (f) {
        memset(f, 0, sizeof(*f));
        f->active      = 1;
        f->outbound    = 1;
        f->dir         = ch->out_dir;
        f->t_start     = now;
        f->t_end       = now + ch->delay_ms;
        f->fate        = fate;
        f->fate_pos    = fate == FATE_LOST ? 0.35 + 0.40 * rdt_rand01()
                                           : 0.15 + 0.45 * rdt_rand01();
        f->flipped_bit = bit;
        f->pkt         = *p;
        if (fate == FATE_CORRUPT)
            rdt_decode(buf, n, &f->damaged);
    }
    return fate;
}

int ch_poll_socket(channel *ch)
{
    uint8_t buf[RDT_MAX_DATAGRAM + 64];
    int count = 0;

    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(ch->sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        long long now;
        ch_flight *f;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;                      /* EAGAIN: nient'altro da leggere */
        }
        now = rdt_now_ms();
        ch->n_recv++;
        count++;

        f = alloc_flight(ch);
        if (!f)
            continue;                   /* nessun posto: equivale a una perdita */
        memset(f, 0, sizeof(*f));
        f->active   = 1;
        f->outbound = 0;
        f->dir      = ch->out_dir == TO_RECEIVER ? TO_SENDER : TO_RECEIVER;
        f->t_start  = now;
        f->t_end    = now + ch->delay_ms;
        f->fate     = FATE_OK;
        f->valid    = rdt_decode(buf, (size_t)n, &f->pkt);
        f->from     = from;
    }
    return count;
}

int ch_pop_arrival(channel *ch, long long now, ch_flight *out)
{
    ch_flight *best = NULL;
    int i;

    for (i = 0; i < CH_MAX_FLIGHTS; i++) {
        ch_flight *f = &ch->fl[i];
        if (f->active && !f->outbound && f->t_end <= now && (!best || f->t_end < best->t_end))
            best = f;
    }
    if (!best)
        return 0;
    *out = *best;
    best->active = 0;
    return 1;
}

const ch_flight *ch_fate_event(channel *ch, long long now)
{
    int i;

    for (i = 0; i < CH_MAX_FLIGHTS; i++) {
        ch_flight *f = &ch->fl[i];
        if (f->active && f->outbound && f->fate != FATE_OK && !f->fate_reported &&
            ch_progress(f, now) >= f->fate_pos) {
            f->fate_reported = 1;
            f->fate_time     = now;
            return f;
        }
    }
    return NULL;
}

void ch_gc(channel *ch, long long now)
{
    int i;

    for (i = 0; i < CH_MAX_FLIGHTS; i++) {
        ch_flight *f = &ch->fl[i];
        if (!f->active || !f->outbound)
            continue;
        if (f->fate == FATE_LOST) {
            if (f->fate_reported && now - f->fate_time > 1500)
                f->active = 0;          /* la croce resta visibile per un po' */
        } else if (now >= f->t_end) {
            f->active = 0;
        }
    }
}

double ch_progress(const ch_flight *f, long long now)
{
    double p;

    if (f->t_end <= f->t_start)
        return 1.0;
    p = (double)(now - f->t_start) / (double)(f->t_end - f->t_start);
    return p < 0.0 ? 0.0 : p > 1.0 ? 1.0 : p;
}
