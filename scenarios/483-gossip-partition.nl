title  Gossip across a partition: anti-entropy brings everyone back in sync
title[it] Gossip attraverso una partizione: l'anti-entropia rimette tutti in sincronia
level  4
layer  8
topic  gossip
about  Ten nodes in two racks (n1..n5 on switch sa, n6..n10 on sb). At 2 s the link between the racks fails. During the partition n1 starts rumor "A" and n10 starts rumor "B": each spreads only on its own side, and the pushing nodes soon lose interest. SWIM, running underneath, declares the other half dead. At 14 s the link is repaired. Nobody is pushing any more, but the periodic pull (anti-entropy: "this is what I know, send me what I miss") makes the two halves exchange what they hold; SWIM members now and then probe the dead, who refute with a higher incarnation, and the group becomes one again. Eventually every node knows both rumors: eventual consistency without any coordinator.
about  Watch: gossip.infected on both sides (1 during the partition, 2 after), member lists going ✗ and back to ●. Experiments: mode=push — after the heal nothing repairs the difference, the scenario fails: rumor mongering alone is not enough; swim=0 to isolate the anti-entropy part; a longer partition.
about[it] Dieci nodi in due rack (n1..n5 sullo switch sa, n6..n10 su sb). A 2 s il collegamento tra i rack si guasta. Durante la partizione n1 fa partire la voce "A" e n10 la voce "B": ognuna si diffonde solo sul proprio lato, e i nodi che la spingono perdono presto interesse. SWIM, che gira sotto, dichiara morta l'altra metà. A 14 s il collegamento viene riparato. Nessuno sta più spingendo, ma il pull periodico (anti-entropia: "ecco cosa so, mandami ciò che mi manca") fa scambiare alle due metà ciò che possiedono; i membri SWIM ogni tanto sondano i morti, che smentiscono con un'incarnazione più alta, e il gruppo torna uno solo. Alla fine ogni nodo conosce entrambe le voci: consistenza finale senza alcun coordinatore.
about[it] Da osservare: gossip.infected sui due lati (1 durante la partizione, 2 dopo), gli elenchi dei membri che passano a ✗ e tornano a ●. Esperimenti: mode=push — dopo la riparazione nulla ripara la differenza, lo scenario fallisce: la sola diffusione di voci non basta; swim=0 per isolare la parte di anti-entropia; una partizione più lunga.
param  mode pushpull Style: push, pull or pushpull
param[it] mode Stile: push, pull o pushpull
param  swim 1 Run SWIM membership too (0/1)
param[it] swim Esegui anche la membership SWIM (0/1)
param+ interval 500ms Gossip round length
param[it] interval Durata di un round di gossip
param+ suspect 3s SWIM suspicion timeout
param[it] suspect Timeout di sospetto di SWIM

node sa switch x=35 y=50
node sb switch x=65 y=50
node n1 host x=8 y=12
node n2 host x=22 y=5
node n3 host x=5 y=50
node n4 host x=22 y=95
node n5 host x=8 y=88
node n6 host x=92 y=12
node n7 host x=78 y=5
node n8 host x=95 y=50
node n9 host x=78 y=95
node n10 host x=92 y=88
link n1 sa delay=2ms
link n2 sa delay=2ms
link n3 sa delay=2ms
link n4 sa delay=2ms
link n5 sa delay=2ms
link n6 sb delay=2ms
link n7 sb delay=2ms
link n8 sb delay=2ms
link n9 sb delay=2ms
link n10 sb delay=2ms
link sa sb delay=5ms
auto all

run n1 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect
run n2 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect
run n3 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect
run n4 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect
run n5 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect
run n6 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect
run n7 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect
run n8 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect
run n9 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect
run n10 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10 mode=$mode interval=$interval swim=$swim suspect=$suspect

at 2s link sa sb down
note 2s The racks are separated.
note[it] 2s I rack sono separati.
at 3s run n1 gossip rumor text=A
at 4s run n10 gossip rumor text=B
note 4s Two rumors, one per side. Each reaches only its own half.
note[it] 4s Due voci, una per lato. Ciascuna raggiunge solo la propria metà.
note 9s With SWIM on, each half has by now declared the other half dead.
note[it] 9s Con SWIM attivo, ogni metà ha ormai dichiarato morta l'altra.
at 14s link sa sb up
note 14s Link repaired. Pull exchanges and probes of "dead" members start to stitch the group together.
note[it] 14s Collegamento riparato. Gli scambi pull e le sonde ai membri "morti" cominciano a ricucire il gruppo.
duration 60s
expect n1 gossip.infected == 2
expect n3 gossip.infected == 2
expect n5 gossip.infected == 2
expect n6 gossip.infected == 2
expect n8 gossip.infected == 2
expect n10 gossip.infected == 2
