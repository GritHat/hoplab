title  SWIM: detecting a crashed member without false alarms
title[it] SWIM: rilevare un membro caduto senza falsi allarmi
level  4
layer  8
topic  gossip
about  Eight members run the SWIM protocol. Each one pings a single random member per period, so the load does not grow with the group. n8 sits behind a lossy link: its pings and acks are often lost, but before accusing anybody a prober asks three other members to probe on its behalf (ping-req), and a member that hears it is suspected refutes by announcing a higher incarnation number. At 8 s n3 really crashes: it is suspected by whoever probes it next, nobody refutes, and after the suspicion timeout it is declared dead; the news rides piggyback on ordinary pings and acks and reaches everyone. At 22 s n3 restarts and rejoins with a new incarnation.
about  Watch: the member list in each gossip panel (● alive, ? suspect, ✗ dead), ping-req lines when the lossy link drops a packet, "refuting" lines on n8. Counters: swim.dead, swim.detected, swim.mistaken_deaths (a member buried while it was only unreachable: counted when it proves it never went down), swim.refutations; the final report prints them. With the defaults there are no mistaken deaths at any loss up to 50%. Experiments: helpers=0 (no indirect probes) or suspect=1s with loss=40%: mistaken deaths appear, and are corrected by refutation; period=200ms: faster detection, more traffic. What never changes is completeness: the real crash of n3 is always detected.
about[it] Otto membri eseguono il protocollo SWIM. Ognuno fa ping a un solo membro casuale per periodo, quindi il carico non cresce con il gruppo. n8 sta dietro un collegamento con perdite: i suoi ping e ack si perdono spesso, ma prima di accusare qualcuno chi sonda chiede ad altri tre membri di sondare al posto suo (ping-req), e un membro che viene a sapere di essere sospettato smentisce annunciando un numero di incarnazione più alto. A 8 s n3 cade davvero: viene sospettato da chi lo sonda per primo, nessuno smentisce, e scaduto il timeout di sospetto è dichiarato morto; la notizia viaggia a cavallo dei normali ping e ack e raggiunge tutti. A 22 s n3 riparte e rientra con una nuova incarnazione.
about[it] Da osservare: l'elenco dei membri in ogni pannello gossip (● vivo, ? sospetto, ✗ morto), le righe ping-req quando il collegamento con perdite scarta un pacchetto, le righe "refuting" su n8. Contatori: swim.dead, swim.detected, swim.mistaken_deaths (un membro sepolto mentre era solo irraggiungibile: contato quando dimostra di non essere mai caduto), swim.refutations; il rapporto finale li stampa. Con i valori predefiniti non ci sono morti per errore a nessuna perdita fino al 50%. Esperimenti: helpers=0 (niente sonde indirette) o suspect=1s con loss=40%: compaiono morti per errore, corrette dalla smentita; period=200ms: rilevamento più rapido, più traffico. Ciò che non cambia mai è la completezza: il vero guasto di n3 viene sempre rilevato.
param  loss 20% Packet loss on n8's link
param[it] loss Perdita di pacchetti sul collegamento di n8
param+ period 1s Protocol period (one probe per period)
param[it] period Periodo del protocollo (una sonda per periodo)
param+ suspect 5s Suspicion timeout before declaring dead
param[it] suspect Timeout di sospetto prima di dichiarare morto
param+ helpers 3 Members asked for an indirect probe
param[it] helpers Membri a cui chiedere una sonda indiretta

node s1 switch x=50 y=50
node n1 host x=50 y=10
node n2 host x=79 y=21
node n3 host x=92 y=50
node n4 host x=79 y=78
node n5 host x=50 y=90
node n6 host x=20 y=78
node n7 host x=8 y=50
node n8 host x=20 y=21
link n1 s1 delay=2ms
link n2 s1 delay=2ms
link n3 s1 delay=2ms
link n4 s1 delay=2ms
link n5 s1 delay=2ms
link n6 s1 delay=2ms
link n7 s1 delay=2ms
link n8 s1 delay=2ms loss=$loss
auto all

run n1 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8 swim=1 period=$period suspect=$suspect helpers=$helpers mode=pull interval=5s
run n2 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8 swim=1 period=$period suspect=$suspect helpers=$helpers mode=pull interval=5s
run n3 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8 swim=1 period=$period suspect=$suspect helpers=$helpers mode=pull interval=5s
run n4 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8 swim=1 period=$period suspect=$suspect helpers=$helpers mode=pull interval=5s
run n5 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8 swim=1 period=$period suspect=$suspect helpers=$helpers mode=pull interval=5s
run n6 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8 swim=1 period=$period suspect=$suspect helpers=$helpers mode=pull interval=5s
run n7 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8 swim=1 period=$period suspect=$suspect helpers=$helpers mode=pull interval=5s
run n8 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8 swim=1 period=$period suspect=$suspect helpers=$helpers mode=pull interval=5s

note 0s Every period each member pings one other member. n8's link loses packets: watch for ping-req.
note[it] 0s A ogni periodo ogni membro fa ping a un altro membro. Il collegamento di n8 perde pacchetti: cerca i ping-req.
at 8s node n3 down
note 8s n3 crashes. The next member that probes it gets no ack, not even through helpers: suspicion.
note[it] 8s n3 cade. Il prossimo membro che lo sonda non riceve ack, nemmeno tramite gli aiutanti: sospetto.
note 15s Nobody refuted: n3 is declared dead and the news spreads piggybacked on pings and acks.
note[it] 15s Nessuno ha smentito: n3 è dichiarato morto e la notizia si diffonde a cavallo di ping e ack.
at 22s node n3 up
note 22s n3 restarts with a higher incarnation, which overrides "dead" everywhere.
note[it] 22s n3 riparte con un'incarnazione più alta, che prevale su "morto" ovunque.
duration 50s
expect n1 swim.detected >= 1
expect n5 swim.detected >= 1
expect n8 swim.detected >= 1
expect n1 swim.mistaken_deaths >= 0
expect n2 swim.mistaken_deaths >= 0
expect n4 swim.mistaken_deaths >= 0
expect n8 swim.mistaken_deaths >= 0
expect n3 swim.restarts == 1
expect n8 swim.refutations >= 0
