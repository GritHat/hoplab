title      NAT behaviours and UDP hole punching
title[it]  Comportamenti dei NAT e UDP hole punching
level  3
layer  3
topic  nat
about  Hosts a and b are each behind their own NAT and want to talk directly (a call, a game, a file
about  transfer). Neither can be reached from outside, so both first contact the rendezvous server rv, which
about  sees their public endpoints and introduces them. Then both send PUNCH datagrams to each other: every
about  outgoing datagram opens the sender's NAT for the peer.
about
about  With the default port-restricted NATs the first PUNCH of a arrives before b has sent anything and is filtered ("unsolicited
about  sender") and the following ones pass: the hole is punched. Change the two behaviours and predict the
about  result before running: full, restricted and port always work with each other; symmetric works only
about  against full or restricted (the cone side learns the real port from the first packet it receives);
about  symmetric with port or symmetric fails, and a relay would be needed.
about
about  Watch the translation tables of na and nb: a cone NAT keeps one public port per inside socket, a
about  symmetric one allocates a new port for every destination.
about[it]  Gli host a e b stanno ciascuno dietro il proprio NAT e vogliono parlarsi direttamente (una
about[it]  chiamata, un gioco, un trasferimento di file). Nessuno dei due è raggiungibile dall'esterno, quindi
about[it]  entrambi contattano prima il server di rendezvous rv, che vede i loro endpoint pubblici e li
about[it]  presenta. Poi entrambi si inviano datagrammi PUNCH: ogni datagramma in uscita apre il NAT del
about[it]  mittente al peer.
about[it]
about[it]  Con i NAT port-restricted di default il primo PUNCH di a arriva prima che b abbia inviato qualcosa e viene filtrato
about[it]  ("unsolicited sender"); i successivi passano: il buco è fatto. Cambia i due comportamenti e
about[it]  prevedi il risultato prima di eseguire: full, restricted e port funzionano sempre tra loro;
about[it]  symmetric funziona solo contro full o restricted (il lato cone impara la porta reale dal primo
about[it]  pacchetto che riceve); symmetric con port o con symmetric fallisce, e servirebbe un relay.
about[it]
about[it]  Osserva le tabelle di traduzione di na e nb: un NAT cone tiene una sola porta pubblica per socket
about[it]  interno, uno simmetrico assegna una porta nuova per ogni destinazione.
param  typea port NAT of host a: full, restricted, port, symmetric
param[it] typea NAT dell'host a: full, restricted, port, symmetric
param  typeb port NAT of host b: full, restricted, port, symmetric
param[it] typeb NAT dell'host b: full, restricted, port, symmetric
param+ waitb 100ms How long b waits before punching (0: both punch at once and nothing is filtered)
param[it] waitb Quanto aspetta b prima di inviare PUNCH (0: partono insieme e nulla viene filtrato)

node a host x=6 y=70
node na router x=26 y=70 role=NAT
node inet router x=50 y=45 role=INTERNET
node nb router x=74 y=70 role=NAT
node b host x=94 y=70
node rv host x=50 y=10 role=RENDEZVOUS
link a na delay=2ms
link na inet delay=10ms
link b nb delay=2ms
link nb inet delay=30ms
link rv inet delay=5ms

ip a 192.168.1.10/24
ip na:eth0 192.168.1.1/24
ip na:eth1 203.0.113.2/30
ip inet:eth0 203.0.113.1/30
ip b 10.0.0.10/24
ip nb:eth0 10.0.0.1/24
ip nb:eth1 198.51.100.2/30
ip inet:eth1 198.51.100.1/30
ip rv 192.0.2.10/24
ip inet:eth2 192.0.2.1/24
gw a 192.168.1.1
gw b 10.0.0.1
gw na 203.0.113.1
gw nb 198.51.100.1
gw rv 192.0.2.1

run na nat out=eth1 type=$typea
run nb nat out=eth1 type=$typeb
run rv rendezvous server
at 1s run a rendezvous join rv name=a peer=b
note 1s a registers: rv sees the public endpoint that na assigned, not 192.168.1.10.
note[it] 1s a si registra: rv vede l'endpoint pubblico assegnato da na, non 192.168.1.10.
at 2s run b rendezvous join rv name=b peer=a wait=$waitb
note 2s b registers too; rv introduces the two, and both start sending PUNCH to the other's public endpoint.
note[it] 2s Anche b si registra; rv presenta i due, ed entrambi iniziano a inviare PUNCH all'endpoint pubblico dell'altro.
note 3s The first PUNCH that arrives too early is filtered, yet it opened its own NAT for the peer: the next ones pass.
note[it] 3s Il primo PUNCH che arriva troppo presto viene filtrato, ma ha aperto il proprio NAT al peer: i successivi passano.
duration 8s

# the pairs that cannot be punched: symmetric against port-restricted or symmetric
at 7900ms run rv check hopeless "(is_$typea == is_symmetric && (is_$typeb == is_port || is_$typeb == is_symmetric)) || (is_$typeb == is_symmetric && is_$typea == is_port)"
at 7900ms run rv check punched "rv:rendezvous.introductions >= 1 && ((rv:check.hopeless == 0 && a:punch.ok == 1 && b:punch.ok == 1) || (rv:check.hopeless == 1 && a:punch.ok == 0 && b:punch.ok == 0 && na:nat.blocked >= 1 && nb:nat.blocked >= 1))"
# with the default timing the early PUNCH of a meets a filtering NAT on b's side
at 7900ms run rv check early "is_$typeb == is_full || $waitb < 50ms || nb:nat.blocked >= 1"
expect rv check.punched == 1
expect rv check.early == 1
