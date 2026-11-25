title     DNS cache poisoning and why port randomisation matters
title[it] Avvelenamento della cache DNS e perché conta la casualità della porta
level  4
layer  7
topic  dns
about     An off-path attacker forges answers for bank.example.com, racing the real server to plant a false address in the resolver cache. With randomize=0 the resolver uses a fixed source port and predictable ids, so a small flood wins. Set randomize=1 (the Kaminsky-era fix): the attacker must also guess a 14-bit port and the race collapses. This is a fully simulated teaching lab.
about[it] Un attaccante fuori percorso falsifica le risposte per bank.example.com, gareggiando col server vero per piazzare un indirizzo falso nella cache del resolver. Con randomize=0 il resolver usa una porta sorgente fissa e id prevedibili, così un piccolo flood vince. Con randomize=1 (la correzione dell'era Kaminsky) l'attaccante deve indovinare anche una porta di 14 bit e la gara crolla. È un laboratorio didattico interamente simulato.
param  randomize 0 Randomise the resolver source port and id (0 vulnerable, 1 hardened)
param[it] randomize Casualità di porta e id del resolver (0 vulnerabile, 1 corretto)
param+ guesses 200 Forged answers per attack
param+[it] guesses Risposte falsificate per attacco
node h1  host
node r   host role=resolver
node auth host role=auth
node evil host role=attacker
link h1 r    delay=2ms
link r auth  delay=40ms
link r evil  delay=3ms
auto all
run auth dns zone=example.com ttl=60 a=bank:10.1.1.1
run r    dns resolver root=auth randomize=$randomize
at 1s run evil dnsspoof resolver=r as=auth name=bank.example.com fake=6.6.6.6 guesses=$guesses port=53
at 4s run h1 dig bank.example.com server=r
duration 7s
expect evil spoof.forged >= 1
