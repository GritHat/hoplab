<!-- level: 2 -->
<!-- labs: 290-lamport-clocks -->
# Il tempo logico

Su un solo calcolatore la domanda «che cosa è successo prima?» ha una risposta ovvia: si legge l'orologio. In un sistema distribuito un orologio così non esiste. Ogni macchina ha il proprio oscillatore, che deriva; i protocolli di sincronizzazione riducono l'errore ma non lo azzerano mai; e un messaggio impiega un tempo ignoto e variabile ad arrivare. Se due macchine marcano gli eventi con i loro orologi locali, un messaggio può risultare ricevuto prima di essere stato inviato.

L'osservazione di Leslie Lamport (1978) è che ai programmi raramente serve il tempo reale. Serve un ordine degli eventi che non contraddica mai la **causalità**. Lamport definì la relazione **«accaduto prima»** (happened-before): l'evento a è accaduto prima dell'evento b se a precede b sullo stesso processo, oppure se a è l'invio di un messaggio e b la sua ricezione, oppure attraverso una catena di passi di questo tipo. Se né a è accaduto prima di b né b prima di a, gli eventi sono **concorrenti**: nessuna informazione poteva passare dall'uno all'altro, e nessun osservatore interno al sistema può dire quale sia venuto prima.

Un **orologio di Lamport** è un contatore per processo che rispetta questa relazione: conta i tuoi eventi, allega il contatore a ogni messaggio e, quando ricevi un messaggio, salta oltre la marca temporale che esso porta. Allora, ogni volta che a è accaduto prima di b, la marca di a è minore di quella di b. Con questo semplice strumento tutti i processi possono accordarsi su un unico ordine di tutti gli eventi.

<!-- detail: 2 -->
## Perché gli orologi fisici non bastano

Un oscillatore al quarzo deriva di 10-100 parti per milione: da uno a nove secondi al giorno. NTP mantiene i calcolatori entro qualche millisecondo attraverso Internet, un decimo su una LAN; ma eventi su macchine diverse possono distare microsecondi, e un orologio corretto da NTP può persino fare un salto all'indietro. Un confronto di marche temporali fra macchine ha dunque senso solo oltre l'errore di sincronizzazione, e il software non può conoscere esattamente quell'errore.

## Accaduto prima

Scriviamo a → b per «a è accaduto prima di b». La definizione:

- Se a e b sono eventi dello stesso processo e a avviene per primo, allora a → b.
- Se a è l'invio di un messaggio e b la ricezione di quel messaggio, allora a → b.
- Se a → b e b → c, allora a → c.

È un ordine *parziale*: alcune coppie non sono affatto ordinate. Quelle coppie sono concorrenti, e si scrive a ∥ b. «a → b» significa precisamente che a *può aver influenzato* b.

## Le regole di Lamport

Ogni processo P mantiene un intero L, inizialmente 0.

- Prima di ogni evento locale o invio: L = L + 1. Un messaggio porta la L del suo evento di invio.
- Alla ricezione di un messaggio con marca T: L = max(L, T) + 1.

Ne segue la **condizione dell'orologio**: se a → b allora L(a) < L(b). Su un processo il contatore può solo crescere; attraverso un messaggio il valore del ricevente vale almeno T + 1.

## La traccia del laboratorio

Il laboratorio 290 ha tre processi; il collegamento di n3 è lento.

```
tempo   processo  evento                             orologio
1,0 s   n1        locale                             1
1,1 s   n1        locale                             2
1,2 s   n1        locale                             3
2,0 s   n1        invio di a verso n2                4
2,05 s  n2        ricezione di a (T=4): max(0,4)+1   5
3,0 s   n2        locale                             6
4,0 s   n2        invio di b verso n3                7
4,27 s  n3        ricezione di b (T=7): max(0,7)+1   8
5,0 s   n3        invio di c verso n1                9
5,26 s  n1        ricezione di c (T=9): max(4,9)+1   10
6,0 s   n1        locale                             11
6,0 s   n3        locale                             10
```

n3 non aveva fatto nulla, eppure il suo primo evento ha orologio 8: il messaggio ha portato con sé il passato di n1 e n2. Lungo la catena causale a, b, c le marche non fanno che crescere: 4, 5, 7, 8, 9, 10.

## Che cosa i numeri non dicono

L'inverso della condizione dell'orologio è falso. Gli ultimi due eventi sono concorrenti: n1 e n3 agiscono a 6,0 s senza essersi più sentiti dopo c. I loro orologi valgono 11 e 10, e 10 < 11 non significa che l'evento di n3 abbia causato, e nemmeno preceduto, quello di n1. Allo stesso modo l'evento locale di n3 e la ricezione di c da parte di n1 hanno entrambi orologio 10 e non sono in relazione.

## Un ordine totale

Molti algoritmi hanno bisogno che tutti i processi concordino su un'unica sequenza. Si ordinano gli eventi per (L, identificatore del processo): si confrontano le marche e si risolvono le parità con l'identificatore. L'ordine è totale e coerente con «accaduto prima», anche se fra eventi concorrenti è arbitrario. L'algoritmo di mutua esclusione di Lamport lo usa: le richieste di una risorsa vengono marcate e inviate a tutti, ogni processo le tiene in una coda ordinata per (L, id), e un processo entra quando la propria richiesta è in testa e ha sentito da tutti qualcosa di successivo. Servono 3(N − 1) messaggi per ogni ingresso.

<!-- detail: 3 -->
## In profondità

**Orologi vettoriali.** Per *rilevare* la concorrenza un processo deve ricordare ciò che sa di ciascuno degli altri. Un orologio vettoriale (Fidge e Mattern, indipendentemente, 1988) è un vettore V con una componente per processo: P_i incrementa V[i] a ogni evento, invia l'intero vettore e, alla ricezione, prende il massimo componente per componente prima di incrementare la propria. Allora a → b se e solo se V(a) ≤ V(b) in ogni componente e i due differiscono; se nessuno dei due domina, gli eventi sono concorrenti. Nella modalità vettoriale del laboratorio gli ultimi due eventi sono [6,3,2] e [4,3,3]: n1 è avanti nella prima componente, n3 nella terza. Il costo è una marca di dimensione N, e Charron-Bost (1991) dimostrò che nessuna struttura più piccola può caratterizzare la causalità in generale. I version vector degli archivi replicati (Dynamo, Riak) usano la stessa idea per rilevare aggiornamenti in conflitto.

**A che cosa bastano gli orologi di Lamport.** Un ordine totale coerente con la causalità è sufficiente per le macchine a stati replicate: se tutte le repliche applicano gli stessi comandi nello stesso ordine restano identiche. È questo il vero argomento dell'articolo di Lamport, e il seme dei protocolli di consenso. L'algoritmo di mutua esclusione, però, presuppone che nessun processo si guasti e nessun messaggio vada perso: un solo processo caduto blocca tutti, perché il suo riscontro non arriva mai. La tolleranza ai guasti richiede il consenso (Paxos, Raft), e il risultato FLP (Fischer, Lynch e Paterson, 1985) mostra che in un sistema completamente asincrono non si può garantire che il consenso termini nemmeno con un solo guasto.

**Limiti del tempo logico.** Gli orologi logici vedono solo gli eventi interni al sistema. Se un utente riceve una conferma da un server e poi telefona a qualcuno che scrive su un altro, le due scritture sono causalmente legate nel mondo reale ma concorrenti per il sistema. Per catturare questa causalità esterna i sistemi limitano l'errore degli orologi fisici: TrueTime di Google Spanner espone il tempo come un intervallo ottenuto da GPS e orologi atomici e attende che l'incertezza sia trascorsa prima di confermare; gli orologi logici ibridi (Kulkarni e altri, 2014) combinano una marca fisica con un contatore di Lamport, restando vicini al tempo reale e conservando la condizione dell'orologio. Il «vince l'ultima scrittura» basato sull'orologio di sistema, come in Cassandra, perde aggiornamenti in silenzio quando gli orologi non concordano.

**Strumenti affini.** Le istantanee globali coerenti (Chandy e Lamport, 1985) si fondano sulla stessa nozione di taglio causale. Sincronizzazione fisica: NTP (RFC 5905), PTP (IEEE 1588) con precisione inferiore al microsecondo sulle LAN.

**Riferimento.** Lamport, «Time, Clocks, and the Ordering of Events in a Distributed System», Communications of the ACM, 1978.

**Che cosa fa hoplab.** Gli eventi sono previsti dallo script con `clock event` e `clock send`; i processi non si guastano; l'identificatore del processo è la posizione nella lista `peers`.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 290: `./hoplab run 290-lamport-clocks`**

- Tieni il registro (`l`) a dettaglio 2: ogni riga marcata ● è un evento con l'orologio che ne risulta. Segui la tabella qui sopra e ricalcola ogni ricezione con max(L, T) + 1.
- Apri la vista Sequenza (`s`): i tre messaggi a, b, c formano una catena causale, e il collegamento lento di n3 si riconosce dalle frecce inclinate. Seleziona un nodo (`n`) e apri la sua scheda `clock`.
- Apri uno dei messaggi nella vista Pacchetti (`p`) e trova la marca temporale che trasporta.
- A 6 s n1 e n3 hanno ciascuno un evento locale. Stabilisci dalla definizione se sono concorrenti, poi confronta i loro valori di Lamport.
- Prevedi, poi prova: `./hoplab run 290-lamport-clocks mode=vector`. Scrivi il vettore di ciascuno dei dodici eventi prima di guardare. Quali coppie di eventi sono concorrenti? Confronta tu i due eventi a 6 s, poi guarda il registro: dopo ogni evento hoplab ne confronta il vettore con l'ultimo evento di ogni altro processo e segna con ∥ quelli concorrenti (è il punto di vista di un osservatore esterno, che nessun processo ha).
- Prevedi, poi prova: `./hoplab run 290-lamport-clocks slow=500ms`. Il messaggio b ora raggiunge n3 *dopo* che n3 ha inviato c. Ricalcola l'intera tabella: quanto vale l'orologio dell'invio di c, dell'evento di n1 a 6 s e della ricezione di c da parte di n1? La causalità è ancora rispettata? (Con questa impostazione i valori finali degli orologi cambiano; le verifiche del laboratorio sono limiti inferiori che valgono ancora: controllale sulla tua tabella.)
- Dalla riga di comando aggiungi eventi tuoi, per esempio `:n3 clock send n2 text=d` e `:n2 clock event`, prevedendo ogni volta il valore.

## Verifica

- Definisci «accaduto prima» e la concorrenza. Fai un esempio con due processi e un messaggio che contenga due eventi concorrenti e due ordinati.
- Tre processi partono da 0. P1: evento locale, invio di m1 a P2, evento locale. P2: ricezione di m1, invio di m2 a P3. P3: evento locale, ricezione di m2, evento locale. Assegna le marche di Lamport a tutti gli eventi.
- L(a) < L(b). Che cosa puoi concludere su a e b, e che cosa no? Che cosa implica L(a) = L(b)?
- Calcola gli orologi vettoriali per l'esercizio precedente e trova una coppia di eventi concorrenti con marche di Lamport diverse.
- Come si ottiene un ordine totale dagli orologi di Lamport, e perché da solo non basta per una mutua esclusione tollerante ai guasti?
