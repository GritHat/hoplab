<!-- level: 4 -->
<!-- labs: 450-consistent-hashing -->
# Hashing per la scalabilità

I grandi servizi distribuiscono il lavoro tramite hashing: la quintupla di un pacchetto sceglie un server, una chiave sceglie un nodo di cache o una partizione dell'archivio. L'hashing non richiede stato condiviso, quindi un numero qualsiasi di bilanciatori o di client può prendere la stessa decisione in modo indipendente. La difficoltà è che l'insieme dei server cambia di continuo, per guasti, rilasci e autoscaling, e un hash ingenuo rimescola quasi tutto a ogni cambiamento. **Come si associano le chiavi a un insieme variabile di server in modo che poche chiavi si spostino, il carico resti uniforme e l'associazione sia poco costosa da calcolare e da memorizzare?**

Questi tre obiettivi, minimo disturbo, bilanciamento e costo, sono in tensione fra loro, e nessuno schema prevale su tutti. Il consistent hashing offre un disturbo quasi minimo ma un carico disuguale, a meno di replicare ogni server molte volte sull'anello. Il rendezvous hashing è semplice ed esattamente minimo, ma ogni ricerca costa una scansione di tutti i server. Maglev ottiene ricerche a tempo costante e un carico molto uniforme rinunciando a un poco di stabilità. Il power of two choices bilancia molto meglio di qualunque singolo hash, ma richiede informazioni sul carico e, da solo, non garantisce affinità.

La questione resta aperta perché i requisiti continuano a spostarsi: affinità di connessione fra bilanciatori senza stato, carico limitato in presenza di chiavi con popolarità sbilanciata, server eterogenei o pesati. Il laboratorio misura direttamente il disturbo, contando i flussi del client il cui server cambia.

<!-- detail: 2 -->
## Gli schemi

**Mod-N.** server = h(chiave) mod N. Nessuna memoria e bilanciamento atteso perfetto. Quando N diventa N+1 una chiave resta al suo posto solo se h mod N coincide con h mod (N+1), il che accade con probabilità 1/(N+1): passando da 3 a 4 server si spostano tre quarti delle chiavi.

**Consistent hashing** (Karger et al. 1997). Server e chiavi sono mappati su una circonferenza e ogni chiave appartiene al primo server in senso orario. Aggiungere un server sposta solo le chiavi dell'arco che esso rileva; rimuoverne uno sposta solo le sue. Con un solo punto per server gli archi sono molto disuguali (il maggiore vale circa log N volte la media), perciò ogni server è collocato in v nodi virtuali; la dispersione relativa del carico decresce all'incirca come 1/sqrt(v). La memoria è O(N·v) e una ricerca è una ricerca binaria.

**Rendezvous, o highest-random-weight hashing** (Thaler e Ravishankar 1998). Si calcola h(chiave, server) per ogni server e si sceglie il massimo. Il disturbo è esattamente minimo: quando un server esce si spostano solo le sue chiavi, ciascuna verso la propria seconda scelta; quando ne entra uno, prende esattamente le chiavi per le quali è ora il massimo. Non serve alcuna tabella e i pesi sono facili da introdurre, ma una ricerca costa O(N).

**Maglev** (Eisenbud et al. 2016). Ogni server possiede una permutazione pseudo-casuale delle M posizioni di una tabella di lookup (M primo, molto maggiore di N); i server, a turno, occupano la prossima posizione libera fra quelle preferite finché la tabella è piena. Una ricerca è un solo accesso a un vettore, tabella[h(chiave) mod M]. Ogni server possiede quasi esattamente M/N posizioni, quindi il bilanciamento è migliore di quello di un anello di dimensioni praticabili. Quando il pool cambia la tabella viene ricostruita e una piccola frazione di posizioni, oltre il minimo, cambia proprietario; tale frazione si riduce al crescere di M, ed è per questo che in produzione si usa M = 65537 o più.

**Power of two choices** (Azar et al.; Mitzenmacher). Si campionano due server a caso e si invia la richiesta al meno carico. Con n oggetti su n server il carico massimo scende da circa log n / log log n a circa log log n: un miglioramento esponenziale al prezzo di un sondaggio in più. È una politica per il nuovo lavoro, non un'associazione: la decisione dipende dal carico corrente, per cui due bilanciatori non concorderanno, ed è necessaria una tabella dei flussi esistenti.

## Affinità di connessione

Un bilanciatore di livello 4 deve inviare ogni pacchetto di una connessione TCP allo stesso server. Una tabella per flusso lo garantisce in modo banale, ma la tabella è locale: con ECMP davanti a più bilanciatori, un pacchetto può raggiungere un bilanciatore che non ha mai visto quel flusso, e un bilanciatore riavviato ha dimenticato tutto. La risposta adottata in produzione è la combinazione delle due cose: un'associazione consistente, affinché bilanciatori indipendenti di norma concordino, più una tabella dei flussi per proteggere le connessioni già stabilite dal rimescolamento residuo quando il pool cambia. I progetti senza stato vanno oltre e lasciano che siano i server stessi a reindirizzare i pacchetti fuori posto dei flussi vecchi (Beamer, Faild).

<!-- detail: 3 -->
## Problemi aperti

**Carico limitato con spostamenti minimi.** Con chiavi di popolarità sbilanciata, una ripartizione perfettamente uniforme dello spazio delle chiavi non è una ripartizione uniforme del carico. Il consistent hashing con carichi limitati (Mirrokni, Thorup e Zadimoghaddam) vincola ogni server a (1+ε) volte la media e limita il numero di spostamenti per aggiornamento, al prezzo di catene di inoltro; come combinarlo con l'affinità per connessione e con i pesi resta ingegneria caso per caso.

**Affinità senza stato.** Le tabelle dei flussi costano memoria e sono un bersaglio per attacchi di negazione del servizio (un SYN flood le riempie). Fin dove ci si possa spingere senza stato per flusso, con ricambio continuo del pool e connessioni di lunga durata, resta una questione attiva; la migrazione di connessione di QUIC aggiunge una complicazione, perché la quintupla non identifica più la connessione.

**Che cosa misurare.** Il disturbo si può contare sulle chiavi, sui flussi vivi al momento del cambiamento o sui byte; con dimensioni dei flussi a coda pesante si ottengono graduatorie diverse.

## Il modello del laboratorio e la realtà

Il bilanciatore di questo emulatore (si veda `./hoplab help lb`) calcola l'hash di una quintupla UDP, e il client `reqgen` cicla su **40 flussi** al massimo, che vivono per tutta l'esecuzione. È un campione minuscolo: con quattro server ciascuno regge una decina di flussi, per cui una differenza di tre o quattro flussi fra schemi rientra nel rumore di campionamento, e i valori di "bilanciamento" sono dominati da dove capitano quaranta particolari porte. Peggio, l'hash è deterministico e i flussi sono fissi, quindi il seme non modifica il risultato: si dispone di un campione, non di una distribuzione. Per ottenerne altri occorre cambiare la popolazione dei flussi (parametro `flows`) o scrivere uno scenario con altri indirizzi.

L'anello usa 100 nodi virtuali per server. La tabella Maglev ha **251 posizioni**, contro le 65537 dell'articolo, per cui sia il bilanciamento sia il disturbo sono più grossolani dei valori pubblicati. Rendezvous hashing, jump hashing e carichi limitati non sono implementati. Con `affinity=0`, round robin, least connections e `p2c` rischedulano ogni pacchetto e il conteggio "moved" riportato per essi è privo di significato: hanno senso solo con la tabella dei flussi. Le richieste sono singoli datagrammi UDP, quindi "una connessione interrotta" è dedotta dal cambio del server che risponde, non osservata come reset TCP. Infine il bilanciatore è uno solo: il disaccordo fra più bilanciatori, che motiva l'intero progetto, è emulato soltanto spegnendo la tabella dei flussi o riavviando il nodo.

## Letteratura primaria

- D. Karger, E. Lehman, T. Leighton, R. Panigrahy, M. Levine, D. Lewin, "Consistent Hashing and Random Trees: Distributed Caching Protocols for Relieving Hot Spots on the World Wide Web", ACM STOC, 1997.
- D. Thaler, C. Ravishankar, "Using Name-Based Mappings to Increase Hit Rates", IEEE/ACM Transactions on Networking, 1998.
- Y. Azar, A. Broder, A. Karlin, E. Upfal, "Balanced Allocations", SIAM Journal on Computing, 1999.
- M. Mitzenmacher, "The Power of Two Choices in Randomized Load Balancing", IEEE Transactions on Parallel and Distributed Systems, 2001.
- G. DeCandia et al., "Dynamo: Amazon's Highly Available Key-value Store", ACM SOSP, 2007.
- P. Patel et al., "Ananta: Cloud Scale Load Balancing", ACM SIGCOMM, 2013.
- J. Lamping, E. Veach, "A Fast, Minimal Memory, Consistent Hash Algorithm", arXiv, 2014.
- D. Eisenbud et al., "Maglev: A Fast and Reliable Software Network Load Balancer", USENIX NSDI, 2016.
- R. Miao, H. Zeng, C. Kim, J. Lee, M. Yu, "SilkRoad: Making Stateful Layer-4 Load Balancing Fast and Cheap Using Switching ASICs", ACM SIGCOMM, 2017.
- V. Mirrokni, M. Thorup, M. Zadimoghaddam, "Consistent Hashing with Bounded Loads", ACM-SIAM SODA, 2018.
- V. Olteanu, A. Agache, A. Voinescu, C. Raiciu, "Stateless Datacenter Load-balancing with Beamer", USENIX NSDI, 2018.
- J. T. Araújo, L. Saino, L. Buytenhek, R. Landa, "Balancing on the Edge: Transport Affinity without Network State", USENIX NSDI, 2018.
- RFC 2992, "Analysis of an Equal-Cost Multi-Path Algorithm" (confronto fra modulo-N, hash-threshold e highest random weight).

<!-- detail: 1 -->
## In laboratorio

Eseguire `./hoplab run 450-consistent-hashing`. Il client c1 invia 920 richieste, 40 al secondo, su 40 flussi verso il bilanciatore `lb`, che parte con s1, s2, s3; s4 viene aggiunto a 8 s e s1 rimosso a 16 s. Parametri: `algo` (consistent, hash, maglev; il modulo accetta anche rr, wrr, leastconn, p2c), `affinity` (0 = senza stato), `flows`.

Che cosa misurare: `reqgen.moved` su c1 (flussi il cui server di risposta è cambiato, da estremo a estremo), `lb.remapped` sul bilanciatore, le due righe di log stampate da `lb` a ogni cambio del pool ("N/40 flussi hanno perso il loro server, M/40 altri ora ricadono per hash su un server diverso") e la riga finale di `reqgen` con la quota di ciascun server. L'ideale per 40 flussi è di circa 10 spostamenti all'ingresso di s4, più i flussi di s1 alla sua uscita. In modalità headless:

```
./hoplab run 450-consistent-hashing algo=maglev --headless --detail 1
```

**Esperimento 1 — disturbo.** Ipotesi: mod-N sposta circa 1 − 1/(N+1) dei flussi a ogni cambiamento; anello e Maglev restano vicini al minimo, Maglev leggermente sopra. Manipolazione: `algo=hash`, `algo=consistent`, `algo=maglev`. Metrica: `reqgen.moved` su 80 spostamenti possibili, e quanti dei flussi che si spostano all'aggiunta vanno su s4 (ogni altro spostamento è superfluo). Esito atteso: circa 60 per mod-N (più spostamenti che flussi: la maggior parte si sposta due volte), circa 20 per gli altri due; la verifica `disruption` del laboratorio enuncia entrambi i limiti. Si noti che all'aggiunta l'anello ha spostato 14 flussi, non 10: prima di incolpare lo schema si decida se si tratta di rumore di campionamento o di un arco sovradimensionato.

**Esperimento 2 — bilanciamento su piccola scala.** Ipotesi: Maglev bilancia meglio di un anello con 100 nodi virtuali. Manipolazione: `algo=consistent` e `algo=maglev`, ciascuno con `flows=10`, `20`, `40`. Metrica: rapporto fra massimo e media delle quote di richieste per server nella riga finale di `reqgen`, separatamente per ogni fase del pool se si aggiungono righe `expect sN whoami.served >= 0` a una copia. Esito atteso: nessun vincitore netto; con così pochi flussi lo sbilanciamento è ampio per entrambi e l'ipotesi non può essere confermata. Si dichiari quale dimensione del campione servirebbe, poi si veda l'argomento 47.

**Esperimento 3 — che cosa nasconde la tabella dei flussi.** Ipotesi: una tabella dei flussi maschera il disturbo di una cattiva associazione finché la tabella non va perduta. Manipolazione: `algo=hash affinity=1`; poi lo stesso con il bilanciatore riavviato a metà esecuzione digitando `node lb down` e `node lb up` intorno ai 12 s; ripetere con `algo=consistent affinity=1`. Metrica: `reqgen.moved`, `reqgen.backends_seen`, `lb.remapped`. Esito atteso: senza riavvio si spostano solo i flussi del server rimosso (circa 11), mentre `lb.remapped` registra i circa 70 che il solo hash avrebbe spostato, e s4 non riceve nulla perché nessun flusso nuovo viene mai aperto e mod-N manda ogni orfano di s1 allo stesso superstite (`backends_seen` vale 3); dopo un riavvio mod-N perde quasi tutti i flussi e l'anello non ne perde più di quanti ne perdeva senza tabella.

## Domande per la discussione

- Il rendezvous hashing è esattamente minimo e non richiede tabelle. Perché Google ha costruito Maglev? Si quantifichi il costo di una ricerca con N = 1000.
- Con dimensioni dei flussi a coda pesante, il disturbo va misurato in flussi, in byte o in errori visibili all'utente? Si progetti la misura.
- Una tabella dei flussi e un hash consistente proteggono da eventi diversi. Si elenchino gli eventi indicando quale meccanismo copre ciascuno; quale evento non è coperto da nessuno dei due?
- Quanti flussi servirebbero all'esperimento 2 per rilevare con ragionevole confidenza una differenza del 5% nel rapporto fra picco e media del carico? Che cosa implica per i limiti dell'emulatore?
- Le connessioni QUIC sopravvivono a un cambio di indirizzo del client. Su che cosa dovrebbe calcolare l'hash un bilanciatore senza stato, e quale nuova superficie d'attacco ne deriva?
