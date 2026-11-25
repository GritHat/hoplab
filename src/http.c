/*
 * http.c - HTTP/1.1 over TCP: a small server (generated pages and objects of
 *          any size) and a client that fetches a page and, optionally, the
 *          objects it embeds, over one persistent connection or one
 *          connection per object.
 *
 * Simplifications: only GET, Content-Length bodies (no chunked encoding),
 * no pipelining, no TLS, no caching headers.
 */
#include "nl_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_COLOR   216
#define MAX_PATHS    12
#define MAX_CLIENTS  4

typedef struct http_node http_node;

typedef struct {                        /* one connection accepted by the server */
    http_node *hn;
    char       req[1024];
    int        rlen;
    long long  body_left;
    int        close_after;
} srv_conn;

typedef struct {                        /* one `http get` */
    http_node *hn;
    int        used, done;
    nl_ip      ip;
    nl_ip6     ip6;
    int        v6;                      /* the request goes over IPv6 */
    int        port, keepalive, embedded;
    char       host[64];
    char       paths[MAX_PATHS][64];
    int        npaths, cur;
    nl_tcp    *conn;
    char       hdr[1024];
    int        hlen, in_body, status, is_html;
    long long  clen, got, total;
    char       body[4096];
    int        blen;
    nl_time    t0, t_req;
    int        nconn, nreq;
} client;

struct http_node {
    nl_node *n;
    int      serving, port, objects;
    long long objsize;
    unsigned served, served_404;
    client   cl[MAX_CLIENTS];
};

static const nl_module http_mod;

/* ------------------------------------------------------------------ */
/* Server                                                             */
/* ------------------------------------------------------------------ */

static void srv_pump(nl_tcp *c, srv_conn *s)
{
    static uint8_t pat[4096];
    int i, w;

    if (!pat[0])
        for (i = 0; i < (int)sizeof(pat); i++)
            pat[i] = (uint8_t)("netlab "[i % 7]);
    while (s->body_left > 0) {
        w = nl_tcp_write(c, pat, s->body_left < (long long)sizeof(pat) ? (int)s->body_left : (int)sizeof(pat));
        if (w <= 0)
            return;
        s->body_left -= w;
    }
    if (s->close_after) {
        s->close_after = 0;
        nl_tcp_close(c);
    }
}

static void srv_request(nl_tcp *c, srv_conn *s, const char *path)
{
    http_node *hn = s->hn;
    char head[300], page[1500];
    const char *p = path + 1;
    long long size = -1;
    nl_ip rip;
    nl_ip6 rip6;
    char peer[64];
    uint16_t rport;
    int html = 0, i;
    size_t k = 0;

    if (nl_tcp_family(c) == 6) {
        nl_tcp_peer6(c, &rip6, &rport);
        snprintf(peer, sizeof(peer), "[%s]", nl_ip6_str(&rip6));
    } else {
        nl_tcp_peer(c, &rip, &rport);
        snprintf(peer, sizeof(peer), "%s", nl_ip_str(rip));
    }
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        html = 1;
        k += (size_t)snprintf(page + k, sizeof(page) - k,
                              "<html><head><title>%s</title></head>\n<body>\n<h1>Hello from %s</h1>\n",
                              hn->n->name, hn->n->name);
        for (i = 0; i < hn->objects && k < sizeof(page) - 80; i++)
            k += (size_t)snprintf(page + k, sizeof(page) - k, "<img src=\"/%lld\">\n", hn->objsize + i);
        k += (size_t)snprintf(page + k, sizeof(page) - k, "</body></html>\n");
        size = (long long)k;
    } else if (isdigit((unsigned char)*p)) {
        size = 0;
        while (isdigit((unsigned char)*p))
            size = size * 10 + (*p++ - '0');
        if (*p)
            size = -1;
    }
    if (size < 0) {
        static const char nf[] = "<html><body><h1>404 Not Found</h1></body></html>\n";

        hn->served_404++;
        nl_count(hn->n, "http.served_404", 1);
        nl_log(hn->n, NL_L_APP, 2, 0, T_FIN "HTTP" T_RESET " %s:%u GET %s → " T_ERR "404 Not Found", peer,
               rport, path);
        snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nServer: netlab\r\nContent-Type: text/html\r\n"
                 "Content-Length: %d\r\nConnection: %s\r\n\r\n", (int)sizeof(nf) - 1,
                 s->close_after ? "close" : "keep-alive");
        nl_tcp_write(c, head, (int)strlen(head));
        nl_tcp_write(c, nf, (int)sizeof(nf) - 1);
        srv_pump(c, s);
        return;
    }
    hn->served++;
    nl_count(hn->n, "http.served", 1);
    nl_log(hn->n, NL_L_APP, 2, 0, T_FIN "HTTP" T_RESET " %s:%u GET %s → " T_OK "200 OK" T_RESET " (%lld B)",
           peer, rport, path, size);
    snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nServer: netlab\r\nContent-Type: %s\r\n"
             "Content-Length: %lld\r\nConnection: %s\r\n\r\n", html ? "text/html" : "application/octet-stream",
             size, s->close_after ? "close" : "keep-alive");
    nl_tcp_write(c, head, (int)strlen(head));
    if (html) {
        nl_tcp_write(c, page, (int)k);
        s->body_left = 0;
    } else {
        s->body_left = size;
    }
    srv_pump(c, s);
}

static void *srv_accept(nl_tcp *c, void *lctx)
{
    srv_conn *s = calloc(1, sizeof(*s));

    if (!s) {
        nl_tcp_abort(c);
        return NULL;
    }
    s->hn = lctx;
    return s;
}

static void srv_data(nl_tcp *c, void *ctx, const uint8_t *d, int len)
{
    srv_conn *s = ctx;
    char *end, path[128], *lf;

    if (!s)
        return;
    if (len > (int)sizeof(s->req) - 1 - s->rlen)
        len = (int)sizeof(s->req) - 1 - s->rlen;
    memcpy(s->req + s->rlen, d, (size_t)len);
    s->rlen += len;
    s->req[s->rlen] = '\0';
    while ((end = strstr(s->req, "\r\n\r\n")) != NULL && s->body_left == 0) {
        int used = (int)(end - s->req) + 4, ok;

        *end = '\0';
        ok = sscanf(s->req, "GET %127s", path) == 1;
        /* HTTP/1.1 is persistent unless told otherwise */
        s->close_after = !ok || strstr(s->req, "Connection: close") != NULL || strstr(s->req, "HTTP/1.0") != NULL;
        lf = strchr(s->req, '\r');
        if (lf)
            *lf = '\0';
        if (!ok) {
            static const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            nl_tcp_write(c, bad, (int)sizeof(bad) - 1);
            nl_tcp_close(c);
            return;
        }
        memmove(s->req, s->req + used, (size_t)(s->rlen - used) + 1);
        s->rlen -= used;
        srv_request(c, s, path);
    }
}

static void srv_sent(nl_tcp *c, void *ctx, int acked)
{
    (void)acked;
    if (ctx)
        srv_pump(c, ctx);
}

static void srv_eof(nl_tcp *c, void *ctx)
{
    (void)ctx;
    nl_tcp_close(c);
}

static void srv_close(nl_tcp *c, void *ctx, int reason)
{
    (void)c;
    (void)reason;
    free(ctx);
}

static const nl_tcp_ops srv_ops = {
    .on_accept = srv_accept, .on_data = srv_data, .on_sent = srv_sent, .on_eof = srv_eof, .on_close = srv_close,
};

/* ------------------------------------------------------------------ */
/* Client                                                             */
/* ------------------------------------------------------------------ */

static const nl_tcp_ops cl_ops;

static void cl_send_request(client *s)
{
    char req[300];

    s->hlen = 0;
    s->in_body = 0;
    s->got = s->clen = 0;
    s->blen = 0;
    s->t_req = nl_now();
    s->nreq++;
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: netlab\r\nConnection: %s\r\n\r\n",
             s->paths[s->cur], s->host, s->keepalive ? "keep-alive" : "close");
    nl_count(s->hn->n, "http.requests", 1);
    nl_log(s->hn->n, NL_L_APP, 2, 0, T_FIN "HTTP" T_RESET " → %s  GET %s", s->host, s->paths[s->cur]);
    nl_tcp_write(s->conn, req, (int)strlen(req));
}

static void cl_open(client *s)
{
    s->nconn++;
    nl_count(s->hn->n, "http.connections", 1);
    s->conn = s->v6 ? nl_tcp_connect6(s->hn->n, &s->ip6, (uint16_t)s->port, &cl_ops, s, NULL)
                    : nl_tcp_connect(s->hn->n, s->ip, (uint16_t)s->port, &cl_ops, s, NULL);
    if (!s->conn) {
        s->done = 1;
        nl_count(s->hn->n, "http.failed", 1);
    }
}

static void cl_finish(client *s)
{
    double ms = (double)(nl_now() - s->t0) / 1000.0;
    nl_tcp *c = s->conn;

    s->done = 1;
    s->conn = NULL;
    nl_log(s->hn->n, NL_L_APP, 1, 0, T_OK "http %s%s: %s" T_RESET " %d %s, %lld B, %.1f ms (%d %s)", s->host,
           s->paths[0], _("complete"), s->nreq, s->nreq == 1 ? _("object") : _("objects"), s->total, ms,
           s->nconn, s->nconn == 1 ? _("TCP connection") : _("TCP connections"));
    nl_count(s->hn->n, "http.pages", 1);
    nl_count_set(s->hn->n, "http.time_ms", ms);
    if (c)
        nl_tcp_close(c);
}

/* Find src="..." references in the HTML we kept */
static void cl_parse_html(client *s)
{
    const char *p = s->body;

    s->body[s->blen] = '\0';
    while ((p = strstr(p, "src=\"")) != NULL && s->npaths < MAX_PATHS) {
        const char *e = strchr(p + 5, '"');
        if (!e)
            break;
        snprintf(s->paths[s->npaths++], sizeof(s->paths[0]), "%.*s", (int)(e - p - 5), p + 5);
        p = e;
    }
}

static void cl_object_done(client *s)
{
    nl_node *n = s->hn->n;
    double ms = (double)(nl_now() - s->t_req) / 1000.0;

    s->total += s->got;
    nl_count(n, "http.bytes", (double)s->got);
    if (s->status == 200)
        nl_count(n, "http.ok", 1);
    else
        nl_count(n, "http.errors", 1);
    nl_log(n, NL_L_APP, s->cur == 0 ? 1 : 2, 0, "%shttp %s%s:" T_RESET " %d %s, %lld B, %.1f ms",
           s->status == 200 ? T_OK : T_ERR, s->host, s->paths[s->cur], s->status,
           s->status == 200 ? "OK" : s->status == 404 ? "Not Found" : "", s->got, ms);
    if (s->cur == 0 && s->is_html && s->embedded && s->status == 200)
        cl_parse_html(s);
    s->cur++;
    if (s->cur >= s->npaths) {
        cl_finish(s);
    } else if (s->keepalive) {
        cl_send_request(s);
    } else {
        /* one connection per object: close this one, open the next */
        nl_tcp *old = s->conn;
        s->conn = NULL;
        nl_tcp_close(old);
        cl_open(s);
    }
}

static void cl_connect(nl_tcp *c, void *ctx)
{
    client *s = ctx;

    if (c == s->conn && !s->done)
        cl_send_request(s);
}

static void cl_data(nl_tcp *c, void *ctx, const uint8_t *d, int len)
{
    client *s = ctx;

    if (c != s->conn || s->done)
        return;
    while (len > 0 && c == s->conn && !s->done) {
        if (!s->in_body) {
            char *end, *cl;
            int take = len < (int)sizeof(s->hdr) - 1 - s->hlen ? len : (int)sizeof(s->hdr) - 1 - s->hlen;
            int before = s->hlen, used;

            if (take <= 0) {
                nl_tcp_abort(c);
                s->done = 1;
                return;
            }
            memcpy(s->hdr + s->hlen, d, (size_t)take);
            s->hlen += take;
            s->hdr[s->hlen] = '\0';
            end = strstr(s->hdr, "\r\n\r\n");
            if (!end) {
                d += take;
                len -= take;
                continue;
            }
            used = (int)(end - s->hdr) + 4 - before;    /* bytes of this chunk that were header */
            d += used;
            len -= used;
            *end = '\0';
            s->status = 0;
            sscanf(s->hdr, "HTTP/%*s %d", &s->status);
            cl = strstr(s->hdr, "Content-Length:");
            s->clen = cl ? atoll(cl + 15) : 0;
            s->is_html = strstr(s->hdr, "text/html") != NULL;
            s->in_body = 1;
        } else {
            long long want = s->clen - s->got;
            int take = (long long)len < want ? len : (int)want;

            if (s->blen < (int)sizeof(s->body) - 1) {
                int keep = take < (int)sizeof(s->body) - 1 - s->blen ? take : (int)sizeof(s->body) - 1 - s->blen;
                memcpy(s->body + s->blen, d, (size_t)keep);
                s->blen += keep;
            }
            s->got += take;
            d += take;
            len -= take;
        }
        if (s->in_body && s->got >= s->clen)
            cl_object_done(s);
    }
}

static void cl_eof(nl_tcp *c, void *ctx)
{
    (void)ctx;
    nl_tcp_close(c);
}

static void cl_close(nl_tcp *c, void *ctx, int reason)
{
    client *s = ctx;

    if (c != s->conn || s->done)
        return;
    s->conn = NULL;
    s->done = 1;
    nl_count(s->hn->n, "http.failed", 1);
    nl_log(s->hn->n, NL_L_APP, 1, 0, T_ERR "http %s%s: %s" T_RESET " (%s)", s->host, s->paths[0], _("failed"),
           reason == NL_TCP_CLOSED_REFUSED ? _("connection refused")
           : reason == NL_TCP_CLOSED_TIMEOUT ? _("timeout")
           : reason == NL_TCP_CLOSED_RST ? _("connection reset") : _("connection closed early"));
}

static const nl_tcp_ops cl_ops = {
    .on_connect = cl_connect, .on_data = cl_data, .on_eof = cl_eof, .on_close = cl_close,
};

/* ------------------------------------------------------------------ */
/* Module                                                             */
/* ------------------------------------------------------------------ */

static int http_run(nl_node *n, const nl_args *a)
{
    http_node *hn = nl_mod_get(n, &http_mod);
    const char *what = nl_arg_pos(a, 0);

    if (!hn) {
        hn = calloc(1, sizeof(*hn));
        if (!hn)
            return -1;
        hn->n = n;
        nl_mod_set(n, &http_mod, hn);
    }
    if (what && strcmp(what, "server") == 0) {
        double sz = 5000;

        hn->port = (int)nl_arg_int(a, "port", 80);
        hn->objects = (int)nl_arg_int(a, "objects", 3);
        if (hn->objects < 0) hn->objects = 0;
        if (hn->objects > MAX_PATHS - 1) hn->objects = MAX_PATHS - 1;
        nl_parse_rate(nl_arg_str(a, "objsize", "5000"), &sz);
        hn->objsize = (long long)sz;
        hn->serving = 1;
        if (nl_tcp_listen(n, (uint16_t)hn->port, &srv_ops, hn, nl_arg_str(a, "tcp", "")) != 0)
            return -1;
        nl_log(n, NL_L_APP, 2, 0, T_FIN "HTTP" T_RESET " %s %d", _("server listening on TCP port"), hn->port);
        return 0;
    }
    if (what && strcmp(what, "get") == 0) {
        const char *url = nl_arg_pos(a, 1), *slash;
        client *s = NULL;
        int i;

        if (!url) {
            nl_log(n, NL_L_APP, 1, 0, T_ERR "http get: %s", _("which URL?"));
            return -1;
        }
        if (strncmp(url, "http://", 7) == 0)
            url += 7;
        for (i = 0; i < MAX_CLIENTS && !s; i++)
            if (!hn->cl[i].used || hn->cl[i].done)
                s = &hn->cl[i];
        if (!s) {
            nl_log(n, NL_L_APP, 1, 0, T_ERR "http get: %s", _("too many requests in progress"));
            return -1;
        }
        memset(s, 0, sizeof(*s));
        s->hn = hn;
        slash = strchr(url, '/');
        snprintf(s->host, sizeof(s->host), "%.*s", slash ? (int)(slash - url) : 63, url);
        snprintf(s->paths[0], sizeof(s->paths[0]), "%s", slash ? slash : "/");
        s->npaths = 1;
        /* IPv6 for a literal (in brackets as in a URL, or bare), for v6=1 with a
         * name, or when the name only has an IPv6 address */
        if (s->host[0] == '[' && strchr(s->host, ']')) {
            char lit[64];
            snprintf(lit, sizeof(lit), "%.*s", (int)(strchr(s->host, ']') - s->host - 1), s->host + 1);
            s->v6 = nl_parse_ip6(lit, &s->ip6);
        } else if (nl_parse_ip6(s->host, &s->ip6)) {
            char lit[64];
            s->v6 = 1;
            snprintf(lit, sizeof(lit), "[%.60s]", s->host);
            snprintf(s->host, sizeof(s->host), "%s", lit);
        } else if (nl_arg_int(a, "v6", 0)) {
            s->v6 = nl_resolve6(s->host, &s->ip6);
        } else if (!nl_resolve(s->host, &s->ip)) {
            s->v6 = nl_ip6_enabled() && nl_resolve6(s->host, &s->ip6);
        }
        if (!s->v6 && (s->host[0] == '[' || nl_arg_int(a, "v6", 0) || !nl_resolve(s->host, &s->ip))) {
            nl_log(n, NL_L_APP, 1, 0, T_ERR "http get: %s '%s'", _("unknown host"), s->host);
            return -1;
        }
        s->used = 1;
        s->port = (int)nl_arg_int(a, "port", 80);
        s->keepalive = (int)nl_arg_int(a, "keepalive", 1);
        s->embedded = (int)nl_arg_int(a, "embedded", 1);
        s->t0 = nl_now();
        nl_log(n, NL_L_APP, 1, 0, T_INFO "http get" T_RESET " http://%s%s (%s, %s)", s->host, s->paths[0],
               s->v6 ? nl_ip6_str(&s->ip6) : nl_ip_str(s->ip), s->keepalive ? _("persistent connection") : _("one connection per object"));
        cl_open(s);
        return s->done ? -1 : 0;
    }
    nl_log(n, NL_L_APP, 1, 0, T_ERR "http: %s '%s' (server, get)", _("unknown command"), what ? what : "");
    return -1;
}

static void http_stop(nl_node *n)
{
    http_node *hn = nl_mod_get(n, &http_mod);
    int i;

    if (!hn)
        return;
    if (hn->serving)
        nl_tcp_unlisten(n, (uint16_t)hn->port);
    for (i = 0; i < MAX_CLIENTS; i++)
        if (hn->cl[i].used && !hn->cl[i].done && hn->cl[i].conn) {
            hn->cl[i].done = 1;
            nl_tcp_abort(hn->cl[i].conn);
        }
    nl_mod_set(n, &http_mod, NULL);
    free(hn);
}

static void http_crash(nl_node *n)
{
    http_node *hn = nl_mod_get(n, &http_mod);
    int i;

    /* TCP forgets its connections on its own: just forget ours */
    for (i = 0; hn && i < MAX_CLIENTS; i++) {
        hn->cl[i].conn = NULL;
        if (hn->cl[i].used)
            hn->cl[i].done = 1;
    }
}

static void http_recover(nl_node *n)
{
    http_node *hn = nl_mod_get(n, &http_mod);

    if (hn && hn->serving)
        nl_tcp_listen(n, (uint16_t)hn->port, &srv_ops, hn, "");
}

static int http_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    http_node *hn = nl_mod_get(n, &http_mod);
    int row = 0, i;

    (void)detail;
    if (hn->serving) {
        row += nl_ui_kv(x, y + row, w, _("server on TCP port"), "%d", hn->port);
        row += nl_ui_kv(x, y + row, w, _("requests served"), "%u (404: %u)", hn->served, hn->served_404);
        row += nl_ui_kv(x, y + row, w, _("index page embeds"), "%d × ~%lld B", hn->objects, hn->objsize);
    }
    for (i = 0; i < MAX_CLIENTS && row < h; i++) {
        const client *s = &hn->cl[i];
        if (!s->used)
            continue;
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), "%s%s" T_RESET " GET %s%s  %d/%d  %lld B  " T_DIM
                  "%d conn", s->done ? T_DIM : T_OK, s->done ? "■" : "●", s->host, s->paths[0],
                  s->cur, s->npaths, s->total + (s->done ? 0 : s->got), s->nconn);
    }
    return row;
}

static const nl_module http_mod = {
    .name = "http", .title = N_("HTTP/1.1 — web server and client"), .layer = NL_L_APP,
    .usage = N_("run NODE http server [port=80] [objects=3] [objsize=5000]\n"
                "run NODE http get HOST[/PATH] [port=80] [keepalive=1] [embedded=1] [v6=1]\n"
                "  The server answers / and /index.html with a page that embeds `objects`\n"
                "  images, and /N with N bytes. The client fetches the page and then, with\n"
                "  embedded=1, every object it references: over the same connection\n"
                "  (keepalive=1) or opening a new connection for each (keepalive=0).\n"
                "  HOST is a node name or an address; an IPv6 address ([2001:db8::1] as in a\n"
                "  URL), or v6=1 with a name, fetches over IPv6. The server answers both."),
    .theory = N_("HTTP is a request/response protocol carried by a TCP connection. The client sends a "
                 "request made of text lines: a request line (method, path, version), headers such as "
                 "Host, and an empty line. The server answers with a status line (200 OK, 404 Not "
                 "Found, ...), its own headers and the body; Content-Length tells the client where the "
                 "body ends, because TCP itself only delivers a stream of bytes without message "
                 "boundaries.\n\n"
                 "A web page is usually an HTML document plus the objects it references. HTTP/1.0 "
                 "opened a new TCP connection for every object, paying a three-way handshake and a "
                 "fresh slow start each time. HTTP/1.1 keeps the connection open (persistent "
                 "connections), so later requests skip the handshake and inherit an already opened "
                 "congestion window: compare keepalive=1 and keepalive=0 on a path with some delay and "
                 "the difference in page load time is exactly those saved round trips. Here requests on "
                 "a connection are strictly sequential; HTTP/2 multiplexes them and HTTP/3 moves the "
                 "whole thing onto QUIC to avoid head-of-line blocking."),
    .run = http_run, .stop = http_stop, .crash = http_crash, .recover = http_recover, .panel = http_panel,
};
NL_MODULE(http_mod)

static void dis_http(nl_dissect *o, const uint8_t *d, int len, int off)
{
    static const char *const starts[] = { "GET ", "POST ", "HEAD ", "PUT ", "HTTP/" };
    char line[100];
    int i, n = 0, is_start = 0;

    for (i = 0; i < (int)(sizeof(starts) / sizeof(starts[0])); i++)
        if (len >= (int)strlen(starts[i]) && memcmp(d, starts[i], strlen(starts[i])) == 0)
            is_start = 1;
    if (!is_start) {
        nl_dis_field(o, "HTTP", "Body data", off, len, "%d bytes", len);
        nl_dis_summary(o, "HTTP", HTTP_COLOR, "Continuation (%d bytes of body)", len);
        return;
    }
    while (n < len && n < (int)sizeof(line) - 1 && d[n] != '\r' && d[n] != '\n') {
        line[n] = (char)d[n];
        n++;
    }
    line[n] = '\0';
    nl_dis_field(o, "HTTP", d[0] == 'H' ? "Status line" : "Request line", off, n, "%s", line);
    /* the header lines that follow */
    i = n;
    while (i + 1 < len && d[i] == '\r' && d[i + 1] == '\n') {
        char hl[96];
        int s = i + 2, k = 0;

        while (s + k < len && k < (int)sizeof(hl) - 1 && d[s + k] != '\r')
            k++;
        if (k == 0) {
            if (s + 2 < len)
                nl_dis_field(o, "HTTP", "Body", off + s + 2, len - s - 2, "%d bytes", len - s - 2);
            break;
        }
        memcpy(hl, d + s, (size_t)k);
        hl[k] = '\0';
        nl_dis_field(o, "HTTP", "Header", off + s, k, "%s", hl);
        i = s + k;
    }
    nl_dis_summary(o, "HTTP", HTTP_COLOR, "%s", line);
}

NL_INIT(http_dissector)
{
    nl_dissector_tcp(80, dis_http);
}
