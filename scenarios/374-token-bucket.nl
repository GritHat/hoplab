title      Token bucket shaping: a rate limit that tolerates bursts
title[it]  Shaping con token bucket: un limite di velocità che tollera le raffiche
level  3
layer  3
topic  qdisc
about  The link r1 - h2 is fast (10 Mbit/s) but the operator sells only `shape` bit/s. A token
about  bucket enforces it: tokens (bytes of credit) drip into a bucket at the contracted rate, up to
about  the bucket size; a packet may leave only by spending tokens equal to its length. A sender
about  below the rate never waits; a burst passes at full speed until the bucket is empty; what
about  exceeds the rate waits in the queue (shaping) and overflows only when the queue is full.
about
about  Watch: on r1 the graphs "tokens (B)" and "queue", the qdisc panel; on h2 the received
about  rate, which is clamped to the shaping rate. Try: bucket=100000 (a long burst at 10 Mbit/s
about  before the clamp); shape=1M; queue=500 (no loss, but seconds of delay).
about[it]  Il collegamento r1 - h2 è veloce (10 Mbit/s) ma l'operatore vende solo `shape` bit/s. Un
about[it]  token bucket lo impone: i token (byte di credito) gocciolano in un secchio alla velocità
about[it]  contrattuale, fino alla dimensione del secchio; un pacchetto può partire solo spendendo token
about[it]  pari alla sua lunghezza. Un mittente sotto la velocità non aspetta mai; una raffica passa a
about[it]  piena velocità finché il secchio non è vuoto; ciò che eccede aspetta in coda (shaping) e
about[it]  trabocca solo quando la coda è piena.
about[it]
about[it]  Osserva: su r1 i grafici "tokens (B)" e "queue", il pannello qdisc; su h2 la velocità
about[it]  ricevuta, che viene limitata alla velocità di shaping. Prova: bucket=100000 (una lunga
about[it]  raffica a 10 Mbit/s prima del limite); shape=1M; queue=500 (nessuna perdita, ma secondi di ritardo).
param      shape 500k Shaping rate
param[it]  shape Velocità di shaping
param      bucket 10000 Bucket size (bytes): the largest burst at full speed
param[it]  bucket Dimensione del secchio (byte): la raffica massima a piena velocità
param+     queue 100 Queue capacity (packets)
param[it]  queue Capacità della coda (pacchetti)

node h1 host x=10 y=50
node r1 router x=45 y=50 role=shaper
node h2 host x=90 y=50
link h1 r1 delay=2ms
link r1 h2 bw=10M delay=5ms qdisc=tbf shape=$shape bucket=$bucket queue=$queue
auto all

run r1 qdisc
run h2 sink
at 1s run h1 cbr h2 rate=300k size=500 duration=3s
at 5s run h1 cbr h2 rate=2M size=500 duration=4s
at 11s run h1 cbr h2 rate=300k size=500 duration=3s

note 1s 300 kbit/s is under the shaping rate: tokens arrive faster than they are spent, the bucket stays full and nothing waits.
note[it] 1s 300 kbit/s è sotto la velocità di shaping: i token arrivano più in fretta di quanto vengano spesi, il secchio resta pieno e nulla aspetta.
note 5s 2 Mbit/s: the first packets spend the saved tokens and leave at once (the burst), then the output is clamped to the shaping rate and the queue grows until it overflows.
note[it] 5s 2 Mbit/s: i primi pacchetti spendono i token risparmiati e partono subito (la raffica), poi l'uscita è limitata alla velocità di shaping e la coda cresce fino a traboccare.
note 9s The source stops; the queue drains at the shaping rate, then the bucket refills.
note[it] 9s La sorgente si ferma; la coda si svuota alla velocità di shaping, poi il secchio si riempie di nuovo.
note 11s Back under the rate: no waiting again.
note[it] 11s Di nuovo sotto la velocità: nessuna attesa.

duration 15s
expect h2 sink.received > 950
expect h2 sink.received < 1150
expect h2 sink.lost > 1000
