/*
 * check.c - Assertions that depend on the parameters of a lab.
 *
 * An `expect` line compares one counter with a constant, which is enough
 * when a lab has one outcome. When the outcome is the lesson and depends on
 * a parameter ("with qdisc=prio the voice flow loses nothing, with droptail
 * it loses a lot") the lab states it as an expression:
 *
 *     at 29s run h1 check voice "(is_$qdisc == is_prio && h2:sink.lost == 0) || (is_$qdisc != is_prio && h2:sink.lost > 20)"
 *     expect h1 check.voice == 1
 *
 * The expression is evaluated at that moment and its value is stored in the
 * counter check.NAME of the node (1 / 0 for a condition), so a later check
 * can also compare "before" and "after" snapshots.
 *
 * Grammar: numbers (with k M G, and ms s us: times are in milliseconds),
 * NODE:counter references, bare words (compared as text with == and !=),
 * + - * /, < <= > >= == !=, && || !, parentheses, min(a,b) max(a,b) abs(a)
 * pow(a,b) exp(a) log2(a) sqrt(a).
 */
#include "nl.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int    is_str;
    double num;
    char   str[40];
} value;

typedef struct {
    const char *p;
    char        err[120];
    char        seen[200];              /* "node:counter=value ..." for the log */
} parser;

static value v_num(double d)
{
    value v;

    memset(&v, 0, sizeof(v));
    v.num = d;
    return v;
}

static void skip(parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t')
        ps->p++;
}

static int eat(parser *ps, const char *tok)
{
    size_t n = strlen(tok);

    skip(ps);
    if (strncmp(ps->p, tok, n) != 0)
        return 0;
    ps->p += n;
    return 1;
}

static void fail(parser *ps, const char *msg, const char *what)
{
    if (!ps->err[0])
        snprintf(ps->err, sizeof(ps->err), "%s '%.40s'", msg, what);
}

static double num(parser *ps, value v)
{
    if (v.is_str)
        fail(ps, _("a number is needed, not the word"), v.str);
    return v.num;
}

static value parse_or(parser *ps);

static value parse_primary(parser *ps)
{
    value v = v_num(0);

    skip(ps);
    if (eat(ps, "(")) {
        v = parse_or(ps);
        if (!eat(ps, ")"))
            fail(ps, _("missing ) before"), ps->p);
        return v;
    }
    if (isdigit((unsigned char)*ps->p) || (*ps->p == '.' && isdigit((unsigned char)ps->p[1]))) {
        char *end;

        v.num = strtod(ps->p, &end);
        if (!strncmp(end, "ms", 2) && !isalnum((unsigned char)end[2]))      end += 2;
        else if (!strncmp(end, "us", 2) && !isalnum((unsigned char)end[2])) { v.num /= 1e3; end += 2; }
        else if (*end == 's' && !isalnum((unsigned char)end[1]))            { v.num *= 1e3; end++; }
        else if ((*end == 'k' || *end == 'K') && !isalnum((unsigned char)end[1])) { v.num *= 1e3; end++; }
        else if (*end == 'M' && !isalnum((unsigned char)end[1]))            { v.num *= 1e6; end++; }
        else if (*end == 'G' && !isalnum((unsigned char)end[1]))            { v.num *= 1e9; end++; }
        else if (*end == '%')                                               { v.num /= 100; end++; }
        ps->p = end;
        return v;
    }
    if (isalpha((unsigned char)*ps->p) || *ps->p == '_') {
        char word[40];
        size_t n = 0;

        while ((isalnum((unsigned char)*ps->p) || *ps->p == '_' || *ps->p == '.' || *ps->p == '-') && n < sizeof(word) - 1) {
            /* a '-' belongs to the word only when a letter or digit follows */
            if (*ps->p == '-' && !isalnum((unsigned char)ps->p[1]))
                break;
            word[n++] = *ps->p++;
        }
        word[n] = '\0';
        if (*ps->p == ':') {                    /* NODE:counter */
            nl_node *node = nl_node_find(word);
            char name[40];
            size_t len;

            ps->p++;
            n = 0;
            while ((isalnum((unsigned char)*ps->p) || strchr("_.-<>", *ps->p)) && *ps->p && n < sizeof(name) - 1) {
                if ((*ps->p == '<' || *ps->p == '>') && !isalnum((unsigned char)ps->p[1]) && ps->p[1] != '>')
                    break;
                name[n++] = *ps->p++;
            }
            name[n] = '\0';
            if (!node) {
                fail(ps, _("unknown node"), word);
                return v;
            }
            v.num = nl_count_get(node, name);
            len = strlen(ps->seen);
            snprintf(ps->seen + len, sizeof(ps->seen) - len, "%s%s:%s=%.6g", len ? "  " : "", word, name, v.num);
            return v;
        }
        if (eat(ps, "(")) {                  /* function */
            value a = parse_or(ps), b = v_num(0);
            int two = eat(ps, ",");

            if (two)
                b = parse_or(ps);
            if (!eat(ps, ")"))
                fail(ps, _("missing ) before"), ps->p);
            if (!strcmp(word, "min") && two)      v.num = fmin(num(ps, a), num(ps, b));
            else if (!strcmp(word, "max") && two) v.num = fmax(num(ps, a), num(ps, b));
            else if (!strcmp(word, "abs") && !two) v.num = fabs(num(ps, a));
            else if (!strcmp(word, "pow") && two) v.num = pow(num(ps, a), num(ps, b));
            else if (!strcmp(word, "exp") && !two) v.num = exp(num(ps, a));
            else if (!strcmp(word, "log2") && !two) v.num = num(ps, a) > 0 ? log2(num(ps, a)) : 0;
            else if (!strcmp(word, "sqrt") && !two) v.num = num(ps, a) > 0 ? sqrt(num(ps, a)) : 0;
            else fail(ps, _("unknown function"), word);
            return v;
        }
        v.is_str = 1;
        snprintf(v.str, sizeof(v.str), "%s", word);
        return v;
    }
    fail(ps, _("unexpected"), *ps->p ? ps->p : _("end of the expression"));
    return v;
}

static value parse_unary(parser *ps)
{
    skip(ps);
    if (*ps->p == '!' && ps->p[1] != '=') {
        ps->p++;
        return v_num(num(ps, parse_unary(ps)) == 0);
    }
    if (*ps->p == '-') {
        ps->p++;
        return v_num(-num(ps, parse_unary(ps)));
    }
    return parse_primary(ps);
}

static value parse_term(parser *ps)
{
    value v = parse_unary(ps);

    for (;;) {
        if (eat(ps, "*")) {
            v = v_num(num(ps, v) * num(ps, parse_unary(ps)));
        } else if (eat(ps, "/")) {
            double d = num(ps, parse_unary(ps));
            v = v_num(d != 0 ? num(ps, v) / d : 0);
        } else {
            return v;
        }
    }
}

static value parse_sum(parser *ps)
{
    value v = parse_term(ps);

    for (;;) {
        if (eat(ps, "+"))
            v = v_num(num(ps, v) + num(ps, parse_term(ps)));
        else if (eat(ps, "-"))
            v = v_num(num(ps, v) - num(ps, parse_term(ps)));
        else
            return v;
    }
}

static value parse_cmp(parser *ps)
{
    value a = parse_sum(ps), b;
    int op;

    skip(ps);
    if (eat(ps, "=="))      op = 0;
    else if (eat(ps, "!=")) op = 1;
    else if (eat(ps, "<=")) op = 2;
    else if (eat(ps, ">=")) op = 3;
    else if (eat(ps, "<"))  op = 4;
    else if (eat(ps, ">"))  op = 5;
    else return a;
    b = parse_sum(ps);
    if (a.is_str || b.is_str) {
        int same;

        if (!a.is_str || !b.is_str || op > 1) {
            fail(ps, _("words can only be compared with == or !=:"), a.is_str ? a.str : b.str);
            return v_num(0);
        }
        same = strcmp(a.str, b.str) == 0;
        return v_num(op == 0 ? same : !same);
    }
    switch (op) {
    case 0:  return v_num(a.num == b.num);
    case 1:  return v_num(a.num != b.num);
    case 2:  return v_num(a.num <= b.num);
    case 3:  return v_num(a.num >= b.num);
    case 4:  return v_num(a.num < b.num);
    default: return v_num(a.num > b.num);
    }
}

static value parse_and(parser *ps)
{
    value v = parse_cmp(ps);

    while (eat(ps, "&&")) {
        double r = num(ps, parse_cmp(ps));
        v = v_num(num(ps, v) != 0 && r != 0);
    }
    return v;
}

static value parse_or(parser *ps)
{
    value v = parse_and(ps);

    while (eat(ps, "||")) {
        double r = num(ps, parse_and(ps));
        v = v_num(num(ps, v) != 0 || r != 0);
    }
    return v;
}

static const nl_module check_mod;

static int check_run(nl_node *n, const nl_args *a)
{
    parser ps;
    char counter[40];
    value v;
    int cond;

    if (a->argc < 2) {
        nl_log(n, NL_L_SIM, 1, 0, T_ERR "check: %s", _("usage: run NODE check NAME \"EXPRESSION\""));
        return -1;
    }
    memset(&ps, 0, sizeof(ps));
    ps.p = a->argv[1];
    v = parse_or(&ps);
    skip(&ps);
    if (*ps.p)
        fail(&ps, _("unexpected"), ps.p);
    if (!ps.err[0] && v.is_str)
        fail(&ps, _("the expression is just the word"), v.str);
    snprintf(counter, sizeof(counter), "check.%.30s", a->argv[0]);
    if (ps.err[0]) {
        nl_count_set(n, counter, -1);
        nl_log(n, NL_L_SIM, 1, 0, T_ERR "check %s: %s" T_RESET " — %s", a->argv[0], ps.err, a->argv[1]);
        return -1;
    }
    nl_count_set(n, counter, v.num);
    /* a value other than 0 / 1 is a measurement kept for a later check */
    cond = v.num == 0 || v.num == 1;
    if (!nl_mod_get(n, &check_mod))
        nl_mod_set(n, &check_mod, (void *)&check_mod);
    if (!cond)
        nl_log(n, NL_L_SIM, 2, 0, T_DIM "check %s = %.6g  (%s)", a->argv[0], v.num, ps.seen);
    else if (v.num != 0)
        nl_log(n, NL_L_SIM, 1, 0, T_OK "✓ check %s" T_RESET T_DIM "  %s", a->argv[0], ps.seen);
    else
        nl_log(n, NL_L_SIM, 1, 0, T_ERR "✗ check %s" T_RESET "  %s  " T_DIM "%s", a->argv[0], ps.seen, a->argv[1]);
    return 0;
}

static void check_stop(nl_node *n)
{
    nl_mod_set(n, &check_mod, NULL);
}

static const nl_module check_mod = {
    .name = "check", .title = N_("check — state what a lab should show, as an expression over counters"),
    .layer = NL_L_SIM,
    .usage = N_("run NODE check NAME \"EXPRESSION\"\n"
                "  Evaluates the expression now and stores the result in the counter check.NAME\n"
                "  (1 or 0 for a condition; any other number is a measurement for a later check).\n"
                "  NODE:counter reads a counter; numbers take k M G and ms s us (times in ms);\n"
                "  bare words compare as text (is_$algo == is_rr); + - * /  < <= > >= == !=\n"
                "  && || !  ( )  min(a,b) max(a,b) pow(a,b) abs(a) exp(a) log2(a) sqrt(a).\n"
                "  Pair it with:  expect NODE check.NAME == 1"),
    .theory = N_("A lab is an experiment, and an experiment has a prediction. Most predictions depend "
                 "on the conditions: the queue discipline chosen, the protocol, the amount of noise. "
                 "A check writes the prediction down in terms of what the nodes counted, for whatever "
                 "value the parameters have, so that running the lab with different settings tests "
                 "the explanation instead of contradicting it."),
    .run = check_run, .stop = check_stop,
};
NL_MODULE(check_mod)
