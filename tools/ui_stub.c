/* ui_stub.c - stands in for the interface in module-only builds (tools/build-mod.sh) */
#include "nl_int.h"
int nl_ui_run(void){return 0;}
int nl_menu(void){return 0;}
int nl_guide(const char*t){(void)t;return 0;}
int nl_launch_windows(const char*s,int c,char**v,const char*m,const char*n,double sp){(void)s;(void)c;(void)v;(void)m;(void)n;(void)sp;return 0;}
int nl_ui_kv(int x,int y,int w,const char*l,const char*f,...){(void)x;(void)y;(void)w;(void)l;(void)f;return 1;}
void nl_ui_graph(int x,int y,int w,int h,const nl_series*s,short c,const char*t){(void)x;(void)y;(void)w;(void)h;(void)s;(void)c;(void)t;}
int nl_ui_wrap(int x,int y,int w,int maxh,tstyle b,const char*t){(void)x;(void)y;(void)w;(void)maxh;(void)b;(void)t;return 1;}
