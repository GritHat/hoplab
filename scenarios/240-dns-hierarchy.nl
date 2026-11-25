title     The DNS hierarchy and the cache
title[it] La gerarchia del DNS e la cache
level  2
layer  7
topic  dns
about     Iterative resolution across the delegation tree. A recursive resolver starts at the root, is referred to the .com servers, then to the authoritative server of example.com, and finally gets the answer; the second lookup is served from the cache. `dig trace=1` makes the client walk the tree itself. Compare the number of upstream queries before and after caching.
about[it] Risoluzione iterativa lungo l'albero delle deleghe. Un resolver ricorsivo parte dalla radice, viene rimandato ai server di .com, poi al server autoritativo di example.com, e infine ottiene la risposta; la seconda ricerca arriva dalla cache. `dig trace=1` fa percorrere l'albero al client stesso. Confronta il numero di query a monte prima e dopo la cache.
node h1 host
node r  router role=resolver
node root host role=root
node com  host role=TLD
node auth host role=auth
link h1 r    delay=2ms
link r root  delay=20ms
link r com   delay=20ms
link r auth  delay=20ms
auto all
run root dns zone=. ns=@:a.root glue=a.root:root ns=com:a.gtld glue=a.gtld:com ttl=3600
run com  dns zone=com ns=@:a.gtld glue=a.gtld:com ns=example:ns.example glue=ns.example:auth ttl=3600
run auth dns zone=example.com a=www:10.9.9.9 a=@:10.9.9.9 ns=@:ns.example glue=ns.example:auth ttl=60
run r    dns resolver root=root
at 1s run h1 dig www.example.com server=r
at 4s run h1 dig www.example.com server=r
at 6s run h1 dig www.example.com type=A trace=1 root=root
duration 9s
expect h1 dig.answers >= 2
expect r  dns.cache_hits >= 1
expect r  dns.upstream_queries >= 3
