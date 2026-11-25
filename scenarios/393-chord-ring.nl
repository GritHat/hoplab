title  Chord: ten nodes form a ring and share a hash table
title[it] Chord: dieci nodi formano un anello e condividono una tabella hash
level  3
layer  8
topic  chord
about  Ten machines build a distributed hash table with no directory. n1 creates the ring, the others join through it at the same moment: at first everybody points at n1, then the periodic stabilization sorts the ring out and the finger tables fill in.
about  Then keys are stored from one node and read from others. Each operation hashes the key to a point of the ring and routes towards the node that follows it; the log shows the owner and how many hops it took: with fingers, about log2(N).
about  Watch the panel of a node: its position on the ring, successor, predecessor, fingers and stored keys (each pair is also copied to the successors of its owner). Try bits=5 or bits=4 to make the ring small enough to check by hand: two names then hash to the same identifier, the collision is detected and one of the two moves to the next position. Or try a slower stabilization (stabilize=800ms).
about[it] Dieci macchine costruiscono una tabella hash distribuita senza alcun elenco centrale. n1 crea l'anello, gli altri entrano tramite n1 nello stesso istante: all'inizio tutti puntano a n1, poi la stabilizzazione periodica mette in ordine l'anello e le finger table si riempiono.
about[it] Poi alcune chiavi vengono scritte da un nodo e lette da altri. Ogni operazione calcola l'hash della chiave, cioè un punto dell'anello, e instrada verso il nodo che lo segue; il registro mostra il proprietario e quanti salti sono serviti: con i finger, circa log2(N).
about[it] Osserva il pannello di un nodo: la posizione sull'anello, successore, predecessore, finger e chiavi memorizzate (ogni coppia è copiata anche sui successori del proprietario). Prova bits=5 o bits=4 per un anello abbastanza piccolo da verificare a mano: due nomi finiscono allora sullo stesso identificatore, la collisione viene rilevata e uno dei due si sposta sulla posizione successiva. Oppure prova una stabilizzazione più lenta (stabilize=800ms).
param bits 8 Bits of the identifiers (ring of 2^bits positions)
param[it] bits Bit degli identificatori (anello di 2^bits posizioni)
param+ stabilize 400ms Period of stabilize and fix_fingers
param[it] stabilize Periodo di stabilize e fix_fingers

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
auto all

run n1 chord bits=$bits stabilize=$stabilize
run n2 chord join=n1 bits=$bits stabilize=$stabilize
run n3 chord join=n1 bits=$bits stabilize=$stabilize
run n4 chord join=n1 bits=$bits stabilize=$stabilize
run n5 chord join=n1 bits=$bits stabilize=$stabilize
run n6 chord join=n1 bits=$bits stabilize=$stabilize
run n7 chord join=n1 bits=$bits stabilize=$stabilize
run n8 chord join=n1 bits=$bits stabilize=$stabilize
run n9 chord join=n1 bits=$bits stabilize=$stabilize
run n10 chord join=n1 bits=$bits stabilize=$stabilize

note 0.5s Everybody has asked n1 for a successor and got n1 itself: the "ring" is a star. Stabilization now repairs it step by step.
note[it] 0.5s Tutti hanno chiesto a n1 un successore e hanno ottenuto n1 stesso: l'"anello" è una stella. La stabilizzazione ora lo ripara passo dopo passo.
note 12s The ring is in order and the fingers are set. n3 stores four keys: each goes to the successor of its hash.
note[it] 12s L'anello è in ordine e i finger sono a posto. n3 memorizza quattro chiavi: ognuna va al successore del suo hash.
at 12s   run n3 chord put key=apple value=red
at 12.2s run n3 chord put key=banana value=yellow
at 12.4s run n3 chord put key=cherry value=dark-red
at 12.6s run n3 chord put key=plum value=purple
note 15s Other nodes read the keys: nobody knows where they are, the ring finds them in a few hops.
note[it] 15s Altri nodi leggono le chiavi: nessuno sa dove siano, l'anello le trova in pochi salti.
at 15s   run n6 chord get key=apple
at 15.3s run n9 chord get key=banana
at 15.6s run n2 chord get key=cherry
at 15.9s run n10 chord get key=plum
at 16.5s run n5 chord get key=mango
duration 19s

expect n3 chord.lookups == 4
expect n6 chord.found == 1
expect n9 chord.found == 1
expect n2 chord.found == 1
expect n10 chord.found == 1
expect n5 chord.notfound == 1
expect n6 chord.hops <= 5
