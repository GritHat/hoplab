<!-- level: 4 -->
<!-- labs: 420-tcp-fairness, 481-raft-split-votes -->
# Fare ricerca con netlab

I laboratori di questo percorso sono dimostrazioni: una topologia, un'esecuzione, un effetto che si vede. Un esperimento è un'altra cosa. Parte da un'**ipotesi** che potrebbe rivelarsi falsa, varia **un solo fattore** tenendo fisso il resto, riporta una **metrica** con la sua variabilità, e dichiara in quali condizioni la conclusione non varrebbe. Questo argomento riguarda come trasformare l'emulatore dal primo tipo di strumento nel secondo, e come riconoscere dove smette di essere affidabile.

netlab ha tre proprietà che ne fanno una ragionevole piccola piattaforma sperimentale. Un'esecuzione è una funzione deterministica del file di scenario, dei parametri e del seme: i risultati si riproducono esattamente e si condividono in tre righe di testo. Funziona in modalità headless, molto più in fretta del tempo reale: una scansione di parametri è un ciclo di shell. E ogni protocollo è un unico file C leggibile che si registra da sé: un nuovo meccanismo si può aggiungere e confrontare ad armi pari con quelli esistenti.

Ha anche proprietà che limitano ciò che si può concludere: al più 64 nodi, protocolli semplificati, orologi perfetti, tempo di elaborazione nullo. **Un risultato ottenuto qui è un'affermazione su un modello.** Diventa un'affermazione sulle reti solo attraverso un argomento che mostri che le semplificazioni non contano per la domanda posta, e costruire quell'argomento fa parte del lavoro.

<!-- detail: 2 -->
## Parametri, semi e riproducibilità

Uno scenario dichiara i parametri con `param NOME DEFAULT descrizione` e li usa come `$NOME`; la riga di comando li ridefinisce con `NOME=valore`. I numeri casuali provengono da un unico generatore inizializzato da `--seed N`, oppure da una riga `seed N` nel file, oppure dall'orologio; il seme in uso è stampato nella prima riga dell'uscita headless. Per ogni numero riportato si registrino il file di scenario (o il suo hash), i valori dei parametri, il seme e la versione del programma. `netlab check` esegue gli scenari con seme 1.

Il seme conta solo dove qualcosa estrae numeri casuali: perdite, jitter, timeout casuali, backoff, scelta dei pari nel gossip. Uno scenario privo di tali elementi produce lo stesso risultato per ogni seme, e ripeterlo non dice nulla sulla variabilità. In quel caso si introduca la variazione che il sistema reale avrebbe: istanti di partenza, un piccolo ritardo su un collegamento, una diversa popolazione di flussi.

## Esecuzioni headless e metriche

```
./netlab run NOME k=v ... --headless --detail 1 --seed 7 --lang en
```

stampa il log degli eventi e poi una riga per ogni `expect`, con il valore trovato: `ok    h1 tcp.goodput_kbps >= 0   (got 1862.27)`. Lo stato di uscita è 1 se un'aspettativa fallisce, e `--quiet` stampa solo il verdetto. `--until TEMPO` sostituisce la durata dello scenario. I messaggi del log sono tradotti (in italiano `got` diventa `ottenuto`): quando uno script li analizza conviene fissare la lingua.

I contatori sono l'interfaccia delle metriche. Ogni modulo mantiene sul proprio nodo contatori con un nome (`tcp.retransmits`, `raft.max_term`, `swim.false_positives`, ...); la scheda Node li elenca a dettaglio 2. Per leggere un contatore da uno script si aggiunga un'aspettativa sempre vera, `expect h1 tcp.retransmits >= 0`, alla **propria copia** dello scenario, e si estragga il valore `got`. Un file può contenere 32 aspettative e un nodo 48 contatori. Le serie temporali (cwnd, RTT) sono disegnate nell'interfaccia ma non vengono esportate; per una grandezza nel tempo si analizzi il log a dettaglio 2 o si aggiunga un contatore.

Uno scenario viene cercato come percorso prima che nel catalogo, quindi le copie possono stare ovunque:

```
cp scenarios/420-tcp-fairness.nl fair.nl
./netlab run ./fair.nl cc1=bbr queue=10 --headless
```

Il linguaggio completo (topologia, parametri dei collegamenti, `at`, `run`, `expect`, discipline di coda) è descritto in `docs/scenario-format.md`.

## Aggiungere un protocollo

Un modulo è un file C in `src/` che compila una struttura `nl_module` (nome, sintassi, `run`, `stop`, `crash`, `recover`, `panel`) e la registra con `NL_MODULE`. Reagisce a callback: timer (`nl_after`), gestori UDP, TCP, IP ed Ethernet, hook sui pacchetti; riferisce tramite `nl_log`, `nl_count` e `nl_sample`. Due regole mantengono validi gli esperimenti: il tempo si prende solo da `nl_now()` e la casualità solo da `nl_rand*()`, mai dal sistema operativo. Discipline di coda e codici di linea si registrano allo stesso modo. `tools/build-mod.sh NOME src/file.c` costruisce un eseguibile headless privato per lo sviluppo. Si veda `docs/writing-modules.md`; `src/apps.c` e `src/rip.c` sono gli esempi reali più brevi. Il controllo di congestione vive dentro `src/tcp.c`: un nuovo controllore è una modifica a quel file, oppure un trasporto proprio sopra UDP.

## Esecuzioni distribuite

`netlab plan SCENARIO` stampa i comandi per eseguire un laboratorio come più processi, `netlab node SCENARIO a,b` esegue alcuni nodi, `netlab monitor` ne raccoglie i rapporti; le righe `place` e `monitor` dello scenario dicono dove gira ogni cosa. I collegamenti fra processi diventano tunnel UDP. Si esercita così lo stesso codice di protocollo su macchine reali, cosa preziosa come dimostrazione e come prova di robustezza. Non è una modalità di misura: ogni processo segue il proprio orologio reale, la rete reale aggiunge latenza, `--seed` non rende più identiche le esecuzioni, e nessun processo fornisce un verdetto globale (si veda `docs/distributed.md`).

<!-- detail: 3 -->
## Limiti dell'emulatore

Da `src/nl.h` e dalla documentazione degli scenari: 64 nodi, 160 collegamenti, 16 interfacce e 12 moduli per nodo, 256 rotte per nodo, frame di al più 1700 byte, 48 contatori e 8 serie temporali per nodo (600 campioni ciascuna). Molti moduli hanno limiti propri più stretti (40 flussi client in `reqgen`, 24 stazioni per cella `mac`, 8 repliche CRDT, uno spazio di identificatori Chord a 8 bit per default). A questa scala non si può misurare nulla che riguardi comportamento di scala, code pesanti o eventi rari.

Il tempo è virtuale, in microsecondi, e avanza solo attraverso gli eventi. I nodi calcolano in tempo nullo: non c'è CPU, né ritardo di schedulazione, né disco. Tutto ciò il cui comportamento reale è governato dal costo di elaborazione (latenza di fsync nel consenso, membri lenti nel rilevamento dei guasti, elaborazione a lotti nel TCP) è assente. Gli orologi sono perfetti, a meno che un modulo aggiunga uno scostamento.

I protocolli sono semplificati, e il testo di aiuto di ciascun modulo dice come. Le semplificazioni che più incidono sulle conclusioni di questo percorso: TCP senza SACK, ACK ritardati o timestamp, e un BBR ridotto; BGP su UDP, solo eBGP, un router per AS; DNS senza ripiego su TCP, EDNS o DNSSEC; un CSMA/CA a tick senza cattura; Raft senza snapshot, cambio di configurazione o letture linearizzabili.

## Minacce alla validità

- **Validità di costrutto.** Il contatore misura il concetto? Il goodput sull'intero trasferimento non è una quota di un collo di bottiglia; il conteggio dei falsi positivi fatto da un oracolo non è ciò che vede un operatore.
- **Validità interna.** Le simulazioni deterministiche si sincronizzano: le code drop-tail con arrivi periodici mostrano effetti di fase, e un millisecondo di ritardo in più può rovesciare un esito. Una differenza fra due configurazioni è una prova solo se sopravvive a piccole perturbazioni di parametri irrilevanti.
- **Validità statistica.** Si riporti una distribuzione sui semi, non un'esecuzione; si scelga il numero di esecuzioni prima di guardare i risultati; quando il seme non ha effetto lo si dica, invece di riportare un intervallo di ampiezza nulla.
- **Validità esterna.** Topologie piccole, un solo collo di bottiglia, traffico sintetico. L'argomento di Floyd e Paxson resta valido: cercare invarianti ed esplorare lo spazio dei parametri, anziché fidarsi di un unico scenario "tipico".
- **Validità dell'implementazione.** Il nuovo modulo può essere sbagliato. Lo si provi su un caso dalla risposta nota prima di confrontarlo con qualunque cosa.

Ne discende la buona pratica: scrivere ipotesi e analisi prima di eseguire; tenere scenario, griglia dei parametri e script sotto controllo di versione; includere una linea di base che non dovrebbe mostrare alcun effetto; verificare un numero chiave con un altro strumento (Mininet, ns-3 o un testbed reale) prima di crederci.

## Letteratura primaria

- R. Jain, "The Art of Computer Systems Performance Analysis", Wiley, 1991.
- S. Floyd, V. Jacobson, "On Traffic Phase Effects in Packet-Switched Gateways", Internetworking: Research and Experience, 1992.
- L. Rizzo, "Dummynet: A Simple Approach to the Evaluation of Network Protocols", ACM SIGCOMM Computer Communication Review, 1997.
- S. Floyd, V. Paxson, "Difficulties in Simulating the Internet", IEEE/ACM Transactions on Networking, 2001.
- K. Pawlikowski, H.-D. J. Jeong, J.-S. R. Lee, "On Credibility of Simulation Studies of Telecommunication Networks", IEEE Communications Magazine, 2002.
- A. Vahdat et al., "Scalability and Accuracy in a Large-Scale Network Emulator", USENIX OSDI, 2002.
- S. Kurkowski, T. Camp, M. Colagrosso, "MANET Simulation Studies: The Incredibles", ACM SIGMOBILE Mobile Computing and Communications Review, 2005.
- B. Lantz, B. Heller, N. McKeown, "A Network in a Laptop: Rapid Prototyping for Software-Defined Networks", ACM HotNets, 2010.
- N. Handigol, B. Heller, V. Jeyakumar, B. Lantz, N. McKeown, "Reproducible Network Experiments Using Container-Based Emulation", ACM CoNEXT, 2012.
- F. Yan et al., "Pantheon: The Training Ground for Internet Congestion-Control Research", USENIX Annual Technical Conference, 2018.

<!-- detail: 1 -->
## In laboratorio

I tre esercizi che seguono riguardano il metodo; usano laboratori già noti.

**Esperimento 1 — che cosa fa il seme.** Ipotesi: semi uguali danno esecuzioni identiche, e semi diversi cambiano il risultato solo dove lo scenario estrae numeri casuali. Manipolazione: eseguire due volte `./netlab run 481-raft-split-votes --seed 7 --headless --detail 2` e confrontare le uscite con `diff`; poi eseguirlo con i semi da 1 a 10; poi fare lo stesso con `420-tcp-fairness cc1=reno cc2=reno`. Metrica: righe differenti; dispersione del tempo senza leader e del goodput. Esito atteso: uscite identiche a parità di seme; una vera distribuzione per Raft; nessuna variazione per i due flussi Reno. Si decida che cosa variare nel secondo caso per ottenere una barra d'errore onesta.

**Esperimento 2 — da un laboratorio a una misura.** Ipotesi: il laboratorio sull'equità, così com'è, confonde la ripartizione con un vantaggio di partenza. Manipolazione: copiare lo scenario in `fair.nl`; far partire entrambi i flussi a 1 s con `bytes=6M`; sostituire le righe `expect` con `expect h1 tcp.goodput_kbps >= 0`, lo stesso per h2, e i due `tcp.retransmits`; poi eseguire la scansione:

```
for q in 10 20 40 80 160; do for s in 1 2 3 4 5; do
  ./netlab run ./fair.nl cc1=bbr cc2=cubic queue=$q --seed $s --headless --lang en \
    | grep got | tr -d '()' | awk -v q=$q -v s=$s '{print q, s, $2, $3, $NF}'
done; done > fair.dat
```

Metrica: indice di Jain e rapporto fra le ritrasmissioni per ogni dimensione della coda, con la dispersione sui semi. Esito atteso: BBR in vantaggio, con un pesante costo in ritrasmissioni, a `queue=10`, ma oltre quel punto nessun andamento monotono netto: nelle nostre esecuzioni il vincitore è cambiato più di una volta fra 10 e 200 pacchetti. È un risultato sulla fragilità delle affermazioni basate su una sola configurazione, non un fallimento dell'esercizio. Si noti inoltre che il goodput del flusso più lento resta gonfiato dal tempo che esso trascorre da solo dopo la fine dell'altro, e si decida come eliminare anche questo.

**Esperimento 3 — un controllo di sensibilità.** Ipotesi: una conclusione tratta da una sola configurazione di una simulazione deterministica può essere un artefatto di fase. Manipolazione: con `fair.nl` e `cc1=reno cc2=reno`, far variare `delay2` da 2 ms a 12 ms a passi di 1 ms. Metrica: rapporto dei goodput in funzione di `delay2`. Esito atteso: il rapporto salta fra valori adiacenti, e cambia perfino di segno, invece di variare con regolarità: si stanno osservando effetti di fase, non l'RTT, e occorre mediare su perturbazioni (scostamenti di partenza, oppure una disciplina come `qdisc=red` che rende casuali gli scarti) prima di affermare qualunque cosa sull'RTT.

## Dieci idee di progetto

- **Recupero delle perdite.** Aggiungere SACK (RFC 2018, RFC 6675) o RACK-TLP (RFC 8985) a `src/tcp.c`. Misurare timeout, ritrasmissioni e goodput con più perdite per finestra, e ripetere gli esperimenti sull'equità: quali conclusioni dell'argomento 40 cambiano?
- **Un trasporto di tipo QUIC su UDP** (RFC 9000, RFC 9002): numeri di pacchetto monotoni, intervalli di ACK, stream indipendenti. Misurare il blocco in testa alla coda rispetto a TCP per due stream in presenza di perdite, e la latenza dell'handshake.
- **Un controllore basato sul ritardo** (Vegas, Copa). Misurare la coda stazionaria e l'aumento dell'RTT in isolamento, poi la quota ottenuta contro Reno e CUBIC in funzione della dimensione del buffer.
- **L4S in miniatura.** Un mittente in stile DCTCP e una disciplina con marcatura a soglia o a doppia coda. Misurare il ritardo di accodamento e il rapporto dei tassi fra flussi scalabili e classici.
- **ECMP o segment routing** (RFC 2992, RFC 8402) nel percorso di inoltro. Misurare la ripartizione del carico fra i percorsi, il riordino dei pacchetti visto da TCP e il tempo di ripristino dopo il guasto di un collegamento.
- **Estensioni di validazione del routing.** Aggiungere a `src/bgp.c` controlli sull'autorizzazione dei provider nello spirito di ASPA, oppure i ruoli della RFC 9234. Misurare la frazione di rotte diffuse impropriamente o non plausibili che viene rifiutata, in funzione di quali AS adottano il controllo.
- **Multi-Paxos o Viewstamped Replication** accanto a Raft, con gli stessi comandi del client. Misurare, su scenari identici, il tempo senza leader dopo un crash, i messaggi per comando confermato e il lavoro di recupero di una replica rimasta indietro.
- **Letture linearizzabili per Raft**: ReadIndex e lease, con un parametro di scostamento dell'orologio. Misurare le letture obsolete e la latenza di lettura, e trovare l'errore di orologio oltre il quale i lease cedono.
- **Una DHT Kademlia.** Confrontare con Chord, a parità di churn programmato, i salti e i fallimenti delle ricerche, e il traffico di manutenzione dei due.
- **Delta-state CRDT e Lifeguard.** Trasmettere delta anziché lo stato completo e misurare byte per aggiornamento e tempo di convergenza; aggiungere un ritardo di elaborazione ai membri e misurare i falsi positivi di SWIM con e senza consapevolezza della propria salute.

Per ogni progetto si enunci prima l'ipotesi, si convalidi il modulo su un caso dalla risposta nota, e si riporti che cosa i limiti dell'emulatore impediscono di concludere.

## Domande per la discussione

- Si scelga un risultato di questo percorso e si elenchino le semplificazioni del modulo interessato. Per ciascuna si argomenti se potrebbe rovesciare il risultato, indebolirlo, o se è irrilevante.
- Quando il seme non ha effetto, qual è la giusta fonte di variazione, e come si evita di scegliere quella che conferma la propria ipotesi?
- Le esecuzioni distribuite non sono riproducibili. Esiste una domanda di ricerca per cui quella modalità è comunque lo strumento giusto?
- Qual è la più piccola modifica all'emulatore che gli consentirebbe di esportare le serie temporali, e quali esperimenti di questo percorso ne trarrebbero più beneficio?
- Emulazione, simulazione, testbed, misura in produzione: per uno dei dieci progetti si dica che cosa aggiungerebbe ciascuna e in quale ordine le si userebbe.
