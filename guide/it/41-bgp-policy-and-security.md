<!-- level: 4 -->
<!-- labs: 430-bgp-policies, 431-bgp-hijack -->
# Politiche e sicurezza di BGP

BGP è l'unico protocollo di routing di largo impiego il cui obiettivo non è un cammino minimo. Ogni sistema autonomo ordina e filtra le rotte secondo il proprio interesse commerciale, e il sistema di routing globale è ciò che emerge da decine di migliaia di decisioni locali di questo tipo. Ne derivano due domande, entrambe tuttora oggetto di ricerca.

**Perché converge?** Politiche arbitrarie possono far oscillare BGP all'infinito, e decidere se una data configurazione ammette una soluzione stabile è computazionalmente difficile. Internet è stabile soprattutto perché gli operatori seguono, per incentivo economico e non per obbligo di protocollo, una gerarchia di relazioni fra cliente, peer e provider.

**Perché chiunque può annunciare qualunque cosa?** Un router BGP non può verificare che un vicino abbia titolo su un prefisso né che il percorso dichiarato esista. Dirottamenti e route leak avvengono ogni anno, per dolo e per errore. La difesa oggi diffusa, la validazione dell'origine tramite RPKI, autentica soltanto l'ultimo AS del percorso; la validazione del percorso è standardizzata ma quasi assente in esercizio.

I due laboratori rendono concreti entrambi i punti: una topologia di cinque AS in cui un percorso fisico perfettamente utilizzabile viene rifiutato per ragioni commerciali, e una fila di quattro AS in cui l'ultimo sottrae un prefisso, prima a una parte della rete e poi a tutti.

<!-- detail: 2 -->
## Relazioni commerciali e condizioni di Gao–Rexford

I collegamenti fra AS sono, in prima approssimazione, di due tipi. In un collegamento **cliente–provider** il cliente paga il transito verso l'intera Internet. In un collegamento **peer–peer** due AS si scambiano il traffico dei soli rispettivi clienti, di norma senza pagamento. Due regole traducono gli incentivi.

- **Preferenza**: preferire le rotte apprese dai clienti a quelle dei peer, e queste a quelle dei provider (si realizza con la local preference, confrontata prima della lunghezza dell'AS path).
- **Esportazione**: annunciare a tutti le rotte dei clienti; annunciare le rotte di peer e provider solo ai clienti.

Gao e Rexford hanno dimostrato che, se ogni AS segue queste regole e il grafo cliente–provider è privo di cicli, BGP converge a un unico stato stabile da qualunque stato iniziale e con qualunque temporizzazione dei messaggi. La sola regola di esportazione produce percorsi **valley-free**: zero o più salti da cliente a provider, al più un salto fra peer, poi zero o più salti da provider a cliente. Una conseguenza, visibile nel primo laboratorio, è che la connettività nel grafo delle politiche è strettamente più debole di quella fisica.

## Route leak

Un route leak (RFC 7908) è la propagazione di un annuncio oltre l'ambito previsto; il caso tipico è un cliente multihomed che riesporta a un provider le rotte dell'altro. Il percorso così diffuso è più lungo, ma il provider che lo riceve lo preferisce perché arriva da un cliente. I leak sono violazioni della proprietà valley-free e non richiedono alcuna falsificazione: ogni AS del percorso è reale. La RFC 9234 introduce i ruoli BGP, negoziati all'apertura della sessione, e l'attributo Only-To-Customer, affinché un leak sia rilevabile dai vicini.

## Dirottamento di un prefisso e di un sotto-prefisso

Un AS che origina il prefisso di un altro compete con l'origine legittima secondo la normale selezione delle rotte: ogni AS sceglie per preferenza e poi per lunghezza del percorso, e Internet si divide in due regioni. Un AS che origina un prefisso **più specifico** non compete affatto, perché l'inoltro usa la corrispondenza più lunga: attira tutto il traffico da ogni AS che accetta l'annuncio. L'attaccante può inoltre conservare un percorso funzionante verso la vittima e inoltrarle il traffico dopo averlo ispezionato (intercettazione).

## Validazione dell'origine con RPKI

La RPKI (RFC 6480) è una gerarchia di certificati che ricalca l'allocazione degli indirizzi. Il titolare di un prefisso firma una Route Origin Authorization: prefisso, AS di origine, lunghezza massima. Un router classifica ogni annuncio (RFC 6811) come **valid** (una ROA che lo copre corrisponde per origine e lunghezza), **invalid** (esistono ROA che lo coprono e nessuna corrisponde) o **not found**; gli operatori scartano gli invalid. Ciò blocca le origini errate accidentali e i dirottamenti di sotto-prefisso quando la ROA ha una lunghezza massima stretta.

Ciò che non protegge: l'attaccante può annunciare il prefisso aggiungendo in coda l'AS legittimo come origine (**forged-origin hijack**), annuncio che risulta valido e costa un solo salto in più; una maxLength larga rende validi anche i dirottamenti di sotto-prefisso con origine falsificata (la RFC 9319 la sconsiglia); i route leak restano intatti; e finché la copertura è parziale i "not found" vanno accettati.

## BGPsec e ASPA

BGPsec (RFC 8205) fa firmare a ogni AS il percorso che inoltra, incluso l'AS successivo, cosicché i percorsi non possano essere accorciati né falsificati. Richiede la partecipazione di ogni AS del percorso e crittografia in linea sui router, e in dispiegamento parziale offre scarso beneficio. ASPA, in corso di definizione all'IETF, sceglie la via più economica di pubblicare, per ciascun AS, l'insieme dei suoi provider: chi riceve può così rilevare le violazioni valley-free, intercettando la maggior parte dei leak e molti percorsi con origine falsificata senza firme per ogni aggiornamento.

<!-- detail: 3 -->
## Problemi aperti

**Dispiegamento parziale.** Lychev, Goldberg e Schapira hanno mostrato che, con adozione parziale e con la sicurezza subordinata alla preferenza commerciale, la validazione del percorso aggiunge poco rispetto a quella dell'origine. Quale meccanismo incrementale offra il miglior beneficio marginale, e a chi, è ancora discusso. Beneficio e danno collaterale (un AS a valle di un transito che non valida resta ingannato) richiedono un'analisi che tenga conto della topologia.

**Le politiche non sono Gao–Rexford.** Le relazioni reali comprendono transito parziale, peering a pagamento, AS fratelli ed eccezioni per prefisso. Inferire le relazioni dai dati pubblici (Gao 2001; Luckie et al. 2013) è a sua volta un problema di misura con scarsa verità di riferimento, e la correttezza di ASPA ne dipende.

**Fiducia nella RPKI stessa.** Validatori, repository e autorità di certificazione diventano nuovi punti di guasto; una ROA emessa per errore o ritirata può rendere invalida una rotta legittima.

**Dinamica della convergenza.** Anche quando uno stato stabile esiste, l'esplorazione dei percorsi dopo un ritiro può richiedere minuti (Labovitz et al. 2000).

## Il modello del laboratorio e la realtà

`./hoplab help bgp` dichiara le semplificazioni: le sessioni viaggiano su TCP, porta 179, con la macchina a stati della RFC 4271 (si possono quindi studiare reset di sessione, scadenza dell'hold timer e collegamenti con perdite), ma solo eBGP, un router per AS, niente MED, community o aggregazione. Ne consegue che tutto ciò che riguarda iBGP, route reflection e hot-potato routing è assente, e che la local preference è fissata meccanicamente dalla relazione dichiarata. Il timer MRAI vale per default 500 ms, contro le decine di secondi della pratica: i tempi di convergenza del laboratorio non sono confrontabili con quelli di Internet.

Nel laboratorio sul dirottamento la tabella delle ROA è una lista statica `roa=` per router: niente certificati, validatore, repository, né maggioranza di "not found". L'attaccante può solo originare un prefisso; non può falsificare un AS path, per cui l'attacco con origine falsificata, che nella pratica aggira la ROV, non è riproducibile con il modulo così com'è (è uno dei progetti dell'argomento 47). Con quattro AS in fila, la "frazione di Internet catturata" degenera in quale di due osservatori viene dirottato; affermazioni sulla topologia reale richiedono grafi su scala CAIDA e un altro simulatore.

## Letteratura primaria

- L. Gao, "On Inferring Autonomous System Relationships in the Internet", IEEE/ACM Transactions on Networking, 2001.
- L. Gao, J. Rexford, "Stable Internet Routing Without Global Coordination", IEEE/ACM Transactions on Networking, 2001.
- T. Griffin, F. B. Shepherd, G. Wilfong, "The Stable Paths Problem and Interdomain Routing", IEEE/ACM Transactions on Networking, 2002.
- C. Labovitz, A. Ahuja, A. Bose, F. Jahanian, "Delayed Internet Routing Convergence", ACM SIGCOMM, 2000.
- H. Ballani, P. Francis, X. Zhang, "A Study of Prefix Hijacking and Interception in the Internet", ACM SIGCOMM, 2007.
- R. Lychev, S. Goldberg, M. Schapira, "BGP Security in Partial Deployment: Is the Juice Worth the Squeeze?", ACM SIGCOMM, 2013.
- M. Luckie, B. Huffaker, A. Dhamdhere, V. Giotsas, k. claffy, "AS Relationships, Customer Cones, and Validation", ACM IMC, 2013.
- S. Goldberg, "Why Is It Taking So Long to Secure Internet Routing?", Communications of the ACM, 2014.
- Y. Gilad, A. Cohen, A. Herzberg, M. Schapira, H. Shulman, "Are We There Yet? On RPKI's Deployment and Security", NDSS, 2017.
- Y. Gilad, O. Sagga, S. Goldberg, "MaxLength Considered Harmful to the RPKI", ACM CoNEXT, 2017.
- RFC 4271 (BGP-4), RFC 7908 (classificazione dei route leak), RFC 6480 (architettura RPKI), RFC 6811 (validazione dell'origine), RFC 9319 (maxLength), RFC 8205 (BGPsec), RFC 9234 (ruoli e Only-To-Customer).

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 430-bgp-policies.** Eseguire `./hoplab run 430-bgp-policies`. p1 (AS 100) e p2 (AS 200) sono in peering; c1 e c2 sono clienti con un solo provider; m è cliente di entrambi. A 20 s il collegamento di peering si guasta. Parametri: `policy` (1 = relazioni commerciali, 0 = AS path più corto ed esportazione di tutto), `keepalive`. Si misurino `ping.reply` e `ping.error` su hc1, `bgp.routes` e `bgp.updates_sent` sui router, l'RTT dei ping e, nel log, il ritardo fra la riga del guasto (`link down`, in italiano `collegamento interrotto`) e ciascuna riga di ritiro (`route withdrawn`, `rotta ritirata`). Nella scheda BGP di m a dettaglio 3 si vedono le rotte conosciute ma non esportate.

- **Esperimento 1.** Ipotesi: l'esportazione valley-free elimina una raggiungibilità che il grafo fisico offre ancora. Manipolazione: `policy=1` contro `policy=0`. Metrica: risposte al secondo `hc1 ping hc2`, `bgp.routes` su p1, RTT. Esito atteso: con le politiche, tre errori "network unreachable" ("rete irraggiungibile") e due rotte su p1; senza, tre risposte più lente di circa 10 ms (il percorso attraversa m) e tre rotte.
- **Esperimento 2.** Ipotesi: il multihoming protegge m, e soltanto m, dal guasto di un singolo collegamento. Manipolazione: prima dei 20 s digitare `link p1 m down`, poi `hc1 ping hm count=3`; ripetere il ping dopo i 20 s. Metrica: risposte, RTT, tempo fino alla nuova riga di best path su p1. Esito atteso: dopo circa un secondo hm è raggiunto via p1 — p2 — m con RTT maggiore; dopo il guasto del peering è irraggiungibile da hc1.
- **Esperimento 3 (route leak, copia dello scenario).** Ipotesi: una relazione etichettata male viola le condizioni di Gao–Rexford e il sistema può non convergere. Manipolazione: copiare lo scenario e, nella riga `run m bgp`, cambiare `neighbor=p1:100:provider` in `neighbor=p1:100:customer`. Metrica: cambi di best path per 10.3.0.0/24 su p1, m e c1 nel tempo; `bgp.updates_sent`. Esito atteso: p1 e m preferiscono ciascuno la rotta "da cliente" dell'altro e il best path si ribalta circa una volta al secondo, senza fine: una dispute wheel in miniatura.

**Laboratorio 431-bgp-hijack.** Eseguire `./hoplab run 431-bgp-hijack`. AS 65001 — 65002 — 65003 — 65066 in fila; `client` è nel 65002, `obs` nel 65003; l'AS 65066 origina il /24 della vittima a 15 s e due /25 a 30 s. Parametro: `rov`. Si misurino `icmp.echo_served` su srv e su fake, il TTL di ogni echo reply (rivela chi ha risposto) e `bgp.rov_rejected`.

- **Esperimento 1.** Ipotesi: un dirottamento a pari lunghezza cattura esattamente gli AS per cui il percorso dell'attaccante è più corto; uno più specifico li cattura tutti. Manipolazione: esecuzione di default. Metrica: chi risponde a `client` e a `obs` nei tre turni. Esito atteso: al turno 2 a obs risponde fake e a client srv; al turno 3 a entrambi risponde fake (6 e 6 in totale).
- **Esperimento 2.** Ipotesi: la validazione dell'origine con maxLength 24 ferma entrambi gli attacchi, al prezzo di vietare alla vittima la propria de-aggregazione. Manipolazione: `rov=1`; poi, intorno ai 33 s, digitare `r1 bgp announce 10.1.0.0/25` e `r1 bgp announce 10.1.0.128/25`, con `rov=0` e con `rov=1`. Metrica: contatori di echo, righe `INVALID`. Esito atteso: con la ROV srv serve 12 richieste e fake 0, e anche i /25 della vittima sono rifiutati come invalid; senza ROV la de-aggregazione recupera client ma non obs.
- **Esperimento 3 (dispiegamento parziale, copia dello scenario).** Ipotesi: la protezione di un AS dipende da dove avviene la validazione lungo il suo percorso, non dalla sua scelta. Manipolazione: copiare lo scenario e sostituire `rov=$rov` con `rov=0` su r1 e r3, poi eseguire con `rov=1`; quindi validare solo su r3. Metrica: `icmp.echo_served` su fake, `bgp.rov_rejected` per router. Esito atteso: validare solo su r2 protegge client e la vittima ma lascia obs dirottato in entrambe le fasi (fake serve 4 richieste); validare solo su r3 protegge tutti.

## Domande per la discussione

- Il teorema di Gao–Rexford richiede una gerarchia di provider aciclica e regole uniformi. Quali pratiche reali violano ciascuna ipotesi, e che cosa impedisce comunque a Internet di oscillare?
- Con la ROV che scarta gli invalid ovunque, si descriva l'attacco residuo più economico contro 10.1.0.0/24 e si stimi quanti AS cattura nella topologia in fila del laboratorio e in una realistica.
- L'esperimento 3 del laboratorio sul dirottamento mostra un beneficio collaterale. Si costruisca una topologia con danno collaterale: un AS che valida e il cui traffico viene comunque dirottato.
- La maxLength è un parametro di sicurezza o una comodità operativa? Si argomenti a partire dall'esperimento di de-aggregazione.
- Che cosa occorrerebbe aggiungere al modulo bgp per valutare ASPA, e quale metrica si riporterebbe?
