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

Sul modello del laboratorio. `dnsspoof` è un falsario interamente simulato: gli viene detto l'indirizzo del resolver e, con `randomize=0`, si affida alla porta fissa e agli identificatori prevedibili; non realizza nessuno dei raffinamenti descritti sopra. Con `randomize=1` il falsario dello script continua a mirare alla porta fissa, quindi fallisce con certezza anziché con una piccola probabilità: il laboratorio mostra che la difesa funziona, non quanto è robusta. Stimare la probabilità residua è un esercizio da fare con carta e penna, o con un modulo scritto da te. Qui i collegamenti non riordinano né frammentano, il resolver non valida firme e non c'è un laboratorio sul bailiwick: le conclusioni sui resolver reali devono venire dalla letteratura, non da questo emulatore.

Fonti primarie: RFC 1034/1035 (il protocollo), RFC 3833 (analisi delle minacce al DNS), RFC 5452 (misure contro le risposte falsificate), RFC 4033–4035 (DNSSEC), RFC 7858, 8484, 9250 (trasporti cifrati).

<!-- detail: 1 -->
## In laboratorio

Esegui `440-dns-poisoning` e apri la vista Sequenza (`s`) e la scheda `dns` del resolver.

- **Ipotesi: un resolver prevedibile accetta una risposta falsificata.** Esegui con i valori predefiniti (`randomize=0`). Metrica: l'indirizzo che `h1` ottiene per bank.example.com a 4 s, e la voce in cache nel pannello del resolver con il suo TTL. Atteso: l'indirizzo falsificato, in cache per circa un'ora — ogni client successivo ne è colpito senza altre azioni.
- **Ipotesi: l'imprevedibilità elimina il falsario fuori percorso.** Esegui `./netlab run 440-dns-poisoning randomize=1`. Metrica: la stessa risposta, e il contatore `dns.mismatched` sul resolver. Atteso: l'indirizzo autentico; le risposte falsificate sono contate come non corrispondenti e scartate. Leggi i limiti qui sopra prima di generalizzare.
- **Ipotesi: il TTL limita il danno, non il rischio.** In `340-dns-ttl` cambia `ttl` e osserva per quanto tempo una risposta in cache continua a essere servita dopo che il dato autoritativo è cambiato. Metrica: tempo prima che i client vedano il nuovo indirizzo. Atteso: proporzionale al TTL — ed è per questo che un record avvelenato con TTL lungo vale tanto, e che accorciare i TTL non è una difesa.

Confronta con `240-dns-hierarchy`: segui una risoluzione e annota, per ogni messaggio, che cosa controlla il destinatario prima di crederci.

## Domande per la discussione

- La casualità delle porte e DNSSEC rispondono entrambe al cache poisoning. Quali modelli di minaccia copre l'una che l'altra non copre?
- Perché cifrare il tratto client–resolver non rende superfluo DNSSEC, e viceversa?
- Un resolver sta dietro un NAT che riscrive le porte sorgente in sequenza. Che ne è della sua entropia, e chi può rimediare?
- L'adozione di DNSSEC è lenta da due decenni. Quali costi ricadono su chi, e chi ne trae il beneficio?
- Come estenderesti il laboratorio per misurare, anziché affermare, la probabilità di successo di un falsario fuori percorso in funzione dell'entropia disponibile?
