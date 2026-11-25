<!-- level: 4 -->
<!-- labs: 440-dns-poisoning, 240-dns-hierarchy, 340-dns-ttl -->
# Sicurezza del DNS: di che cosa può fidarsi un resolver

Un resolver pone una domanda su UDP e crede alla prima risposta che sembra giusta. «Sembra giusta» significa soltanto che la risposta proviene dall'indirizzo interrogato, arriva sulla porta da cui è partita la domanda, ripete la domanda e porta lo stesso identificatore a 16 bit. Nulla di tutto ciò dimostra chi l'abbia scritta. Chi riesce a far sembrare giusta una risposta prima che arrivi quella vera decide che cosa il resolver mette in cache, e quindi dove viene mandato ogni client di quel resolver, per tutto il tempo indicato dal TTL del record.

La difesa che tutta Internet ha adottato nel 2008 consiste semplicemente nel rendere la risposta più difficile da indovinare: scegliere a caso l'identificatore e la porta sorgente, così che un falsario che non vede la domanda debba indovinare decine di bit invece di una manciata. La difesa duratura è di natura diversa: firmare i dati (DNSSEC), in modo che la risposta dimostri la propria origine qualunque strada abbia percorso.

<!-- detail: 2 -->
## Perché la difesa è l'imprevedibilità

Un attaccante che si trova sul percorso vede la domanda e può rispondere; contro di lui serve solo la crittografia. Il caso interessante è l'attaccante **fuori percorso**, che non vede nulla e deve indovinare. Ciò che c'è da indovinare è l'entropia del resolver:

- l'identificatore di transazione a 16 bit: 65 536 possibilità se casuale, quasi nessuna se sequenziale
- la porta sorgente UDP: circa altri 2^16 se casuale, nessuna se il resolver usa sempre la stessa porta
- facoltativamente le maiuscole e minuscole del nome richiesto (codifica 0x20), qualche bit in più che i server onesti ripetono

Con porta fissa e identificatore casuale, un falsario che riesce a consegnare n risposte nella finestra che precede quella autentica vince con probabilità circa n / 65 536 a ogni tentativo. L'intuizione pubblicata da Dan Kaminsky nel 2008 fu che i tentativi non sono limitati dal TTL: chiedendo nomi inesistenti sotto il dominio della vittima, ogni tentativo apre una nuova gara, e una risposta falsificata può portare record di autorità per l'intero dominio. La risposta fu la casualità della porta sorgente (RFC 5452), che moltiplica lo spazio di ricerca per decine di migliaia.

Altre misure restringono ciò che un resolver accetta: il **controllo del bailiwick** (ignorare i record esterni alla zona per cui il server è autoritativo), la preferenza dei dati in base all'affidabilità (la graduatoria della RFC 2181) e il limite al numero di domande identiche in sospeso (l'effetto del compleanno).

**DNSSEC** cambia la domanda da «è arrivata nel modo giusto?» a «è firmata dalla zona?». Le zone firmano gli insiemi di record (RRSIG) con chiavi (DNSKEY) garantite dalla zona superiore (DS), fino a un'ancora di fiducia alla radice; la negazione firmata dell'esistenza (NSEC/NSEC3) copre le risposte negative. Offre integrità e autenticazione dell'origine, non riservatezza. I trasporti cifrati — DNS su TLS (RFC 7858), su HTTPS (RFC 8484), su QUIC (RFC 9250) — proteggono il tratto fra un client e il suo resolver, che è un altro anello della catena.

<!-- detail: 3 -->
## Questioni aperte e limiti del laboratorio

La casualità è una difesa probabilistica, e la ricerca continua a trovare modi per riprendersi entropia. Gli attacchi basati sulla frammentazione (Herzberg e Shulman, «Fragmentation Considered Poisonous», 2013) sfruttano il fatto che identificatore e porta viaggiano solo nel primo frammento. Canali laterali possono rivelare la porta effimera (Man et al., «DNS Cache Poisoning Attack Reloaded: Revolutions with Side Channels», ACM CCS 2020, noto come SAD DNS). Un traduttore di indirizzi davanti a un resolver può annullarne la casualità delle porte. A ciascuno di questi è stata data una contromisura specifica; la risposta strutturale resta la validazione di dati firmati, la cui diffusione è parziale: molte zone non sono firmate e la validazione sul client è rara, quindi l'ultimo tratto si affida al trasporto.

DNSSEC ha costi che meritano studio: risposte più grandi (e il loro uso nell'amplificazione), gestione delle chiavi e rinnovi falliti che rendono una zona irrisolvibile, enumerazione della zona tramite NSEC, e l'onere operativo che ne spiega la lenta adozione.

Sul modello del laboratorio. `dnsspoof` è un falsario interamente simulato che vive dentro la rete emulata: gli viene detto l'indirizzo del resolver, gli fa chiedere un nome, e invia risposte false con l'indirizzo del server autoritativo come sorgente. Contro `randomize=0` conosce la porta e percorre gli identificatori. Contro `randomize=1` estrae a caso identificatore e porta dalle stesse 2^entropy coppie usate dal resolver, quindi una gara in cui n risposte false arrivano prima di quella vera è vinta con probabilità circa 1 − exp(−n / 2^entropy); il laboratorio ripete la gara su nomi nuovi e confronta le vittorie con quella legge. Il parametro `entropy` esiste solo per rendere visibile la legge: un resolver reale ha circa 30 bit e nessuna manopola per ridurli. Il falsario non realizza nessuno dei raffinamenti descritti sopra, non tocca i record di autorità o aggiuntivi (ciò che rese l'attacco del 2008 pericoloso per interi domini), qui i collegamenti non riordinano né frammentano, il resolver non valida firme e non c'è un laboratorio sul bailiwick: le conclusioni sui resolver reali devono venire dalla letteratura, non da questo emulatore.

**IPv6 cambia qualcosa?** Non per questa classe di attacchi. La domanda porta sempre un identificatore a 16 bit e parte da una porta sorgente a 16 bit, quindi un resolver ha la stessa entropia qualunque protocollo trasporti il datagramma; lo spazio di indirizzi a 128 bit rende gli host difficili da trovare per scansione, ma il falsario sa già a quale resolver mira e quale server deve impersonare. Cambia invece la famiglia di attacchi basati sulla frammentazione: i router IPv6 non frammentano mai, lo fa solo il mittente, dopo un messaggio Packet Too Big (RFC 8200), quindi un attaccante deve indurre il server autoritativo stesso a frammentare le proprie risposte, e gli operatori rispondono tenendo le risposte DNS su UDP sotto la MTU minima (la dimensione di buffer EDNS di 1232 byte concordata con il DNS Flag Day 2020). Il laboratorio svolge la stessa gara su entrambe le famiglie con il parametro `family`, falsificando un record A o AAAA; non modella nessuna delle varianti basate sulla frammentazione.

Fonti primarie: RFC 1034/1035 (il protocollo), RFC 3833 (analisi delle minacce al DNS), RFC 5452 (misure contro le risposte falsificate), RFC 4033–4035 (DNSSEC), RFC 7858, 8484, 9250 (trasporti cifrati).

<!-- detail: 1 -->
## In laboratorio

Esegui `440-dns-poisoning` e apri la vista Sequenza (`s`) e la scheda `dns` del resolver.

- **Ipotesi: un resolver prevedibile accetta una risposta falsificata.** Esegui con i valori predefiniti (`randomize=0`). Metrica: gare vinte su 40 nelle ultime righe del registro (`spoof.success` rispetto a `spoof.rounds`), l'indirizzo che `h1` ottiene per bank.example.com a 10 s, e la voce in cache nel pannello del resolver con il suo TTL. Atteso: ogni gara vinta, e h1 riceve l'indirizzo falsificato, in cache per circa un'ora — ogni client successivo ne è colpito senza altre azioni.
- **Ipotesi: la probabilità di un falsario alla cieca è n / 2^entropy.** Esegui `./hoplab run 440-dns-poisoning randomize=1` (30 bit: nessuna gara vinta, e `dns.mismatched` sul resolver conta le risposte false scartate). Poi rendi visibile la legge: `randomize=1 entropy=12`, `10`, `8`, con `rounds=70`. Metrica: gare vinte su gare tentate, rispetto alla probabilità che il falsario stampa alla fine e alla verifica `hardened` del laboratorio (vittorie entro quattro deviazioni standard della binomiale). Atteso: circa 5%, 18% e 54% con 200 risposte false per gara. Poi `guesses=2000` con `entropy=8`: il conteggio per gara si ferma intorno a 430, perché la risposta vera arriva dopo 80 ms e chiude la gara — ed è per questo che un server autoritativo lento è di per sé una debolezza.
- **Ipotesi: il TTL limita il danno, non il rischio.** In `340-dns-ttl` il resolver mette in cache una catena di record a 1 s e viene interrogato di nuovo a 7 s. Eseguilo con `ttl=5` e con `ttl=300`. Metrica: il contatore `dns.upstream_queries` su `r` (3 contro 2: con il TTL corto il resolver ha dovuto chiedere di nuovo). Atteso: ciò che è in cache, vero o falso, viene servito esattamente per il suo TTL — ed è per questo che un record avvelenato con TTL lungo vale tanto, e che accorciare i TTL non è una difesa.
- **Ipotesi: trasportare la domanda su IPv6 non aiuta né danneggia.** Esegui `./hoplab run 440-dns-poisoning family=6`, poi aggiungi `randomize=1 entropy=8`. Metrica: gare vinte, a confronto con le stesse esecuzioni con `family=4`. Atteso: ogni gara vinta contro il resolver prevedibile (il record falsificato ora è l'AAAA; l'A che `h1` chiede comunque è autentico), e la stessa probabilità per gara contro quello corretto.

Confronta con `240-dns-hierarchy`: segui una risoluzione e annota, per ogni messaggio, che cosa controlla il destinatario prima di crederci.

## Domande per la discussione

- La casualità delle porte e DNSSEC rispondono entrambe al cache poisoning. Quali modelli di minaccia copre l'una che l'altra non copre?
- Perché cifrare il tratto client–resolver non rende superfluo DNSSEC, e viceversa?
- Un resolver sta dietro un NAT che riscrive le porte sorgente in sequenza. Che ne è della sua entropia, e chi può rimediare?
- L'adozione di DNSSEC è lenta da due decenni. Quali costi ricadono su chi, e chi ne trae il beneficio?
- Come estenderesti il laboratorio per misurare, anziché affermare, la probabilità di successo di un falsario fuori percorso in funzione dell'entropia disponibile?
