/*
 * nl_int.h - netlab internals shared by the core files (not for modules).
 */
#ifndef NL_INT_H
#define NL_INT_H

#include "nl.h"

#include <netinet/in.h>

/* ---- capture: one record per frame put on a link ---- */
typedef struct nl_cap {
    uint64_t seq;
    nl_time  t0, t1;           /* transmission start, arrival */
    short    link, dir;
    short    from, to;         /* node indexes */
    uint8_t  fate;
    uint8_t  inbound;          /* created by the receiving process of a tunnel */
    float    fate_pos;         /* where along the link the loss/corruption shows */
    short    color;
    char     proto[10];
    char     summary[150];
    char     addr[64];
    nl_pkt   pkt;              /* the frame as sent */
} nl_cap;

typedef struct nl_logrec {
    uint64_t seq;
    nl_time  t;
    short    node;             /* -1 = none */
    uint8_t  layer, level;
    uint32_t pkt_id;
    char     text[260];
} nl_logrec;

#define NL_CAP_RING 2048
#define NL_LOG_RING 4096

typedef struct nl_note   { nl_time t; int loc; char text[400]; } nl_note;
typedef struct nl_expect { char node[16], name[28], op[3]; double v; } nl_expect;
typedef struct nl_param  {
    char name[24], value[64], def[64], label[100];
    int  advanced;
} nl_param;

/* What a scenario file says about itself (also read alone by the catalog) */
typedef struct nl_meta {
    char     file[512];
    char     title[160], about[4000], topic[32];
    int      level, layer;
    int      nnodes, nlinks;   /* counted while peeking */
    nl_param params[32];
    int      nparams;
    int      loc_title, loc_about;
} nl_meta;

enum { NL_MODE_RUN, NL_MODE_NODE, NL_MODE_MONITOR };

typedef struct nl_world {
    nl_node  nodes[NL_MAX_NODES];
    int      nnodes;
    nl_link  links[NL_MAX_LINKS];
    int      nlinks;

    nl_time  now;
    double   speed;
    int      speed_set;        /* chosen by the user or the scenario: no automatic time scale */
    int      paused;
    int      detail;           /* 1..3 */
    int      mode;
    int      headless;
    int      quiet;            /* headless: do not print the log */
    uint64_t seed;

    /* scenario */
    nl_meta  m;
    nl_time  duration;         /* headless run length */
    nl_note  notes[48];
    int      nnotes;
    nl_expect expects[32];
    int      nexpects;
    int      auto_addr, auto_routes;
    int      baseport;
    int      errors;           /* scenario errors */
    char     errmsg[300];

    /* captures and log */
    nl_cap    *caps;
    uint64_t   cap_seq;        /* next sequence number; ring index = seq % NL_CAP_RING */
    nl_logrec *logs;
    uint64_t   log_seq;

    /* monitor */
    int      mon_sock;         /* -1 = no monitor */
    struct sockaddr_in mon_addr;
    int      mon_listen;       /* monitor or hub: socket we receive reports on */
    int      waiting;          /* time is held until every process of the lab is there */
    int      hub;              /* node mode: also collect the reports of the other processes */
    int      baseport_fixed;   /* chosen on the command line: the scenario cannot change it */
} nl_world;

extern nl_world W;

/* core.c */
void     nl_world_init(void);
int      nl_step(nl_time until);               /* run events up to `until`; returns events run */
nl_time  nl_next_event(void);                  /* time of the next event or -1 */
nl_cap  *nl_cap_new(void);
const nl_cap *nl_cap_get(uint64_t seq);        /* NULL when overwritten */
const nl_logrec *nl_log_get(uint64_t seq);
void     nl_strip_markup(const char *in, char *out, size_t n);
nl_node *nl_node_add(const char *name, int kind);
int      nl_if_add(nl_node *n);
const nl_qdisc *nl_qdisc_find(const char *name);
const nl_qdisc *nl_qdisc_at(int i);

/* link.c */
nl_link *nl_link_add(nl_node *a, int ifa, nl_node *b, int ifb, const nl_linkcfg *cfg);
void     nl_linkcfg_default(nl_linkcfg *c);
int      nl_linkcfg_apply(nl_linkcfg *c, const nl_args *a, char *err, size_t errsz);
void     nl_link_tx(nl_node *n, int ifx, nl_pkt *p);          /* takes ownership */
void     nl_link_reconf(nl_link *l);                          /* after cfg changes */
int      nl_net_open(void);                                   /* tunnel/monitor sockets; 0 = ok */
int      nl_net_fds(int *fds, int max);
void     nl_net_poll(void);                                   /* read tunnels and monitor datagrams */
void     nl_net_sync(void);                                   /* start-up rendezvous; call every loop */
void     nl_net_start(void);                                  /* stop waiting (and tell the others, if hub) */
int      nl_net_missing(char *buf, size_t n);                 /* hub: names of the nodes not heard yet */
void     nl_mon_send_log(const nl_logrec *r);
void     nl_mon_send_cap(const nl_cap *c);

/* stack.c */
void nl_stack_init(nl_node *n);
void nl_stack_rx(nl_node *n, int ifx, nl_pkt *p);             /* frame arrived; takes ownership */
void nl_stack_node_down(nl_node *n);

/* scenario.c */
int  nl_scenario_load(const char *path, int argc, char **argv);   /* argv: key=value overrides; 0 = ok */
int  nl_scenario_peek(const char *path, nl_meta *out);            /* only the metadata and params */
void nl_scenario_finalize(void);
int  nl_expect_check(int print);                                  /* number of failed expectations */
const char *nl_scenario_path(const char *name, char *buf, size_t n);  /* find by file or catalog name */

/* ui.c */
int  nl_ui_run(void);                          /* interactive main loop; returns when the user quits */
int  nl_key(void);                             /* decoded key or -1 */
enum { NK_UP = 1000, NK_DOWN, NK_LEFT, NK_RIGHT, NK_PGUP, NK_PGDN, NK_HOME, NK_END, NK_ENTER = '\n',
       NK_ESC = 27, NK_BACKSPACE = 127, NK_TAB = '\t', NK_BTAB = 1010, NK_DEL = 1011 };
int  nl_line_edit(char *buf, size_t size, int *cursor, int key);  /* 1 = key consumed */
void nl_md_draw(int x, int y, int w, int h, const char *text, int *scroll, int detail);

/* menu.c / launch.c / guide.c */
int  nl_menu(void);
int  nl_spawn_wait(const char *prog, char **argv, int pause);   /* leave the screen, run, come back */
int  nl_guide(const char *topic);
int  nl_launch_windows(const char *scenario, int argc, char **argv, const char *mode, const char *nodes,
                       double speed);

#endif
