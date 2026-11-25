title      VLANs and trunks: several networks on the same switches
title[it]  VLAN e trunk: più reti sugli stessi switch
level 2
layer 2
topic vlan
about      Two switches, four hosts, two virtual LANs. Ports are assigned to VLAN 10 or VLAN 20; the cable between the switches is a trunk that carries both, marking each frame with an 802.1Q tag. A broadcast from a host in VLAN 10 reaches only the other VLAN 10 ports: the two groups are separate networks even though they share the hardware. a1 can ping a2 across the trunk, but its ARP request for b2 is never seen by b2: crossing between VLANs needs a router.
about
about      Things to try: open a frame captured on the s1-s2 link and find the 802.1Q tag; look at the MAC table tab of a switch, which is kept per VLAN; move a port with  :vlan  in the scenario file and run again.
about[it]  Due switch, quattro host, due LAN virtuali. Le porte sono assegnate alla VLAN 10 o alla VLAN 20; il cavo fra gli switch è un trunk che le trasporta entrambe, marcando ogni frame con un tag 802.1Q. Un broadcast da un host della VLAN 10 raggiunge solo le altre porte della VLAN 10: i due gruppi sono reti separate anche se condividono l'hardware. a1 può fare ping ad a2 attraverso il trunk, ma la sua richiesta ARP per b2 non viene mai vista da b2: per passare da una VLAN all'altra serve un router.
about[it]
about[it]  Da provare: apri un frame catturato sul collegamento s1-s2 e trova il tag 802.1Q; guarda la scheda Tabella MAC di uno switch, tenuta separata per VLAN; sposta una porta con  vlan  nel file dello scenario e riesegui.

node a1 host x=8 y=20
node b1 host x=8 y=80
node s1 switch x=35 y=50
node s2 switch x=65 y=50
node a2 host x=92 y=20
node b2 host x=92 y=80
link a1 s1:p0 delay=2ms
link b1 s1:p1 delay=2ms
link s1:p2 s2:p2 delay=5ms
link a2 s2:p0 delay=2ms
link b2 s2:p1 delay=2ms
vlan s1:p0 10
vlan s1:p1 20
vlan s1:p2 trunk
vlan s2:p0 10
vlan s2:p1 20
vlan s2:p2 trunk
ip a1 10.0.0.1/24
ip a2 10.0.0.2/24
ip b1 10.0.0.3/24
ip b2 10.0.0.4/24

run a1 ping a2 count=2
note 0     VLAN 10: the ARP broadcast of a1 crosses the trunk with tag 10 and reaches only a2.
note[it] 0 VLAN 10: il broadcast ARP di a1 attraversa il trunk con tag 10 e raggiunge solo a2.
at 3s run b1 ping b2 count=2
note 3s    VLAN 20 works the same way, on the same switches and the same trunk.
note[it] 3s La VLAN 20 funziona allo stesso modo, sugli stessi switch e sullo stesso trunk.
at 6s run a1 ping b2 count=1
note 6s    a1 asks for b2 (same IP subnet, different VLAN): the question never leaves VLAN 10. No answer.
note[it] 6s a1 cerca b2 (stessa sottorete IP, VLAN diversa): la domanda non esce mai dalla VLAN 10. Nessuna risposta.
duration 12s
expect a1 ping.reply == 2
expect b1 ping.reply == 2
expect a1 arp.failed == 1
