title Regression: IPv4 routes with an IPv6 next hop (static)
level 0
layer 3
about IPv4 islands joined by a link that has only IPv6 addresses; the routers reach each other's IPv4 networks through an IPv6 next hop found with Neighbor Discovery.
node h1 host
node r1 router
node r2 router
node h2 host
link h1 r1 delay=2ms
link r1 r2 delay=5ms
link r2 h2 delay=2ms
ip h1 10.0.1.10/24
ip r1:eth0 10.0.1.1/24
ip r1:eth1 2001:db8:12::1/64
ip r2:eth0 2001:db8:12::2/64
ip r2:eth1 10.0.2.1/24
ip h2 10.0.2.10/24
gw h1 10.0.1.1
gw h2 10.0.2.1
route r1 10.0.2.0/24 via 2001:db8:12::2
route r2 10.0.1.0/24 via 2001:db8:12::1 dev eth0
run h1 ping h2 count=3
at 5s run h1 traceroute h2
duration 9s
expect h1 ping.reply == 3
expect h1 traceroute.hops == 3
