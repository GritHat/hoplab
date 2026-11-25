title     DNS cache poisoning and why port randomisation matters, over IPv4 and IPv6
title[it] Avvelenamento della cache DNS e perché conta la casualità della porta, su IPv4 e IPv6
level  4
layer  7
topic  dns
about     A resolver believes a UDP answer that comes from the address it asked, arrives on the port the question left from, and repeats its 16-bit identifier. An attacker who is not on the path (evil) cannot see the question, but can send packets with a forged source address: here it makes the resolver ask for a name and then races the real, slower server with forged answers. Everything happens inside the emulated network.
about     With randomize=0 the resolver always asks from port 53 with identifiers 1, 2, 3, ...: there is nothing to guess, every race is won, and h1 is then given the false address for as long as the TTL says. With randomize=1 identifier and port are drawn at random, 2^entropy possibilities, and a forger that lands n answers in time wins a race with probability about n / 2^entropy. With the real 30 bits that is never. So that you can see the law and not only its extremes, the lab repeats the race `rounds` times on fresh names and lets you shrink the entropy: try entropy=12, 10, 8 and compare the races won with the odds printed at the end.
about     Watch: the dns panel of r (cache, mismatched answers), the dnsspoof panel of evil, the last log lines. Try: guesses (more forged answers per race, until the genuine answer cuts the race short at about 400); a faster authoritative server (link r auth delay) shortens the window. The lasting defence is not more entropy but signed data (DNSSEC), which this emulator does not model.
about     With family=6 the same race is run over IPv6 and the forged record is an AAAA. Nothing else changes, and that is the lesson: the question still carries a 16-bit identifier and leaves from a 16-bit port, so the resolver has exactly the same entropy. The enormous IPv6 address space protects hosts from being found by scanning; it does nothing for a resolver whose address the forger already knows.
about[it] Un resolver crede a una risposta UDP che proviene dall'indirizzo interrogato, arriva sulla porta da cui è partita la domanda e ne ripete l'identificatore a 16 bit. Un attaccante che non è sul percorso (evil) non vede la domanda, ma può inviare pacchetti con indirizzo sorgente falsificato: qui fa chiedere un nome al resolver e poi gareggia con il server vero, più lento, a colpi di risposte false. Tutto avviene dentro la rete emulata.
about[it] Con randomize=0 il resolver chiede sempre dalla porta 53 con identificatori 1, 2, 3, ...: non c'è nulla da indovinare, ogni gara è vinta, e h1 riceve poi l'indirizzo falso per tutta la durata del TTL. Con randomize=1 identificatore e porta sono estratti a caso, 2^entropy possibilità, e un falsario che fa arrivare in tempo n risposte vince una gara con probabilità circa n / 2^entropy. Con i 30 bit reali, mai. Perché tu possa vedere la legge e non solo i suoi estremi, il laboratorio ripete la gara `rounds` volte su nomi nuovi e ti lascia ridurre l'entropia: prova entropy=12, 10, 8 e confronta le gare vinte con la probabilità stampata alla fine.
about[it] Osserva: il pannello dns di r (cache, risposte non corrispondenti), il pannello dnsspoof di evil, le ultime righe del registro. Prova: guesses (più risposte false per gara, finché la risposta vera interrompe la gara intorno a 400); un server autoritativo più veloce (ritardo del collegamento r auth) accorcia la finestra. La difesa duratura non è più entropia ma dati firmati (DNSSEC), che questo emulatore non modella.
about[it] Con family=6 la stessa gara si svolge su IPv6 e il record falsificato è un AAAA. Non cambia nient'altro, ed è questa la lezione: la domanda porta sempre un identificatore a 16 bit e parte da una porta a 16 bit, quindi il resolver ha esattamente la stessa entropia. L'enorme spazio di indirizzi IPv6 protegge gli host dall'essere trovati per scansione; non fa nulla per un resolver di cui il falsario conosce già l'indirizzo.
param  family 4 Address family of the race: 4 (forged A record) or 6 (forged AAAA record)
param[it] family Famiglia di indirizzi della gara: 4 (record A falso) o 6 (record AAAA falso)
param  randomize 0 Randomise the resolver source port and id (0 vulnerable, 1 hardened)
param[it] randomize Casualità di porta e id del resolver (0 vulnerabile, 1 corretto)
param  guesses 200 Forged answers per race
param[it] guesses Risposte falsificate per gara
param+ entropy 30 Random bits of the hardened resolver (30 = real; 8 to 12 make the odds visible)
param[it] entropy Bit casuali del resolver corretto (30 = reale; da 8 a 12 rendono visibile la probabilità)
param+ rounds 40 Races run by the forger, each on a fresh name (about 70 fit in the run)
param[it] rounds Gare tentate dal falsario, ognuna su un nome nuovo (nell'esecuzione ce ne stanno circa 70)
node h1  host
node r   host role=resolver
node auth host role=auth
node evil host role=attacker
link h1 r    delay=2ms
link r auth  delay=40ms
link r evil  delay=3ms
auto all
auto all6
run auth dns zone=example.com ttl=60 a=bank:10.1.1.1 aaaa=bank:2001:db8:1::1
run r    dns resolver root=auth family=$family randomize=$randomize entropy=$entropy
# neighbours resolved in advance (ARP, Neighbor Discovery), so that the first race is like the others
at 1200ms run evil ping r count=1
at 1200ms run evil ping r count=1 v6=1
at 1200ms run r ping auth count=1
at 1200ms run r ping auth count=1 v6=1
at 2s run evil dnsspoof resolver=r as=auth name=bank.example.com family=$family guesses=$guesses rounds=$rounds blind=$randomize entropy=$entropy
note 2s evil asks r for a name and at once sends its forged answers, with the address of auth as source. The genuine answer needs 80 ms to come back.
note[it] 2s evil chiede un nome a r e subito invia le sue risposte false, con l'indirizzo di auth come sorgente. La risposta vera impiega 80 ms a tornare.
note 3s The race is repeated on names nobody has asked before: a name already in the cache gives no race. Count the wins.
note[it] 3s La gara viene ripetuta su nomi che nessuno ha ancora chiesto: un nome già in cache non dà alcuna gara. Conta le vittorie.
at 10s run h1 dig bank.example.com server=r family=$family
note 10s h1 asks for the bank, A and AAAA: it gets whatever is in the cache of r, true or false, and cannot tell the difference. Only the record of the family the race was run for can be false.
note[it] 10s h1 chiede della banca, A e AAAA: riceve ciò che c'è nella cache di r, vero o falso, e non può distinguerlo. Solo il record della famiglia su cui si è svolta la gara può essere falso.
duration 13s
# the odds of one race: forged answers that arrived in time, over the pairs the resolver draws from
at 12900ms run evil check odds "1 - exp(-evil:spoof.per_round / pow(2, $entropy))"
# predictable resolver: nothing to guess, every race is won, the first included (the one h1 pays for)
at 12900ms run evil check predictable "$randomize == 1 || (evil:spoof.success == evil:spoof.rounds && evil:spoof.first == 1 && evil:spoof.rounds >= min($rounds, 70))"
# hardened resolver: the wins follow the odds (within four standard deviations of a binomial)
at 12900ms run evil check hardened "$randomize == 0 || (evil:spoof.rounds >= min($rounds, 70) && abs(evil:spoof.success - evil:spoof.rounds * evil:check.odds) <= 4 * sqrt(evil:spoof.rounds * evil:check.odds * (1 - evil:check.odds)) + 1)"
expect evil check.predictable == 1
expect evil check.hardened == 1
# h1 asks for both records: the A and the AAAA
expect h1 dig.answers == 2
