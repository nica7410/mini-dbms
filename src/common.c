#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 잘린 메시지 끝에 남은 미완성 UTF-8 문자를 지운다. 메시지가 UTF-8 검사를 통과하게 한다. */
static void trim_partial_utf8(char *s) {
    size_t len = strlen(s);
    size_t cont = 0;
    unsigned char lead;
    size_t need;

    while (len > 0 && cont < 3 && ((unsigned char)s[len - 1] & 0xC0) == 0x80) {
        len--;
        cont++;
    }
    if (len == 0) {
        return;
    }
    lead = (unsigned char)s[len - 1];
    need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
    if (need > cont) {
        s[len - 1] = '\0';
    }
}

void db_error_set(DbError *err, const char *fmt, ...) {
    va_list ap;
    int n;

    if (err == NULL) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof err->msg) {
        trim_partial_utf8(err->msg);
    }
}
