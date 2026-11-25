<!-- level: 2 -->
<!-- labs: 232-ospf-costs -->
# Instradamento a stato dei collegamenti

Un router a vettore delle distanze crede a ciò che i vicini dicono delle reti lontane: sentito dire. Un router a **stato dei collegamenti** (link state) chiede a ogni router soltanto fatti: «chi sono i tuoi vicini, e quanto costa ogni collegamento?». Questi fatti vengono diffusi inalterati in tutta la rete, così ogni router si ritrova con la stessa **mappa** completa. Ciascuno calcola poi da sé i cammini minimi sulla mappa, con l'algoritmo di Dijkstra, e installa nella tabella di instradamento il primo salto di ogni cammino.

Il protocollo ha tre compiti. I pacchetti **Hello** scoprono i vicini e rilevano quando scompaiono. Ogni router descrive i propri collegamenti in un **link-state advertisement** (LSA), e l'**inondazione** (flooding) consegna in modo affidabile ogni LSA a ogni router. L'insieme degli LSA è il **database dello stato dei collegamenti**: la mappa.

I vantaggi derivano dall'avere la mappa. Un cambiamento viene inondato subito e tutti ricalcolano a partire da fatti, quindi non c'è conteggio all'infinito e la convergenza richiede una frazione di secondo. I costi possono essere numeri positivi qualsiasi, così l'amministratore può far preferire al traffico i collegamenti veloci. Il prezzo è più memoria, più calcolo e un protocollo più complesso. OSPF e IS-IS, i protocolli usati all'interno di quasi tutte le grandi reti, sono a stato dei collegamenti.

<!-- detail: 2 -->
## I vicini: Hello

OSPF (RFC 2328) viaggia direttamente su IP, numero di protocollo 89, e invia gli Hello all'indirizzo multicast 224.0.0.5 ogni *hello interval* (10 s per default sulle LAN). Un Hello contiene l'identificatore del router mittente, gli intervalli e l'elenco dei vicini sentiti di recente. Vedere il *proprio* identificatore nell'Hello di un vicino dimostra che il collegamento funziona nei due versi: lo stato passa da **Init** a **2-Way**. Un vicino che tace per il *dead interval* (4 hello, 40 s) viene dichiarato caduto. I due intervalli devono coincidere, altrimenti l'adiacenza non si forma.

Il vero OSPF sincronizza poi i database attraverso gli stati ExStart, Exchange e Loading (pacchetti database description, link-state request, link-state update) e raggiunge **Full**. hoplab salta lo scambio delle descrizioni: una nuova adiacenza va direttamente a Full e ciascun lato invia l'intero database.

## LSA e inondazione

Un router-LSA elenca i collegamenti di chi lo ha generato: per ciascuno, il vicino (o la rete connessa) e il costo. La sua intestazione lo identifica tramite l'advertising router e contiene un **numero di sequenza** e un'età. Regola di inondazione, alla ricezione di un LSA da un vicino:

- Se è **più recente** (numero di sequenza più alto) della copia memorizzata, o sconosciuto: lo si memorizza, lo si riscontra, lo si invia a tutti gli altri vicini e si pianifica un calcolo dei cammini minimi.
- Se è uguale: lo si riscontra soltanto (è un duplicato arrivato per un'altra via).
- Se è più vecchio: si risponde con la copia più recente.

Ogni LSA inviato viene ritrasmesso finché non è riscontrato, quindi l'inondazione è affidabile salto per salto. Quando un collegamento o un costo cambia, il router emette un nuovo LSA con il numero di sequenza aumentato di uno.

## Dijkstra sulla mappa del laboratorio

Nel laboratorio 232 il costo di un'interfaccia è 100 Mbit/s diviso per la sua banda: 1 per i collegamenti a 100 Mbit/s r1-r2 e r2-r4, 10 per quelli a 10 Mbit/s r1-r3 e r3-r4. Dijkstra da r1 mantiene un insieme di nodi *definitivi* e distanze provvisorie per gli altri, e rende definitivo ogni volta il nodo provvisorio più vicino:

```
passo  definitivi        distanze provvisorie
 0     r1 (0)            r2 = 1 via r2,  r3 = 10 via r3
 1     + r2 (1)          r4 = 1 + 1 = 2 via r2,  r3 = 10
 2     + r4 (2)          r3 = min(10, 2 + 10) = 10
 3     + r3 (10)         fine
```

La rete di h4 è attaccata a r4 con costo 1, quindi r1 la raggiunge con costo 3 e next hop r2; il cammino inferiore costerebbe 10 + 10 + 1 = 21. Il registro mostra esattamente queste metriche.

A 20 s il costo dell'interfaccia di r1 verso r2 diventa 50. r1 inonda un nuovo LSA; 200 ms dopo ogni router ha rieseguito l'SPF. Per r1 il cammino superiore ora costa 52, quindi usa r3 (21). Ma un costo appartiene a un'*interfaccia di uscita*: il costo di r2 verso r1 è ancora 1, quindi r4 continua a rispondere attraverso r2. Andata e ritorno seguono cammini diversi.

## Confronto con il vettore delle distanze

- **Informazione**: l'intera topologia, inondata, contro le sole distanze dei vicini.
- **Cattive notizie**: veloci quanto le buone; nessun conteggio all'infinito.
- **Messaggi**: piccoli e guidati dagli eventi, contro l'intera tabella a intervalli regolari.
- **Costo**: memoria per il database e calcolo O((N + E) log N) a ogni cambiamento.
- **Cicli**: solo transitori, finché non tutti i router hanno ricalcolato.

<!-- detail: 3 -->
## In profondità

**Scalabilità.** Ogni cambiamento fa ricalcolare tutti i router, e il database cresce con la rete. OSPF divide un dominio grande in **aree**: i router conoscono la mappa completa solo della propria area, e i router di confine riassumono i prefissi verso la dorsale (area 0), che fra le aree si comporta come un vettore delle distanze. Su una LAN broadcast con n router, n(n−1)/2 adiacenze sarebbero uno spreco: i router eleggono un **designated router**, tutti si sincronizzano solo con lui, ed è lui a generare un network-LSA che rappresenta la LAN. hoplab ha una sola area e nessun designated router.

**Invecchiamento e numeri di sequenza.** Ogni LSA ha un'età; chi lo ha generato lo rinfresca ogni 30 minuti, e a 1 ora viene eliminato, così l'informazione di un router morto prima o poi scompare. I numeri di sequenza sono valori lineari a 32 bit che partono da 0x80000001. ARPANET usava numeri circolari, e nel 1980 aggiornamenti corrotti, ciascuno dei quali sembrava più recente del successivo, continuarono a essere inondati all'infinito e fecero cadere la rete (RFC 789, Rosen): è la ragione per cui gli LSA moderni hanno un checksum e un'età. In hoplab gli LSA non invecchiano.

**Velocità di convergenza.** La parte lenta è il rilevamento: con i soli timer richiede il dead interval. I router reagiscono perciò alla perdita della portante, o usano BFD (RFC 5880) con decine di millisecondi. L'SPF viene ritardato e limitato in frequenza (200 ms in hoplab) in modo che una raffica di LSA provochi un solo calcolo: si scambia un po' di velocità con la stabilità. Fra l'aggiornamento del primo router e quello dell'ultimo sono possibili micro-cicli; i loop-free alternate (RFC 5286) precalcolano next hop di riserva.

**Costi.** La banda di riferimento di 100 Mbit/s fa costare 1 ogni collegamento più veloce, per cui gli operatori la alzano o fissano i costi a mano. Far dipendere il costo dal *carico* fu tentato su ARPANET e provocò oscillazioni: il traffico si spostava sul cammino vuoto, che diventava così quello carico (Khanna e Zinky, «The Revised ARPANET Routing Metric», 1989). I cammini di costo uguale vengono usati insieme (ECMP).

**Sicurezza e parenti.** Un singolo router può mentire sui propri collegamenti e attirare traffico; OSPF prevede l'autenticazione crittografica dei pacchetti. IS-IS (ISO 10589, RFC 1195) è la stessa idea direttamente sopra il livello 2, preferito da molti operatori. OSPFv3 (RFC 5340) trasporta IPv6.

**Riferimenti.** Dijkstra, «A Note on Two Problems in Connexion with Graphs» (1959); McQuillan, Richer e Rosen, «The New Routing Algorithm for the ARPANET» (1980); RFC 2328.

**Che cosa semplifica hoplab.** Oltre ai punti già detti, i formati dei pacchetti somigliano a quelli di OSPF ma non sono esatti al bit.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 232: `./hoplab run 232-ospf-costs`**

- Rallenta (`-`) per i primi 50 ms: Hello, `nuovo vicino, stato Init`, `bidirezionale, adiacenza Full`, poi gli LSA `più recente: memorizzato e inoltrato agli altri`. Nota come ogni router rigenera il proprio LSA (seq=1, 2, 3) man mano che trova vicini.
- Seleziona r1 e poi r4 (`n`) e apri la scheda `ospf`: i due database elencano gli stessi quattro LSA con gli stessi numeri di sequenza. Apri la scheda Rotte di r1: 10.0.6.0/24 via 10.0.2.2, metrica 3.
- Nella vista Pacchetti (`p`) apri un Hello (trova l'elenco dei vicini) e un link-state update (trova collegamenti e costi).
- A 14 s traceroute passa da r1, r2, r4. A 20 s il costo cambia: la rotta di r1 si sposta su 10.0.3.2 con metrica 21. Controlla la scheda Rotte di r4: la sua rotta verso 10.0.1.0/24 passa ancora da r2.
- A 32 s il collegamento r1-r2 si guasta; entrambi gli estremi generano nuovi LSA ed entro circa 0,2 s tutte le rotte usano r3.
- Prevedi, poi prova: `./hoplab run 232-ospf-costs newcost=15`. Calcola prima il costo dei due cammini. r1 cambia next hop a 20 s? Quale metrica compare nella sua tabella?
- Prevedi, poi prova: nell'esecuzione di default, dopo i 21 s digita `:r1 ospf cost eth1 1` e osserva il numero di sequenza dell'LSA di r1 e la rotta. Poi, dopo i 33 s, `:link r3 r4 down`: che cosa contengono ora le tabelle di r1 e r4, e che cosa segnala `:h1 ping h4 count=1`?

## Verifica

- Esegui Dijkstra a mano dal nodo A su questo grafo e scrivi la tabella di instradamento di A: A-B 2, A-C 5, B-C 1, B-D 4, C-D 1, D-E 3.
- Quali sono le tre regole che un router applica quando riceve un LSA, e perché l'inondazione termina anche se la topologia contiene cicli?
- Come fa un router a sapere che un collegamento è bidirezionale prima di usarlo? A che cosa serve il dead interval?
- Nel laboratorio 232, dopo il cambio di costo, le richieste da h1 a h4 passano da r3 ma le risposte tornano da r2. Spiega perché, e di' se è un errore.
- Confronta stato dei collegamenti e vettore delle distanze su: informazione scambiata, reazione al guasto di un collegamento, calcolo e memoria, tipi di cicli.
