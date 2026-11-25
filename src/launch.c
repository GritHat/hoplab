/*
 * launch.c - `netlab windows`: the same lab as several processes in tiled
 *            windows on this machine. One pane (the hub) shows the whole
 *            network and runs every node that has no pane of its own; the
 *            others run one node each. Links between processes are UDP
 *            tunnels on 127.0.0.1, exactly as they would be across machines.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PANES 6

static int has(const char *prog)
{
    char cmd[128];

    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", prog);
    return system(cmd) == 0;
}

/* Single-quote s for the shell */
static void quote(char *out, size_t n, const char *s)
{
    size_t k = 0;

    if (k + 1 < n)
        out[k++] = '\'';
    for (; *s && k + 6 < n; s++) {
        if (*s == '\'') {
            memcpy(out + k, "'\\''", 4);
            k += 4;
        } else {
            out[k++] = *s;
        }
    }
    out[k++] = '\'';
    out[k] = '\0';
}

int nl_launch_windows(const char *scenario, int argc, char **argv, const char *mode, const char *nodes, double speed)
{
    char path[1024], self[1024], qpath[1100], qself[1100], params[1024] = "", common[1400];
    static char cmd[MAX_PANES][4200], own[MAX_PANES][16], rest[512] = "", sess[64];
    int npanes = 0, i, base, in_tmux = getenv("TMUX") != NULL;
    int gui = getenv("DISPLAY") || getenv("WAYLAND_DISPLAY");
    ssize_t sl;
    size_t len = 0;

    if (!nl_scenario_path(scenario, path, sizeof(path))) {
        fprintf(stderr, "netlab: %s: %s\n", scenario, _("scenario not found (see `netlab list`)"));
        return 2;
    }
    if (path[0] != '/') {               /* panes may start in another directory */
        char cwd[512], abs[1600];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
            if (strlen(abs) < sizeof(path))
                strcpy(path, abs);
        }
    }
    sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl <= 0)
        return 2;
    self[sl] = '\0';
    nl_world_init();
    W.mode = NL_MODE_RUN;
    if (nl_scenario_load(path, argc, argv) != 0)
        return 2;

    /* who gets a pane: the nodes asked for, else hosts and routers in order */
    for (i = 0; i < W.nnodes; i++) {
        const nl_node *n = &W.nodes[i];
        int want;

        if (nodes) {
            char pat[20], list[300];
            snprintf(pat, sizeof(pat), ",%s,", n->name);
            snprintf(list, sizeof(list), ",%s,", nodes);
            want = strstr(list, pat) != NULL;
        } else {
            want = n->kind != NL_SWITCH;
        }
        if (want && npanes < MAX_PANES - 1) {
            snprintf(own[npanes++], sizeof(own[0]), "%s", n->name);
        } else {
            size_t rl = strlen(rest);
            snprintf(rest + rl, sizeof(rest) - rl, "%s%s", rl ? "," : "", n->name);
        }
    }

    for (i = 0; i < argc; i++) {
        char q[300];
        quote(q, sizeof(q), argv[i]);
        len += (size_t)snprintf(params + len, sizeof(params) - len, " %s", q);
        if (len >= sizeof(params))
            return 2;
    }
    base = 20000 + (int)(getpid() % 1000) * 40;
    quote(qpath, sizeof(qpath), path);
    quote(qself, sizeof(qself), self);
    if (speed <= 0) {
        /* every process must use the same time scale: slow enough to watch */
        double sum = 0;
        for (i = 0; i < W.nlinks; i++)
            sum += (double)W.links[i].cfg.delay + (W.links[i].cfg.bw > 0 ? 8000.0 * 1e6 / W.links[i].cfg.bw : 0);
        speed = W.nlinks ? sum / W.nlinks / 900000.0 : 1.0;
        if (speed > 1.0) speed = 1.0;
        if (speed < 0.02) speed = 0.02;     /* periodic timers must still tick */
    }
    snprintf(common, sizeof(common), "--baseport %d --speed %g --seed %llu --lang %s%s", base, speed,
             (unsigned long long)(W.seed ? W.seed : (uint64_t)getpid()), nl_lang(), params);

    /* pane 0: the hub */
    if (rest[0])
        snprintf(cmd[0], sizeof(cmd[0]), "%s node %s %s --hub %s", qself, qpath, rest, common);
    else
        snprintf(cmd[0], sizeof(cmd[0]), "%s monitor %s %s", qself, qpath, common);
    for (i = 0; i < npanes; i++)
        snprintf(cmd[i + 1], sizeof(cmd[0]), "sleep 0.4; %s node %s %s --monitor 127.0.0.1:%d %s", qself, qpath,
                 own[i], base - 1, common);
    npanes++;

    if (!strcmp(mode, "auto"))
        mode = in_tmux ? "tmux" : gui && has("kitty") ? "kitty" : has("tmux") ? "tmux" : "print";

    if (!strcmp(mode, "kitty")) {
        char file[] = "/tmp/netlab-session-XXXXXX", line[300];
        int fd = mkstemp(file);
        FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;

        if (!f)
            return 2;
        fprintf(f, "new_tab netlab\nenabled_layouts tall:bias=55,grid\nlayout tall\n");
        for (i = 0; i < npanes; i++) {
            static char q[4600];
            quote(q, sizeof(q), cmd[i]);
            fprintf(f, "launch --title %s sh -c %s\n", i ? own[i - 1] : "network", q);
        }
        fclose(f);
        snprintf(line, sizeof(line), "kitty --detach --start-as=maximized --title netlab --session %s", file);
        if (system(line) != 0)
            return 2;
        snprintf(line, sizeof(line), "(sleep 20; rm -f %s) >/dev/null 2>&1 &", file);
        if (system(line) != 0)
            return 2;
        printf("%s\n", _("Opened a kitty window: the whole network on the left, one pane per node on the right."));
        return 0;
    }
    if (!strcmp(mode, "tmux")) {
        static char line[5200], q[4600];

        snprintf(sess, sizeof(sess), "netlab-%d", (int)getpid());
        quote(q, sizeof(q), cmd[0]);
        if (in_tmux)
            snprintf(line, sizeof(line), "tmux new-window -n netlab %s", q);
        else
            snprintf(line, sizeof(line), "tmux new-session -d -s %s -x 220 -y 60 %s", sess, q);
        if (system(line) != 0)
            return 2;
        for (i = 1; i < npanes; i++) {
            quote(q, sizeof(q), cmd[i]);
            snprintf(line, sizeof(line), "tmux split-window %s%s %s && tmux select-layout %s%s main-vertical",
                     in_tmux ? "" : "-t ", in_tmux ? "" : sess, q, in_tmux ? "" : "-t ", in_tmux ? "" : sess);
            if (system(line) != 0)
                return 2;
        }
        if (in_tmux)
            return 0;
        snprintf(line, sizeof(line), "tmux set-option -t %s mouse on >/dev/null 2>&1; "
                 "tmux select-pane -t %s.0; tmux attach -t %s", sess, sess, sess);
        if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
            return system(line) == 0 ? 0 : 2;
        printf("%s  tmux attach -t %s\n", _("tmux session started; attach with:"), sess);
        return 0;
    }
    printf("%s\n\n", _("# Run each command in its own terminal (the first one first):"));
    for (i = 0; i < npanes; i++)
        printf("%s\n\n", cmd[i]);
    return 0;
}
