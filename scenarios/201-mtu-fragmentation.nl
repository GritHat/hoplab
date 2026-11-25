title      MTU, fragmentation and path MTU discovery
title[it]  MTU, frammentazione e scoperta della MTU del percorso
level 2
layer 3
topic ip
about      The middle link of this path only carries 600-byte packets. A 1400-byte ping is cut into fragments by r1; each fragment travels as an independent packet and only the destination puts them back together, so losing one fragment loses the whole datagram. With the "don't fragment" flag the router refuses, drops the packet and reports the MTU of the next hop in an ICMP "fragmentation needed" message: this is how a sender discovers the path MTU and shrinks its packets.
about      Things to try: in the Packets view open a fragment and read the identification, MF flag and offset fields; add loss on the middle link and compare how often a fragmented ping fails with a small one.
about[it]  Il collegamento centrale di questo percorso trasporta solo pacchetti da 600 byte. Un ping da 1400 byte viene tagliato in frammenti da r1; ogni frammento viaggia come un pacchetto indipendente e solo la destinazione li rimette insieme, quindi perdere un frammento significa perdere l'intero datagramma. Con il flag "don't fragment" il router si rifiuta, scarta il pacchetto e riferisce la MTU del salto successivo in un messaggio ICMP "fragmentation needed": è così che un mittente scopre la MTU del percorso e riduce i suoi pacchetti.
about[it]  Da provare: nella vista Pacchetti apri un frammento e leggi i campi identification, flag MF e offset; aggiungi perdita sul collegamento centrale e confronta quanto spesso fallisce un ping frammentato rispetto a uno piccolo.
param      mtu 600 MTU of the middle link
param[it]  mtu MTU del collegamento centrale
param+     loss 0 Loss on the middle link (0..1)
param[it]  loss Perdita sul collegamento centrale (0..1)

node h1 host
node r1 router
node r2 router
node h2 host
link h1 r1 delay=2ms
link r1 r2 delay=10ms mtu=$mtu loss=$loss
link r2 h2 delay=2ms
auto all

run h1 ping h2 count=2 size=1400
note 0     1400 bytes of data do not fit in $mtu: r1 fragments, h2 reassembles, and the reply is fragmented by r2.
note[it] 0 1400 byte di dati non entrano in $mtu: r1 frammenta, h2 riassembla, e la risposta viene frammentata da r2.
at 4s run h1 ping h2 count=2 size=1400 df=1
note 4s    Same ping with DF set: r1 drops it and answers "fragmentation needed, next-hop MTU $mtu".
note[it] 4s Stesso ping con DF impostato: r1 lo scarta e risponde "fragmentation needed, MTU del salto successivo $mtu".
at 8s run h1 ping h2 count=2 size=500 df=1
note 8s    The sender shrinks its packets below the path MTU: they pass untouched.
note[it] 8s Il mittente riduce i pacchetti sotto la MTU del percorso: passano intatti.
duration 12s
expect h1 ping.reply == 4
expect h1 ping.error == 2
expect r1 ip.df_drop == 2
expect h2 ip.reassembled == 2
