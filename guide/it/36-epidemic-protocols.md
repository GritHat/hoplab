<!-- level: 3 -->
<!-- labs: 382-gossip-rumor -->
# Protocolli epidemici: diffusione di una voce e anti-entropia

Come far arrivare un'informazione a migliaia di macchine, alcune delle quali in ogni momento sono guaste, lente o irraggiungibili? Un coordinatore che invia a tutti è un collo di bottiglia e un singolo punto di guasto; uno spanning tree consegna con il numero minimo di messaggi ma perde un intero sottoalbero quando un nodo interno si guasta, e va ricostruito. Entrambi richiedono una conoscenza accurata e globale di chi è attivo, cioè proprio ciò che un sistema grande non possiede.

L'approccio **epidemico** (gossip) rinuncia alla struttura. A ogni round ciascun nodo contatta pochi peer scelti uniformemente a caso e scambia informazioni con loro. Nessuno coordina, nessuno conosce la topologia, nessun messaggio è indispensabile. La difficoltà si sposta dall'ingegneria alla probabilità: nulla è garantito in modo deterministico, quindi bisogna dimostrare che l'esito vale con alta probabilità e quantificarne il costo.

Il fatto chiave è che un'epidemia cresce esponenzialmente. Finché i nodi informati sono pochi, ogni contatto raggiunge quasi certamente un nodo non informato e l'insieme degli informati si moltiplica a ogni round; `N` nodi vengono coperti in `O(log N)` round, con un carico costante per nodo e per round. I messaggi persi e i nodi guasti riducono soltanto il tasso di crescita. Ciò che si paga è la ridondanza: verso la fine la maggior parte dei contatti è sprecata su nodi che sanno già, e decidere quando fermarsi è a sua volta un compromesso fra traffico e rischio di lasciare fuori qualcuno.

<!-- detail: 2 -->
## Algoritmi e modelli

**Tre stili.** In un round il nodo `p` contatta un peer casuale `q`.

- **Push**: se `p` conosce la voce, la invia a `q`.
- **Pull**: `p` chiede novità a `q`; `q` risponde se la conosce.
- **Push-pull**: entrambe le cose.

**Approssimazione deterministica.** Sia `s_t` la frazione di nodi ancora non informati dopo il round `t`, con fanout `f` e `N` grande. Un nodo resta non informato con il push se nessuno degli `N*(1 - s_t)*f` invii lo colpisce, con il pull se tutti i suoi `f` contatti sono non informati:

```
push:       s(t+1) = s(t) * exp( -f*(1 - s(t)) )
pull:       s(t+1) = s(t) ^ (f + 1)
push-pull:  s(t+1) = s(t) ^ (f + 1) * exp( -f*(1 - s(t)) )
```

Il push parte veloce (l'insieme degli informati si moltiplica all'incirca per `1 + f` finché è piccolo) ma finisce lentamente: quando quasi tutti sanno, `s` si riduce solo di `e^(-f)` per round, quindi gli ultimi nodi richiedono altri `ln N / f` round. Per `f` = 1 il risultato classico è

```
round(push) = log2 N + ln N + O(1)      con alta probabilità
```

Il pull è l'opposto: lento a partire, perché i pochi nodi informati vengono interrogati di rado, ma una volta che `s < 1/2` la frazione di non informati viene elevata al quadrato a ogni round, quindi la coda richiede solo `O(log log N)` round. Il push-pull ha entrambe le fasi buone: `O(log N)` round e `O(N log log N)` messaggi in totale, contro `O(N log N)` per il solo push.

**Il modello SI.** A tempo continuo, con `i` la frazione informata e `β` il tasso di contatto:

```
di/dt = β * i * (1 - i)          i(t) = 1 / (1 + (N - 1) * exp(-β*t))
```

La curva logistica: crescita esponenziale, un flesso in `i = 1/2` raggiunto a `t = ln(N - 1)/β`, poi avvicinamento esponenziale a 1. I nodi non guariscono mai; la voce viene diffusa per sempre (**epidemia semplice**).

**Rumor mongering (SIR).** Per limitare il traffico, un nodo infettivo prima o poi perde interesse e diventa **rimosso**. Varianti: fermarsi con probabilità `1/k` dopo ogni contatto inutile (moneta), oppure dopo `k` contatti inutili (contatore); decidere in base alla risposta del peer (feedback) o a prescindere (blind). Ora alcuni nodi possono non sentire mai la voce. Per il feedback con moneta il residuo `s` risolve

```
s = exp( -(k + 1)*(1 - s) )        k = 1: s = 0.20     k = 2: s = 0.06
```

e i contatori fanno meglio delle monete a parità di traffico. Il residuo cala esponenzialmente in `k`, il traffico cresce linearmente.

**Anti-entropia.** Periodicamente ogni nodo sceglie un peer a caso e i due riconciliano l'intero stato (scambiando digest, version vector o alberi di Merkle per trovare le differenze). È un'epidemia semplice, quindi ogni aggiornamento raggiunge ogni nodo con probabilità 1, compresi i nodi che erano spenti o separati da una partizione; è troppo costosa per essere eseguita spesso. I sistemi reali combinano un rumor mongering veloce per gli aggiornamenti nuovi con un'anti-entropia lenta come rete di sicurezza.

**Esempio svolto.** `N` = 12, come nel laboratorio: `log2 12` = 3,6 e `ln 12` = 2,5. Push con fanout 2: da `s` = 11/12 la ricorrenza dà 0,78, 0,50, 0,18, 0,04, 0,005, quindi circa 5 round finché manca meno di un nodo. Push-pull con fanout 2: 0,65, 0,13, 0,0004: da 2 a 3 round.

<!-- detail: 3 -->
## In profondità

**Perché è robusto.** L'analisi usa solo due fatti: i contatti sono uniformi e indipendenti, e ciascuno riesce con una certa probabilità. Un tasso di perdita `ε` o una frazione di nodi guasti moltiplicano il fanout effettivo per una costante; cambiano gli esponenti, non la forma logaritmica. Dopo un guasto non c'è stato da riparare, perché non c'è struttura. La stessa indipendenza è il punto debole: le garanzie sono probabilistiche, l'ordine di consegna è arbitrario e il protocollo non può sapere di aver finito.

**Modi di guasto.** Il rumor mongering può estinguersi presto (il residuo). Una partizione più lunga del periodo infettivo lascia un lato permanentemente non informato, a meno che giri l'anti-entropia. Cancellare un dato non equivale all'assenza del dato: senza un **certificato di morte** (death certificate) il dato cancellato viene resuscitato da una replica che lo possiede ancora, e i certificati vanno a loro volta conservati (e prima o poi eliminati, tenendone copie dormienti). Il campionamento uniforme richiede di conoscere tutti i membri, il che non scala: i servizi di peer sampling (SCAMP, Cyclon, HyParView) mantengono una piccola vista parziale casuale diffondendo per gossip le viste stesse. Su topologie che non sono grafi completi il tempo di diffusione dipende dalla conduttanza del grafo e può essere tutt'altro che logaritmico; il gossip spaziale sbilancia la scelta verso i nodi vicini per risparmiare i collegamenti lunghi. Nodi malevoli possono distorcere o inondare un'epidemia; le varianti tolleranti ai guasti bizantini limitano ciò che un singolo nodo può immettere.

**Compromessi rispetto alla struttura.** Un albero invia `N - 1` messaggi, il gossip `O(N log N)`; i protocolli ibridi (Plumtree) diffondono pigramente per gossip solo gli identificatori dei messaggi e lasciano emergere un albero dalle prime consegne, tornando al gossip quando si rompe.

**Che cosa fanno i sistemi reali.** L'origine è Clearinghouse (Xerox). Dynamo e Cassandra diffondono per gossip l'appartenenza al gruppo e riparano le repliche con l'anti-entropia su alberi di Merkle, più read repair e hinted handoff. SWIM e Serf trasportano gli aggiornamenti di appartenenza sulle sonde del failure detector. Il bimodal multicast affianca a un broadcast inaffidabile una riparazione via gossip. Le reti blockchain inondano transazioni e blocchi in modo epidemico. Il gossip calcola anche aggregati (medie, conteggi) con ripetute medie a coppie.

**In netlab.** Il modulo `gossip` esegue round sincroni di durata fissa su un elenco completo dei membri; `stop=k` è la regola feedback/contatore; n1 riporta a ogni round il numero di nodi informati grazie a una conoscenza globale che un nodo reale non avrebbe. Questo laboratorio non ha anti-entropia (si veda `483-gossip-partition` nel percorso Frontier).

**Letteratura primaria.**

- W. Kermack, A. McKendrick, "A Contribution to the Mathematical Theory of Epidemics", 1927.
- A. Frieze, G. Grimmett, "The Shortest-Path Problem for Graphs with Random Arc-Lengths", 1985.
- B. Pittel, "On Spreading a Rumor", 1987.
- A. Demers et al., "Epidemic Algorithms for Replicated Database Maintenance", PODC 1987.
- K. Birman et al., "Bimodal Multicast", 1999.
- R. Karp, C. Schindelhauer, S. Shenker, B. Vöcking, "Randomized Rumor Spreading", FOCS 2000.
- A. Das, I. Gupta, A. Motivala, "SWIM: Scalable Weakly-consistent Infection-style Process Group Membership Protocol", 2002.
- D. Kempe, A. Dobra, J. Gehrke, "Gossip-Based Computation of Aggregate Information", 2003.
- P. Eugster, R. Guerraoui, A.-M. Kermarrec, L. Massoulié, "Epidemic Information Dissemination in Distributed Systems", 2004.

<!-- detail: 1 -->
## In laboratorio

`./netlab run 382-gossip-rumor`. Dodici nodi su una LAN; a 2 s n1 avvia la voce `fire!`. Selezionare n1 e aprire la scheda `gossip`: il grafico `nodi che conoscono la voce` è la curva dell'epidemia. Nel log (`l`) n1 stampa una riga per round, "round k — m/12 nodi conoscono", e alla fine "tutti sanno dopo R round (log2 N = 3.6)". n1 conosce il conteggio perché ogni nodo avvisa l'origine la prima volta che sente la voce (un censimento estraneo all'epidemia); `gossip.reached` e `gossip.residue` sono nel rapporto finale. Gli altri nodi mostrano il proprio stato: suscettibile, infettivo, rimosso. I contatori `gossip.messages` e `gossip.rounds` sono nel pannello di n1. I round durano 500 ms. Poiché il processo è casuale, ripetere ogni esperimento con alcuni semi (`--seed 1`, `--seed 2`, …) e ragionare sulla distribuzione, non su una singola esecuzione.

**Riferimento.** Default `mode=pushpull fanout=2`. Calcolare prima la ricorrenza (l'esempio svolto prevede da 2 a 3 round), poi misurare. Atteso: 1, poi 6–8, poi 12 nodi.

**Esperimento 1: push, pull, push-pull.** Eseguire `mode=push` e `mode=pull`. Previsione: al push servono da 3 a 6 round, e gli ultimi li spende per uno o due nodi mancanti; al pull ne servono 3 o 4, con un primo round lento e un finale brusco. Confrontare `gossip.messages` fra i tre modi.

**Esperimento 2: il residuo.** `./netlab run 382-gossip-rumor mode=push stop=1`. Ogni nodo smette di spingere dopo il primo peer che risponde "lo sapevo già". Usare l'equazione del residuo per prevedere quanti dei 12 nodi non vengono mai raggiunti (circa due), poi eseguire con più semi: nella maggior parte delle esecuzioni il conteggio si ferma sotto 12 e, quando nulla cambia più per alcuni round, n1 stampa che la voce si è estinta, con il residuo (contatore `gossip.residue`; le verifiche passano, perché per push si afferma solo che l'epidemia termina). Anche il valore predefinito `stop=3` lascia a volte fuori un nodo con `mode=push`. Spiegare perché pull e push-pull qui non hanno residuo.

**Esperimento 3: fanout e perdite.** `fanout=1` contro `fanout=4`: prevedere i round dalla ricorrenza e osservare che i round calano lentamente mentre i messaggi crescono in proporzione. Poi `loss=0.2`: ogni collegamento scarta il 20% dei pacchetti. Previsione: copertura comunque completa, in uno o due round in più; nient'altro cambia. In un laboratorio in esecuzione guastare anche due nodi prima che la voce parta (`node n7 down`, `node n8 down`) e verificare che il resto venga coperto.

## Verifica

- Ricavare la ricorrenza del push s(t+1) = s(t)·exp(−f(1 − s(t))) e usarla per giustificare le due fasi, log2 N e ln N, per f = 1.
- Perché il pull converge in modo doppiamente esponenziale alla fine ma lentamente all'inizio? Che cosa implica per il numero di messaggi del push-pull?
- Risolvere l'equazione logistica del modello SI e dare l'istante in cui metà della popolazione è informata. Quale ipotesi del modello cade su una topologia sparsa?
- Per il rumor mongering con feedback e moneta di probabilità 1/k, ricavare l'equazione del residuo e calcolare il residuo per k = 3. Perché il residuo non è accettabile per una base di dati replicata, e che cosa si aggiunge per rimediare?
- Una replica cancella un dato e diffonde la cancellazione come "dato assente". Descrivere come il dato ricompare, e il meccanismo (con il suo costo) che lo impedisce.
