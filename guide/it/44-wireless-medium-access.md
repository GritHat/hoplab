<!-- level: 4 -->
<!-- labs: 460-hidden-terminal -->
# Accesso al mezzo wireless: terminali nascosti e che cosa fa 802.11

Il carrier sensing poggia su un'ipotesi vera su un cavo e falsa in radio: che il trasmettitore possa sentire tutto ciò che interferirebbe presso il ricevitore. L'interferenza avviene al ricevitore; l'ascolto avviene al mittente; su un canale radio sono due luoghi diversi con visioni diverse. Un **terminale nascosto** è una stazione che il ricevitore sente e il mittente no: il carrier sensing segnala canale libero e i frame collidono. Un **terminale esposto** è l'errore opposto: il mittente sente una trasmissione che non disturberebbe il proprio ricevitore, e tace inutilmente.

Il rimedio classico è un breve scambio di prenotazione, RTS e CTS, con cui è il ricevitore stesso ad annunciare che il canale è impegnato. Costa tempo di trasmissione a ogni frame e non risolve il caso del terminale esposto, perciò 802.11 lo rende facoltativo e la maggior parte delle reti lo lascia disattivato per i frame ordinari. **Quando una prenotazione vale il suo overhead, e quanto del quadro da manuale sopravvive nelle radio reali, dove un frame più forte può essere decodificato attraverso uno più debole e il raggio d'interferenza supera il raggio di comunicazione?**

La domanda è tornata attuale: installazioni dense, canali larghi e trasmissione multiutente in uplink hanno cambiato sia il costo di una collisione sia gli strumenti per evitarla. Il laboratorio confronta tre celle a parità di carico.

<!-- detail: 2 -->
## CSMA/CA secondo lo standard

Una stazione con un frame da inviare attende che il mezzo resti libero per un DIFS, poi decrementa un backoff estratto uniformemente in [0, CW] slot, congelando il contatore ogni volta che il mezzo è occupato, e trasmette quando il contatore arriva a zero. Il ricevitore risponde con un ACK dopo un SIFS, più breve del DIFS affinché nessuno possa inserirsi. Un ACK mancante è interpretato come collisione: CW raddoppia, fino a CWmax, e il frame viene ritentato fino a un limite, oltre il quale è scartato. Le collisioni fra stazioni che si sentono avvengono solo quando due contatori di backoff scadono nello stesso slot; la loro probabilità è quindi governata da CW e dal numero di contendenti (modello di Bianchi), e il loro costo è un intero frame, perché una radio non può ascoltare mentre trasmette.

## Terminali nascosti ed esposti

Con i terminali nascosti il periodo di vulnerabilità non è più uno slot ma il doppio della durata del frame, come in ALOHA puro: il carrier sensing non offre alcuna protezione fra i due gruppi, e il backoff si limita a diluire i tentativi. Tobagi e Kleinrock mostrarono nel 1975 che il throughput di CSMA degrada allora verso quello di ALOHA. I frame lunghi sono i più colpiti.

## RTS/CTS e la portante virtuale

Il mittente trasmette un breve RTS che riporta la durata dello scambio imminente; il ricevitore risponde con un CTS che riporta la stessa informazione. Ogni stazione che sente uno dei due frame imposta il proprio network allocation vector (NAV) e tace per quel tempo: è il carrier sensing virtuale. Le stazioni nascoste sentono il CTS pur avendo perso l'RTS. Una collisione può ora distruggere soltanto un RTS, ed è rilevata dopo poche decine di microsecondi anziché dopo un frame intero.

L'overhead consiste in due frame di controllo e due SIFS per ogni frame di dati, trasmessi a una velocità bassa e robusta: in termini relativi è piccolo per i frame lunghi e grande per quelli corti. Di qui la soglia RTS: lo scambio si usa solo oltre una certa dimensione del frame. L'analisi di Bianchi mostra che, anche senza terminali nascosti, RTS/CTS aumenta il throughput di saturazione quando i frame sono lunghi e i contendenti numerosi, perché accorcia le collisioni.

## Cattura e raggio d'interferenza

Due fatti guastano il modello pulito. **Cattura**: un ricevitore riesce spesso a decodificare il più forte di due frame sovrapposti, quindi non ogni sovrapposizione è una perdita, e le stazioni vicine prevalgono sistematicamente sulle lontane. **Raggio d'interferenza**: un trasmettitore può corrompere la ricezione a distanze dalle quali il suo CTS non è decodificabile, per cui una stazione può trovarsi fuori dalla portata della prenotazione e ancora dentro quella del danno (Xu, Gerla e Bae). RTS/CTS inoltre aggrava il problema del terminale esposto, zittendo stazioni che avrebbero potuto trasmettere.

## Che cosa fa oggi 802.11

RTS/CTS è disattivato per default per i frame ordinari ed è usato in modo selettivo: per proteggere aggregati lunghi (A-MPDU), come sonda economica prima di una trasmissione su canale largo, e per la coesistenza con PHY più vecchi (CTS-to-self). L'adattamento della velocità deve distinguere le perdite da collisione da quelle da canale, altrimenti abbassa la velocità, allunga i frame e rende più probabili le collisioni da terminale nascosto. 802.11ax aggiunge l'uplink schedulato (OFDMA con trigger frame, MU-RTS), che sottrae alla contesa le stazioni servite dall'access point, e il BSS colouring con soglie di ascolto adattive, un attacco esplicito al problema del terminale esposto.

<!-- detail: 3 -->
## Problemi aperti

**Soglie di ascolto.** Un'unica soglia di carrier sense non può essere corretta sia per i terminali nascosti sia per quelli esposti; sceglierla per collegamento, per vicino e per velocità, in modo stabile quando ogni stazione si adatta, è un problema irrisolto, riaperto dal riuso spaziale di 802.11ax.

**Decodificare invece di evitare.** La decodifica ZigZag recupera entrambi i frame da due collisioni della stessa coppia, e la cancellazione successiva dell'interferenza e le radio full-duplex (Choi et al.) cambiano ciò che si intende per collisione. Quanto di tutto ciò sia praticabile alle larghezze di banda moderne e con hardware comune è ancora oggetto di lavoro.

**Diagnosi.** Una stazione non può distinguere direttamente una perdita da terminale nascosto da una perdita per segnale debole. Inferirlo dagli andamenti delle perdite, e decidere quando attivare RTS/CTS, è un'arte antica e ancora imperfetta.

**Accesso schedulato o casuale.** Man mano che gli access point assumono il controllo dell'uplink, il problema residuo si sposta sull'interferenza fra celle sovrapposte, dove nessuno schedula.

## Il modello del laboratorio e la realtà

Il modulo `mac` è un'astrazione a slot, e il suo testo di aiuto lo dichiara: il tempo avanza in tick pari a un ritardo di propagazione, ogni stazione dista un tick da ogni altra, i frame hanno lunghezza fissa, e le durate di slot, DIFS, RTS, CTS e ACK del CSMA/CA sono contate in tick, **non secondo le temporizzazioni di 802.11**. Un RTS e un CTS durano ciascuno un ventesimo di un frame di dati (almeno due tick), per cui il rapporto di overhead è fissato dal modello e non dalla dimensione del frame e dalla velocità del PHY; lo si può variare solo indirettamente tramite `prop`. Il backoff iniziale è uniforme su 32 tick.

I terminali nascosti sono modellati come due gruppi, le stazioni di indice pari e quelle di indice dispari, che sentono perfettamente il proprio gruppo e per nulla l'altro, mentre l'access point sente tutti. Non c'è geometria, né attenuazione, **né cattura** (ogni sovrapposizione distrugge entrambi i frame), né un raggio d'interferenza maggiore di quello di decodifica, e un CTS zittisce immancabilmente tutte le stazioni. I terminali esposti non sono rappresentabili, perché il ricevitore è uno solo. Mancano adattamento della velocità e aggregazione. Il modello mostra dunque il meccanismo nella forma più pura e sovrastima sia il danno dei terminali nascosti sia il rimedio offerto da RTS/CTS. L'esecuzione di default dura 600 tempi di frame, abbastanza pochi perché due semi differiscano di diversi punti: prima di confrontare numeri si usino `frame=10ms` e più semi.

## Letteratura primaria

- F. Tobagi, L. Kleinrock, "Packet Switching in Radio Channels: Part II — The Hidden Terminal Problem in Carrier Sense Multiple-Access and the Busy-Tone Solution", IEEE Transactions on Communications, 1975.
- P. Karn, "MACA — A New Channel Access Method for Packet Radio", ARRL/CRRL Amateur Radio 9th Computer Networking Conference, 1990.
- V. Bharghavan, A. Demers, S. Shenker, L. Zhang, "MACAW: A Media Access Protocol for Wireless LAN's", ACM SIGCOMM, 1994.
- G. Bianchi, "Performance Analysis of the IEEE 802.11 Distributed Coordination Function", IEEE Journal on Selected Areas in Communications, 2000.
- K. Xu, M. Gerla, S. Bae, "How Effective is the IEEE 802.11 RTS/CTS Handshake in Ad Hoc Networks?", IEEE GLOBECOM, 2002.
- M. Heusse, F. Rousseau, G. Berger-Sabbatel, A. Duda, "Performance Anomaly of 802.11b", IEEE INFOCOM, 2003.
- K. Jamieson, B. Hull, A. Miu, H. Balakrishnan, "Understanding the Real-World Performance of Carrier Sense", ACM SIGCOMM Workshop E-WIND, 2005.
- S. Gollakota, D. Katabi, "ZigZag Decoding: Combating Hidden Terminals in Wireless Networks", ACM SIGCOMM, 2008.
- J. I. Choi, M. Jain, K. Srinivasan, P. Levis, S. Katti, "Achieving Single Channel, Full Duplex Wireless Communication", ACM MobiCom, 2010.
- IEEE Std 802.11-2020 e l'emendamento 802.11ax (2021) per OFDMA, trigger frame e BSS colouring.

<!-- detail: 1 -->
## In laboratorio

Eseguire `./hoplab run 460-hidden-terminal`. Tre celle indipendenti eseguono lo stesso CSMA/CA con lo stesso carico: `open` (tutti sentono tutti), `hidden` (due gruppi che non si sentono), `rtscts` (gli stessi gruppi nascosti, con RTS/CTS). Parametri: `load` (nuovi frame per tempo di frame, tutte le stazioni insieme), `stations`, `frame`. Contatori su ogni cella: `mac.throughput_pct`, `mac.sent`, `mac.success`, `mac.collisions`, `mac.dropped`. Nei pannelli mac si confrontino S e la quantità di rosso.

Con i valori di default ci si attende circa il 58% in `open`, il 14% in `hidden` e il 58% in `rtscts`. Per numeri che valga la pena confrontare servono un'esecuzione più lunga in tempi di frame e più semi:

```
./hoplab run 460-hidden-terminal frame=10ms --seed 1 --headless
```

**Esperimento 1 — a basso carico il throughput è la metrica sbagliata.** Ipotesi: i terminali nascosti causano collisioni a qualunque carico, ma fanno perdere throughput solo quando le ritrasmissioni non possono più essere assorbite. Manipolazione: `load=0.05`, `0.2`, `0.6`, `1.0`. Metrica: `mac.throughput_pct` e il rapporto di collisione `mac.collisions / mac.sent`, in `hidden` rispetto a `open`. Esito atteso: a 0.2 tutte e tre le celle smaltiscono circa il 20% (lo verifica il controllo `light` del laboratorio), mentre `hidden` mostra già da uno a due ordini di grandezza di collisioni in più; da 0.6 in su `hidden` satura intorno al 14% e comincia a scartare frame.

**Esperimento 2 — dimensione della popolazione nascosta.** Ipotesi: il danno cresce con il numero di stazioni reciprocamente nascoste, non soltanto con il carico. Manipolazione: `stations=2`, `4`, `10` con `load=0.6`. Metrica: throughput di `hidden` rispetto a `open`. Esito atteso: circa 42%, 33% e 14%, contro un 58% pressoché costante in `open`.

**Esperimento 3 — il prezzo di RTS/CTS quando nessuno è nascosto.** Ipotesi: senza terminali nascosti RTS/CTS costa throughput con pochi contendenti e ne fa guadagnare con molti, e il punto di incrocio dipende dal rapporto fra la lunghezza dei frame di controllo e quella dei frame di dati. Manipolazione: al prompt `:` riavviare due celle in saturazione, `open mac proto=csmaca load=1.5 stations=2 frame=10ms` e `rtscts mac proto=csmaca rts=1 load=1.5 stations=2 frame=10ms`; ripetere con `stations=24` (il massimo); poi aggiungere a entrambe `prop=1ms`, che rende un RTS pari a un quinto di frame. Metrica: `mac.throughput_pct` delle due celle. Esito atteso: con 2 stazioni RTS/CTS perde circa cinque punti, con 24 ne guadagna circa dieci, e con i frame di controllo lunghi perde in entrambi i casi: una soglia, come prevede Bianchi, la cui posizione è però quella del modello e non quella di 802.11.

## Domande per la discussione

- Il laboratorio non modella l'effetto cattura. Si preveda, argomentando, come cambia il risultato di `hidden` se il più forte di due frame sovrapposti sopravvive, e chi ne trae vantaggio.
- RTS/CTS prenota il canale attorno al ricevitore. Si disegni una topologia in cui zittisce una stazione che avrebbe potuto trasmettere senza danno, e si stimi il throughput perduto.
- Un algoritmo di adattamento della velocità osserva il 30% di frame persi. Quali osservazioni gli permetterebbero di decidere fra "terminale nascosto" e "segnale debole", e perché la decisione sbagliata peggiora le cose?
- L'esperimento 3 trova un punto di incrocio nel numero di stazioni. Quali parametri di 802.11 andrebbero introdotti nel modello per localizzare la vera soglia RTS?
- L'accesso schedulato in uplink di 802.11ax elimina il problema del terminale nascosto o lo sposta? Dove?
