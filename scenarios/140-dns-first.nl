title     A first name lookup
title[it] Una prima risoluzione di nome
level  1
layer  7
topic  dns
about     A host asks a name server to turn a name into an address. One authoritative server holds the zone example.com; the client runs `dig` and gets back the A record. Watch the single query and its response. Try type=MX or a name that does not exist to see NXDOMAIN.
about[it] Un host chiede a un name server di tradurre un nome in un indirizzo. Un unico server autoritativo tiene la zona example.com; il client esegue `dig` e riceve il record A. Osserva la singola domanda e la sua risposta. Prova type=MX o un nome inesistente per vedere NXDOMAIN.
node h1 host
node ns host role=DNS
node web host role=web
link h1 ns delay=5ms
link ns web delay=5ms
auto all
run ns dns zone=example.com ttl=120 a=www:web a=@:web mx=@:mail txt=@:"hello from hoplab"
run web echo
at 1s   run h1 dig www.example.com server=ns
at 3s   run h1 dig example.com type=MX server=ns
at 5s   run h1 dig nope.example.com server=ns
duration 8s
expect h1 dig.answers >= 2
expect h1 dig.nxdomain == 1
expect ns dns.queries >= 3
