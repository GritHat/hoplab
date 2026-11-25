<!-- level: 2 -->
<!-- labs: 200-static-routing, 202-routing-loop -->
# Inoltro e rotte statiche

Un router non conosce i percorsi. Conosce, per ogni prefisso di destinazione, il vicino a cui consegnare il pacchetto: il **next hop** (salto successivo). Un pacchetto attraversa Internet perché ogni router lungo la strada prende, in modo indipendente, una di queste decisioni locali. Nessuno verifica che le decisioni siano coerenti fra loro.

La decisione è una ricerca nella **tabella di instradamento**. Ogni riga contiene un prefisso (indirizzo di rete e lunghezza), un next hop e un'interfaccia di uscita. Più righe possono corrispondere a una destinazione; il router sceglie quella con il **prefisso più lungo** (longest prefix match), perché il prefisso più lungo è l'informazione più specifica di cui dispone.

Ne seguono due conseguenze, che spiegano la maggior parte dei guasti di instradamento. Primo: un percorso funziona solo se ogni router che lo compone ha una riga adatta, e anche la risposta ha bisogno di un percorso: le rotte per il ritorno sono righe distinte su router distinti. Secondo: se due router si indicano a vicenda per lo stesso prefisso, il pacchetto gira in tondo. L'inoltro non ha memoria e non può accorgersene; è il campo **TTL** dell'intestazione IP, decrementato a ogni salto, che alla fine elimina il pacchetto.

Con l'instradamento statico è un amministratore a scrivere a mano ogni riga. È semplice e prevedibile, ma non reagisce ai guasti.

<!-- detail: 2 -->
## L'algoritmo di inoltro

Per ogni pacchetto IP che non è destinato al router stesso:

- Si verifica il checksum dell'intestazione, poi si decrementa il TTL. Se arriva a 0, il pacchetto viene scartato e si invia alla sorgente un ICMP **time exceeded** (tipo 11, codice 0).
- Si cerca l'indirizzo di destinazione: fra tutte le righe il cui prefisso corrisponde si sceglie la più lunga. Se nessuna corrisponde, si scarta e si invia un ICMP **destination unreachable, network unreachable** (tipo 3, codice 0).
- Se la riga ha un gateway, il next hop è quel gateway; se la rete è direttamente connessa, il next hop è la destinazione stessa.
- Si risolve con ARP l'indirizzo MAC del next hop e si trasmette il frame. Gli indirizzi IP del pacchetto non cambiano; cambiano solo gli indirizzi Ethernet, il TTL e il checksum dell'intestazione.

Un host fa la stessa cosa con una tabella di due righe: la propria sottorete (direttamente connessa) e una rotta di default `0.0.0.0/0` verso il suo gateway. La rotta di default corrisponde a qualsiasi indirizzo con lunghezza 0, quindi vince solo quando non corrisponde nient'altro.

## Longest prefix match, con i numeri

Supponiamo che una tabella contenga:

```
10.0.0.0/8      via 10.0.12.2
10.0.4.0/24     via 10.0.23.3
10.0.4.10/32    via 10.0.12.1
0.0.0.0/0       via 192.0.2.1
```

Un pacchetto per 10.0.4.10 corrisponde a tutte e quattro le righe; vince la /32. Uno per 10.0.4.77 corrisponde a tre; vince la /24. Uno per 10.7.1.1 corrisponde alla /8 e alla default; vince la /8. Uno per 8.8.8.8 corrisponde solo alla default. Per verificare una corrispondenza si calcola l'AND fra destinazione e maschera: 10.0.4.77 AND 255.255.255.0 = 10.0.4.0, uguale al prefisso, quindi corrisponde.

## Il next hop non è un percorso

Nel laboratorio 200 tre router sono in fila: `h1 - r1 - r2 - r3 - h2`. r1 ha una rotta per 10.0.4.0/24 via r2, ma all'inizio r2 non ne ha. r1 inoltra tranquillamente: non può sapere che il next hop è ignaro. Il pacchetto muore su r2, che segnala network unreachable. Aggiunta la rotta, la risposta richiede che r3 e r2 conoscano 10.0.1.0/24. Una rotta di ritorno mancante dà un sintomo subdolo: la richiesta arriva, la risposta viene scartata lontano dal mittente e l'errore ICMP va a *chi ha risposto*, così il mittente vede solo silenzio.

## Cicli e TTL

Nel laboratorio 202, r1 dice «10.9.0.0/16 via r2» e r2 dice «10.9.0.0/16 via r1». Un ping inviato con TTL 16 viene decrementato a 15 da r1, a 14 da r2, e così via: attraversa il collegamento r1-r2 15 volte prima che r2 porti il TTL a 0. Un solo pacchetto ha consumato quindici trasmissioni; con il TTL iniziale abituale di 64 sarebbero 63. traceroute, che invia sonde con TTL 1, 2, 3, ..., mostra gli stessi due indirizzi che si alternano: la firma di un ciclo.

<!-- detail: 3 -->
## In profondità

**Da dove vengono le righe.** Una tabella reale fonde reti connesse, rotte statiche e rotte apprese dai protocolli di instradamento. Quando due fonti propongono lo stesso prefisso, il router preferisce la fonte più affidabile (administrative distance su Cisco, preference su Juniper) e solo dopo confronta le metriche. Il longest prefix match si applica per primo e su tutte le fonti: una /24 appresa da RIP batte una /16 statica. I prefissi senza classi e questa regola sono stati introdotti con CIDR (RFC 1519, oggi RFC 4632); il comportamento dei router è specificato nell'RFC 1812, il formato del pacchetto nell'RFC 791, ICMP nell'RFC 792.

**Velocità della ricerca.** Una tabella di dorsale contiene circa un milione di prefissi IPv4 e una ricerca deve richiedere nanosecondi. I router software usano trie compressi (Linux un LC-trie, i BSD un albero radix); i router hardware usano le TCAM, memorie che confrontano l'indirizzo con tutti i prefissi in parallelo. Il piano di controllo calcola la tabella di instradamento (RIB); il piano dati inoltra usando una copia compatta (FIB).

**Guasti dell'instradamento statico.** Una rotta statica resta in tabella quando il percorso oltre il next hop si interrompe: il traffico cade in un buco nero. Gli operatori usano rotte statiche flottanti (una riserva con preferenza peggiore) oppure legano la rotta a una sonda di raggiungibilità come BFD (RFC 5880). Una rotta statica verso un'interfaccia «null» è un buco nero voluto, usato per scartare traffico d'attacco e per ancorare i prefissi aggregati in modo che non possano entrare in ciclo.

**I cicli nella pratica.** I cicli permanenti nascono da errori di configurazione, tipicamente una rotta aggregata che punta verso un cliente il quale ha una rotta di default che punta indietro. Micro-cicli transitori compaiono durante la convergenza di qualsiasi protocollo di instradamento, quando i router passano ai nuovi next hop in istanti leggermente diversi. Il TTL limita il danno per pacchetto ma non il carico: un ciclo fra due router moltiplica il traffico per quel prefisso fino al valore del TTL iniziale.

**Asimmetria e filtri.** Su Internet il percorso di andata e quello di ritorno sono spesso diversi. Questo mette in crisi il reverse-path filtering stretto (RFC 3704), che scarta i pacchetti in arrivo da un'interfaccia che non verrebbe usata per raggiungerne la sorgente.

**Che cosa semplifica hoplab.** Le rotte hanno un solo next hop (niente multipath a costo uguale) e la tabella viene scandita linearmente. L'opzione `auto routes` usata da altri laboratori calcola i cammini minimi al posto tuo; i laboratori 200 e 202 la disattivano di proposito.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 200: `./hoplab run 200-static-routing`**

- Premi `n` per selezionare a turno r1, r2 e r3 e leggi la scheda **Rotte**. Prima dei 6 s, r2 ha righe solo per 10.0.12.0/24, 10.0.23.0/24 e 10.0.1.0/24.
- Nel registro (`l`) il primo ping fallisce con `da 10.0.12.2: rete irraggiungibile`: l'indirizzo dice quale router si è arreso.
- A 6 s la rotta compare nella tabella di r2 e il secondo ping funziona con `ttl=61`: 64 meno tre router.
- A 11 s traceroute elenca r1, r2, r3, h2. Apri la vista Pacchetti (`p`), premi Invio su una sonda e sull'ICMP time exceeded che le risponde.
- Prevedi, poi prova: `:route r3 del 10.0.1.0/24` e poi `:h1 ping h2 count=2`. h2 riceve la richiesta? Chi riceve l'errore ICMP, e che cosa stampa h1?
- Prevedi, poi prova: `:route r2 add 10.0.4.10/32 via 10.0.12.1` e ripeti il ping. La /24 è ancora presente e corretta. Perché ora il ping termina con «tempo di vita (TTL) esaurito»? Annulla con `:route r2 del 10.0.4.10/32`.

**Laboratorio 202: `./hoplab run 202-routing-loop`**

- Osserva la mappa: una sola echo request rimbalza sul collegamento r1-r2. Nella vista Pacchetti lo stesso pacchetto ricompare più volte, ogni volta con il TTL diminuito di uno.
- L'errore arriva da 10.0.12.2 (r2) dopo circa 0,14 s; il traceroute a 4 s alterna r1, r2, r1, r2.
- Prevedi, poi prova: `./hoplab run 202-routing-loop ttl=7`. Quante volte viene attraversato il collegamento, e quale router invia ora l'errore?
- Prevedi, poi prova: `:route r2 del 10.9.0.0/16`, poi `:h1 ping 10.9.1.1 count=1`. Quale messaggio sostituisce «time exceeded», e chi lo invia?

## Verifica

- Una tabella contiene 172.16.0.0/12 via A, 172.16.32.0/19 via B, 172.16.40.0/24 via C e una default via D. Indica il next hop per 172.16.41.9, 172.16.40.200, 172.31.255.1 e 172.32.0.1, mostrando le corrispondenze.
- Spiega perché un router che ha una rotta corretta verso la destinazione può comunque essere la causa del fallimento di un ping. Quale sintomo distingue una rotta di andata mancante da una rotta di ritorno mancante?
- Quali campi dell'intestazione IP e dell'intestazione Ethernet cambiano a ogni salto, e quali non cambiano mai fra sorgente e destinazione?
- Due router mandano in ciclo un prefisso. Un host vi invia 100 pacchetti al secondo con TTL iniziale 64. Quanti pacchetti al secondo attraversano, all'incirca, il collegamento fra i due router?
- Perché traceroute funziona, e che cosa si vede nel suo output quando il percorso contiene un ciclo?
