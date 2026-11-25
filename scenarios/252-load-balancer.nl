title      Load balancer: one address, three servers, and one of them fails
title[it]  Load balancer: un indirizzo, tre server, e uno si guasta
level  2
layer  4
topic  lb
about  The client only knows one address: the virtual IP of the balancer lb. Behind it three servers run
about  the same service (whoami: it answers with the server's name). The client opens many short flows (one per
about  source port, a few requests each); for each new flow lb picks a server in turn (round robin), rewrites the destination,
about  and remembers the choice in its flow table so the flow stays on that server. The answers go back
about  through lb, which puts the virtual address back as the source.
about
about  At 2 s server s2 crashes. For a moment a third of the requests are lost; the health checks (an ICMP
about  echo every `check`) notice after `fall` misses, s2 leaves the pool and its flows are moved. The
about  requests lost are about 20/3 per second of detection time: count them in c1's panel. At 9 s s2
about  returns and takes new flows again. Follow it in the panels of lb (pool, flows, hits) and of c1
about  (who answered).
about
about  Try: check=250ms for a faster reaction, fall=1, or algo=leastconn and see where the flows of s2 go
about  after its return; life=0 (flows never end): s2 comes back to nothing, because flow affinity keeps
about  every existing flow on the server it was moved to and there are no new flows to give it.
about[it]  Il client conosce un solo indirizzo: l'IP virtuale del bilanciatore lb. Dietro, tre server
about[it]  offrono lo stesso servizio (whoami: risponde con il nome del server). Il client apre molti flussi brevi
about[it]  (uno per porta sorgente, poche richieste ciascuno); per ogni nuovo flusso lb sceglie un server a turno (round robin),
about[it]  riscrive la destinazione e ricorda la scelta nella tabella dei flussi, così il flusso resta su quel
about[it]  server. Le risposte ripassano da lb, che rimette l'indirizzo virtuale come sorgente.
about[it]
about[it]  A 2 s il server s2 va in crash. Per un momento un terzo delle richieste va perso; gli health check
about[it]  (un ICMP echo ogni `check`) se ne accorgono dopo `fall` mancate risposte, s2 esce dal pool e i suoi
about[it]  flussi vengono spostati. Le richieste perse sono circa 20/3 per ogni secondo di rilevazione:
about[it]  contale nel pannello di c1. A 9 s s2 ritorna e riceve di nuovo flussi nuovi. Seguilo nei pannelli di
about[it]  lb (pool, flussi, richieste) e di c1 (chi ha risposto).
about[it]
about[it]  Prova: check=250ms per una reazione più rapida, fall=1, oppure algo=leastconn e guarda dove vanno
about[it]  i flussi di s2 dopo il suo ritorno; life=0 (i flussi non finiscono mai): s2 torna e non riceve
about[it]  nulla, perché l'affinità tiene ogni flusso esistente sul server dove è stato spostato e non ci
about[it]  sono flussi nuovi da dargli.
param  algo rr Algorithm: rr, wrr, leastconn, hash, consistent, p2c, maglev
param[it] algo Algoritmo: rr, wrr, leastconn, hash, consistent, p2c, maglev
param  check 1s Interval between health checks
param[it] check Intervallo tra gli health check
param+ fall 2 Missed checks before a server is taken out
param[it] fall Controlli falliti prima di escludere un server
param+ life 4 Requests per flow before the client opens a new one (0 = flows never end)
param[it] life Richieste per flusso prima che il client ne apra uno nuovo (0 = i flussi non finiscono mai)
param+ flows 30 Client flows (source ports, max 40)
param[it] flows Flussi del client (porte sorgente, max 40)

node c1 host x=8 y=50
node lb router x=40 y=50 role=LB
node s1 host x=85 y=15 role=SERVER
node s2 host x=85 y=50 role=SERVER
node s3 host x=85 y=85 role=SERVER
link c1 lb delay=5ms
link lb s1 delay=1ms
link lb s2 delay=1ms
link lb s3 delay=1ms
auto all

run s1 whoami
run s2 whoami
run s3 whoami
run lb lb vip=lb:eth0 port=8000 backends=s1,s2,s3 algo=$algo check=$check fall=$fall
run c1 reqgen lb count=400 rate=20 flows=$flows life=$life
note 0s Every new flow gets the next server in turn; later packets of the flow follow the flow table.
note[it] 0s Ogni nuovo flusso riceve il server successivo a turno; i pacchetti seguenti del flusso seguono la tabella dei flussi.
at 2s node s2 down
note 2s s2 crashes. lb does not know yet: requests of the flows pinned to s2 are lost until the health check fails twice.
note[it] 2s s2 va in crash. lb ancora non lo sa: le richieste dei flussi legati a s2 si perdono finché l'health check non fallisce due volte.
at 9s node s2 up
note 9s s2 is back: at the next successful check it rejoins the pool and gets its share of the new flows.
note[it] 9s s2 è tornato: al prossimo controllo riuscito rientra nel pool e riceve la sua parte dei nuovi flussi.
duration 23s

# all three servers answered, and every request is either answered or lost
at 22900ms run c1 check pool "c1:reqgen.backends_seen == 3 && c1:reqgen.answers + c1:reqgen.lost == 400 && c1:reqgen.s2 >= 10 && lb:lb.nobackend == 0"
# s2 is taken out after `fall` missed checks (if that happens before it returns at 9 s) and comes back
at 22900ms run c1 check health "lb:lb.health_up == lb:lb.health_down && (($fall + 1) * $check >= 7s || lb:lb.health_down == 1)"
# until then a third of the 20 requests per second go to the dead server: the loss measures the detection time
at 22900ms run c1 check loss "c1:reqgen.lost >= min(47, 6.67 * ($fall - 1) * $check / 1s) - 8 && c1:reqgen.lost <= min(47, 6.67 * ($fall + 1) * $check / 1s) + 10"
expect c1 check.pool == 1
expect c1 check.health == 1
expect c1 check.loss == 1
