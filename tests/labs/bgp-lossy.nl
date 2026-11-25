title  Regression: BGP sessions over TCP on lossy links, and a router restart
level  0
layer  3
topic  bgp
about  Not a teaching lab: a regression test. Three ASes in a line; the links between the routers
about  lose packets. Because the sessions run over TCP, no OPEN, UPDATE or withdrawal is ever lost:
about  tables converge and stay converged. At 20 s r2 crashes without a word: its neighbors notice
about  when the hold timer expires and withdraw what they learned. At 40 s it comes back, the
about  sessions are re-established and the tables are rebuilt. At 60 s r3 withdraws its prefix. With a
about  short hold time and heavy loss a session may also flap (TCP backs off, the hold timer does
about  not wait): it must come back by itself.
param  loss 0.1 Loss on the links between the routers
param  keepalive 3s BGP keepalive interval
param+ bw 10M Bandwidth of the links between the routers

node r1 router
node r2 router
node r3 router
link r1 r2 delay=5ms loss=$loss bw=$bw
link r2 r3 delay=5ms loss=$loss bw=$bw
ip r1:eth0 172.16.12.1/24
ip r2:eth0 172.16.12.2/24
ip r2:eth1 172.16.23.2/24
ip r3:eth0 172.16.23.3/24

run r1 bgp as=65001 keepalive=$keepalive network=10.1.0.0/24 network=10.11.0.0/16 neighbor=r2:4200000002
run r2 bgp as=4200000002 keepalive=$keepalive network=10.2.0.0/24 neighbor=r1:65001 neighbor=r3:65003
run r3 bgp as=65003 keepalive=$keepalive network=10.3.0.0/24 neighbor=r2:4200000002

at 20s node r2 down
at 40s node r2 up
at 60s run r3 bgp withdraw 10.3.0.0/24
duration 70s
expect r1 bgp.established == 1
expect r2 bgp.established == 2
expect r3 bgp.established == 1
expect r1 bgp.routes == 1
expect r2 bgp.routes == 2
expect r3 bgp.routes == 3
expect r1 bgp.session_drops >= 1
expect r3 bgp.session_drops >= 1
