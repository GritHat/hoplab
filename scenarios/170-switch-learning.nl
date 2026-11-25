title      Learning switches: flood, learn, forward
title[it]  Switch che imparano: inondare, imparare, inoltrare
level  1
layer  2
topic  switching
about  Three hosts on two switches. A switch starts knowing nothing: it looks at the SOURCE
about  address of every frame to learn behind which port each station lives, and at the
about  DESTINATION address to decide where to send it. Broadcasts (the ARP request) and unknown
about  destinations are flooded out of every other port; known destinations go out of one port only.
about
about  Watch: the first ARP request reaches every host; the ARP reply and the pings travel only
about  between h1 and h3, and h2 never sees them. Select a switch to read its forwarding table.
about  Try: set detail 3 to see every learning step; start `h2 ping h1` and see how little is flooded.
about[it]  Tre host su due switch. Uno switch all'inizio non sa nulla: guarda l'indirizzo SORGENTE di
about[it]  ogni frame per imparare dietro quale porta si trova ogni stazione, e l'indirizzo di
about[it]  DESTINAZIONE per decidere dove inviarlo. I broadcast (la richiesta ARP) e le destinazioni
about[it]  sconosciute vengono inondati su tutte le altre porte; le destinazioni note escono da una sola porta.
about[it]
about[it]  Osserva: la prima richiesta ARP raggiunge tutti gli host; la risposta ARP e i ping viaggiano
about[it]  solo fra h1 e h3, e h2 non li vede mai. Seleziona uno switch per leggerne la tabella di inoltro.
about[it]  Prova: imposta il dettaglio a 3 per vedere ogni passo dell'apprendimento; avvia `h2 ping h1`
about[it]  e guarda quanto poco viene inondato.
param      delay 5ms Delay of every link
param[it]  delay Ritardo di ogni collegamento

node s1 switch x=35 y=40
node s2 switch x=65 y=40
node h1 host x=15 y=20
node h2 host x=15 y=70
node h3 host x=85 y=40
link h1 s1 delay=$delay
link h2 s1 delay=$delay
link s1 s2 delay=$delay
link s2 h3 delay=$delay
ip h1 10.0.0.1/24
ip h2 10.0.0.2/24
ip h3 10.0.0.3/24

run h1 ping h3 count=3
at 5s run h2 ping h1 count=2

note 0s h1 does not know the MAC address of h3: it broadcasts an ARP request. s1 learns "h1 is on p0" and floods the request to h2 and to s2, which floods it to h3.
note[it] 0s h1 non conosce l'indirizzo MAC di h3: invia una richiesta ARP in broadcast. s1 impara "h1 è su p0" e inonda la richiesta verso h2 e verso s2, che la inonda verso h3.
note 1s The ARP reply of h3 is unicast to h1: both switches already know where h1 is, so it follows one path and h2 sees nothing. On the way the switches learn where h3 is.
note[it] 1s La risposta ARP di h3 è unicast verso h1: entrambi gli switch sanno già dov'è h1, quindi segue un solo percorso e h2 non vede nulla. Lungo la strada gli switch imparano dov'è h3.
note 5s h2 pings h1: one more broadcast (h2's ARP request), then everything is forwarded on a single port.
note[it] 5s h2 fa ping verso h1: un altro broadcast (la richiesta ARP di h2), poi tutto viene inoltrato su una sola porta.

duration 9s
expect h1 ping.reply == 3
expect h2 ping.reply == 2
expect s1 bridge.flood >= 2
expect s1 bridge.flood <= 4
expect h2 eth.not_for_me == 0
