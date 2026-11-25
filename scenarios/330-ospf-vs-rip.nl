title      Convergence after a failure: link state against distance vector
title[it]  Convergenza dopo un guasto: link state contro distance vector
level  3
layer  3
topic  ospf
about  Five routers: a short path r1-r2-r3 and a longer detour r1-r4-r5-r3. A steady stream of 50
about  datagrams per second flows from h1 to h3 while, at 20 s, the link r2-r3 breaks. Every datagram
about  sent before the routers agree on the detour is lost, so the counter of lost datagrams at the
about  sink (h3, Sink tab) measures the convergence time: lost / 50 = seconds of blackout.
about
about  With proto=ospf r2 floods a new LSA at once, everybody recomputes after the SPF hold-down and
about  the outage lasts a few tenths of a second. With proto=rip r2 tells r1 immediately that the
about  route is gone (triggered update), but r1 has forgotten the worse offer of r4 and must wait
about  for r4's next periodic update to learn the detour: up to a full update period.
about
about  Try both protocols, then: a longer RIP update period (up to 20s fits in the run); ospf with
about  carrier=0 (failures found only by the dead interval, 4 hellos, as when a switch sits between
about  the routers); spf=2s. The blackout to expect is: OSPF, at most hello + spf; OSPF with carrier=0,
about  between 3 and 4 hello intervals plus spf; RIP, anything up to about one update period,
about  depending on where in its period r4 happens to be when the link breaks.
about[it]  Cinque router: un percorso corto r1-r2-r3 e una deviazione più lunga r1-r4-r5-r3. Un flusso
about[it]  costante di 50 datagrammi al secondo va da h1 a h3 mentre, a 20 s, il collegamento r2-r3 si
about[it]  rompe. Ogni datagramma inviato prima che i router concordino sulla deviazione è perso,
about[it]  quindi il contatore dei datagrammi persi al ricevitore (h3, scheda Sink) misura il tempo di
about[it]  convergenza: persi / 50 = secondi di interruzione.
about[it]
about[it]  Con proto=ospf r2 diffonde subito un nuovo LSA, tutti ricalcolano dopo l'attesa SPF e
about[it]  l'interruzione dura pochi decimi di secondo. Con proto=rip r2 avvisa subito r1 che la rotta
about[it]  non c'è più (aggiornamento immediato), ma r1 ha dimenticato l'offerta peggiore di r4 e deve
about[it]  aspettare il prossimo aggiornamento periodico di r4 per imparare la deviazione: fino a un
about[it]  intero periodo.
about[it]
about[it]  Prova entrambi i protocolli, poi: un periodo RIP più lungo (fino a 20s sta nell'esecuzione);
about[it]  ospf con carrier=0 (guasti scoperti solo dal dead interval, 4 hello, come quando c'è uno
about[it]  switch fra i router); spf=2s. L'interruzione attesa è: OSPF, al più hello + spf; OSPF con
about[it]  carrier=0, fra 3 e 4 intervalli hello più spf; RIP, qualunque valore fino a circa un periodo
about[it]  di aggiornamento, a seconda di dove si trova r4 nel suo periodo quando il collegamento cade.
param  proto ospf Routing protocol: ospf or rip
param[it] proto Protocollo di routing: ospf oppure rip
param  update 5s RIP: period of the updates
param[it] update RIP: periodo degli aggiornamenti
param+ hello 1s OSPF: hello interval
param+[it] hello OSPF: intervallo Hello
param+ spf 200ms OSPF: SPF hold-down
param+[it] spf OSPF: attesa prima di SPF
param+ carrier 1 OSPF: react to link-down signals (0 = wait for the dead interval)
param+[it] carrier OSPF: reagisce ai segnali di link caduto (0 = aspetta il dead interval)

node h1 host   x=5  y=30
node r1 router x=22 y=30
node r2 router x=50 y=12
node r3 router x=78 y=30
node r4 router x=35 y=80
node r5 router x=65 y=80
node h3 host   x=95 y=30
link h1 r1 delay=1ms
link r1 r2 delay=3ms
link r2 r3 delay=3ms
link r1 r4 delay=3ms
link r4 r5 delay=3ms
link r5 r3 delay=3ms
link r3 h3 delay=1ms
auto addr

run r1 $proto update=$update hello=$hello spf=$spf carrier=$carrier
run r2 $proto update=$update hello=$hello spf=$spf carrier=$carrier
run r3 $proto update=$update hello=$hello spf=$spf carrier=$carrier
run r4 $proto update=$update hello=$hello spf=$spf carrier=$carrier
run r5 $proto update=$update hello=$hello spf=$spf carrier=$carrier
run h3 sink

note 0s  The routers build their tables. The short path through r2 wins.
note[it] 0s  I router costruiscono le tabelle. Vince il percorso corto via r2.
note 12s A constant stream starts: 50 datagrams per second from h1 to h3.
note[it] 12s Parte un flusso costante: 50 datagrammi al secondo da h1 a h3.
note 20s Link r2-r3 breaks. Count the datagrams lost at h3 until the detour through r4 and r5 works.
note[it] 20s Il collegamento r2-r3 si rompe. Conta i datagrammi persi su h3 finché la deviazione via r4 e r5 funziona.

at 12s run h1 cbr h3 rate=200k size=500 duration=50s
at 20s link r2 r3 down
at 58s run h1 traceroute h3
duration 66s
# datagrams lost = datagrams sent during the blackout, 50 per second (one every 20 ms)
at 65s run h1 check lost "h1:cbr.sent - h3:sink.received"
at 65s run h1 check detour "h1:traceroute.hops == 5 && h3:sink.received >= 1000"
# OSPF told by the interface: found at the next hello tick, then the SPF hold-down.
# OSPF with carrier=0: three to four hello intervals of silence (the dead interval), then SPF.
# RIP: until the next periodic update of r4, at most a (jittered) period.
at 65s run h1 check blackout "(is_$proto == is_ospf && $carrier == 1 && h1:check.lost <= ($hello + $spf) / 20 + 15) || (is_$proto == is_ospf && $carrier == 0 && h1:check.lost >= 3 * $hello / 20 - 15 && h1:check.lost <= (4 * $hello + $spf) / 20 + 15) || (is_$proto == is_rip && h1:check.lost <= (1.25 * $update + 1500) / 20)"
expect h1 check.detour == 1
expect h1 check.blackout == 1
