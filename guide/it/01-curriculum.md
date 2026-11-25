<!-- level: 0 -->
<!-- labs: 100-two-hosts, 101-ping, 120-tcp-handshake, 200-static-routing, 220-tcp-loss, 320-tcp-congestion, 380-raft-election, 420-tcp-fairness -->
# Il percorso di studio

I laboratori di netlab formano un programma di studio in quattro percorsi di difficoltà crescente. Ogni percorso è un gruppo nel catalogo e in questa guida, e all'interno di un percorso i laboratori vanno dal fondo della pila dei protocolli verso l'alto.

- **Rudimenti** è il primo contatto. Nessun prerequisito: se sai che aspetto ha un indirizzo IP, puoi cominciare. Frame, indirizzi, ping, una connessione TCP, una pagina web, la risoluzione di un nome e le prime idee del livello di collegamento.
- **Fondamenti** è la materia di un corso universitario di reti del primo ciclo: tabelle e protocolli di instradamento, affidabilità e controllo di flusso di TCP, DNS e DHCP in profondità, NAT, switching, VLAN, spanning tree, accesso al mezzo.
- **Avanzato** è materia da laurea magistrale: controllo di congestione, gestione delle code, instradamento fra domini, consenso e algoritmi distribuiti.
- **Frontiera** raccoglie laboratori dal sapore di ricerca, dove la parte interessante è ciò che va storto: equità fra controlli di congestione e BBR, sicurezza di BGP, avvelenamento del DNS, consistent hashing, terminali nascosti, partizioni di rete.

Non occorre finire un percorso prima di entrare nel successivo. Un buon modo di studiare è seguire un tema attraverso i percorsi: TCP dall'handshake all'equità, oppure l'instradamento da una tabella statica ai dirottamenti BGP.

## Come studiare con un laboratorio

Un laboratorio è un esperimento, e insegna di più quando lo tratti come tale.

- **Prevedi.** Leggi la descrizione del laboratorio, poi metti in pausa al tempo zero e di' che cosa ti aspetti: quale frame parte per primo, chi risponde, quanto ci vuole.
- **Esegui.** Lascia scorrere il laboratorio e segui il commento in fondo alla mappa.
- **Ispeziona.** Metti in pausa dove qualcosa ti ha sorpreso. Apri il pacchetto, leggi la tabella di instradamento, guarda il grafico.
- **Cambia un parametro.** Uno alla volta: raddoppia la perdita, accorcia la coda, taglia un collegamento. Prevedi di nuovo prima di eseguire.
- **Spiega.** Di' con parole tue perché il risultato è cambiato. Se non ci riesci, leggi l'argomento a una profondità maggiore e ripeti.

<!-- detail: 2 -->
Gli strumenti per ogni passo: `spazio` e `.` per mettere in pausa e avanzare di un evento alla volta; `p` e poi Invio per sezionare un frame; `s` per il diagramma di sequenza; `n` per selezionare un nodo e le frecce per leggerne le schede; `g` per i grafici; `:` per comandi come `link r1 r2 down` o `h1 ping h2`; e il modulo dei parametri del catalogo (o `nome=valore` sulla riga di comando) per eseguire lo stesso laboratorio in condizioni diverse.

<!-- detail: 1 -->
## Le profondità di lettura

Ogni argomento di questa guida, e la guida interna a ogni laboratorio, si può leggere a tre profondità. Premi `1`, `2` o `3`.

- **1, essenziale**: una lettura breve e completa. Che cos'è la cosa e che cosa osservare nel laboratorio.
- **2, standard**: la conoscenza operativa. Come funziona il meccanismo, i campi e i timer che contano, che cosa provare.
- **3, approfondito**: i punti fini. Casi limite, compromessi di progetto, che cosa fanno di diverso i protocolli reali.

Gli stessi tre livelli governano un laboratorio in esecuzione: al livello 1 il registro mostra solo l'esito di ogni azione, al 2 i messaggi dei protocolli, al 3 ogni frame. Comincia ogni laboratorio dall'1, e scendi più in profondità quando vuoi sapere come.

## Rudimenti

Affrontali in ordine; ognuno usa ciò che ha mostrato il precedente.

- `100-two-hosts` Due host e un cavo: frame, indirizzi e ARP
- `101-ping` Due reti e un router: ping, traceroute, frammentazione
- `102-link-properties` Che cosa fa un collegamento ai pacchetti: ritardo, banda, perdita, coda
- `103-ipv6-first` IPv6, primo contatto: indirizzi link-local e Neighbor Discovery
- `120-tcp-handshake` TCP, segmento per segmento: handshake, dati, chiusura
- `121-http-page` Caricare una pagina web: HTTP su TCP
- `140-dns-first` Una prima risoluzione di nome
- `141-dhcp-join` Entrare in una rete con DHCP
- `160-error-detection` Rilevare gli errori di bit: niente, un bit di parità, un CRC
- `161-error-correction` Correggere invece di scartare: ripetizione e codici di Hamming
- `162-aloha` Condividere un canale: ALOHA puro e slotted ALOHA
- `170-switch-learning` Switch che imparano: inondare, imparare, inoltrare

<!-- detail: 2 -->
I primi sette laboratori sono il viaggio di una richiesta web visto dal basso verso l'alto: un frame su un cavo, un pacchetto attraverso un router, che cosa gli fa il collegamento, una connessione TCP, una pagina, la risoluzione del nome e l'assegnazione dell'indirizzo che vengono prima di tutto il resto. Gli ultimi quattro aprono il livello di collegamento: come gli errori vengono scoperti o riparati, come le stazioni condividono un canale, come uno switch impara dove si trovano tutti. Se hai poco tempo, fai 100, 101, 120 e 140.

<!-- detail: 1 -->
## Fondamenti

- `200-static-routing` Instradamento statico: tabelle, next hop e una rotta mancante
- `201-mtu-fragmentation` MTU, frammentazione e scoperta della MTU del percorso
- `202-routing-loop` Un ciclo di instradamento, e il TTL che salva la rete
- `203-vlans` VLAN e trunk: più reti sugli stessi switch
- `204-ipv6-slaac` Autoconfigurazione IPv6: router advertisement e SLAAC
- `205-ipv6-pmtu` IPv6 non frammenta lungo il percorso: Packet Too Big e scoperta della MTU
- `206-dual-stack` Dual stack: IPv4 e IPv6 fianco a fianco sugli stessi cavi
- `220-tcp-loss` TCP su un percorso con perdite: timeout e fast retransmit
- `221-tcp-flow-control` Controllo di flusso TCP: mittente veloce, lettore lento
- `230-rip-convergence` RIP: i router scoprono la rete, poi aggirano un guasto
- `231-rip-count-to-infinity` RIP: il conteggio all'infinito, e come lo evita lo split horizon
- `232-ospf-costs` OSPF: una mappa uguale per tutti, e costi che guidano il traffico
- `240-dns-hierarchy` La gerarchia del DNS e la cache
- `241-dhcp-leases` Lease DHCP: rinnovo, scadenza ed esaurimento del pool
- `250-nat-basics` NAT: perché gli indirizzi privati hanno bisogno di un traduttore
- `251-port-forwarding` Port forwarding: pubblicare un server che sta dietro un NAT
- `252-load-balancer` Load balancer: un indirizzo, tre server, e uno si guasta
- `260-coding-bench` Il banco dei codici: i codici al lavoro, bit per bit
- `261-csma-cd` Ascoltare prima di parlare: CSMA e il CSMA/CD di Ethernet
- `270-broadcast-storm` Un anello di switch senza spanning tree: la tempesta di broadcast
- `271-spanning-tree` Spanning tree: un anello reso sicuro, e riparato quando un collegamento cade
- `272-priority-queueing` Code a priorità: voce e traffico massivo su un collegamento lento
- `290-lamport-clocks` Orologi di Lamport: ordinare gli eventi senza un orologio comune

<!-- detail: 2 -->
Un ordine consigliato per un corso: prima il livello di rete (200, 201, 202, poi i protocolli di instradamento 230, 231, 232), poi il trasporto (220, 221), poi i servizi che tutti usano (240, 241, 250, 251, 252), e infine il livello di collegamento (203, 260, 261, 270, 271, 272). Il laboratorio 290 è un primo assaggio di algoritmi distribuiti e prepara il percorso Avanzato. Fai il 200 prima di qualunque protocollo di instradamento: RIP si apprezza solo dopo aver configurato le rotte a mano. Fai il 270 prima del 271: la tempesta è il motivo per cui esiste lo spanning tree.

<!-- detail: 1 -->
## Avanzato

- `320-tcp-congestion` Controllo di congestione su un collo di bottiglia: il dente di sega
- `330-ospf-vs-rip` Convergenza dopo un guasto: link state contro distance vector
- `331-bgp-basics` BGP: sistemi autonomi, AS path e ritiri
- `340-dns-ttl` Catene di CNAME, caching negativo e il compromesso del TTL
- `350-nat-hole-punching` Comportamenti dei NAT e UDP hole punching
- `351-lb-algorithms` Algoritmi di bilanciamento a confronto
- `360-rs-vs-hamming` Reed-Solomon contro Hamming: errori casuali, burst e interleaving
- `361-mac-curves` Throughput in funzione del carico: le curve S(G) di tutti i protocolli di accesso
- `370-stp-tuning` Progettare uno spanning tree: priorità, costi delle porte, timer e guasto della radice
- `371-bufferbloat-codel` Bufferbloat: un grande buffer contro CoDel, fianco a fianco
- `372-red-ecn` RED: scarti casuali anticipati, la coda media ed ECN
- `373-fair-queueing` Fair queueing: un flusso piccolo protetto da un'inondazione
- `374-token-bucket` Shaping con token bucket: un limite di velocità che tollera le raffiche
- `380-raft-election` Raft: eleggere un leader, e sostituirlo quando cade
- `381-raft-replication` Raft: replicare il log, fare commit e recuperare dopo un guasto
- `382-gossip-rumor` Gossip: quanto in fretta una voce raggiunge tutti?
- `390-causal-broadcast` Orologi vettoriali e broadcast causale: la risposta che supera la domanda
- `391-two-phase-commit` Two-phase commit: tutto o niente su tre database
- `392-commit-blocking` Il problema del blocco: il coordinatore si guasta dopo PREPARE (2PC e 3PC)
- `393-chord-ring` Chord: dieci nodi formano un anello e condividono una tabella hash
- `394-crdt-counter` Un contatore replicato che non può perdere incrementi (CRDT)

<!-- detail: 2 -->
Tre cammini attraversano questo percorso. **Congestione e code**: 320, poi 371, 372, 373, 374; prima il punto di vista del mittente, poi ciò che può fare il router. **Instradamento fra domini e al loro interno**: 330, 331, con il 370 per le stesse domande al livello 2. **Sistemi distribuiti**: 390 per il tempo e l'ordinamento, 380 e 381 per il consenso, 391 e 392 per il commit atomico, 382 per i protocolli epidemici, 393 e 394 per le strutture che non hanno bisogno di un coordinatore. I laboratori restanti (340, 350, 351, 360, 361) approfondiscono ciascuno un argomento dei Fondamenti e si possono affrontare quando ci arrivi.

<!-- detail: 1 -->
## Frontiera

- `420-tcp-fairness` Due flussi, un collo di bottiglia: equità fra Reno, CUBIC e BBR
- `430-bgp-policies` Politiche BGP: clienti, peer, provider e percorsi valley-free
- `431-bgp-hijack` BGP: dirottamento di un prefisso, prefisso più specifico e validazione dell'origine
- `440-dns-poisoning` Avvelenamento della cache DNS e perché conta la casualità della porta
- `450-consistent-hashing` Consistent hashing: cosa succede quando il pool di server cambia
- `460-hidden-terminal` Il punto cieco del Wi-Fi: terminali nascosti, CSMA/CA e RTS/CTS
- `480-raft-partition` Raft durante una partizione di rete: nessuno split brain
- `481-raft-split-votes` Raft: timeout di elezione, voti divisi e disponibilità
- `482-swim-failure-detection` SWIM: rilevare un membro caduto senza falsi allarmi
- `483-gossip-partition` Gossip attraverso una partizione: l'anti-entropia rimette tutti in sincronia
- `490-chord-churn` Chord sotto churn: guasti e ingressi mentre le ricerche continuano
- `491-crdt-partition` Partizione e ricongiunzione: OR-Set contro last-writer-wins

<!-- detail: 2 -->
Ogni laboratorio di Frontiera ne prosegue uno Avanzato: il 420 segue il 320; 430 e 431 seguono il 331; il 440 segue il 340; il 450 segue il 351; il 460 segue il 361; 480 e 481 seguono 380 e 381; 482 e 483 seguono il 382; il 490 segue il 393; il 491 segue il 394. Questi laboratori hanno più parametri degli altri e ripagano molte esecuzioni: sono esperimenti da progettare, non dimostrazioni da guardare.

<!-- detail: 1 -->
## Per livello dello stack

Il catalogo si può anche filtrare per livello con `Tab`, cosa utile quando studi un tema alla volta.

- **Fisico e collegamento**: 100, 102, 160, 161, 162, 170, 203, 260, 261, 270, 271, 360, 361, 370, 460
- **Rete**: 101, 200, 201, 202, 230, 231, 232, 250, 251, 272, 330, 331, 350, 371, 372, 373, 374, 430, 431
- **Trasporto**: 120, 220, 221, 252, 320, 351, 420, 450
- **Applicazione**: 121, 140, 141, 240, 241, 340, 440
- **Distribuito**: 290, 380, 381, 382, 390, 391, 392, 393, 394, 480, 481, 482, 483, 490, 491

<!-- detail: 3 -->
Il numero di un laboratorio dice dove si colloca. La prima cifra è il percorso (1 Rudimenti, 2 Fondamenti, 3 Avanzato, 4 Frontiera). La seconda è il tema: 0 primi passi, IP e inoltro, 2 TCP, 3 protocolli di instradamento, 4 DNS e DHCP, 5 NAT e bilanciamento del carico, 6 codifica e accesso al mezzo, 7 switching e code, 8 consenso e gossip, 9 altri algoritmi distribuiti. Così 220, 320 e 420 sono il filo di TCP, e 380, 381, 480, 481 il filo di Raft. I laboratori che scrivi tu, o che costruisci con la procedura guidata, compaiono nel catalogo in un quinto gruppo, **I miei scenari**.

Ogni laboratorio è un semplice file di testo in `scenarios/`. Quando un laboratorio ti lascia con una domanda a cui non risponde, copia il file, modificalo ed esegui la tua copia: **Scrivere i propri laboratori** mostra come.
