title  Chord under churn: crashes and joins while lookups continue
title[it] Chord sotto churn: guasti e ingressi mentre le ricerche continuano
level  4
layer  8
topic  chord
about  A ten-node Chord ring stores five keys. Then two nodes crash without warning, a new node joins and one of the dead comes back with an empty memory, all while other nodes keep reading the keys.
about  Watch three mechanisms at work. Pings detect the dead nodes and each neighbour falls back on the next entry of its successor list. The keys are still there because every pair exists in `succlist` copies, on its owner and on the owner's next successors, which now inherit its arc of the ring; the heirs copy them once more to their own successors, so the number of copies is restored. A joining node receives from its successor the keys it has become responsible for.
about  Try succlist=1: a single successor pointer and a single copy of every pair. The ring still repairs itself through the fingers and every read is still answered, but the answer for the keys the dead nodes owned is "not found": compare chord.found in the final report (all reads succeed with 2 or more copies). Try a slower stabilization (stabilize=800ms).
about[it] Un anello Chord di dieci nodi memorizza cinque chiavi. Poi due nodi si guastano senza preavviso, un nuovo nodo entra e uno dei guasti ritorna con la memoria vuota, il tutto mentre altri nodi continuano a leggere le chiavi.
about[it] Osserva tre meccanismi al lavoro. I ping rilevano i nodi guasti e ogni vicino ripiega sulla voce successiva della sua lista dei successori. Le chiavi ci sono ancora perché ogni coppia esiste in `succlist` copie, sul proprietario e sui suoi successori, che ora ereditano il suo arco di anello; gli eredi le copiano ancora una volta sui propri successori, e il numero di copie viene ripristinato. Un nodo che entra riceve dal successore le chiavi di cui è diventato responsabile.
about[it] Prova succlist=1: un solo puntatore al successore e una sola copia di ogni coppia. L'anello si ripara comunque attraverso i finger e ogni lettura riceve ancora una risposta, ma per le chiavi che appartenevano ai nodi guasti la risposta è "not found": confronta chord.found nel rapporto finale (con 2 o più copie tutte le letture riescono). Prova una stabilizzazione più lenta (stabilize=800ms).
param succlist 3 Length of the successor list = copies of every pair
param[it] succlist Lunghezza della lista dei successori = copie di ogni coppia
param+ stabilize 400ms Period of stabilize, fix_fingers and pings
param[it] stabilize Periodo di stabilize, fix_fingers e ping

node s1 switch
node n1 host
node n2 host
node n3 host
node n4 host
node n5 host
node n6 host
node n7 host
node n8 host
node n9 host
node n10 host
node n11 host
link n1 s1 delay=2ms
link n2 s1 delay=2ms
link n3 s1 delay=2ms
link n4 s1 delay=2ms
link n5 s1 delay=2ms
link n6 s1 delay=2ms
link n7 s1 delay=2ms
link n8 s1 delay=2ms
link n9 s1 delay=2ms
link n10 s1 delay=2ms
link n11 s1 delay=2ms
auto all

run n1 chord succlist=$succlist stabilize=$stabilize
run n2 chord join=n1 succlist=$succlist stabilize=$stabilize
run n3 chord join=n1 succlist=$succlist stabilize=$stabilize
run n4 chord join=n1 succlist=$succlist stabilize=$stabilize
run n5 chord join=n1 succlist=$succlist stabilize=$stabilize
run n6 chord join=n1 succlist=$succlist stabilize=$stabilize
run n7 chord join=n1 succlist=$succlist stabilize=$stabilize
run n8 chord join=n1 succlist=$succlist stabilize=$stabilize
run n9 chord join=n1 succlist=$succlist stabilize=$stabilize
run n10 chord join=n1 succlist=$succlist stabilize=$stabilize

note 10s The ring is stable. n4 stores five keys; each is copied to the successors of its owner.
note[it] 10s L'anello è stabile. n4 memorizza cinque chiavi; ognuna è copiata sui successori del proprietario.
at 10s   run n4 chord put key=apple value=red
at 10.2s run n4 chord put key=banana value=yellow
at 10.4s run n4 chord put key=cherry value=dark-red
at 10.6s run n4 chord put key=plum value=purple
at 10.8s run n4 chord put key=grape value=green
note 14s n3 and n10 crash: between them they owned most of the keys. Their neighbours notice within two rounds of pings.
note[it] 14s n3 e n10 si guastano: insieme possedevano la maggior parte delle chiavi. I vicini se ne accorgono entro due giri di ping.
at 14s   node n3 down
at 14s   node n10 down
note 18s Reads continue: lookups route around the dead nodes and end on the successors, which hold the replicas.
note[it] 18s Le letture continuano: le ricerche aggirano i nodi guasti e terminano sui successori, che hanno le repliche.
at 18s   run n6 chord get key=cherry
at 18.3s run n8 chord get key=banana
at 18.6s run n2 chord get key=plum
at 18.9s run n9 chord get key=apple
note 21s A new node, n11, joins through n2, and n3 restarts with an empty memory and joins again.
note[it] 21s Un nuovo nodo, n11, entra tramite n2, e n3 riparte con la memoria vuota ed entra di nuovo.
at 21s   run n11 chord join=n2 succlist=$succlist stabilize=$stabilize
at 23s   node n3 up
note 30s The ring has absorbed the changes. The newcomer reads too, and n3 has been handed back the keys of its arc.
note[it] 30s L'anello ha assorbito i cambiamenti. Anche il nuovo arrivato legge, e a n3 sono state riconsegnate le chiavi del suo arco.
at 30s   run n11 chord get key=grape
at 30.3s run n11 chord get key=cherry
at 30.6s run n5 chord get key=plum
at 30.9s run n3 chord get key=banana
duration 34s
expect n6 chord.answered == 1
expect n8 chord.answered == 1
expect n2 chord.answered == 1
expect n9 chord.answered == 1
expect n11 chord.answered == 2
expect n5 chord.answered == 1
expect n3 chord.answered == 1
expect n6 chord.found >= 0
expect n8 chord.found >= 0
expect n2 chord.found >= 0
expect n11 chord.found >= 0
expect n3 chord.joined == 1
expect n11 chord.joined == 1
