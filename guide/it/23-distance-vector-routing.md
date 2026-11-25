<!-- level: 2 -->
<!-- labs: 230-rip-convergence, 231-rip-count-to-infinity -->
# Instradamento a vettore delle distanze

Le rotte statiche non scalano e non si riparano da sole. Un protocollo di instradamento permette ai router di costruire le proprie tabelle parlandosi. L'approccio a **vettore delle distanze** (distance vector) è il più semplice: ogni router comunica periodicamente ai vicini l'elenco delle reti che sa raggiungere e a quale distanza. Nessuno vede mai la topologia; un router sa soltanto che «la rete X è a 3 salti in direzione del vicino B».

La regola per usare ciò che si sente sta in una riga. Se il vicino B dice «raggiungo X a distanza d», allora io raggiungo X a distanza d più il costo del mio collegamento verso B. Fra tutti i vicini tengo l'offerta più piccola e ricordo chi l'ha fatta: quel vicino è il mio next hop. Ripetuto da tutti, questo è l'algoritmo distribuito di **Bellman-Ford**, che converge ai cammini minimi.

Le buone notizie si diffondono in fretta: una nuova rete è nota a n salti di distanza dopo n scambi. Il punto debole sono le cattive notizie. Quando una rete scompare, un router può sentirsi annunciare una rotta che in realtà passava da lui stesso, crederci e riannunciarla. I due router alzano allora la distanza un passo alla volta finché raggiunge un valore che per convenzione significa infinito. È il **conteggio all'infinito** (count to infinity); RIP, il classico protocollo a vettore delle distanze, fissa l'infinito a 16 e aggiunge tre rimedi: split horizon, poisoned reverse e triggered update.

<!-- detail: 2 -->
## Bellman-Ford

Sia c(x,v) il costo del collegamento da x al vicino v e D_v(y) la distanza da v a y annunciata da v. Allora:

```
D_x(y) = min sui vicini v di [ c(x,v) + D_v(y) ]
```

Ogni router memorizza il proprio vettore delle distanze e i next hop, ricalcola quando cambia il vettore di un vicino o il costo di un collegamento, e annuncia il proprio vettore quando è cambiato (e comunque periodicamente).

## RIP in dettaglio

RIP versione 2 (RFC 2453) funziona su UDP, porta 520, e invia le risposte all'indirizzo multicast 224.0.0.9. Un messaggio ha un'intestazione di 4 byte (command: 1 = request, 2 = response; version; due byte a zero) seguita da un massimo di 25 voci di 20 byte ciascuna:

- address family (2 byte) e route tag (2 byte)
- indirizzo IP (4), maschera di sottorete (4), next hop (4)
- metrica (4): da 1 a 15, e 16 = irraggiungibile

Ogni collegamento costa 1, quindi la metrica è un conteggio dei salti e nessun cammino può superare i 15 salti. Tre timer secondo l'RFC: **update** ogni 30 s (con un piccolo scarto casuale), **timeout** 180 s (una rotta non rinfrescata viene marcata irraggiungibile), **garbage collection** 120 s (viene poi annunciata con metrica 16 prima di essere cancellata). In hoplab il periodo di aggiornamento è un parametro, il timeout vale 6 aggiornamenti e la garbage collection 4.

Ricevendo una voce con metrica m dal vicino N, il router calcola m' = min(m + 1, 16) e: installa la rotta se la destinazione è nuova e m' < 16; sostituisce la propria rotta se m' è minore; e, punto cruciale, **accetta m' anche se maggiore quando N è il next hop attuale**, perché il vicino su cui fa affidamento ne sa di più.

## Il conteggio all'infinito, con i numeri

Il laboratorio 231 è una linea `h1 - r1 - r2 - r3 - h3`, con aggiornamenti ogni 2 s e senza protezioni. (hoplab mostra la metrica memorizzata, con 0 per le reti direttamente connesse; sul cavo il valore annunciato è maggiore di uno, come nell'RFC.) Prima del guasto r2 raggiunge la rete di h3 a 1 e r1 a 2.

- 12,0 s: il collegamento r2-r3 si guasta. r2 elimina la sua rotta.
- 12,5 s: l'aggiornamento periodico di r1 dice ancora «la raggiungo» (metrica memorizzata 2): r2 la installa via r1 con metrica 3. r1 punta a r2 e r2 punta a r1: un ciclo.
- 13,5 s: l'aggiornamento di r2 porta la sua nuova rotta, peggiore; r1, il cui next hop è r2, deve accettarla: 2 → 4.
- 14,5 s: r2 passa da 3 a 5; poi r1 da 4 a 6, e così via, un passo al secondo.
- 25,5 s: r1 raggiunge 16; a 26,5 s anche r2. La rotta è finalmente sparita.

Per tredici secondi i pacchetti per h3 rimbalzano fra r1 e r2 finché il loro TTL scade. Con il vero timer di 30 secondi lo stesso conteggio durerebbe diversi minuti.

## I rimedi

- **Split horizon**: non annunciare mai una rotta sull'interfaccia da cui è stata appresa. r1 non dice nulla a r2 sulla rete di h3, quindi r2 non ha nessuno a cui credere.
- **Poisoned reverse**: annunciarla su quell'interfaccia, ma con metrica 16. Più forte, perché spezza attivamente un ciclo invece di aspettare un timeout, al prezzo di messaggi più grandi.
- **Triggered update**: quando una metrica cambia, inviare subito le voci cambiate invece di attendere il timer. Le cattive notizie viaggiano allora in millisecondi.

<!-- detail: 3 -->
## In profondità

**Che cosa lo split horizon non può fare.** Elimina solo i cicli fra due vicini. Con tre router a triangolo, A può ancora apprendere da C una rotta che C ha appreso da B che B ha appreso da A. Il conteggio all'infinito avviene allora lungo il ciclo; i triggered update lo rendono veloce e il piccolo valore dell'infinito lo rende breve, ma non viene impedito. Che un ciclo si formi o no è una gara fra la cattiva notizia e un aggiornamento periodico ormai superato, ed è per questo che il laboratorio 231 fissa la fase dei timer per riprodurlo in modo affidabile.

**Hold-down.** Alcune implementazioni (IGRP, il RIP di Cisco) rifiutano per un certo tempo qualsiasi nuova rotta verso una destinazione appena persa, per lasciare che la cattiva notizia raggiunga tutti. Previene i cicli ma ritarda le alternative legittime.

**Sincronizzazione.** Router che partono insieme, o che azzerano i timer a ogni triggered update, finiscono per inviare gli aggiornamenti periodici nello stesso istante, causando raffiche periodiche e perdite. Floyd e Jacobson lo analizzarono in «The Synchronization of Periodic Routing Messages» (1994); lo scarto casuale dell'RFC è la cura. Anche i triggered update sono limitati in frequenza (da 1 a 5 s) per evitare tempeste.

**Limiti di RIP.** Il conteggio dei salti ignora la banda; 15 salti limitano il diametro; la convergenza dopo un guasto senza cammini alternativi dipende dal timeout di 180 s se il guasto non è rilevato localmente; ogni router invia l'intera tabella ogni 30 s. RIPv1 (RFC 1058) non trasportava le maschere ed era classful; RIPv2 aggiunge maschere, next hop, multicast e autenticazione (RFC 4822); RIPng (RFC 2080) è la versione per IPv6.

**I successori.** EIGRP mantiene i vettori delle distanze ma garantisce l'assenza di cicli con l'algoritmo DUAL (Garcia-Luna-Aceves, 1993): un vicino può essere usato solo se la sua distanza è minore di quanto la nostra sia mai stata. BGP è un protocollo a *vettore dei cammini* (path vector): annuncia l'intero elenco dei sistemi autonomi, così un router rifiuta ogni rotta che contiene sé stesso. Babel (RFC 8966) è un moderno protocollo a vettore delle distanze per reti mesh wireless, con numeri di sequenza per evitare i cicli.

**Origini.** Bellman (1958) e Ford e Fulkerson (1962) diedero l'algoritmo; l'instradamento originale di ARPANET del 1969 era a vettore delle distanze con il ritardo come metrica, e la sua lenta reazione alle cattive notizie fu il motivo del passaggio allo stato dei collegamenti nel 1979.

**Che cosa semplifica hoplab.** Formato reale dei messaggi; niente autenticazione e niente route tag.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 230: `./hoplab run 230-rip-convergence`**

- Seleziona r1 (`n`) e apri la scheda Rotte e la scheda `rip`: entro il primo decimo di secondo arrivano le reti dei vicini con metrica 1, poi la rete di h3 (10.0.6.0/24) con metrica 2 via r2.
- Nella vista Pacchetti (`p`) premi Invio su una risposta RIP: leggi l'intestazione e le voci di 20 byte. Confronta un aggiornamento periodico con uno immediato («solo le rotte cambiate»).
- A 16 s traceroute mostra r1, r2, r3. A 20 s il collegamento r1-r2 si guasta: metrica 16 nel registro, triggered update, e a 45 s traceroute passa da r4.
- Prevedi, poi prova: `./hoplab run 230-rip-convergence triggered=0`. Quando r3 viene a sapere che la rete di r1 non è più raggiungibile via r2, e quando r1 ottiene una nuova rotta verso la rete di h3? Confronta gli istanti con l'esecuzione di default.
- Prevedi, poi prova: `./hoplab run 230-rip-convergence update=10s`. Quali eventi vengono ritardati e quali no? Poi digita `:link r1 r2 up` dopo i 45 s e osserva il ritorno del cammino più breve.

**Laboratorio 231: `./hoplab run 231-rip-count-to-infinity`**

- Imposta il dettaglio 2 e segui il registro dai 12 s: la metrica di 10.0.4.0/24 sale alternatamente su r1 e r2. Tieni aperta la scheda Rotte di r1 per vedere cambiare next hop e metrica.
- A 17 s il ping di h1 segnala `tempo di vita (TTL) esaurito` da 10.0.2.2: apri la vista Pacchetti per vedere la echo request che rimbalza.
- Prevedi, poi prova: `./hoplab run 231-rip-count-to-infinity split=1`. Che cosa annuncia r1 a r2 a 12,5 s? Che cosa segnala il ping, e chi invia l'errore? (Le verifiche del laboratorio conoscono l'impostazione: con una protezione attiva controllano che nessuno conti e che nessun pacchetto giri in tondo.) Poi prova `triggered=1` da solo: perché in questa piccola rete basta già, e perché non ci faresti affidamento?
- Prevedi, poi prova: `split=1 poison=1` e apri una risposta RIP da r1 a r2: quali voci hanno metrica 16? Poi `split=0 triggered=1`: il ciclo si forma ancora? Perché è una gara e non una garanzia?

## Verifica

- Enuncia l'equazione di Bellman-Ford e applicala: il router A ha come vicini B (costo del collegamento 1) e C (costo 4). B annuncia distanza 5 verso la rete N, C annuncia 1. Che cosa memorizza A? E se B più tardi annuncia 7?
- Descrivi campo per campo la voce di rotta di RIPv2. Perché una risposta può contenere al massimo 25 rotte?
- Tre router in linea A-B-C; la rete di C si guasta. Mostra, aggiornamento dopo aggiornamento, come si sviluppa il conteggio all'infinito senza split horizon, e perché con esso non avviene.
- Spiega la differenza fra split horizon e poisoned reverse, e indica una topologia in cui nessuno dei due impedisce il conteggio all'infinito.
- Perché un router deve accettare una metrica peggiore dal suo next hop attuale, pur ignorando le offerte peggiori degli altri vicini?
