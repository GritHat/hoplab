<!-- level: 3 -->
<!-- labs: 350-nat-hole-punching, 351-lb-algorithms -->
# Attraversamento dei NAT e algoritmi di bilanciamento

Due middlebox riscrivono gli indirizzi lungo il percorso, per ragioni opposte: un NAT nasconde molti client dietro un solo indirizzo, un load balancer nasconde molti server dietro un solo indirizzo. Entrambi mantengono stato per flusso, ed entrambi pongono una questione algoritmica.

**Attraversamento dei NAT.** Due host dietro NAT diversi vogliono un percorso diretto (una chiamata, un gioco, un trasferimento di file). Nessuno dei due ha un indirizzo raggiungibile, e ogni NAT scarta i pacchetti che non ha sollecitato. È difficile perché il comportamento dei NAT non è mai stato fissato da uno standard: quale porta pubblica un NAT assegna, e chi lascia rientrare, cambiano da dispositivo a dispositivo, e gli host devono scoprirlo dall'interno. L'idea chiave è l'**hole punching**: un server di rendezvous con indirizzo pubblico comunica a ciascun peer l'endpoint pubblico dell'altro, così come appare dall'esterno, ed entrambi trasmettono nello stesso momento, in modo che ogni NAT veda prima un pacchetto in uscita e scambi per risposte i pacchetti del peer. Che funzioni o no dipende da due sole proprietà dei NAT: mapping e filtering.

**Bilanciamento del carico.** Le richieste vanno assegnate ai server in modo che nessuno sia sovraccarico, con una decisione presa per flusso, in nanosecondi, spesso da più bilanciatori che non condividono stato. L'informazione perfetta (il server meno carico) è costosa e invecchia; nessuna informazione (scelta casuale o hash) costa poco ma è disuniforme. L'idea chiave è che basta pochissima informazione: campionare **due** server a caso e scegliere il meno carico migliora il caso peggiore in modo esponenziale.

<!-- detail: 2 -->
## Algoritmi e modelli

**Comportamento dei NAT (RFC 4787).** Un socket interno `X:x` trasmette verso endpoint esterni. La regola di **mapping** dice quando l'endpoint pubblico `X':x'` viene riutilizzato:

- **Endpoint-independent mapping**: lo stesso `X':x'` per ogni destinazione.
- **Address-dependent**, **address-and-port-dependent mapping**: una nuova porta pubblica per ogni indirizzo di destinazione, oppure per ogni coppia indirizzo e porta di destinazione.

La regola di **filtering** dice quali sorgenti esterne possono trasmettere verso `X':x'`:

- **Endpoint-independent filtering**: chiunque.
- **Address-dependent filtering**: solo gli indirizzi verso cui `X:x` ha trasmesso.
- **Address-and-port-dependent filtering**: solo gli esatti endpoint verso cui `X:x` ha trasmesso.

I nomi più vecchi dell'RFC 3489 sono quattro punti di questa griglia: full cone = mapping indipendente + filtering indipendente; restricted cone = mapping indipendente + filtering dipendente dall'indirizzo; port-restricted cone = mapping indipendente + filtering dipendente da indirizzo e porta; symmetric = mapping e filtering dipendenti da indirizzo e porta.

**UDP hole punching.**

```
A -> S : registrazione     S vede A':a'     (il mapping di A verso S)
B -> S : registrazione     S vede B':b'
S -> A : B':b'             S -> B : A':a'
A -> B':b'  e  B -> A':a'   ripetutamente (PUNCH)
```

Il primo pacchetto di A crea, nel NAT di A, una voce di filtro per `B':b'`; se raggiunge il NAT di B prima che B abbia trasmesso viene scartato, ma il pacchetto di B trova poi aperto il NAT di A. Lo schema presuppone che l'endpoint visto da S sia quello che vedrà il peer, cioè un **mapping indipendente dall'endpoint**. Con un NAT simmetrico dal lato di A, i pacchetti verso B escono da una nuova porta `a''`. B può ancora apprendere `a''` dal primo pacchetto che riceve, ma solo se il NAT di B lascia passare un pacchetto da una porta sconosciuta: full cone o address-restricted. Quindi:

```
cone x cone                      funziona
symmetric x full/restricted      funziona (il lato cone risponde alla porta vista)
symmetric x port-restricted      fallisce
symmetric x symmetric            fallisce   -> relay (TURN)
```

**Algoritmi di bilanciamento.** Per i server `i` con peso `w_i` e `c_i` flussi attivi:

- **Round robin / weighted round robin**: ciclico, quote proporzionali a `w_i`; cieco rispetto alla durata dei flussi.
- **Least connections**: sceglie il minimo di `c_i/w_i`; si adatta ai flussi lunghi, richiede uno stato condiviso esatto.
- **Hashing**: server = `h(chiave) mod N`; senza stato e ripetibile fra bilanciatori diversi, ma quando `N` diventa `N+1` una frazione `N/(N+1)` delle chiavi cambia server.
- **Consistent hashing**: server (ciascuno come `V` nodi virtuali) e chiavi su un anello, una chiave va al primo server in senso orario; un cambiamento sposta circa `1/N` delle chiavi; la dispersione del carico si riduce come `1/sqrt(V)`.
- **Maglev**: ogni server riempie una tabella di dimensione prima `M` seguendo una propria permutazione; la ricerca è `table[h(chiave) mod M]`: bilanciamento quasi perfetto, perturbazione minima ma non nulla.
- **Power of two choices**: sceglie due server uniformemente a caso e invia a quello con meno flussi.

**Palline e urne (balls into bins).** Si lancino `n` palline in `n` urne. Con una sola scelta casuale l'urna più piena contiene circa `ln n / ln ln n` palline; scegliendo la più vuota fra `d >= 2` urne casuali si ottiene

```
max load = ln ln n / ln d + O(1)
```

**Esempio svolto.** Per `n` = 1000 l'urna più piena contiene tipicamente 5 o 6 palline con una scelta (il termine dominante `ln 1000 / ln ln 1000` vale 3,6) e 3 con due (`ln ln 1000 / ln 2` vale 2,8). Nel laboratorio 351 i pesi sono 3:1:1: WRR e least connections inviano il 60/20/20% dei flussi; il semplice RR ne invia un terzo a ciascuno, quindi s2 e s3 portano 1,67 volte la quota corrispondente alla loro capacità.

<!-- detail: 3 -->
## In profondità

**NAT: invarianti e modi di guasto.** L'hole punching riesce se e solo se ciascun lato può inviare un pacchetto a un endpoint che l'altro NAT accetterà e che riconduce al socket del peer. Oltre ai casi simmetrici, fallisce quando: il mapping scade fra la registrazione e il punch (l'RFC 4787 chiede almeno 2 minuti per UDP; molti dispositivi usano 30 s, da cui i keepalive); i due peer sono dietro lo stesso NAT e questo non fa hairpinning (per questo fra i candidati ci sono gli indirizzi privati); ci sono più livelli di NAT, come con il carrier-grade NAT; un firewall blocca UDP. I NAT simmetrici che conservano la porta o la assegnano in sequenza si possono talvolta battere con la predizione della porta, in modo inaffidabile. L'hole punching TCP usa l'apertura simultanea e funziona meno spesso. I sistemi reali non tirano a indovinare: ICE (RFC 8445) raccoglie candidati host, server-reflexive (STUN, RFC 8489) e relayed (TURN, RFC 8656), prova tutte le coppie e tiene la migliore che funziona; qualche punto percentuale delle chiamate finisce su un relay. Ford et al. hanno misurato che circa l'82% dei NAT supportava il punching UDP.

**Bilanciamento: perché due scelte bastano, e quando non bastano.** Nel modello del supermercato con tasso di arrivo `λ < 1` per server, la frazione di code con almeno `k` lavori è `λ^k` con assegnazione casuale e `λ^((d^k - 1)/(d - 1))` con `d` scelte: un decadimento doppiamente esponenziale. Ulteriori scelte danno solo un guadagno di un fattore costante. Un secondo effetto favorisce il campionamento: con informazione di carico ritardata, "il meno carico" fa scegliere a tutti i bilanciatori lo stesso server (effetto gregge), mentre due scelte casuali restano stabili. Gli schemi basati su hash scambiano bilanciamento con coerenza: mantengono i flussi sullo stesso server attraverso bilanciatori e guasti senza una tabella condivisa, ed è per questo che i router ECMP davanti a Maglev o Katran possono cambiare senza rompere le connessioni; una piccola tabella per flusso protegge comunque i flussi già stabiliti durante i cambiamenti del pool. Le varianti pesate e a carico limitato del consistent hashing limitano un server a `(1 + ε)` volte la media.

**In netlab.** Il modulo `nat` realizza i quattro tipi classici (default port-restricted, conservazione della porta quando è libera, timeout di inattività di 30 s); `rendezvous` è un server minimale di tipo STUN e i peer rispondono alla porta sorgente che osservano. Il modulo `lb` bilancia richieste UDP di un solo datagramma; da quanto si osserva in questo laboratorio, `p2c` confronta il numero grezzo di flussi e non usa i pesi, e della famiglia hash solo `consistent` li rispetta.

**Letteratura primaria.**

- D. Karger et al., "Consistent Hashing and Random Trees: Distributed Caching Protocols for Relieving Hot Spots on the World Wide Web", STOC 1997.
- D. Thaler, C. Ravishankar, "Using Name-Based Mappings to Increase Hit Rates", 1998 (rendezvous hashing).
- Y. Azar, A. Broder, A. Karlin, E. Upfal, "Balanced Allocations", 1994/1999.
- M. Mitzenmacher, "The Power of Two Choices in Randomized Load Balancing", 2001.
- B. Ford, P. Srisuresh, D. Kegel, "Peer-to-Peer Communication Across Network Address Translators", USENIX ATC 2005.
- D. Eisenbud et al., "Maglev: A Fast and Reliable Software Network Load Balancer", NSDI 2016.
- RFC 3489 e RFC 8489 (STUN), RFC 4787 (requisiti di comportamento dei NAT per UDP), RFC 5128 (stato del P2P attraverso i NAT), RFC 8445 (ICE), RFC 8656 (TURN).

<!-- detail: 1 -->
## In laboratorio

**350-nat-hole-punching.** `./netlab run 350-nat-hole-punching`. Gli host a e b stanno dietro i NAT na e nb; rv è il server di rendezvous. Aprire la scheda `nat` di na e nb (tabelle di traduzione) e la vista di sequenza (`s`). Con i NAT port-restricted di default: a si registra a 1 s, b a 2 s, rv li presenta; il primo PUNCH di a raggiunge nb prima che b abbia trasmesso e il log dice "mittente non sollecitato, filtrato"; il primo PUNCH di b attraversa na; entrambi riportano "buco aperto". Si noti che entrambi i NAT hanno mantenuto la stessa porta pubblica (61001) verso rv e verso il peer.

- Compilare prima su carta la matrice 4 x 4, poi provare le celle più istruttive: `typea=symmetric typeb=restricted` (funziona: b scrive "il peer ci raggiunge da …, non da … — il suo NAT è simmetrico; rispondo lì"), `typea=symmetric typeb=port` (fallisce: ogni PUNCH è filtrato da entrambi i lati, "serve un relay"), `typea=symmetric typeb=symmetric` (fallisce), `typea=full typeb=symmetric` (funziona).
- Nelle esecuzioni con NAT simmetrico leggere la tabella di na: due associazioni per lo stesso socket interno, una verso rv e una verso il peer, con porte pubbliche diverse. Quella differenza è tutta la spiegazione.
- `waitb=0`: entrambi fanno il punch nello stesso istante e nulla viene filtrato. Spiegare perché la simultaneità conta più per TCP che per UDP.

**351-lb-algorithms.** `./netlab run 351-lb-algorithms`. Il client c1 apre molti flussi brevi, c2 ne mantiene 20 di lunga durata; s1 ha peso 3. Aprire la scheda `lb` di lb (barre per server, `flussi attivi`) e leggere l'ultima riga di c1 e di c2 (`reqgen: … s1 … s2 … s3 …`). Eseguire una volta per algoritmo e annotare le quote: `algo=wrr`, `rr`, `leastconn`, `p2c`, `hash`, `consistent`, `maglev`.

- Previsioni: `wrr` e `leastconn` danno circa 60/20/20; `rr`, `p2c`, `hash` e `maglev` circa un terzo ciascuno (qui ignorano i pesi); `consistent` favorisce s1. Con soli 20 flussi da c2 le quote degli schemi a hash sono rumorose: stimare la deviazione standard di una binomiale con 20 prove prima di giudicarle.
- `algo=hash key=src`: due client, quindi al più due server lavorano; qui ogni client finisce interamente su uno.
- `algo=rr affinity=0`: ogni datagramma viene rischedulato e reqgen conta più di cento "flussi hanno cambiato server". Innocuo per queste richieste di un solo datagramma; dire che cosa accadrebbe a una connessione TCP.
- In un laboratorio in esecuzione, `:` poi `node s2 down`: dopo due health check mancati il bilanciatore lo rimuove. Confrontare quanti flussi si spostano con `hash` e con `consistent`.

## Verifica

- Classificare, nei termini dell'RFC 4787, un NAT che riusa la porta pubblica per tutte le destinazioni ma accetta pacchetti in ingresso solo da indirizzi già contattati. Con quali tipi di NAT del peer l'hole punching riesce, e perché?
- Mostrare passo per passo perché symmetric contro port-restricted fallisce benché ciascun NAT abbia inviato un pacchetto verso l'altro. Quale singolo allentamento, da uno dei due lati, lo fa funzionare?
- Con l'hashing modulo N, calcolare la frazione di flussi riassegnati quando un pool di 10 server ne perde uno, e confrontarla con il consistent hashing. Perché di solito un bilanciatore mantiene comunque una tabella dei flussi?
- Enunciare il risultato balls-into-bins per d scelte e spiegare perché passare da una scelta a due conta molto più che passare da due a tre.
- Più bilanciatori indipendenti usano least connections su dati di carico aggiornati una volta al secondo. Descrivere la patologia e spiegare perché power of two choices vi è meno esposto.
