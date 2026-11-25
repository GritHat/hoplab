title     CNAME chains, negative caching and the TTL trade-off
title[it] Catene di CNAME, caching negativo e il compromesso del TTL
level  3
layer  7
topic  dns
about     Aliases and the cost of caching. www is a CNAME to host, host is an A record; the resolver follows the chain and caches each step. Repeated lookups of a missing name are answered from the negative cache for the SOA minimum. Raise or lower `ttl`: a short TTL means fresh data but more upstream traffic, a long TTL the opposite.
about[it] Alias e il costo della cache. www è un CNAME verso host, host è un record A; il resolver segue la catena e mette in cache ogni passo. Ricerche ripetute di un nome assente vengono servite dalla cache negativa per il minimo del SOA. Alza o abbassa `ttl`: un TTL corto significa dati freschi ma più traffico a monte, un TTL lungo il contrario.
param  ttl 60 Record TTL in seconds
param[it] ttl Durata (TTL) dei record, in secondi
param+ negttl 20 Negative-cache TTL in seconds
param+[it] negttl TTL della cache negativa, in secondi
node h1 host
node r  host role=resolver
node auth host role=auth
link h1 r   delay=2ms
link r auth delay=15ms
auto all
run auth dns zone=example.com ttl=$ttl negttl=$negttl a=host:10.7.7.7 cname=www:host cname=shop:www
run r    dns resolver root=auth
at 1s run h1 dig shop.example.com server=r
at 3s run h1 dig ghost.example.com server=r
at 5s run h1 dig ghost.example.com server=r
at 7s run h1 dig www.example.com server=r
duration 10s
expect h1 dig.answers >= 2
expect h1 dig.nxdomain >= 1
# shop -> www -> host is resolved once (2 questions: the chain comes back in one answer, then ghost).
# The lookup of www at 7 s costs one more question only if the records have expired (ttl under 6 s),
# and the second lookup of ghost at 5 s only if the negative answer has (negttl under 2 s)
at 9900ms run r check cached "r:dns.upstream_queries == 2 + ($ttl < 6) + ($negttl < 2)"
expect r check.cached == 1
