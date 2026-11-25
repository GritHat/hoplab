title      Wi-Fi's blind spot: hidden terminals, CSMA/CA and RTS/CTS
title[it]  Il punto cieco del Wi-Fi: terminali nascosti, CSMA/CA e RTS/CTS
level  4
layer  2
topic  mac
about  Carrier sensing assumes that whoever could collide with you can be heard. On radio that is false: two stations on opposite sides of an access point both reach it, but not each other. Three wireless cells run the same CSMA/CA with the same load. In `open` every station hears every other. In `hidden` the stations form two groups that cannot hear each other: each group senses an idle channel while the other is transmitting, and frames collide at the access point for their whole length. In `rtscts` the groups are still hidden, but a transmission starts with a short RTS that the access point answers with a CTS everybody hears: the channel is reserved, and only the tiny RTS can collide.
about  Compare S and the red in the three mac panels. RTS/CTS is not free: every frame pays for the extra exchange, which is why 802.11 uses it only above a frame-size threshold.
about  Try: load=0.2 (the channel is mostly idle and every frame still gets through, but count the collisions in `hidden`: each one is a frame sent twice or more); stations=4; run `open mac proto=csmaca rts=1 load=0.6 frame=100ms` to measure the overhead of RTS/CTS alone.
about[it]  Il carrier sensing presuppone che chi può collidere con te si possa sentire. In radio è falso: due stazioni ai lati opposti di un access point lo raggiungono entrambe, ma non si sentono fra loro. Tre celle wireless eseguono lo stesso CSMA/CA con lo stesso carico. In `open` ogni stazione sente tutte le altre. In `hidden` le stazioni formano due gruppi che non si sentono: ciascun gruppo vede il canale libero mentre l'altro trasmette, e i frame collidono all'access point per tutta la loro durata. In `rtscts` i gruppi restano nascosti, ma una trasmissione comincia con un breve RTS a cui l'access point risponde con un CTS che tutti sentono: il canale è prenotato, e può collidere solo il piccolo RTS.
about[it]  Confronta S e il rosso nei tre pannelli mac. RTS/CTS non è gratis: ogni frame paga lo scambio in più, ed è per questo che 802.11 lo usa solo oltre una soglia di dimensione del frame.
about[it]  Prova: load=0.2 (il canale è quasi sempre libero e ogni frame arriva comunque, ma conta le collisioni in `hidden`: ognuna è un frame inviato due o più volte); stations=4; esegui `open mac proto=csmaca rts=1 load=0.6 frame=100ms` per misurare il solo overhead di RTS/CTS.
param      load 0.6 Offered load (new frames per frame time, all stations)
param[it]  load Carico offerto (nuovi frame per tempo di frame, tutte le stazioni)
param      stations 10 Number of stations per cell (2 to 64)
param[it]  stations Numero di stazioni per cella (da 2 a 64)
param+     frame 100ms Duration of one data frame
param+[it] frame Durata di un frame di dati

node open host x=20 y=50 role=AP
node hidden host x=50 y=50 role=AP
node rtscts host x=80 y=50 role=AP

run open mac proto=csmaca stations=$stations load=$load frame=$frame
run hidden mac proto=csmaca hidden=1 stations=$stations load=$load frame=$frame
run rtscts mac proto=csmaca hidden=1 rts=1 stations=$stations load=$load frame=$frame

note 1s      `open`: everyone hears everyone. Backoff counters freeze while the channel is busy; collisions are rare.
note[it] 1s  `open`: tutti sentono tutti. I contatori di backoff si fermano mentre il canale è occupato; le collisioni sono rare.
note 15s     `hidden`: even stations cannot hear odd ones. Each group talks over the other: long red collisions.
note[it] 15s `hidden`: le stazioni pari non sentono le dispari. Ciascun gruppo parla sopra l'altro: lunghe collisioni rosse.
note 30s     `rtscts`: same hidden groups, but the access point's CTS silences everybody. Collisions shrink to the RTS.
note[it] 30s `rtscts`: stessi gruppi nascosti, ma il CTS dell'access point zittisce tutti. Le collisioni si riducono all'RTS.

duration 60s
# hidden terminals collide where a shared channel does not; under a very light
# load collisions of any kind are rare events (their number grows with the
# square of the load), so there the claim is only that they are not fewer
at 59900ms run open check collide "hidden:mac.collisions >= open:mac.collisions && ($load < 0.1 || hidden:mac.collisions > open:mac.collisions + 5)"
# light load: retries still get everything through, at the price of those collisions
at 59900ms run open check light "$load > 0.25 || (open:mac.throughput_pct >= 55 * $load && hidden:mac.throughput_pct >= 55 * $load && rtscts:mac.throughput_pct >= 55 * $load)"
# from moderate load on (and with at least two stations per group) the hidden cell collapses, and
# RTS/CTS brings it back near the open one
at 59900ms run open check collapse "$load < 0.4 || $stations < 4 || (hidden:mac.throughput_pct <= 0.85 * open:mac.throughput_pct && rtscts:mac.throughput_pct >= 0.8 * open:mac.throughput_pct && rtscts:mac.throughput_pct > hidden:mac.throughput_pct + 5)"
expect open check.collide == 1
expect open check.light == 1
expect open check.collapse == 1
