title      Names in a dual-stack world: A, AAAA and DNS over IPv6
title[it]  I nomi in un mondo dual-stack: A, AAAA e DNS su IPv6
level 2
layer 7
topic dns
about      Every node here speaks both IPv4 and IPv6. A name can have an A record (an IPv4 address), an AAAA record (an IPv6 address) or both, and which records exist is independent of how the question travels: the same DNS message can be carried over IPv4 or IPv6. The host h1 first asks its resolver for www.example.com without naming a type, and like any dual-stack host it sends two questions, one for A and one for AAAA. The resolver starts from the root, is referred to the server of example.com with glue of both families, and gets the answers. Then h1 asks for the AAAA of a name that only has an A (the name exists, the type does not: NODATA, not NXDOMAIN), and finally resolves the name and pings the address it got, once over IPv6 and once over IPv4.
about
about      Things to try: set v6 to 0 and watch every DNS message travel over IPv4 while the AAAA answers stay exactly the same; open a DNS packet (p, Enter) and compare the 4-byte and 16-byte record data; look at the cache in the dns tab of res, where the name servers appear with both an A and an AAAA record; type  :h1 dig www.example.com trace=1 root=root v6=1  to follow the referrals yourself.
about[it]  Qui ogni nodo parla sia IPv4 sia IPv6. Un nome può avere un record A (un indirizzo IPv4), un record AAAA (un indirizzo IPv6) o entrambi, e quali record esistano non dipende da come viaggia la domanda: lo stesso messaggio DNS può essere trasportato su IPv4 o su IPv6. L'host h1 chiede prima al suo resolver www.example.com senza indicare un tipo e, come ogni host dual-stack, invia due domande, una per A e una per AAAA. Il resolver parte dalla radice, viene rimandato al server di example.com con glue di entrambe le famiglie, e ottiene le risposte. Poi h1 chiede l'AAAA di un nome che ha solo un A (il nome esiste, il tipo no: NODATA, non NXDOMAIN), e infine risolve il nome e fa ping all'indirizzo ottenuto, una volta su IPv6 e una su IPv4.
about[it]
about[it]  Da provare: imposta v6 a 0 e osserva ogni messaggio DNS viaggiare su IPv4 mentre le risposte AAAA restano identiche; apri un pacchetto DNS (p, Invio) e confronta i dati dei record da 4 e da 16 byte; guarda la cache nella scheda dns di res, dove i name server compaiono con un record A e uno AAAA; scrivi  :h1 dig www.example.com trace=1 root=root v6=1  per seguire tu stesso i rimandi.
param      v6 1 Carry the DNS messages over IPv6 (1) or IPv4 (0)
param[it]  v6 Trasporta i messaggi DNS su IPv6 (1) o IPv4 (0)
param+     delay 5ms Delay of each link
param[it]  delay Ritardo di ogni collegamento

node h1 host
node r1 router
node s1 switch
node res host role=resolver
node root host role=root
node auth host role=auth
node web host role=www
link h1 r1 delay=$delay
link r1 s1 delay=$delay
link s1 res delay=1ms
link s1 root delay=1ms
link s1 auth delay=1ms
link s1 web delay=1ms
auto all
auto all6

run root dns zone=. ttl=3600 ns=example.com:ns.example.com glue=ns.example.com:auth glue6=ns.example.com:auth
run auth dns zone=example.com ttl=60 ns=@:ns a=ns:auth aaaa=ns:auth a=www:web aaaa=www:web a=old:web
run res dns resolver root=root v6=$v6

at 2s run h1 dig www.example.com server=res v6=$v6
note 2s    No type asked: a dual-stack host sends two questions, A and AAAA. The resolver walks root, then example.com.
note[it] 2s Nessun tipo indicato: un host dual-stack invia due domande, A e AAAA. Il resolver passa dalla radice, poi da example.com.
at 5s run h1 dig old.example.com type=AAAA
note 5s    old.example.com has an A record but no AAAA: the answer is empty (NODATA), and the name still exists.
note[it] 5s old.example.com ha un record A ma nessun AAAA: la risposta è vuota (NODATA), e il nome esiste comunque.
at 7s run h1 dig www.example.com type=AAAA then="ping count=2"
note 7s    Resolve, then use: the AAAA comes from the resolver's cache and h1 pings that address over IPv6.
note[it] 7s Risolvi, poi usa: l'AAAA arriva dalla cache del resolver e h1 fa ping a quell'indirizzo su IPv6.
at 11s run h1 dig www.example.com type=A then="ping count=2"
note 11s   The same name, the other family: an A record and a ping over IPv4.
note[it] 11s Lo stesso nome, l'altra famiglia: un record A e un ping su IPv4.
duration 15s
expect h1 dig.answers == 4
expect h1 dig.aaaa == 2
expect h1 dig.nodata == 1
expect h1 ping.reply == 4
expect res dns.cache_hits >= 2
expect res dns.upstream_v6 >= 3 if $v6 == 1
expect res dns.upstream_v6 == 0 if $v6 == 0
