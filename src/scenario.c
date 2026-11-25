/*
 * scenario.c - The scenario language: topology, addressing, scripted
 *              events, parameters and expectations; automatic addressing,
 *              routing and map layout; and the command interpreter shared
 *              by scenario files and the interactive console.
 *
 * See docs/scenario-format.md for the language reference.
 */
#include "nl_int.h"
#include "rdt.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int      g_loading;              /* parsing a file: `run` is deferred to t=0 */
static int      g_peek;                 /* only metadata */
static nl_meta *g_meta;                 /* where metadata goes */

static int fail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(W.errmsg, sizeof(W.errmsg), fmt, ap);
    va_end(ap);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Finding scenario files                                             */
/* ------------------------------------------------------------------ */

const char *nl_scenario_path(const char *name, char *buf, size_t n)
{
    struct stat st;
    char dir[600];
    DIR *d;
    struct dirent *e;
    size_t nl = strlen(name);

    if (stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(buf, n, "%s", name);
        return buf;
    }
    snprintf(dir, sizeof(dir), "%s/scenarios", nl_home());
    snprintf(buf, n, "%s/%s", dir, name);
    if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
        return buf;
    snprintf(buf, n, "%s/%s.nl", dir, name);
    if (stat(buf, &st) == 0)
        return buf;
    d = opendir(dir);
    if (!d)
        return NULL;
    while ((e = readdir(d)) != NULL) {
        size_t el = strlen(e->d_name);
        /* "ping" matches "101-ping.nl" */
        if (el > nl + 4 && strcmp(e->d_name + el - 3, ".nl") == 0 &&
            strncmp(e->d_name + el - 3 - nl, name, nl) == 0 && e->d_name[el - 4 - nl] == '-') {
            snprintf(buf, n, "%s/%s", dir, e->d_name);
            closedir(d);
            return buf;
        }
    }
    closedir(d);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Topology directives                                                */
/* ------------------------------------------------------------------ */

/* "r1" or "r1:eth2" -> node and interface (created on demand when create) */
static nl_node *end_point(const char *s, int *ifx, int create)
{
    char buf[40];
    char *colon;
    nl_node *n;

    snprintf(buf, sizeof(buf), "%s", s);
    colon = strchr(buf, ':');
    if (colon)
        *colon = '\0';
    n = nl_node_find(buf);
    if (!n)
        return NULL;
    *ifx = -1;
    if (colon) {
        *ifx = nl_if_find(n, colon + 1);
        while (*ifx < 0 && create && n->nif < NL_MAX_IF) {
            int i = nl_if_add(n);
            if (i >= 0 && strcmp(n->ifc[i].name, colon + 1) == 0)
                *ifx = i;
        }
        if (*ifx < 0)
            return NULL;
    }
    return n;
}

static int free_if(nl_node *n)
{
    int i;

    for (i = 0; i < n->nif; i++)
        if (!n->ifc[i].link)
            return i;
    return nl_if_add(n);
}

static int cmd_node(const nl_args *a)
{
    const char *name = nl_arg_pos(a, 1), *kind = nl_arg_pos(a, 2);
    nl_node *n;
    int k = NL_HOST;

    if (!name)
        return fail(_("usage: node NAME host|router|switch [x=N y=N] [role=TEXT]"));
    if (kind && strcmp(kind, "router") == 0) k = NL_ROUTER;
    else if (kind && strcmp(kind, "switch") == 0) k = NL_SWITCH;
    else if (kind && strcmp(kind, "host") != 0)
        return fail(_("unknown node kind '%s' (host, router, switch)"), kind);
    if (nl_node_find(name))
        return fail(_("node '%s' already exists"), name);
    if (strlen(name) > 12 || !isalpha((unsigned char)name[0]))
        return fail(_("bad node name '%s' (a letter first, at most 12 characters)"), name);
    n = nl_node_add(name, k);
    if (!n)
        return fail(_("too many nodes (max %d)"), NL_MAX_NODES);
    if (nl_arg_has(a, "x") && nl_arg_has(a, "y")) {
        n->x = nl_arg_dbl(a, "x", 50);
        n->y = nl_arg_dbl(a, "y", 50);
        n->placed = 1;
    }
    snprintf(n->role, sizeof(n->role), "%s", nl_arg_str(a, "role", ""));
    return 0;
}

static int cmd_link(const nl_args *a)
{
    const char *sa = nl_arg_pos(a, 1), *sb = nl_arg_pos(a, 2), *what = nl_arg_pos(a, 3);
    nl_node *na, *nb;
    nl_linkcfg cfg;
    nl_link *l;
    int ia, ib;
    char err[200];

    if (!sa || !sb)
        return fail(_("usage: link A[:IF] B[:IF] [bw=10M delay=10ms loss=0 queue=50 ...] | link A B up|down"));
    na = end_point(sa, &ia, g_loading);
    nb = end_point(sb, &ib, g_loading);
    if (!na || !nb)
        return fail(_("unknown node or interface in 'link %s %s'"), sa, sb);
    l = nl_link_find(na, nb);
    if (l && (ia < 0 || na->ifc[ia].link == l)) {
        /* existing link: change its state or parameters */
        if (what && (strcmp(what, "down") == 0 || strcmp(what, "up") == 0)) {
            nl_link_set_up(l, strcmp(what, "up") == 0);
            return 0;
        }
        if (nl_linkcfg_apply(&l->cfg, a, err, sizeof(err)) != 0)
            return fail("%s", err);
        nl_link_reconf(l);
        if (!g_loading)
            nl_log(NULL, NL_L_SIM, 1, 0, T_INFO "link %s — %s:" T_RESET " %s, %s %s, %s %.1f%%", na->name, nb->name,
                   nl_rate_str(l->cfg.bw), _("delay"), nl_time_str(l->cfg.delay), _("loss"), l->cfg.loss * 100);
        return 0;
    }
    if (!g_loading)
        return fail(_("no link between %s and %s"), sa, sb);
    if (na == nb)
        return fail(_("a link needs two different nodes"));
    if (ia < 0) ia = free_if(na);
    if (ib < 0) ib = free_if(nb);
    if (ia < 0 || ib < 0)
        return fail(_("too many interfaces on a node (max %d)"), NL_MAX_IF);
    if (na->ifc[ia].link || nb->ifc[ib].link)
        return fail(_("interface already connected in 'link %s %s'"), sa, sb);
    nl_linkcfg_default(&cfg);
    if (nl_linkcfg_apply(&cfg, a, err, sizeof(err)) != 0)
        return fail("%s", err);
    if (!nl_link_add(na, ia, nb, ib, &cfg))
        return fail(_("too many links (max %d)"), NL_MAX_LINKS);
    return 0;
}

/* ---- IPv6 variants of ip / gw / route ---- */

/* First global (not link-local) address of an interface, or NULL */
static const nl_ip6 *global6(const nl_node *n, int ifx)
{
    int k;

    for (k = 0; k < n->ifc[ifx].nip6; k++)
        if (!nl_ip6_linklocal(&n->ifc[ifx].ip6[k].a))
            return &n->ifc[ifx].ip6[k].a;
    return NULL;
}

/* Interface of n whose prefix contains gw, or -1 */
static int onlink6(const nl_node *n, const nl_ip6 *gw)
{
    int i, k;

    for (i = 0; i < n->nif; i++)
        for (k = 0; k < n->ifc[i].nip6; k++)
            if (!nl_ip6_linklocal(&n->ifc[i].ip6[k].a) &&
                nl_ip6_match(gw, &n->ifc[i].ip6[k].a, n->ifc[i].ip6[k].plen))
                return i;
    return -1;
}

/* Address of node `name` on a subnet shared with n; 1 = found */
static int shared6(const nl_node *n, const char *name, nl_ip6 *out, int *ifx)
{
    const nl_node *r = nl_node_find(name);
    int i, k;

    for (i = 0; r && i < r->nif; i++)
        for (k = 0; k < r->ifc[i].nip6; k++)
            if (!nl_ip6_linklocal(&r->ifc[i].ip6[k].a) && (*ifx = onlink6(n, &r->ifc[i].ip6[k].a)) >= 0) {
                *out = r->ifc[i].ip6[k].a;
                return 1;
            }
    return 0;
}

static int cmd_ip6(nl_node *n, int ifx, const nl_ip6 *ip, int plen)
{
    int i;

    for (i = 0; ifx < 0 && i < n->nif; i++)
        if (!global6(n, i))
            ifx = i;
    if (ifx < 0)
        return fail(_("node %s has no free interface for an address"), n->name);
    W.ip6 = 1;
    if (nl_if_add_addr6(n, ifx, ip, plen) != 0)
        return fail(_("too many IPv6 addresses on %s:%s (max %d)"), n->name, n->ifc[ifx].name, NL_MAX_IP6 - 1);
    return 0;
}

static int cmd_gw6(nl_node *n, const nl_ip6 *gw)
{
    int ifx = onlink6(n, gw);

    if (ifx < 0)
        return fail(_("gateway %s is not on a subnet of %s"), nl_ip6_str(gw), n->name);
    nl_route6_set(n, &(nl_ip6){ { 0 } }, 0, gw, ifx, 1, NL_RT_STATIC);
    return 0;
}

static int cmd_route6(nl_node *n, const nl_ip6 *dst, int plen, const char *kw, const char *arg, int del, int metric)
{
    nl_ip6 gw;
    int ifx;

    if (del) {
        nl_route6_del(n, dst, plen, NL_RT_STATIC);
        return 0;
    }
    if (!kw || !arg)
        return fail(_("usage: route NODE [add|del] PREFIX via GATEWAY | route NODE PREFIX dev IF"));
    if (strcmp(kw, "dev") == 0) {
        ifx = nl_if_find(n, arg);
        if (ifx < 0)
            return fail(_("unknown interface '%s'"), arg);
        nl_route6_set(n, dst, plen, NULL, ifx, metric, NL_RT_STATIC);
        return 0;
    }
    if (nl_parse_ip6(arg, &gw)) {
        ifx = onlink6(n, &gw);
        if (ifx < 0)
            return fail(_("gateway %s is not on a subnet of %s"), arg, n->name);
    } else if (!shared6(n, arg, &gw, &ifx)) {
        return fail(_("unknown gateway '%s'"), arg);
    }
    nl_route6_set(n, dst, plen, &gw, ifx, metric, NL_RT_STATIC);
    return 0;
}

static int cmd_ip(const nl_args *a)
{
    const char *who = nl_arg_pos(a, 1), *addr = nl_arg_pos(a, 2);
    nl_node *n;
    nl_ip ip;
    int ifx, plen, i;

    if (who && addr && strchr(addr, ':')) {
        nl_ip6 ip6;
        if (!nl_parse_prefix6(addr, &ip6, &plen) || !(n = end_point(who, &ifx, 0)))
            return fail(_("usage: ip NODE[:IF] A.B.C.D/LEN | ip NODE[:IF] IPV6-ADDRESS/LEN"));
        return cmd_ip6(n, ifx, &ip6, plen);
    }
    if (!who || !addr || !nl_parse_prefix(addr, &ip, &plen))
        return fail(_("usage: ip NODE[:IF] A.B.C.D/LEN | ip NODE[:IF] IPV6-ADDRESS/LEN"));
    n = end_point(who, &ifx, 0);
    if (!n)
        return fail(_("unknown node or interface '%s'"), who);
    for (i = 0; ifx < 0 && i < n->nif; i++)
        if (!n->ifc[i].ip)
            ifx = i;
    if (ifx < 0)
        return fail(_("node %s has no free interface for an address"), n->name);
    nl_if_set_addr(n, ifx, ip, plen);
    return 0;
}

static int cmd_gw(const nl_args *a)
{
    const char *who = nl_arg_pos(a, 1), *gw = nl_arg_pos(a, 2);
    nl_node *n = who ? nl_node_find(who) : NULL;
    nl_ip g;
    int i;

    if (!n || !gw)
        return fail(_("usage: gw NODE GATEWAY"));
    if (strchr(gw, ':')) {
        nl_ip6 g6;
        if (!nl_parse_ip6(gw, &g6))
            return fail(_("usage: gw NODE GATEWAY"));
        return cmd_gw6(n, &g6);
    }
    {   /* a node name also sets the IPv6 default route when both have IPv6 */
        nl_ip6 g6;
        int ifx6;
        if (!nl_parse_ip(gw, &g) && shared6(n, gw, &g6, &ifx6)) {
            nl_route6_set(n, &(nl_ip6){ { 0 } }, 0, &g6, ifx6, 1, NL_RT_STATIC);
            if (!nl_node_ip(n))
                return 0;
        }
    }
    if (!nl_parse_ip(gw, &g)) {
        /* a node name: pick its address on a subnet we share */
        nl_node *r = nl_node_find(gw);
        int k, found = 0;

        for (i = 0; r && i < n->nif && !found; i++)
            for (k = 0; k < r->nif && !found; k++)
                if (n->ifc[i].ip && r->ifc[k].ip && n->ifc[i].plen == r->ifc[k].plen &&
                    ((n->ifc[i].ip ^ r->ifc[k].ip) & nl_mask(n->ifc[i].plen)) == 0) {
                    g = r->ifc[k].ip;
                    found = 1;
                }
        if (!found)
            return fail(_("gateway '%s' is not on a subnet of %s"), gw, n->name);
    }
    for (i = 0; i < n->nif; i++)
        if (n->ifc[i].ip && ((n->ifc[i].ip ^ g) & nl_mask(n->ifc[i].plen)) == 0) {
            nl_route_set(n, 0, 0, g, i, 1, NL_RT_STATIC);
            return 0;
        }
    return fail(_("gateway %s is not on a subnet of %s"), nl_ip_str(g), n->name);
}

static int cmd_route(const nl_args *a)
{
    const char *who = nl_arg_pos(a, 1), *pfx = nl_arg_pos(a, 2), *kw = nl_arg_pos(a, 3), *arg = nl_arg_pos(a, 4);
    nl_node *n = who ? nl_node_find(who) : NULL;
    nl_ip dst, gw = 0;
    int plen, ifx = -1, i, del = 0;

    if (n && pfx && (strcmp(pfx, "del") == 0 || strcmp(pfx, "add") == 0)) {
        del = pfx[0] == 'd';
        pfx = kw;
        kw = arg;
        arg = nl_arg_pos(a, 5);
    }
    if (n && pfx && strchr(pfx, ':')) {
        nl_ip6 d6;
        if (!nl_parse_prefix6(pfx, &d6, &plen))
            return fail(_("usage: route NODE [add|del] PREFIX via GATEWAY | route NODE PREFIX dev IF"));
        return cmd_route6(n, &d6, plen, kw, arg, del, (int)nl_arg_int(a, "metric", 1));
    }
    if (!n || !pfx || !nl_parse_prefix(pfx, &dst, &plen))
        return fail(_("usage: route NODE [add|del] PREFIX via GATEWAY | route NODE PREFIX dev IF"));
    if (del) {
        nl_route_del(n, dst, plen, NL_RT_STATIC);
        return 0;
    }
    if (!kw || !arg)
        return fail(_("usage: route NODE [add|del] PREFIX via GATEWAY | route NODE PREFIX dev IF"));
    if (strcmp(kw, "dev") == 0) {
        ifx = nl_if_find(n, arg);
        if (ifx < 0)
            return fail(_("unknown interface '%s'"), arg);
    } else if (strchr(arg, ':')) {
        /* an IPv6 next hop for an IPv4 prefix: `via fe80::1 dev eth0`, or a global address on a shared prefix */
        const char *dev = NULL;
        nl_ip6 gw6;

        for (i = 3; i + 1 < a->argc; i++)
            if (!strcmp(a->argv[i], "dev"))
                dev = a->argv[i + 1];
        if (!nl_parse_ip6(arg, &gw6))
            return fail(_("unknown gateway '%s'"), arg);
        ifx = dev ? nl_if_find(n, dev) : onlink6(n, &gw6);
        if (ifx < 0)
            return fail(_("an IPv6 next hop needs `dev IF` unless it is on a prefix of %s"), n->name);
        nl_route_set_via6(n, dst, plen, &gw6, ifx, (int)nl_arg_int(a, "metric", 1), NL_RT_STATIC);
        return 0;
    } else {
        if (!nl_resolve(arg, &gw))
            return fail(_("unknown gateway '%s'"), arg);
        if (!nl_parse_ip(arg, &gw)) {
            /* a node name: its address on a shared subnet */
            nl_node *r = nl_node_find(arg);
            int k;
            for (i = 0; r && i < n->nif; i++)
                for (k = 0; k < r->nif; k++)
                    if (n->ifc[i].ip && r->ifc[k].ip &&
                        ((n->ifc[i].ip ^ r->ifc[k].ip) & nl_mask(n->ifc[i].plen)) == 0)
                        gw = r->ifc[k].ip;
        }
        for (i = 0; i < n->nif; i++)
            if (n->ifc[i].ip && ((n->ifc[i].ip ^ gw) & nl_mask(n->ifc[i].plen)) == 0)
                ifx = i;
        if (ifx < 0)
            return fail(_("gateway %s is not on a subnet of %s"), nl_ip_str(gw), n->name);
    }
    nl_route_set(n, dst, plen, gw, ifx, (int)nl_arg_int(a, "metric", 1), NL_RT_STATIC);
    return 0;
}

static int cmd_vlan(const nl_args *a)
{
    const char *who = nl_arg_pos(a, 1), *v = nl_arg_pos(a, 2);
    nl_node *n;
    int ifx;

    if (!who || !v)
        return fail(_("usage: vlan SWITCH:PORT N|trunk"));
    n = end_point(who, &ifx, 0);
    if (!n || ifx < 0)
        return fail(_("unknown port '%s'"), who);
    n->ifc[ifx].vlan = strcmp(v, "trunk") == 0 ? -1 : atoi(v);
    nl_fdb_flush(n);
    return 0;
}

static int split_hostport(const char *s, char *host, size_t hn, int *port)
{
    const char *colon = strrchr(s, ':');

    if (colon) {
        snprintf(host, hn, "%.*s", (int)(colon - s), s);
        *port = atoi(colon + 1);
    } else {
        snprintf(host, hn, "%s", s);
    }
    return host[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* Running modules                                                    */
/* ------------------------------------------------------------------ */

static int run_module(const char *node, const nl_args *a, int first)
{
    nl_node *n = nl_node_find(node);
    const char *mname = first < a->argc ? a->argv[first] : NULL;
    const nl_module *m = mname ? nl_module_find(mname) : NULL;
    nl_args sub;
    char line[1024];
    int i;
    size_t len = 0;

    if (!n)
        return fail(_("unknown node '%s'"), node);
    if (!m)
        return fail(_("unknown module '%s' (try: help)"), mname ? mname : "");
    if (!n->local)
        return 0;                       /* another process runs this node */
    if (!n->up)
        return fail(_("node %s is down"), n->name);
    line[0] = '\0';
    for (i = first + 1; i < a->argc; i++) {
        int quote = strchr(a->argv[i], ' ') != NULL;
        const char *eq = strchr(a->argv[i], '=');

        if (quote && eq && eq < strchr(a->argv[i], ' '))
            len += (size_t)snprintf(line + len, sizeof(line) - len, "%s%.*s\"%s\"", len ? " " : "",
                                    (int)(eq - a->argv[i] + 1), a->argv[i], eq + 1);
        else
            len += (size_t)snprintf(line + len, sizeof(line) - len, quote ? "%s\"%s\"" : "%s%s", len ? " " : "",
                                    a->argv[i]);
        if (len >= sizeof(line))
            break;
    }
    nl_args_parse(&sub, line);
    if (!m->run)
        return fail(_("module '%s' cannot be run"), m->name);
    return m->run(n, &sub) == 0 ? 0 : fail(_("%s on %s failed (see the log)"), m->name, n->name);
}

typedef struct { char *line; } deferred;

static void run_deferred(void *ctx)
{
    deferred *d = ctx;

    if (nl_exec(d->line) != 0)
        nl_log(NULL, NL_L_SIM, 1, 0, T_ERR "%s: %s", d->line, W.errmsg);
    free(d->line);
    free(d);
}

static int defer(nl_time when, const char *line)
{
    deferred *d = malloc(sizeof(*d));

    if (!d)
        return -1;
    d->line = strdup(line);
    nl_after(NULL, when - W.now, run_deferred, d);
    return 0;
}

/* Rest of the line starting at token k (tokens re-joined with spaces) */
static const char *rest_of(const char *line, int k)
{
    const char *s = line;

    while (*s == ' ' || *s == '\t')
        s++;
    while (k-- > 0) {
        if (*s == '"') {
            s++;
            while (*s && *s != '"')
                s++;
            if (*s)
                s++;
        } else {
            while (*s && *s != ' ' && *s != '\t')
                s++;
        }
        while (*s == ' ' || *s == '\t')
            s++;
    }
    return s;
}

static int cmd_help(const nl_args *a)
{
    const char *what = nl_arg_pos(a, 1);
    const nl_module *m;
    int i;

    if (what && (m = nl_module_find(what)) != NULL) {
        char buf[1200];
        char *line, *save = NULL;

        nl_log(NULL, NL_L_SIM, 1, 0, T_INFO T_BOLD "%s" T_RESET " — %s", m->name, _(m->title));
        snprintf(buf, sizeof(buf), "%s", m->usage ? _(m->usage) : "");
        for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
            nl_log(NULL, NL_L_SIM, 1, 0, "  %s", line);
        return 0;
    }
    nl_log(NULL, NL_L_SIM, 1, 0, T_INFO "%s", _("Commands: NODE MODULE args… · link A B up|down|key=value · "
           "node N up|down · stop NODE MODULE · set speed|detail N · help MODULE"));
    for (i = 0; (m = nl_module_at(i)) != NULL; i++)
        nl_log(NULL, NL_L_SIM, 1, 0, "  " T_BOLD "%-11s" T_RESET T_DIM "L%d" T_RESET " %s", m->name, m->layer,
               _(m->title));
    return 0;
}

/* ------------------------------------------------------------------ */
/* Metadata directives                                                */
/* ------------------------------------------------------------------ */

/* "about[it]" -> key "about", returns 1 when the line applies to the
 * current language, 0 when it must be skipped; *loc = localized variant */
static int lang_key(char *key, int *loc)
{
    char *br = strchr(key, '[');

    *loc = 0;
    if (!br)
        return 1;
    *br = '\0';
    *loc = 1;
    return strncmp(br + 1, nl_lang(), 2) == 0;
}

static nl_param *param_find(nl_meta *m, const char *name)
{
    int i;

    for (i = 0; i < m->nparams; i++)
        if (strcmp(m->params[i].name, name) == 0)
            return &m->params[i];
    return NULL;
}

/* Returns 1 when the directive was a metadata one */
static int meta_directive(const char *key, int loc, const nl_args *a, const char *line)
{
    nl_meta *m = g_meta;
    const char *text = rest_of(line, 1);

    if (strcmp(key, "title") == 0) {
        if (loc || !m->loc_title)
            snprintf(m->title, sizeof(m->title), "%s", text);
        m->loc_title |= loc;
    } else if (strcmp(key, "about") == 0) {
        size_t len;
        if (loc && !m->loc_about)
            m->about[0] = '\0';
        if (!loc && m->loc_about)
            return 1;
        m->loc_about |= loc;
        /* consecutive `about` lines continue the paragraph; an empty one ends it */
        len = strlen(m->about);
        if (!*text) {
            if (len && m->about[len - 1] != '\n')
                snprintf(m->about + len, sizeof(m->about) - len, "\n\n");
        } else {
            snprintf(m->about + len, sizeof(m->about) - len, "%s%s",
                     len && m->about[len - 1] != '\n' ? " " : "", text);
        }
    } else if (strcmp(key, "level") == 0) {
        m->level = atoi(text);
    } else if (strcmp(key, "layer") == 0) {
        m->layer = atoi(text);
    } else if (strcmp(key, "topic") == 0) {
        snprintf(m->topic, sizeof(m->topic), "%s", text);
    } else if (strcmp(key, "param") == 0 || strcmp(key, "param+") == 0) {
        const char *name = nl_arg_pos(a, 1);
        nl_param *p;

        if (!name)
            return 1;
        p = param_find(m, name);
        if (loc) {                      /* param[it] NAME label */
            if (p)
                snprintf(p->label, sizeof(p->label), "%s", rest_of(line, 2));
            return 1;
        }
        if (!p) {
            if (m->nparams >= (int)(sizeof(m->params) / sizeof(m->params[0])))
                return 1;
            p = &m->params[m->nparams++];
            memset(p, 0, sizeof(*p));
            snprintf(p->name, sizeof(p->name), "%s", name);
        }
        snprintf(p->def, sizeof(p->def), "%s", a->argc > 2 ? a->argv[2] : "");
        if (!p->value[0])
            snprintf(p->value, sizeof(p->value), "%s", p->def);
        if (!p->label[0])
            snprintf(p->label, sizeof(p->label), "%s", a->argc > 3 ? rest_of(line, 3) : name);
        p->advanced = key[5] == '+';
    } else {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* The interpreter                                                    */
/* ------------------------------------------------------------------ */

int nl_exec(const char *line0)
{
    nl_args a;
    char key[32];
    const char *line = line0;
    int loc, mine;

    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line || *line == '#')
        return 0;
    nl_args_parse(&a, line);
    if (a.argc == 0)
        return 0;
    snprintf(key, sizeof(key), "%s", a.argv[0]);
    mine = lang_key(key, &loc);

    if (g_loading) {
        if (!strcmp(key, "title") || !strcmp(key, "about") || !strcmp(key, "level") || !strcmp(key, "layer") ||
            !strcmp(key, "topic") || !strcmp(key, "param") || !strcmp(key, "param+")) {
            if (mine)
                meta_directive(key, loc, &a, line);
            return 0;
        }
        if (g_peek) {
            if (!strcmp(key, "node")) g_meta->nnodes++;
            if (!strcmp(key, "link")) g_meta->nlinks++;
            return 0;
        }
        if (!strcmp(key, "note")) {
            nl_time t;
            int i;

            if (!mine)
                return 0;
            if (a.argc < 3 || !nl_parse_time(a.argv[1], &t))
                return fail(_("usage: note TIME text"));
            for (i = 0; i < W.nnotes; i++)
                if (W.notes[i].t == t)
                    break;
            if (i < W.nnotes && W.notes[i].loc && !loc)
                return 0;
            if (i == W.nnotes) {
                if (W.nnotes >= (int)(sizeof(W.notes) / sizeof(W.notes[0])))
                    return 0;
                W.nnotes++;
            }
            W.notes[i].t = t;
            W.notes[i].loc = loc;
            snprintf(W.notes[i].text, sizeof(W.notes[i].text), "%s", rest_of(line, 2));
            return 0;
        }
        if (!strcmp(key, "duration"))
            return a.argc > 1 && nl_parse_time(a.argv[1], &W.duration) ? 0 : fail(_("usage: duration TIME"));
        if (!strcmp(key, "seed")) {
            if (a.argc > 1 && !W.seed)
                W.seed = strtoull(a.argv[1], NULL, 0);
            return 0;
        }
        if (!strcmp(key, "node"))   return cmd_node(&a);
        if (!strcmp(key, "vlan"))   return cmd_vlan(&a);
        if (!strcmp(key, "auto")) {
            const char *w = nl_arg_pos(&a, 1);
            if (!w || !strcmp(w, "all")) W.auto_addr = W.auto_routes = 1;
            else if (!strcmp(w, "addr")) W.auto_addr = 1;
            else if (!strcmp(w, "routes")) W.auto_routes = 1;
            else if (!strcmp(w, "addr6")) W.ip6 = W.auto_addr6 = 1;
            else if (!strcmp(w, "routes6")) W.ip6 = W.auto_routes6 = 1;
            else if (!strcmp(w, "all6")) W.ip6 = W.auto_addr6 = W.auto_routes6 = 1;
            else return fail(_("usage: auto addr|routes|all|addr6|routes6|all6"));
            return 0;
        }
        if (!strcmp(key, "ipv6")) {
            const char *w = nl_arg_pos(&a, 1);
            if (!w || (strcmp(w, "on") && strcmp(w, "off")))
                return fail(_("usage: ipv6 on|off"));
            W.ip6 = w[1] == 'n';
            return 0;
        }
        if (!strcmp(key, "place")) {
            nl_node *n = a.argc > 2 ? nl_node_find(a.argv[1]) : NULL;
            if (!n)
                return fail(_("usage: place NODE HOST[:PORT]"));
            split_hostport(a.argv[2], n->under_host, sizeof(n->under_host), &n->under_port);
            return 0;
        }
        if (!strcmp(key, "monitor")) {
            char host[64];
            int port = W.baseport - 1;
            if (a.argc < 2 || !split_hostport(a.argv[1], host, sizeof(host), &port))
                return fail(_("usage: monitor HOST[:PORT]"));
            if (W.mon_sock == -1 && rdt_resolve(host, port, &W.mon_addr) == 0)
                W.mon_sock = -2;
            return 0;
        }
        if (!strcmp(key, "expect")) {
            nl_expect *e;
            if (a.argc < 5)
                return fail(_("usage: expect NODE COUNTER ==|!=|>=|<=|>|< VALUE [if A ==|!= B]"));
            /* `if $param == value`: the expectation only applies to that configuration */
            if (a.argc >= 9 && !strcmp(a.argv[5], "if")) {
                int same = strcmp(a.argv[6], a.argv[8]) == 0;
                if (strcmp(a.argv[7], "==") && strcmp(a.argv[7], "!="))
                    return fail(_("usage: expect NODE COUNTER ==|!=|>=|<=|>|< VALUE [if A ==|!= B]"));
                if (same != (a.argv[7][0] == '='))
                    return 0;
            } else if (a.argc > 5) {
                return fail(_("usage: expect NODE COUNTER ==|!=|>=|<=|>|< VALUE [if A ==|!= B]"));
            }
            if (W.nexpects >= (int)(sizeof(W.expects) / sizeof(W.expects[0])))
                return 0;
            e = &W.expects[W.nexpects++];
            snprintf(e->node, sizeof(e->node), "%s", a.argv[1]);
            snprintf(e->name, sizeof(e->name), "%s", a.argv[2]);
            snprintf(e->op, sizeof(e->op), "%s", a.argv[3]);
            e->v = atof(a.argv[4]);
            return 0;
        }
        if (!strcmp(key, "run"))
            return defer(0, line);
    }

    if (!strcmp(key, "at")) {
        nl_time t;
        if (a.argc < 3 || !nl_parse_time(a.argv[1], &t))
            return fail(_("usage: at TIME command"));
        return defer(g_loading ? t : W.now + t, rest_of(line, 2));
    }
    if (!strcmp(key, "run")) {
        if (a.argc < 3)
            return fail(_("usage: run NODE MODULE [args]"));
        return run_module(a.argv[1], &a, 2);
    }
    if (!strcmp(key, "stop")) {
        nl_node *n = a.argc > 2 ? nl_node_find(a.argv[1]) : NULL;
        const nl_module *m = a.argc > 2 ? nl_module_find(a.argv[2]) : NULL;
        if (!n || !m)
            return fail(_("usage: stop NODE MODULE"));
        if (n->local && m->stop && nl_mod_get(n, m))
            m->stop(n);
        return 0;
    }
    if (!strcmp(key, "link"))  return cmd_link(&a);
    if (!strcmp(key, "ip"))    return cmd_ip(&a);
    if (!strcmp(key, "gw"))    return cmd_gw(&a);
    if (!strcmp(key, "route")) return cmd_route(&a);
    if (!strcmp(key, "help"))  return cmd_help(&a);
    if (!strcmp(key, "node")) {
        nl_node *n = a.argc > 2 ? nl_node_find(a.argv[1]) : NULL;
        if (!n || (strcmp(a.argv[2], "up") && strcmp(a.argv[2], "down")))
            return fail(_("usage: node NAME up|down"));
        if (n->local)
            nl_node_set_up(n, a.argv[2][0] == 'u');
        return 0;
    }
    if (!strcmp(key, "set")) {
        const char *k = nl_arg_pos(&a, 1), *v = nl_arg_pos(&a, 2);
        if (!k || !v)
            return fail(_("usage: set speed|detail|baseport VALUE"));
        if (!strcmp(k, "speed"))         { W.speed = atof(v) > 0 ? atof(v) : 1.0; W.speed_set = 1; }
        else if (!strcmp(k, "detail"))   W.detail = atoi(v) < 1 ? 1 : atoi(v) > 3 ? 3 : atoi(v);
        else if (!strcmp(k, "baseport")) { if (!W.baseport_fixed) W.baseport = atoi(v); }
        else return fail(_("unknown setting '%s'"), k);
        return 0;
    }
    if (!strcmp(key, "log")) {
        nl_log(NULL, NL_L_SIM, 1, 0, T_INFO "%s", rest_of(line, 1));
        return 0;
    }
    /* "h1 ping h2" is short for "run h1 ping h2" */
    if (nl_node_find(key) && a.argc >= 2)
        return g_loading ? defer(0, line) : run_module(key, &a, 1);
    return fail(_("unknown command '%s' (try: help)"), key);
}

/* ------------------------------------------------------------------ */
/* Loading                                                            */
/* ------------------------------------------------------------------ */

/* Replace $name and ${name} with parameter values */
static void substitute(const nl_meta *m, const char *in, char *out, size_t n)
{
    size_t k = 0;

    while (*in && k + 1 < n) {
        if (*in == '$' && (isalpha((unsigned char)in[1]) || in[1] == '{')) {
            const char *s = in + 1, *e;
            char name[24];
            int i, brace = *s == '{';

            if (brace)
                s++;
            for (e = s; isalnum((unsigned char)*e) || *e == '_'; e++)
                ;
            snprintf(name, sizeof(name), "%.*s", (int)(e - s), s);
            for (i = 0; i < m->nparams; i++)
                if (strcmp(m->params[i].name, name) == 0)
                    break;
            if (i < m->nparams) {
                k += (size_t)snprintf(out + k, n - k, "%s", m->params[i].value);
                if (k >= n)
                    k = n - 1;
                in = brace && *e == '}' ? e + 1 : e;
                continue;
            }
        }
        out[k++] = *in++;
    }
    out[k] = '\0';
}

static int load_file(const char *path, nl_meta *meta, int peek, int argc, char **argv)
{
    FILE *f = fopen(path, "r");
    char raw[2048], line[2048];
    int lineno = 0, errors = 0, i;

    if (!f) {
        fprintf(stderr, "hoplab: %s: %s\n", path, _("cannot open scenario"));
        return -1;
    }
    memset(meta, 0, sizeof(*meta));
    snprintf(meta->file, sizeof(meta->file), "%s", path);
    /* command-line overrides come first: `param` keeps a value already set */
    for (i = 0; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        nl_param *p;

        if (!eq || meta->nparams >= (int)(sizeof(meta->params) / sizeof(meta->params[0])))
            continue;
        p = &meta->params[meta->nparams++];
        snprintf(p->name, sizeof(p->name), "%.*s", (int)(eq - argv[i]), argv[i]);
        snprintf(p->value, sizeof(p->value), "%s", eq + 1);
    }
    g_loading = 1;
    g_peek = peek;
    g_meta = meta;
    while (fgets(raw, sizeof(raw), f)) {
        size_t len = strlen(raw);

        lineno++;
        while (len && (raw[len - 1] == '\n' || raw[len - 1] == '\r' || raw[len - 1] == ' '))
            raw[--len] = '\0';
        substitute(meta, raw, line, sizeof(line));
        if (nl_exec(line) != 0) {
            if (!peek)
                fprintf(stderr, "%s:%d: %s\n", path, lineno, W.errmsg);
            errors++;
        }
    }
    fclose(f);
    g_loading = g_peek = 0;
    g_meta = &W.m;
    if (!meta->title[0]) {
        const char *base = strrchr(path, '/');
        snprintf(meta->title, sizeof(meta->title), "%s", base ? base + 1 : path);
    }
    return errors ? -1 : 0;
}

int nl_scenario_peek(const char *path, nl_meta *out)
{
    return load_file(path, out, 1, 0, NULL);
}

int nl_scenario_load(const char *path, int argc, char **argv)
{
    int rc = load_file(path, &W.m, 0, argc, argv);

    if (rc == 0)
        nl_scenario_finalize();
    return rc;
}

/* ------------------------------------------------------------------ */
/* Automatic addressing, routing and layout                           */
/* ------------------------------------------------------------------ */

static int g_dom[NL_MAX_NODES][NL_MAX_IF];      /* broadcast domain of every interface */

static int dom_find(int *parent, int i)
{
    while (parent[i] != i)
        i = parent[i] = parent[parent[i]];
    return i;
}

static void compute_domains(void)
{
    static int parent[NL_MAX_NODES * NL_MAX_IF];
    int i, k;

    for (i = 0; i < NL_MAX_NODES * NL_MAX_IF; i++)
        parent[i] = i;
    for (i = 0; i < W.nlinks; i++) {
        nl_link *l = &W.links[i];
        int a = dom_find(parent, l->n[0]->idx * NL_MAX_IF + l->ifx[0]);
        int b = dom_find(parent, l->n[1]->idx * NL_MAX_IF + l->ifx[1]);
        parent[a] = b;
    }
    for (i = 0; i < W.nnodes; i++) {
        nl_node *n = &W.nodes[i];
        int j;
        if (n->kind != NL_SWITCH)
            continue;
        /* ports of a switch in the same VLAN share the domain */
        for (k = 0; k < n->nif; k++)
            for (j = k + 1; j < n->nif; j++)
                if (n->ifc[k].vlan == n->ifc[j].vlan && n->ifc[k].vlan >= 0)
                    parent[dom_find(parent, i * NL_MAX_IF + k)] = dom_find(parent, i * NL_MAX_IF + j);
    }
    for (i = 0; i < W.nnodes; i++)
        for (k = 0; k < W.nodes[i].nif; k++)
            g_dom[i][k] = dom_find(parent, i * NL_MAX_IF + k);
}

static void auto_addr(void)
{
    int subnet = 1, li, i, k;

    for (li = 0; li < W.nlinks; li++) {
        nl_link *l = &W.links[li];
        int d = g_dom[l->n[0]->idx][l->ifx[0]], any = 0, members = 0, r = 1, h = 10;

        for (i = 0; i < W.nnodes; i++)
            for (k = 0; k < W.nodes[i].nif; k++)
                if (g_dom[i][k] == d && W.nodes[i].kind != NL_SWITCH && W.nodes[i].ifc[k].link) {
                    members++;
                    if (W.nodes[i].ifc[k].ip)
                        any = 1;
                }
        if (any || members < 2)
            continue;
        for (i = 0; i < W.nnodes; i++)
            for (k = 0; k < W.nodes[i].nif; k++)
                if (g_dom[i][k] == d && W.nodes[i].kind != NL_SWITCH && W.nodes[i].ifc[k].link)
                    nl_if_set_addr(&W.nodes[i], k,
                                   NL_IP(10, 0, subnet, W.nodes[i].forward ? r++ : h++), 24);
        subnet++;
    }
    /* default gateway of hosts: the first router on one of their subnets */
    for (i = 0; i < W.nnodes; i++) {
        nl_node *n = &W.nodes[i];
        int j, m, done = 0;

        if (n->forward || n->kind == NL_SWITCH || nl_route_lookup(n, NL_IP(198, 51, 100, 1)))
            continue;
        for (k = 0; k < n->nif && !done; k++)
            for (j = 0; j < W.nnodes && !done; j++)
                for (m = 0; m < W.nodes[j].nif && !done; m++)
                    if (W.nodes[j].forward && n->ifc[k].ip && W.nodes[j].ifc[m].ip && g_dom[j][m] == g_dom[i][k]) {
                        nl_route_set(n, 0, 0, W.nodes[j].ifc[m].ip, k, 1, NL_RT_STATIC);
                        done = 1;
                    }
    }
}

/* Static shortest-path routes computed with global knowledge, for labs
 * where routing is not the subject */
static void auto_routes(void)
{
    int s, i, k, j, m;

    for (s = 0; s < W.nnodes; s++) {
        nl_node *src = &W.nodes[s];
        int dist[NL_MAX_NODES], fif[NL_MAX_NODES], queue[NL_MAX_NODES], qh = 0, qt = 0;
        nl_ip fgw[NL_MAX_NODES];

        if (src->kind == NL_SWITCH)
            continue;
        for (i = 0; i < W.nnodes; i++)
            dist[i] = -1;
        dist[s] = 0;
        queue[qt++] = s;
        while (qh < qt) {
            int u = queue[qh++];
            nl_node *nu = &W.nodes[u];

            if (u != s && !nu->forward)
                continue;
            for (k = 0; k < nu->nif; k++) {
                if (!nu->ifc[k].ip)
                    continue;
                for (j = 0; j < W.nnodes; j++)
                    for (m = 0; m < W.nodes[j].nif; m++)
                        if (dist[j] < 0 && W.nodes[j].ifc[m].ip && g_dom[j][m] == g_dom[u][k] &&
                            W.nodes[j].kind != NL_SWITCH) {
                            dist[j] = dist[u] + 1;
                            fif[j] = u == s ? k : fif[u];
                            fgw[j] = u == s ? W.nodes[j].ifc[m].ip : fgw[u];
                            queue[qt++] = j;
                        }
            }
        }
        for (j = 0; j < W.nnodes; j++) {
            if (j == s || dist[j] < 0)
                continue;
            for (m = 0; m < W.nodes[j].nif; m++) {
                const nl_iface *f = &W.nodes[j].ifc[m];
                const nl_route *have;
                nl_ip net = f->ip & nl_mask(f->plen);
                int z, known = 0;

                if (!f->ip)
                    continue;
                for (z = 0; z < src->nrt; z++) {
                    have = &src->rt[z];
                    if (have->dst == net && have->plen == f->plen &&
                        (have->proto == NL_RT_CONNECTED || have->metric <= dist[j]))
                        known = 1;
                }
                if (!known)
                    nl_route_set(src, net, f->plen, fgw[j], fif[j], dist[j], NL_RT_STATIC);
            }
        }
    }
}

/* IPv6 twins of the two functions above: 2001:db8:N::/64 per subnet,
 * routers ::1, ::2, ..., hosts ::10, ::11, ... (hexadecimal) */
static void auto_addr6(void)
{
    int subnet = 1, li, i, k;

    for (li = 0; li < W.nlinks; li++) {
        nl_link *l = &W.links[li];
        int d = g_dom[l->n[0]->idx][l->ifx[0]], any = 0, members = 0, r = 1, h = 0x10;

        for (i = 0; i < W.nnodes; i++)
            for (k = 0; k < W.nodes[i].nif; k++)
                if (g_dom[i][k] == d && W.nodes[i].kind != NL_SWITCH && W.nodes[i].ifc[k].link) {
                    members++;
                    if (global6(&W.nodes[i], k))
                        any = 1;
                }
        if (any || members < 2)
            continue;
        for (i = 0; i < W.nnodes; i++)
            for (k = 0; k < W.nodes[i].nif; k++)
                if (g_dom[i][k] == d && W.nodes[i].kind != NL_SWITCH && W.nodes[i].ifc[k].link) {
                    nl_ip6 a = { { 0x20, 0x01, 0x0d, 0xb8 } };
                    nl_put16(a.b + 4, (uint16_t)subnet);
                    nl_put16(a.b + 14, (uint16_t)(W.nodes[i].forward ? r++ : h++));
                    nl_if_add_addr6(&W.nodes[i], k, &a, 64);
                }
        subnet++;
    }
    for (i = 0; i < W.nnodes; i++) {
        nl_node *n = &W.nodes[i];
        nl_ip6 far = { { 0x20, 0x01, 0x0d, 0xb8, 0xff, 0xff } };
        int j, m, done = 0;

        if (n->forward || n->kind == NL_SWITCH || nl_route6_lookup(n, &far))
            continue;
        for (k = 0; k < n->nif && !done; k++)
            for (j = 0; j < W.nnodes && !done; j++)
                for (m = 0; m < W.nodes[j].nif && !done; m++)
                    if (W.nodes[j].forward && global6(n, k) && global6(&W.nodes[j], m) && g_dom[j][m] == g_dom[i][k]) {
                        nl_route6_set(n, &(nl_ip6){ { 0 } }, 0, global6(&W.nodes[j], m), k, 1, NL_RT_STATIC);
                        done = 1;
                    }
    }
}

static void auto_routes6(void)
{
    int s, i, k, j, m;

    for (s = 0; s < W.nnodes; s++) {
        nl_node *src = &W.nodes[s];
        int dist[NL_MAX_NODES], fif[NL_MAX_NODES], queue[NL_MAX_NODES], qh = 0, qt = 0;
        nl_ip6 fgw[NL_MAX_NODES];

        if (src->kind == NL_SWITCH)
            continue;
        for (i = 0; i < W.nnodes; i++)
            dist[i] = -1;
        dist[s] = 0;
        queue[qt++] = s;
        while (qh < qt) {
            int u = queue[qh++];
            nl_node *nu = &W.nodes[u];

            if (u != s && !nu->forward)
                continue;
            for (k = 0; k < nu->nif; k++) {
                if (!global6(nu, k))
                    continue;
                for (j = 0; j < W.nnodes; j++)
                    for (m = 0; m < W.nodes[j].nif; m++)
                        if (dist[j] < 0 && global6(&W.nodes[j], m) && g_dom[j][m] == g_dom[u][k] &&
                            W.nodes[j].kind != NL_SWITCH) {
                            dist[j] = dist[u] + 1;
                            fif[j] = u == s ? k : fif[u];
                            fgw[j] = u == s ? *global6(&W.nodes[j], m) : fgw[u];
                            queue[qt++] = j;
                        }
            }
        }
        for (j = 0; j < W.nnodes; j++) {
            if (j == s || dist[j] < 0)
                continue;
            for (m = 0; m < W.nodes[j].nif; m++) {
                const nl_ip6 *g = global6(&W.nodes[j], m);
                int z, known = 0;

                if (!g)
                    continue;
                for (z = 0; z < src->nrt6; z++) {
                    const nl_route6 *have = &src->rt6[z];
                    if (have->plen == 64 && nl_ip6_match(g, &have->dst, 64) &&
                        (have->proto == NL_RT_CONNECTED || have->metric <= dist[j]))
                        known = 1;
                }
                if (!known)
                    nl_route6_set(src, g, 64, &fgw[j], fif[j], dist[j], NL_RT_STATIC);
            }
        }
    }
}

static void auto_layout(void)
{
    double x[NL_MAX_NODES], y[NL_MAX_NODES], dx[NL_MAX_NODES], dy[NL_MAX_NODES];
    double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9, kk, temp = 20.0;
    int n = W.nnodes, i, j, it, todo = 0;

    for (i = 0; i < n; i++)
        if (!W.nodes[i].placed)
            todo++;
    if (!todo || n == 0)
        return;
    kk = 90.0 / sqrt((double)n);
    for (i = 0; i < n; i++) {
        x[i] = W.nodes[i].placed ? W.nodes[i].x : 50 + 40 * cos(2 * M_PI * i / n);
        y[i] = W.nodes[i].placed ? W.nodes[i].y : 50 + 40 * sin(2 * M_PI * i / n);
    }
    for (it = 0; it < 300; it++) {
        for (i = 0; i < n; i++) {
            dx[i] = dy[i] = 0;
            for (j = 0; j < n; j++) {
                double ddx = x[i] - x[j], ddy = y[i] - y[j], d = sqrt(ddx * ddx + ddy * ddy) + 0.01;
                if (i == j)
                    continue;
                dx[i] += ddx / d * kk * kk / d;
                dy[i] += ddy / d * kk * kk / d;
            }
        }
        for (j = 0; j < W.nlinks; j++) {
            int a = W.links[j].n[0]->idx, b = W.links[j].n[1]->idx;
            double ddx = x[a] - x[b], ddy = y[a] - y[b], d = sqrt(ddx * ddx + ddy * ddy) + 0.01;
            double f = d * d / kk;
            dx[a] -= ddx / d * f; dy[a] -= ddy / d * f;
            dx[b] += ddx / d * f; dy[b] += ddy / d * f;
        }
        for (i = 0; i < n; i++) {
            double d = sqrt(dx[i] * dx[i] + dy[i] * dy[i]) + 0.01, lim = d < temp ? d : temp;
            if (W.nodes[i].placed)
                continue;
            x[i] += dx[i] / d * lim;
            y[i] += dy[i] / d * lim;
        }
        temp *= 0.985;
    }
    if (todo < n) {                     /* mixed: keep the manual frame */
        for (i = 0; i < n; i++)
            if (!W.nodes[i].placed) {
                W.nodes[i].x = x[i] < 3 ? 3 : x[i] > 97 ? 97 : x[i];
                W.nodes[i].y = y[i] < 3 ? 3 : y[i] > 97 ? 97 : y[i];
            }
        return;
    }
    {   /* turn the drawing so that its longest direction lies horizontally */
        double mx = 0, my = 0, sxx = 0, syy = 0, sxy = 0, ang, ca, sa;
        for (i = 0; i < n; i++) { mx += x[i] / n; my += y[i] / n; }
        for (i = 0; i < n; i++) {
            sxx += (x[i] - mx) * (x[i] - mx);
            syy += (y[i] - my) * (y[i] - my);
            sxy += (x[i] - mx) * (y[i] - my);
        }
        ang = 0.5 * atan2(2 * sxy, sxx - syy);
        ca = cos(-ang);
        sa = sin(-ang);
        for (i = 0; i < n; i++) {
            double rx = (x[i] - mx) * ca - (y[i] - my) * sa, ry = (x[i] - mx) * sa + (y[i] - my) * ca;
            x[i] = rx;
            y[i] = ry;
        }
        /* the first node of the file goes on the left, as one reads */
        if (x[0] > 0)
            for (i = 0; i < n; i++)
                x[i] = -x[i];
    }
    for (i = 0; i < n; i++) {
        if (x[i] < minx) minx = x[i];
        if (x[i] > maxx) maxx = x[i];
        if (y[i] < miny) miny = y[i];
        if (y[i] > maxy) maxy = y[i];
    }
    for (i = 0; i < n; i++) {
        W.nodes[i].x = maxx - minx > 1 ? 6 + 88 * (x[i] - minx) / (maxx - minx) : 50;
        W.nodes[i].y = maxy - miny > 1 ? 8 + 84 * (y[i] - miny) / (maxy - miny) : 50;
    }
}

void nl_scenario_finalize(void)
{
    int i;

    compute_domains();
    if (W.auto_addr)
        auto_addr();
    if (W.auto_routes)
        auto_routes();
    if (W.auto_addr6)
        auto_addr6();
    if (W.auto_routes6)
        auto_routes6();
    nl_ip6_start();
    auto_layout();
    for (i = 0; i < W.nnodes; i++)
        if (!W.nodes[i].under_port)
            W.nodes[i].under_port = W.baseport + i;
}

int nl_expect_check(int print)
{
    int i, failed = 0;

    for (i = 0; i < W.nexpects; i++) {
        const nl_expect *e = &W.expects[i];
        const nl_node *n = nl_node_find(e->node);
        double v = n ? nl_count_get(n, e->name) : 0;
        int ok = n != NULL;

        if (!strcmp(e->op, "=="))      ok = ok && v == e->v;
        else if (!strcmp(e->op, "!=")) ok = ok && v != e->v;
        else if (!strcmp(e->op, ">=")) ok = ok && v >= e->v;
        else if (!strcmp(e->op, "<=")) ok = ok && v <= e->v;
        else if (!strcmp(e->op, ">"))  ok = ok && v > e->v;
        else if (!strcmp(e->op, "<"))  ok = ok && v < e->v;
        else ok = 0;
        if (n && !n->local && W.mode != NL_MODE_RUN)
            continue;                   /* checked by the process running the node */
        if (!ok)
            failed++;
        if (print)
            printf("  %s  %s %s %s %g   (%s %g)\n", ok ? "ok  " : "FAIL", e->node, e->name, e->op, e->v,
                   _("got"), v);
    }
    return failed;
}
