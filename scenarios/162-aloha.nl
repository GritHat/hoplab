title      Sharing one channel: pure ALOHA and slotted ALOHA
title[it]  Condividere un canale: ALOHA puro e slotted ALOHA
level  1
layer  2
topic  mac
about  Ten stations share one radio channel and nobody coordinates them. Two transmissions that overlap, even by an instant, are both lost: a collision. Two laboratories run side by side with the same traffic. In `pure` each station transmits the moment it has a frame (ALOHA, 1970); in `slotted` it waits for the next tick of a common clock, so frames either overlap completely or not at all.
about  Open the `mac` panel of each node: one row per station, green for a delivered frame, red for a collision, yellow dots while a station waits to retry. Below it, G is how many transmissions are attempted per frame time and S how many succeed. Theory says S = G·e^(-2G) for pure ALOHA (at best 18%) and S = G·e^(-G) for slotted (at best 37%): the measured S follows it. The parameter `load` is only the NEW traffic: every collision adds a retransmission, so G is larger than the load, and when the load exceeds what a protocol can carry the retries pile up, G runs away and S falls. With the default load of 0.4 that has already happened to pure ALOHA (its ceiling is 0.18) and not to slotted ALOHA.
about  Try: load=0.1 (few collisions, both carry everything), load=0.25 (pure ALOHA collapses, slotted still copes), load=1 (both saturate, the channel is mostly collisions); stations=3; frame=500ms to slow the timeline down.
about[it]  Dieci stazioni condividono un canale radio e nessuno le coordina. Due trasmissioni che si sovrappongono, anche per un istante, vanno entrambe perse: una collisione. Due laboratori lavorano affiancati con lo stesso traffico. In `pure` ogni stazione trasmette appena ha un frame (ALOHA, 1970); in `slotted` aspetta il prossimo scatto di un orologio comune, così i frame o si sovrappongono del tutto o per niente.
about[it]  Apri il pannello `mac` di ciascun nodo: una riga per stazione, verde per un frame consegnato, rosso per una collisione, puntini gialli mentre una stazione attende di riprovare. Sotto, G è il numero di trasmissioni tentate per tempo di frame e S quante riescono. La teoria dice S = G·e^(-2G) per ALOHA puro (al massimo 18%) e S = G·e^(-G) per slotted (al massimo 37%): l'S misurato la segue. Il parametro `load` è solo il traffico NUOVO: ogni collisione aggiunge una ritrasmissione, quindi G è maggiore del carico, e quando il carico supera ciò che un protocollo può trasportare i tentativi si accumulano, G cresce senza controllo e S cala. Con il carico predefinito di 0.4 questo è già successo ad ALOHA puro (il suo tetto è 0.18) e non a slotted ALOHA.
about[it]  Prova: load=0.1 (poche collisioni, entrambi trasportano tutto), load=0.25 (ALOHA puro collassa, slotted regge ancora), load=1 (entrambi saturano, il canale è quasi solo collisioni); stations=3; frame=500ms per rallentare la linea del tempo.
param      load 0.4 Offered load (new frames per frame time, all stations)
param[it]  load Carico offerto (nuovi frame per tempo di frame, tutte le stazioni)
param      stations 10 Number of stations
param[it]  stations Numero di stazioni
param+     frame 100ms Duration of one frame
param+[it] frame Durata di un frame

node pure host x=30 y=50 role=ALOHA
node slotted host x=70 y=50 role=S-ALOHA

run pure mac proto=aloha stations=$stations load=$load frame=$frame
run slotted mac proto=slotted stations=$stations load=$load frame=$frame

note 1s      Select `pure` and open its mac panel: overlapping transmissions turn red. Both frames are lost.
note[it] 1s  Seleziona `pure` e apri il pannello mac: le trasmissioni che si sovrappongono diventano rosse. Entrambi i frame sono persi.
note 15s     Now `slotted`: transmissions start only on slot boundaries, so partial overlaps are impossible.
note[it] 15s Ora `slotted`: le trasmissioni partono solo ai confini degli slot, quindi le sovrapposizioni parziali sono impossibili.
note 40s     Compare S: slotted ALOHA carries about twice the traffic of pure ALOHA. Neither gets past 37%.
note[it] 40s Confronta S: slotted ALOHA porta circa il doppio del traffico di ALOHA puro. Nessuno dei due supera il 37%.

duration 60s
# S = sent that succeeded, G = all attempts, both per frame time: the measured
# point must sit on the theoretical curve, wherever the load has pushed it
at 59900ms run pure check pure "abs(pure:mac.throughput_pct / 100 - (pure:mac.sent * $frame / 60s) * exp(-2 * pure:mac.sent * $frame / 60s)) <= 0.09"
at 59900ms run pure check slotted "abs(slotted:mac.throughput_pct / 100 - (slotted:mac.sent * $frame / 60s) * exp(-slotted:mac.sent * $frame / 60s)) <= 0.09"
# neither carries more than its ceiling, and what is offered below it gets through
at 59900ms run pure check ceiling "pure:mac.throughput_pct <= 23 && slotted:mac.throughput_pct <= 46 && ($load > 0.15 || pure:mac.throughput_pct >= $load * 60) && ($load > 0.3 || slotted:mac.throughput_pct >= $load * 60)"
expect pure check.pure == 1
expect pure check.slotted == 1
expect pure check.ceiling == 1
