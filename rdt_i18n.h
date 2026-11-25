/*
 * rdt_i18n.h - Translated text with colours for the reliable-transfer demos.
 *
 * Translatable sentences mark colour changes with readable tags instead of
 * the control characters of tui.h, so that catalogs stay plain text:
 *
 *     tui_log(tm(_("{d}▶ {0}%s sent {m}· timer started")), name);
 *
 * {x} stands for the tui.h markup code x: {0} reset, {b} bold, {m} dim,
 * {d} data, {a} ack, {f} fin, {g} ok, {r} error, {y} warning, {t} time,
 * {c} channel, {i} info, {D} {A} {F} {R} {Y} {O} chips, {K} key.
 */
#ifndef RDT_I18N_H
#define RDT_I18N_H

#include <string.h>

#include "i18n.h"

/* Expand the {x} tags of s into tui.h markup. The result lives in one of a
 * few rotating buffers: valid until a dozen later calls. */
static inline const char *tm(const char *s)
{
    static char bufs[12][640];
    static int  next;
    char *out = bufs[next];
    size_t k = 0;

    next = (next + 1) % 12;
    while (*s && k + 3 < sizeof(bufs[0])) {
        if (s[0] == '{' && s[1] && s[2] == '}' && strchr("0bmwdafgrytciDAFRYOK", s[1])) {
            out[k++] = '\001';
            out[k++] = s[1];
            s += 3;
        } else {
            out[k++] = *s++;
        }
    }
    out[k] = '\0';
    return out;
}

#endif
