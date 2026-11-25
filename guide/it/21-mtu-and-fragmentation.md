<!-- level: 2 -->
<!-- labs: 201-mtu-fragmentation -->
# MTU, frammentazione e scoperta della MTU del percorso

Ogni collegamento ha una **maximum transmission unit** (MTU): il più grande pacchetto IP, intestazione compresa, che un frame può trasportare. Quella di Ethernet è 1500 byte; i tunnel e alcuni collegamenti di accesso ne hanno meno. Un mittente conosce solo la MTU del proprio collegamento, ma il pacchetto deve attraversare tutti i collegamenti del percorso, ed è la MTU più piccola lungo la strada, la **MTU del percorso** (path MTU), a decidere che cosa passa.

IPv4 offre due risposte. Un router che deve inviare un pacchetto più grande della MTU del collegamento successivo può **frammentarlo**: taglia il carico utile in pezzi, dà a ogni pezzo una copia dell'intestazione e li invia come pacchetti indipendenti. Solo la destinazione li rimette insieme. Funziona sempre, ma è fragile: se si perde un frammento si perde l'intero datagramma, e il riassemblaggio costa memoria e tempo.

L'alternativa è far usare al mittente la dimensione giusta. Il mittente imposta il flag **don't fragment** (DF); un router che non può inoltrare il pacchetto lo scarta e restituisce un errore ICMP che dichiara la MTU del collegamento responsabile. Il mittente riduce i pacchetti e riprova. È la **scoperta della MTU del percorso** (path MTU discovery), ed è ciò che TCP fa oggi praticamente su ogni connessione.

<!-- detail: 2 -->
## I campi dell'intestazione

Tre campi dell'intestazione IPv4 esistono solo per la frammentazione:

- **Identification** (16 bit): scelto dal mittente, uguale in tutti i frammenti di uno stesso datagramma. Insieme a sorgente, destinazione e protocollo dice al ricevente quali frammenti vanno insieme.
- **Flags** (3 bit): un bit riservato, **DF** (don't fragment) e **MF** (more fragments). MF vale 1 in tutti i frammenti tranne l'ultimo.
- **Fragment offset** (13 bit): la posizione del carico di questo frammento nel carico originale, in unità di 8 byte. Perciò ogni frammento tranne l'ultimo deve trasportare un multiplo di 8 byte.

Il campo total length e il checksum dell'intestazione vengono ricalcolati per ogni frammento. Un pacchetto completo, non frammentato, ha MF = 0 e offset = 0.

## Frammentare: un esempio svolto

Nel laboratorio 201 h1 esegue un ping verso h2 con 1400 byte di dati. ICMP aggiunge 8 byte di intestazione, IP ne aggiunge 20: il pacchetto è lungo 1428 byte, con un carico di 1408. Il collegamento centrale ha MTU 600.

Ogni frammento può essere lungo al massimo 600 byte, quindi al massimo 580 byte di carico; arrotondando per difetto a un multiplo di 8 si ottiene 576. Il router r1 produce:

```
frammento  byte del carico  campo offset   MF   total length
   1          0 - 575            0         1       596
   2        576 - 1151          72         1       596
   3       1152 - 1407         144         0       276
```

Verifica: 72 × 8 = 576, 144 × 8 = 1152, e 576 + 576 + 256 = 1408. Il registro dice esattamente `1428 B > MTU 600 → 3 frammenti`, e più tardi h2 segnala `datagramma riassemblato (1408 B)`. Solo il primo frammento contiene l'intestazione ICMP; in un datagramma UDP o TCP solo il primo contiene i numeri di porta.

## Riassemblaggio

La destinazione mantiene un buffer per ogni (sorgente, destinazione, protocollo, identification). Colloca ogni frammento alla posizione offset × 8, ricava la lunghezza totale dal frammento con MF = 0 e consegna il datagramma quando non restano buchi. Un timer parte con il primo frammento; se scade (tipicamente fra 15 e 60 s) tutto viene scartato. I frammenti possono arrivare in qualsiasi ordine, e un frammento può essere frammentato di nuovo da un router successivo: i campi sono progettati in modo che non serva alcun caso speciale.

La perdita viene amplificata. Se ogni pacchetto si perde con probabilità p, un datagramma in n frammenti sopravvive con probabilità (1 − p)^n. Con p = 0,2 a un ping piccolo servono due pacchetti che sopravvivono (richiesta e risposta): 0,8² = 64%. Al ping frammentato del laboratorio ne servono sei: 0,8⁶ = 26%.

## Scoperta della MTU del percorso

Con DF = 1 il router non può frammentare. Scarta il pacchetto e invia un ICMP **destination unreachable, fragmentation needed and DF set** (tipo 3, codice 4), che riporta la MTU del salto successivo. Il mittente registra quel valore come MTU del percorso verso quella destinazione e invia pacchetti più piccoli; se un collegamento successivo è ancora più piccolo, il procedimento si ripete. Nel laboratorio r1 risponde `fragmentation needed` con MTU 600; un ping da 500 byte di dati (528 in totale) passa poi intatto.

Per TCP la MTU del percorso fissa la dimensione dei segmenti: MSS = MTU del percorso − 40, in assenza di opzioni.

<!-- detail: 3 -->
## In profondità

**Perché si evita la frammentazione.** L'articolo di Kent e Mogul «Fragmentation Considered Harmful» (1987) ne elencava i problemi, e l'RFC 8900 («IP Fragmentation Considered Fragile», 2020) li conferma: amplificazione delle perdite; stato di riassemblaggio che un attaccante può esaurire; i frammenti successivi al primo non contengono le porte, perciò firewall, NAT e bilanciatori non riescono a classificarli e spesso li scartano; e l'identification a 16 bit si riavvolge in fretta ad alte velocità, così che frammenti di datagrammi diversi possono mescolarsi (RFC 4963, RFC 6864). I frammenti sovrapposti sono stati usati per mandare in crash gli host e per eludere i controlli (gli attacchi «teardrop»).

**IPv6** elimina del tutto la frammentazione da parte dei router (RFC 8200): solo la sorgente può frammentare, con un'intestazione di estensione, e ogni collegamento deve supportare 1280 byte. In IPv4 ogni host deve accettare datagrammi di 576 byte e ogni collegamento deve trasportarne 68.

**Quando la scoperta fallisce.** La PMTUD classica (RFC 1191; RFC 8201 per IPv6) dipende dall'arrivo dell'errore ICMP al mittente. I firewall che scartano tutto ICMP creano un **buco nero**: l'handshake funziona perché i suoi pacchetti sono piccoli, poi il primo segmento di dimensione piena sparisce e la connessione si blocca. I rimedi sono l'MSS clamping (un router riscrive l'opzione MSS nei segmenti SYN, comune su PPPoE e tunnel) e la Packetization Layer PMTUD (RFC 4821, e RFC 8899 per i trasporti a datagrammi), in cui è il trasporto a sondare con pacchetti più grandi e a interpretare la perdita di una sonda come segnale sulla dimensione, senza bisogno di ICMP. I valori scoperti devono invecchiare, perché i percorsi cambiano; e un errore ICMP va convalidato rispetto al pacchetto citato, altrimenti chiunque potrebbe ridurre la MTU di una connessione.

**I tunnel** rendono il problema permanente: ogni incapsulamento (VPN, VXLAN, GRE) sottrae la propria intestazione alla MTU, per cui percorsi con MTU effettiva fra 1400 e 1480 sono comuni.

**Che cosa semplifica netlab.** Il laboratorio mostra il meccanismo ma non l'automatismo: il terzo ping è semplicemente previsto dallo script con una dimensione minore. La MTU di un collegamento è un parametro (`mtu=`) e vale in entrambe le direzioni.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 201: `./netlab run 201-mtu-fragmentation`**

- Prima fase (0 s): nel registro r1 segnala `IP frammentazione: 1428 B > MTU 600 → 3 frammenti`, h2 riassembla e la risposta viene frammentata da r2 nell'altra direzione.
- Apri la vista Pacchetti (`p`), scegli uno dei tre frammenti sul collegamento r1-r2 e premi Invio. Leggi identification, flag e fragment offset, e confronta i tre frammenti con la tabella qui sopra. Poi apri lo stesso pacchetto sul collegamento h1-r1: un solo pezzo, MF = 0.
- Seconda fase (4 s): lo stesso ping con DF impostato. r1 lo scarta e invia `fragmentation needed`; apri il messaggio ICMP e trova la MTU del salto successivo e l'intestazione citata del pacchetto scartato.
- Terza fase (8 s): 500 byte di dati con DF passano senza essere toccati.
- Prevedi, poi prova: `./netlab run 201-mtu-fragmentation mtu=1000`. Quanti frammenti, con quali offset e lunghezze? Poi `mtu=500`: che cosa succede alla terza fase, e perché?
- Prevedi, poi prova: `./netlab run 201-mtu-fragmentation loss=0.2` più volte (ogni esecuzione ha un seme diverso). Quale delle tre fasi perde più ping? Confronta con 0,8⁶ e 0,8².
- Trova il limite a mano dalla riga di comando del laboratorio: `:h1 ping h2 count=1 size=572 df=1` e poi `size=573`. Spiega dove cade il confine.

## Verifica

- Un datagramma con 3000 byte di carico e un'intestazione di 20 byte attraversa un collegamento con MTU 1500 e poi uno con MTU 620. Elenca i frammenti che arrivano a destinazione: lunghezza del carico, campo offset e MF di ciascuno.
- Perché il fragment offset è espresso in unità di 8 byte, e quale vincolo impone alle dimensioni dei frammenti?
- Perché i frammenti vengono riassemblati solo a destinazione e non dal router successivo?
- Descrivi passo per passo la scoperta della MTU del percorso, e spiega che cos'è un «buco nero PMTUD» e perché l'handshake TCP non lo rivela.
- Un firewall vuole bloccare la porta UDP 53. Perché i frammenti sono un problema?
