<!-- level: 3 -->
<!-- labs: 360-rs-vs-hamming, 361-mac-curves, 370-stp-tuning, 340-dns-ttl -->
# Codici, canali condivisi, spanning tree e cache: quattro compromessi di progetto

Questo argomento riunisce quattro problemi di progetto classici, sotto e accanto al livello di rete. Ciascuno ha un modello quantitativo, e in ciascuno la risposta da manuale diventa interessante solo quando si fa variare un parametro.

**Codici a correzione d'errore.** La ridondanza compra affidabilità, ma quale codice vinca dipende dalla statistica degli errori: inversioni di bit indipendenti oppure a raffica (burst). L'idea chiave è che i codici lunghi su simboli grandi (Reed–Solomon) mediano il rumore, e che un interleaver trasforma una raffica in errori isolati.

**Accesso al mezzo.** Stazioni che condividono un canale senza un arbitro collidono. Il throughput `S` in funzione del carico offerto `G` sale, raggiunge un massimo e crolla; l'intera famiglia ALOHA, CSMA, CSMA/CD, CSMA/CA è una successione di modi per ridurre il tempo in cui una trasmissione è vulnerabile, governata da un solo parametro: il rapporto `a` fra ritardo di propagazione e tempo di frame.

**Spanning tree.** Ethernet non ha un limite di hop, quindi i collegamenti ridondanti vanno bloccati con un'elezione distribuita. Il protocollo è semplice; l'ingegneria sta nella scelta della radice, dei costi e dei timer, dove la sicurezza (nessun ciclo transitorio) si paga in tempo di ripristino.

**Cache del DNS.** Un TTL è la promessa di non chiedere di nuovo. TTL lunghi risparmiano query e latenza e nascondono i disservizi; TTL brevi rendono presto visibili i cambiamenti. Anche le risposte negative hanno bisogno di un TTL.

Ciò che rende difficili tutti e quattro è la stessa cosa: una decisione locale e poco costosa (quanti bit di parità, quando trasmettere, quale porta bloccare, per quanto ricordare) fissa un comportamento globale che solo un modello o una misura rivelano.

<!-- detail: 2 -->
## Algoritmi e modelli

**Codici a blocco.** Un codice `(n, k)` con distanza minima `d` ha rate `k/n` e corregge `t = floor((d - 1)/2)` errori per blocco.

```
Hamming(7,4):  d = 3,   t = 1 bit per blocco di 7 bit,      rate 0.57
RS(255,223):   d = 33,  t = 16 byte per blocco di 255 byte, rate 0.87
               (d = n - k + 1)
```

Su un canale con tasso d'errore sul bit `p` e errori indipendenti, un blocco di Hamming fallisce quando due o più dei suoi 7 bit si invertono:

```
P(blocco errato) ~ C(7,2)*p^2 = 21*p^2
P(frame di L bit di dati errato) ~ (L/4)*21*p^2
```

**Esempio svolto (laboratorio 360).** `p` = 0,001, frame di circa 4300 bit: 1084 blocchi, quindi circa `1084*21e-6` = 2,3% dei frame risulta danneggiato. Per Reed–Solomon un byte è errato con probabilità di circa `8p` = 0,008, un blocco contiene in media 2 byte errati e fallisce solo oltre 16: in pratica mai. Vince il codice con meno ridondanza. Una raffica di `b` bit rovina al più `floor((b - 1)/8) + 2` byte, quindi una raffica di 12 bit costa a RS due o tre dei suoi sedici simboli, mentre mette più errori in uno o due blocchi di Hamming. Un **interleaver** di profondità `D` trasmette le parole di codice colonna per colonna: una raffica non più lunga di `D` lascia al più un errore in ciascuna.

**Curve S(G).** Con tentativi di Poisson a tasso `G` per tempo di frame, infinite stazioni, e `a = ritardo di propagazione / tempo di frame`:

```
ALOHA puro:           S = G*exp(-2G)      max 1/(2e) = 0.184 per G = 0.5
slotted ALOHA:        S = G*exp(-G)       max 1/e   = 0.368 per G = 1
CSMA non persistente: S = G*exp(-a*G) / ( G*(1 + 2a) + exp(-a*G) )
CSMA/CD (saturazione): S ~ 1 / (1 + 2e*a)
```

Con `a` = 0,01 e `G` = 5,55 la formula del CSMA dà `5.25/6.61` = 0,795, il valore stampato dal laboratorio. Il CSMA 1-persistente fa meglio a basso carico e ha il massimo vicino a 0,53 per lo stesso `a`.

**Spanning tree.** Ogni BPDU porta un vettore di priorità, confrontato lessicograficamente (il minore è il migliore):

```
( ID del root bridge, costo verso la radice, ID del bridge mittente,
  ID della porta mittente )
bridge ID = priorità (16 bit) . indirizzo MAC
```

Il bridge con l'ID più basso è la radice; su ogni altro bridge la porta che riceve il vettore migliore è la root port; su ogni segmento il bridge che offre il vettore migliore possiede la designated port; tutte le altre porte si bloccano. Una porta che passa a forwarding trascorre un forward delay in listening e uno in learning. Il ripristino richiede `2*fwd` dopo un guasto rilevato localmente, e fino a `max_age + 2*fwd` quando l'informazione deve scadere (20 + 30 s con i timer standard 2/20/15 s).

**Cache del DNS.** Per un nome interrogato presso un resolver secondo un processo di Poisson di tasso `λ`, con TTL `T`:

```
query a monte al secondo = λ/(1 + λ*T)       hit ratio = λ*T/(1 + λ*T)
obsolescenza massima dopo un cambiamento = T
```

Con `λ` = 1/s, `T` = 60 s dà un hit ratio del 98,4% e `T` = 5 s dà l'83%. Una catena di CNAME viene messa in cache anello per anello; la durata effettiva è il minimo lungo la catena. Una risposta negativa (NXDOMAIN, oppure nessun dato di quel tipo) resta in cache per il minimo fra il TTL del record SOA e il suo campo MINIMUM (RFC 2308).

<!-- detail: 3 -->
## In profondità

**Codici.** Il teorema di Shannon dice che un canale binario simmetrico ha capacità `1 - H(p)`: per `p` = 0,001 vale 0,989, quindi entrambi i codici del laboratorio sprecano rate, e i codici corti non possono fare altrimenti: l'affidabilità vicino alla capacità richiede blocchi lunghi. I codici Reed–Solomon sono maximum distance separable (raggiungono il limite di Singleton) e il loro decodificatore o corregge o dichiara il fallimento, mentre un decodificatore di Hamming davanti a due errori inverte con sicurezza un terzo bit: è la colonna "sbagliati in silenzio" del banco di prova. Per questo i collegamenti mantengono un CRC sopra il FEC. L'interleaving costa ritardo e memoria proporzionali alla profondità e non fa nulla contro gli errori casuali. I sistemi reali concatenano i codici (RS all'esterno di un codice convoluzionale, da Voyager al DVB), oppure usano codici LDPC e polari (Wi-Fi, 5G), con l'hybrid ARQ sopra.

**Canali.** Le curve presuppongono popolazione infinita e assenza di memoria; un sistema reale è un anello di retroazione. ALOHA con probabilità di ritrasmissione fissa è instabile: oltre il massimo l'arretrato cresce, `G` aumenta e `S` cala ancora. Lo stabilizzatore è il backoff, e il backoff esponenziale binario è stabile solo per popolazioni limitate. Il CSMA peggiora al crescere di `a` (cavi lunghi, frame corti, velocità elevate), ed è la ragione della dimensione minima del frame Ethernet. Per l'802.11 il modello giusto è la catena di Markov di Bianchi sul contatore di backoff, non `S(G)`.

**Spanning tree.** Invariante di sicurezza: una porta inoltra solo dopo che ogni bridge ha avuto il tempo (due forward delay) di apprendere la nuova topologia. Il pericolo non è un collegamento perso ma un BPDU perso: una porta bloccata che smette di sentire BPDU (collegamento unidirezionale, CPU sovraccarica, timer troppo stretti per il tasso di perdita) si crede designated e apre un ciclo. Da qui il vincolo dello standard `2*(fwd - 1) >= max_age >= 2*(hello + 1)`, e funzioni come loop guard, root guard e BPDU guard. La radice (e una riserva con la priorità successiva) va posta al centro della topologia: l'albero non dà cammini minimi fra le foglie. RSTP (802.1w) sostituisce i timer con un handshake proposal/agreement; le fabric instradate con ECMP, TRILL e SPB evitano del tutto di bloccare porte.

**DNS.** Le misure (Jung et al.) mostrano che, poiché la popolarità dei nomi ha coda pesante, TTL di qualche minuto sui record A costano poco in hit ratio; ciò che conta è la cache dei record NS della catena di deleghe. TTL brevi permettono bilanciamento e failover ma espongono gli utenti a un disservizio degli autoritativi; i resolver limitano i TTL estremi e possono servire dati scaduti (RFC 8767).

**In hoplab.** Codifica: i frame sono codificati per intero, l'interleaver copre un solo frame, RS usa blocchi fissi (255,223). MAC: il tempo avanza in tick di un ritardo di propagazione, ogni stazione dista un tick da ogni altra, dieci stazioni con code finite, quindi l'accordo con la teoria è nella forma. STP: timer accorciati (1/6/3 s); `rapid=1` è Rapid Spanning Tree con proposal/agreement, sync, porte alternate ed edge automatiche e scadenza dopo tre hello (niente porte backup, regole 802.1D per i cambi di topologia). DNS: niente TCP, EDNS o DNSSEC.

**Letteratura primaria.**

- C. Shannon, "A Mathematical Theory of Communication", 1948; R. Hamming, "Error Detecting and Error Correcting Codes", 1950; I. Reed, G. Solomon, "Polynomial Codes over Certain Finite Fields", 1960.
- N. Abramson, "The ALOHA System: Another Alternative for Computer Communications", 1970.
- L. Kleinrock, F. Tobagi, "Packet Switching in Radio Channels: Part I, Carrier Sense Multiple-Access Modes and Their Throughput-Delay Characteristics", 1975.
- R. Metcalfe, D. Boggs, "Ethernet: Distributed Packet Switching for Local Computer Networks", 1976.
- G. Bianchi, "Performance Analysis of the IEEE 802.11 Distributed Coordination Function", 2000.
- R. Perlman, "An Algorithm for Distributed Computation of a Spanning Tree in an Extended LAN", 1985; IEEE 802.1D.
- P. Mockapetris, RFC 1034 e RFC 1035, 1987; RFC 2308 (cache negativa).
- J. Jung, E. Sit, H. Balakrishnan, R. Morris, "DNS Performance and the Effectiveness of Caching", 2002.

<!-- detail: 1 -->
## In laboratorio

**360-rs-vs-hamming.** `./hoplab run 360-rs-vs-hamming`. Due collegamenti con BER 0,001: h1–h2 codificato con Hamming(7,4), r1–r2 con Reed–Solomon. A 10 s confrontare le schede `sink` di h2 e r2: su 200 datagrammi RS li consegna tutti, Hamming ne perde qualcuno (calcolare prima il 2,3% atteso; il log mostra "NON rilevati dal collegamento: frame consegnato danneggiato" seguito da "UDP checksum errato"). Da 11 s il nodo lab stampa tre scansioni con errori in raffiche di 12 bit: leggere "consegnati", "riparati", "scartati", "sbagliati in silenzio" per `hamming74`, `hamming74i` e `rs`. Il banco mantiene fisso il BER medio, quindi raffiche più lunghe significano meno frame colpiti: si giudichi un codice dalla quota di frame colpiti che ripara, non dal solo "consegnati". Esperimenti: `burst=300` (circa 38 byte, più dei 16 che RS corregge: ora RS non ripara nulla e scarta, mentre Hamming con interleaving ripara ancora, perché un frame da 200 byte è fatto di 400 blocchi e la raffica mette al più un errore in ciascuno); `burst=600` (più lunga della profondità dell'interleaver: prevedere che nessun codice ripari più); nel laboratorio, `:` poi `lab coding sweep code=rs size=500 burst=200`, e `lab coding sweep code=rep3`. Si noti che RS scarta là dove Hamming consegna dati sbagliati.

**361-mac-curves.** `./hoplab run 361-mac-curves`. Il nodo bench esegue sette scansioni; aprire la scheda `mac` (barre, il segno giallo è la teoria) oppure leggere le tabelle nel log: `carico`, `G` misurato, `S`, `teoria`. Verificare a mano una riga di ALOHA e una di CSMA non persistente con le formule precedenti. Esperimenti: `prop=10ms` (quindi `a` = 0,1: prevedere che i massimi di ALOHA non si muovano e che ogni massimo dei CSMA scenda, all'incirca da 0,84 a 0,6 per il non persistente); `stations=3` e `stations=24` (distanza dal modello a popolazione infinita); `:` poi `bench mac sweep proto=csmap p=0.01`.

**370-stp-tuning.** `./hoplab run 370-stp-tuning`. Quattro switch ad anello, s3 imposto come radice con priorità 4096. Nella scheda `stp` di ogni switch leggere ruolo, stato e costo di ogni porta. Verificare i costi: s1 raggiunge la radice con costo 110 passando per s4 (100 + 10) e 200 passando per s2, quindi p1 è la sua root port e p0 è bloccata. Fra h1 e h2 corrono due flussi da 50 datagrammi al secondo. A 20 s la radice si guasta: misurare l'interruzione come datagrammi di h1 che mancano a h2 (le ultime righe del registro, `check outage`, 20 ms ciascuno), oppure come tempo fino a "learning → forwarding" su s1. Atteso circa 10,8 s: l'età residua dell'informazione più due forward delay. Esperimenti:

- `hello=200ms maxage=1s fwd=500ms`: prevedere l'interruzione (meno di 2 s). Poi, nel laboratorio, `:` e `link s1 s4 loss=0.3`: con quale probabilità si perdono cinque BPDU consecutivi, e che cosa accade quando succede?
- `:` poi `s1 stp cost p1 500`: prevedere la nuova root port di s1 e quale porta dell'anello si blocca.
- `rapid=1`: prevedere il guadagno. La radice morta viene abbandonata dopo tre hello mancanti (3 s al massimo, invece di max age), i superstiti eleggono s1, e la sua porta verso s2 si apre con uno scambio proposal/agreement invece di due forward delay: circa 2 s di interruzione invece di circa 11. Quale parte dei 2 s rimanenti potrebbe togliere `hello=500ms`?
- `root=s1` oppure `root=s2`: ora la radice è uno switch con un host collegato. Che cosa non può riparare nessun protocollo?
- `fast=10M`: tutti i costi sono uguali. Prevedere la root port di s1, e perché la morte della radice non interrompe più i flussi.

**340-dns-ttl.** `./hoplab run 340-dns-ttl`. Nel log: il primo `dig shop.example.com` costa una query a monte e circa 68 ms e restituisce la catena shop → www → host → A; la seconda ricerca di `ghost` riceve risposta dalla cache negativa in 4 ms; `www` a 7 s riceve risposta dalla cache con il TTL decrementato a 55. Esperimenti: `ttl=3` (a 7 s la voce è scaduta: un'altra query a monte); `negttl=1` (il secondo NXDOMAIN va a monte); nel laboratorio, `:` poi `r dns flush` e `h1 dig www.example.com server=r`.

## Verifica

- Per un canale con errori indipendenti a BER p, stimare la probabilità di fallimento del frame per Hamming(7,4) e per RS(255,223) con frame da 1500 byte, e trovare all'incirca il p al quale RS comincia a fallire. Perché RS scarta là dove Hamming consegna dati sbagliati?
- Un interleaver di profondità D precede un codice a correzione di errore singolo con blocchi di lunghezza n. Quali lunghezze di raffica vengono corrette completamente, e a quale costo?
- Ricavare S = G·e^(−2G) e il suo massimo. Spiegare perché lavorare con G oltre il massimo è instabile senza backoff.
- Con la formula del CSMA non persistente, mostrare il limite di S per G → ∞ quando a → 0, e spiegare fisicamente perché CSMA/CD è insensibile al sovraccarico mentre CSMA non lo è.
- Una rete usa hello 2 s, max age 20 s, forward delay 15 s. Dare il tempo di ripristino nel caso peggiore dopo il guasto della radice e dopo il guasto di un collegamento adiacente a una porta bloccata. Perché è pericoloso accorciare il solo forward delay?
