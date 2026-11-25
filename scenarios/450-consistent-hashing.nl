title      Consistent hashing: what happens when the server pool changes
title[it]  Consistent hashing: cosa succede quando il pool di server cambia
level  4
layer  4
topic  lb
about  A stateless balancer (no flow table: affinity=0) must send every packet of a flow to the same server
about  using only a hash of the flow. This is how large sites spread load over many balancers that share
about  no state, and how distributed caches and stores place keys. The question is what happens when the
about  pool changes: at 8 s a fourth server is added, at 16 s server s1 is removed.
about
about  With consistent hashing (default) only the flows that the new server takes over, or that belonged
about  to the removed one, change server: about a quarter each time. Run it again with algo=hash (hash mod
about  N): nearly every flow moves at each change, which would break every open connection and empty every
about  cache. algo=maglev builds a lookup table from per-server permutations: lookups are O(1), the load is
about  more even than with a ring, and disruption stays close to the minimum.
about
about  The counter reqgen.moved on c1 is the number of flows whose server changed, measured end to end;
about  lb.remapped is the balancer's own count (what the hash alone would have moved). Try affinity=1:
about  the flow table hides the problem for existing flows on this balancer, but not on a second
about  balancer or after a restart. With it the new server s4 receives only flows orphaned by the removal
about  of s1 (these client flows never end, so there are no new ones), and with algo=hash possibly none
about  at all: mod N sends every orphan of s1 to the same survivor. A stateful algorithm (rr, wrr,
about  leastconn, p2c) with affinity=0 is the worst case: every packet is scheduled afresh.
about[it]  Un bilanciatore senza stato (nessuna tabella dei flussi: affinity=0) deve inviare ogni pacchetto
about[it]  di un flusso allo stesso server usando solo un hash del flusso. È così che i grandi siti
about[it]  distribuiscono il carico su molti bilanciatori che non condividono stato, ed è così che cache e
about[it]  archivi distribuiti collocano le chiavi. La domanda è cosa succede quando il pool cambia: a 8 s
about[it]  viene aggiunto un quarto server, a 16 s viene rimosso il server s1.
about[it]
about[it]  Con il consistent hashing (default) cambiano server solo i flussi che il nuovo server prende in
about[it]  carico, o che appartenevano a quello rimosso: circa un quarto ogni volta. Riesegui con algo=hash
about[it]  (hash mod N): quasi tutti i flussi si spostano a ogni cambiamento, il che romperebbe ogni
about[it]  connessione aperta e svuoterebbe ogni cache. algo=maglev costruisce una tabella di lookup da
about[it]  permutazioni per server: le ricerche sono O(1), il carico è più uniforme che con un anello e il
about[it]  disturbo resta vicino al minimo.
about[it]
about[it]  Il contatore reqgen.moved su c1 è il numero di flussi il cui server è cambiato, misurato da
about[it]  estremo a estremo; lb.remapped è il conteggio del bilanciatore (ciò che il solo hash avrebbe
about[it]  spostato). Prova affinity=1: la tabella dei flussi nasconde il problema per i flussi esistenti su
about[it]  questo bilanciatore, ma non su un secondo bilanciatore o dopo un riavvio. Con essa il nuovo server
about[it]  s4 riceve solo i flussi rimasti orfani per la rimozione di s1 (questi flussi del client non
about[it]  finiscono mai, quindi non ce ne sono di nuovi), e con algo=hash forse nessuno: mod N manda ogni
about[it]  orfano di s1 allo stesso superstite. Un algoritmo con stato (rr, wrr, leastconn, p2c) con
about[it]  affinity=0 è il caso peggiore: ogni pacchetto viene riassegnato da capo.
param  algo consistent Hashing scheme: consistent, hash (mod N), maglev
param[it] algo Schema di hashing: consistent, hash (mod N), maglev
param+ affinity 0 Flow table (0 = stateless, the hash alone decides)
param[it] affinity Tabella dei flussi (0 = senza stato, decide solo l'hash)
param+ flows 40 Client flows (max 40)
param[it] flows Flussi del client (max 40)

node c1 host x=8 y=50
node lb router x=40 y=50 role=LB
node s1 host x=88 y=10 role=SERVER
node s2 host x=88 y=36 role=SERVER
node s3 host x=88 y=63 role=SERVER
node s4 host x=88 y=90 role=SERVER
link c1 lb delay=5ms
link lb s1 delay=1ms
link lb s2 delay=1ms
link lb s3 delay=1ms
link lb s4 delay=1ms
auto all

run s1 whoami
run s2 whoami
run s3 whoami
run s4 whoami
run lb lb vip=lb:eth0 port=8000 backends=s1,s2,s3 algo=$algo affinity=$affinity check=0
run c1 reqgen lb count=920 rate=40 flows=$flows
note 0s Three servers. Each flow is hashed to one of them; with no flow table the hash is recomputed for every packet.
note[it] 0s Tre server. Ogni flusso viene assegnato a uno di essi tramite hash; senza tabella dei flussi l'hash si ricalcola a ogni pacchetto.
at 8s run lb lb add s4
note 8s s4 joins the pool. Ideal: only the flows s4 takes (about 1/4) move. Count the "flow from port" lines.
note[it] 8s s4 entra nel pool. Ideale: si spostano solo i flussi che s4 prende (circa 1/4). Conta le righe "flow from port".
at 16s run lb lb remove s1
note 16s s1 leaves. Ideal: only its own flows move, spread over the three that remain.
note[it] 16s s1 esce. Ideale: si spostano solo i suoi flussi, distribuiti sui tre che restano.
duration 25s

at 24900ms run c1 check served "c1:reqgen.answers == 920 && c1:reqgen.lost == 0"
# stateless: consistent hashing and Maglev move about 1/4 of the flows at the first change and 1/3
# at the second; hash mod N moves most of them twice (so more moves than flows)
at 24900ms run c1 check disruption "$affinity == 1 || (is_$algo == is_hash && c1:reqgen.moved >= 1.1 * $flows) || ((is_$algo == is_consistent || is_$algo == is_maglev) && c1:reqgen.moved >= 0.2 * $flows && c1:reqgen.moved <= 0.85 * $flows && c1:reqgen.backends_seen == 4) || (is_$algo != is_hash && is_$algo != is_consistent && is_$algo != is_maglev && c1:reqgen.moved >= 5 * $flows)"
# with a flow table only the flows of the removed server move, whatever the hash would say
at 24900ms run c1 check pinned "$affinity == 0 || (c1:reqgen.moved <= 0.6 * $flows && lb:lb.remapped >= c1:reqgen.moved)"
expect c1 check.served == 1
expect c1 check.disruption == 1
expect c1 check.pinned == 1
