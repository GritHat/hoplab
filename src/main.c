/*
 * main.c - netlab command line.
 */
#include "nl_int.h"
#include "rdt.h"

#include <dirent.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define NETLAB_VERSION "1.0"

static void usage(void)
{
    printf("%s",
           _("netlab — a network laboratory in your terminal\n"
             "\n"
             "Usage:\n"
             "  netlab                         interactive menu: catalog, guide, wizard\n"
             "  netlab run SCENARIO [k=v ...]  simulate every node in this window\n"
             "  netlab windows SCENARIO [...]  one tiled window per node plus a monitor\n"
             "  netlab node SCENARIO NODE[,NODE...]   run only these nodes (other machines\n"
             "                                 or windows run the others; links are UDP tunnels)\n"
             "  netlab monitor SCENARIO        global view of a distributed run\n"
             "  netlab plan SCENARIO           print the commands for a distributed run\n"
             "  netlab check [SCENARIO ...]    run headless and verify the `expect` lines\n"
             "  netlab list                    scenarios in the catalog\n"
             "  netlab modules | help MODULE   protocols and tools, and how to use them\n"
             "  netlab guide [TOPIC]           study guide and curriculum\n"
             "\n"
             "Options:\n"
             "  --headless        no interface: print the event log (default when not a terminal)\n"
             "  --until TIME      headless run length (default: the scenario's `duration`)\n"
             "  --detail 1|2|3    overview, protocol messages, every frame\n"
             "  --speed X         time scale (0.1 slow motion ... 100)\n"
             "  --seed N          random seed (same seed, same run)\n"
             "  --quiet           headless: only the result of the expectations\n"
             "  --monitor H[:P]   node mode: report to a monitor\n"
             "  --mode M          windows: auto, kitty, tmux, print\n"
             "  --nodes a,b,c     windows: the nodes that get a window of their own\n"
             "  --hub             node mode: also be the monitor for the other processes\n"
             "  --baseport N      first UDP port used by tunnels (monitor: N-1)\n"
             "  --lang xx         language (default from NETLAB_LANG / LANG)\n"
             "\n"
             "SCENARIO is a file or a catalog name (see `netlab list`); k=v pairs set the\n"
             "scenario parameters.\n"));
}

typedef struct {
    const char *until, *monitor, *mode, *lang;
    int   headless, quiet, detail, hub, baseport;
    const char *nodes;
    double speed;
    uint64_t seed;
    char *pos[64];
    int   npos;
} options;

static void parse_options(int argc, char **argv, options *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(a, "--headless"))                 o->headless = 1;
        else if (!strcmp(a, "--quiet"))               o->quiet = o->headless = 1;
        else if (!strcmp(a, "--until") && v)          { o->until = v; i++; }
        else if (!strcmp(a, "--detail") && v)         { o->detail = atoi(v); i++; }
        else if (!strcmp(a, "--speed") && v)          { o->speed = atof(v); i++; }
        else if (!strcmp(a, "--seed") && v)           { o->seed = strtoull(v, NULL, 0); i++; }
        else if (!strcmp(a, "--monitor") && v)        { o->monitor = v; i++; }
        else if (!strcmp(a, "--mode") && v)           { o->mode = v; i++; }
        else if (!strcmp(a, "--lang") && v)           { o->lang = v; i++; }
        else if (!strcmp(a, "--hub"))                 o->hub = 1;
        else if (!strcmp(a, "--baseport") && v)       { o->baseport = atoi(v); i++; }
        else if (!strcmp(a, "--nodes") && v)          { o->nodes = v; i++; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { o->pos[0] = "help"; o->npos = 1; return; }
        else if (o->npos < 64)                        o->pos[o->npos++] = argv[i];
    }
}

static int load(const options *o, int first_param)
{
    char path[1024];

    if (o->npos < 2) {
        fprintf(stderr, "netlab: %s\n", _("which scenario? (see `netlab list`)"));
        return -1;
    }
    if (!nl_scenario_path(o->pos[1], path, sizeof(path))) {
        fprintf(stderr, "netlab: %s: %s\n", o->pos[1], _("scenario not found (see `netlab list`)"));
        return -1;
    }
    W.seed = o->seed;
    W.hub = o->hub;
    if (o->baseport) {
        W.baseport = o->baseport;
        W.baseport_fixed = 1;
    }
    if (o->monitor) {
        char host[64];
        int port = W.baseport - 1;
        const char *colon = strrchr(o->monitor, ':');

        snprintf(host, sizeof(host), "%.*s", colon ? (int)(colon - o->monitor) : 63, o->monitor);
        if (colon)
            port = atoi(colon + 1);
        if (rdt_resolve(host, port, &W.mon_addr) == 0)
            W.mon_sock = -2;
    }
    if (nl_scenario_load(path, o->npos - first_param, (char **)o->pos + first_param) != 0)
        return -1;
    if (!W.seed)
        W.seed = (uint64_t)rdt_now_ms() ^ ((uint64_t)getpid() << 20);
    nl_srand(W.seed);
    if (o->detail)
        W.detail = o->detail < 1 ? 1 : o->detail > 3 ? 3 : o->detail;
    if (o->speed > 0) {
        W.speed = o->speed;
        W.speed_set = 1;
    }
    if (o->until && !nl_parse_time(o->until, &W.duration)) {
        fprintf(stderr, "netlab: --until: %s\n", _("bad time"));
        return -1;
    }
    return 0;
}

/* Headless: virtual time runs as fast as possible when everything is in
 * this process, in real time when tunnels are involved. */
static int run_headless(void)
{
    int failed;

    W.headless = 1;
    if (!W.quiet)
        printf("# %s — seed %llu\n", W.m.title, (unsigned long long)W.seed);
    if (W.mode == NL_MODE_RUN) {
        for (;;) {
            nl_time t = nl_next_event();
            if (t < 0 || t > W.duration)
                break;
            nl_step(t);
        }
        nl_step(W.duration);
    } else {
        long long t0 = rdt_now_ms();
        int fds[NL_MAX_NODES + 1], nf = nl_net_fds(fds, NL_MAX_NODES + 1), i;
        struct pollfd pf[NL_MAX_NODES + 1];

        for (i = 0; i < nf; i++) {
            pf[i].fd = fds[i];
            pf[i].events = POLLIN;
        }
        long long last = t0;

        while (W.now < W.duration) {
            nl_time next = nl_next_event();
            int wait_ms = next < 0 ? 50 : (int)((double)(next - W.now) / 1000.0 / W.speed);
            long long now;

            poll(pf, (nfds_t)nf, wait_ms < 1 ? 1 : wait_ms > 50 ? 50 : wait_ms);
            now = rdt_now_ms();
            nl_net_sync();
            if (!W.waiting)             /* advance first: arrivals are stamped with the current time */
                nl_step(W.now + (nl_time)((double)(now - last) * 1000.0 * W.speed));
            last = now;
            nl_net_poll();
        }
    }
    if (W.nexpects == 0)
        return 0;
    if (!W.quiet)
        printf("# %s\n", _("expectations"));
    failed = nl_expect_check(!W.quiet);
    printf("%s  %s\n", failed ? "FAIL" : "PASS", W.m.title);
    return failed ? 1 : 0;
}

static int cmd_run(const options *o, int mode)
{
    int first_param = 2, i;

    nl_world_init();
    W.mode = mode;
    if (mode == NL_MODE_NODE) {
        if (o->npos < 3) {
            fprintf(stderr, "netlab: %s\n", _("usage: netlab node SCENARIO NODE[,NODE...]"));
            return 2;
        }
        first_param = 3;
    }
    if (load(o, first_param) != 0)
        return 2;
    if (mode == NL_MODE_NODE) {
        char list[256];
        char *tok, *save = NULL;

        for (i = 0; i < W.nnodes; i++)
            W.nodes[i].local = 0;
        snprintf(list, sizeof(list), "%s", o->pos[2]);
        for (tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            nl_node *n = nl_node_find(tok);
            if (!n) {
                fprintf(stderr, "netlab: %s '%s'\n", _("unknown node"), tok);
                return 2;
            }
            n->local = 1;
        }
    } else if (mode == NL_MODE_MONITOR) {
        for (i = 0; i < W.nnodes; i++)
            W.nodes[i].local = 0;
    }
    if (nl_net_open() != 0)
        return 2;
    W.quiet = o->quiet;
    if (o->headless || !isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO))
        return run_headless();
    return nl_ui_run();
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Sorted list of scenario files in the catalog; returns the count */
static int catalog(char **out, int max)
{
    char dir[600];
    DIR *d;
    struct dirent *e;
    int n = 0;

    snprintf(dir, sizeof(dir), "%s/scenarios", nl_home());
    d = opendir(dir);
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL && n < max) {
        size_t el = strlen(e->d_name);
        char path[1024];

        if (el < 4 || strcmp(e->d_name + el - 3, ".nl") != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        out[n++] = strdup(path);
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(char *), cmp_str);
    return n;
}

static int cmd_list(void)
{
    static const char *const levels[] = { "", N_("Foundations"), N_("Core"), N_("Advanced"), N_("Frontier") };
    char *files[512];
    int n = catalog(files, 512), i, last = -1;
    nl_meta *m = malloc(sizeof(*m));

    if (!m)
        return 1;
    for (i = 0; i < n; i++) {
        const char *base = strrchr(files[i], '/');

        nl_scenario_peek(files[i], m);
        if (m->level != last) {
            last = m->level;
            printf("\n%s\n", m->level >= 1 && m->level <= 4 ? _(levels[m->level]) : _("Other"));
        }
        printf("  %-28.*s %s\n", (int)strlen(base + 1) - 3, base + 1, m->title);
        free(files[i]);
    }
    free(m);
    if (!n)
        printf("%s %s/scenarios\n", _("no scenarios found in"), nl_home());
    return 0;
}

static int cmd_check(const options *o, const char *self)
{
    char *files[512];
    int n = 0, i, failed = 0, status;

    if (o->npos > 1) {
        for (i = 1; i < o->npos && n < 512; i++)
            files[n++] = o->pos[i];
    } else {
        n = catalog(files, 512);
    }
    for (i = 0; i < n; i++) {
        pid_t pid = fork();

        if (pid == 0) {
            execl("/proc/self/exe", self, "run", files[i], "--quiet", "--seed", "1", (char *)NULL);
            _exit(127);
        }
        if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed++;
            if (pid > 0 && (!WIFEXITED(status) || WEXITSTATUS(status) > 1))
                printf("FAIL  %s (%s)\n", files[i], _("error or crash"));
        }
    }
    printf("\n%d %s, %d %s\n", n, _("scenarios"), failed, _("failed"));
    return failed ? 1 : 0;
}

static int cmd_modules(const char *name)
{
    const nl_module *m;
    int i, last = -1;

    if (name) {
        m = nl_module_find(name);
        if (!m) {
            fprintf(stderr, "netlab: %s '%s'\n", _("unknown module"), name);
            return 1;
        }
        printf("%s — %s\n\n%s\n", m->name, _(m->title), m->usage ? _(m->usage) : "");
        if (m->theory)
            printf("\n%s\n", _(m->theory));
        return 0;
    }
    for (i = 0; (m = nl_module_at(i)) != NULL; i++) {
        if (m->layer != last) {
            last = m->layer;
            printf("\n%s\n", nl_layer_name(m->layer));
        }
        printf("  %-12s %s\n", m->name, _(m->title));
    }
    printf("\n%s\n", _("Queue disciplines (link ... qdisc=NAME):"));
    printf("  %-12s %s\n", "droptail", _("drop arrivals when the queue is full (default)"));
    for (i = 0; nl_qdisc_at(i); i++)
        printf("  %-12s %s\n", nl_qdisc_at(i)->name, _(nl_qdisc_at(i)->title));
    printf("\n%s\n", _("Link codes (link ... codec=NAME):"));
    printf("  %-12s %s\n", "fcs", _("frame check sequence: damaged frames are discarded (default)"));
    printf("  %-12s %s\n", "none", _("no check: damaged frames reach the upper layers"));
    for (i = 0; nl_codec_at(i); i++)
        printf("  %-12s %s\n", nl_codec_at(i)->name, _(nl_codec_at(i)->title));
    return 0;
}

/* The commands to start a scenario on several machines */
static int cmd_plan(const options *o)
{
    int i;

    nl_world_init();
    W.mode = NL_MODE_RUN;
    if (load(o, 2) != 0)
        return 2;
    printf("%s\n\n", _("# Copy netlab and the scenario file to every machine, then run:"));
    for (i = 0; i < W.nnodes; i++) {
        nl_node *n = &W.nodes[i];
        printf("  [%s]  netlab node %s %s\n", n->under_host[0] ? n->under_host : "127.0.0.1", o->pos[1], n->name);
    }
    printf("\n%s\n  netlab monitor %s\n\n%s\n", _("# and, on the machine named in the `monitor` line:"), o->pos[1],
           _("# Each node listens on the UDP port of its `place` line (default: baseport + index).\n"
             "# Nodes placed on the same machine can share one process: netlab node FILE a,b,c"));
    return 0;
}

int main(int argc, char **argv)
{
    options o;
    const char *cmd;

    parse_options(argc, argv, &o);
    nl_i18n_init(o.lang);
    cmd = o.npos ? o.pos[0] : "menu";

    if (!strcmp(cmd, "help") && o.npos > 1)
        return cmd_modules(o.pos[1]);
    if (!strcmp(cmd, "help")) {
        usage();
        return 0;
    }
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
        printf("netlab %s\n", NETLAB_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "run"))     return cmd_run(&o, NL_MODE_RUN);
    if (!strcmp(cmd, "node"))    return cmd_run(&o, NL_MODE_NODE);
    if (!strcmp(cmd, "monitor")) return cmd_run(&o, NL_MODE_MONITOR);
    if (!strcmp(cmd, "list"))    return cmd_list();
    if (!strcmp(cmd, "check"))   return cmd_check(&o, argv[0]);
    if (!strcmp(cmd, "modules")) return cmd_modules(NULL);
    if (!strcmp(cmd, "plan"))    return cmd_plan(&o);
    if (!strcmp(cmd, "guide"))   return nl_guide(o.npos > 1 ? o.pos[1] : NULL);
    if (!strcmp(cmd, "windows")) {
        if (o.npos < 2) {
            fprintf(stderr, "netlab: %s\n", _("which scenario? (see `netlab list`)"));
            return 2;
        }
        return nl_launch_windows(o.pos[1], o.npos - 2, o.pos + 2, o.mode ? o.mode : "auto", o.nodes, o.speed);
    }
    if (!strcmp(cmd, "menu")) {
        if (!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)) {
            usage();
            return 0;
        }
        return nl_menu();
    }
    fprintf(stderr, "netlab: %s '%s'\n\n", _("unknown command"), cmd);
    usage();
    return 2;
}
