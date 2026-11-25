title      Load balancing algorithms compared
title[it]  Algoritmi di bilanciamento a confronto
level  3
layer  4
topic  lb
about  Three servers of different size (s1 is three times bigger) behind one balancer, and two clients
about  with different habits: c1 opens many short flows, c2 keeps a few long-lived ones. Run the lab once
about  per algorithm and compare the shares in the panel of lb and in the final lines of c1 and c2.
about
about  wrr (default) follows the weights 3:1:1 exactly. rr ignores them and overloads the small servers.
about  leastconn balances the number of active flows, scaled by weight, so it adapts when flows have
about  different durations. p2c (power of two choices) picks two servers at random and takes the less
about  loaded: nearly as good as leastconn with far less bookkeeping. hash, consistent and maglev decide
about  from the flow key alone: no state, but the shares are only statistically even (and only consistent
about  honours the weights here). With key=src every flow of a client lands on the same server.
about
about  Try: algo=hash with key=src (two clients, so at most two servers ever work), affinity=0 with rr
about  (every packet rescheduled: fine for these one-datagram requests, fatal for TCP).
about[it]  Tre server di taglia diversa (s1 è tre volte più grande) dietro un bilanciatore, e due client
about[it]  con abitudini diverse: c1 apre molti flussi brevi, c2 ne tiene pochi e di lunga durata. Esegui il
about[it]  laboratorio una volta per algoritmo e confronta le quote nel pannello di lb e nelle righe finali
about[it]  di c1 e c2.
about[it]
about[it]  wrr (default) segue esattamente i pesi 3:1:1. rr li ignora e sovraccarica i server piccoli.
about[it]  leastconn bilancia il numero di flussi attivi, in proporzione al peso, quindi si adatta quando i
about[it]  flussi hanno durate diverse. p2c (power of two choices) sceglie due server a caso e prende il meno
about[it]  carico: quasi buono quanto leastconn con molta meno contabilità. hash, consistent e maglev
about[it]  decidono dalla sola chiave del flusso: nessuno stato, ma le quote sono uniformi solo
about[it]  statisticamente (e qui solo consistent rispetta i pesi). Con key=src tutti i flussi di un client
about[it]  finiscono sullo stesso server.
about[it]
about[it]  Prova: algo=hash con key=src (due client, quindi al massimo due server lavorano), affinity=0 con
about[it]  rr (ogni pacchetto viene riassegnato: va bene per queste richieste di un solo datagramma, fatale
about[it]  per TCP).
param  algo wrr Algorithm: rr, wrr, leastconn, hash, consistent, p2c, maglev
param[it] algo Algoritmo: rr, wrr, leastconn, hash, consistent, p2c, maglev
param  weights 3,1,1 Weights of s1,s2,s3
param[it] weights Pesi di s1,s2,s3
param+ key flow Hash key: flow (address and port) or src (client address only)
param[it] key Chiave di hash: flow (indirizzo e porta) o src (solo indirizzo del client)
param+ affinity 1 Flow table: 1 keeps a flow on its server, 0 schedules every packet again
param[it] affinity Tabella dei flussi: 1 tiene il flusso sul suo server, 0 riassegna ogni pacchetto
param+ life 2 Requests per short flow of c1
param[it] life Richieste per ogni flusso breve di c1

node c1 host x=8 y=25
node c2 host x=8 y=75
node lb router x=42 y=50 role=LB
node s1 host x=88 y=15 role=BIG
node s2 host x=88 y=50 role=SERVER
node s3 host x=88 y=85 role=SERVER
link c1 lb delay=5ms
link c2 lb delay=8ms
link lb s1 delay=1ms
link lb s2 delay=1ms
link lb s3 delay=1ms
auto all

run s1 whoami
run s2 whoami
run s3 whoami
run lb lb vip=lb:eth0 port=8000 backends=s1,s2,s3 algo=$algo weights=$weights key=$key affinity=$affinity
run c1 reqgen lb count=300 rate=25 flows=30 life=$life
at 500ms run c2 reqgen lb count=200 rate=15 flows=20
note 0s c1 opens short flows: each new one is a fresh decision for the balancer.
note[it] 0s c1 apre flussi brevi: ognuno nuovo è una nuova decisione per il bilanciatore.
note 1s c2 joins with 20 long-lived flows: they stay where the first packet put them.
note[it] 1s c2 si aggiunge con 20 flussi di lunga durata: restano dove li ha messi il primo pacchetto.
note 8s Compare the bars in the panel of lb with the weights: does the algorithm respect them?
note[it] 8s Confronta le barre nel pannello di lb con i pesi: l'algoritmo li rispetta?
duration 16s

expect c1 reqgen.answers == 300
expect c2 reqgen.answers == 200
expect c1 reqgen.backends_seen == 3
expect lb lb.hits.s1 >= 280
expect lb lb.hits.s2 >= 80
expect lb lb.hits.s2 <= 120
