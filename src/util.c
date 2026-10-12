#include "util.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_intern_mu = PTHREAD_MUTEX_INITIALIZER;
static char **g_tab;
static size_t g_cap, g_cnt;

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static void intern_grow(void)
{
    size_t ncap = g_cap ? g_cap * 2 : 1024;
    char **nt = calloc(ncap, sizeof *nt);
    for (size_t i = 0; i < g_cap; i++) {
        if (!g_tab[i])
            continue;
        size_t j = fnv1a(g_tab[i]) & (ncap - 1);
        while (nt[j])
            j = (j + 1) & (ncap - 1);
        nt[j] = g_tab[i];
    }
    free(g_tab);
    g_tab = nt;
    g_cap = ncap;
}

const char *intern(const char *s)
{
    if (!s)
        s = "";
    uint32_t h = fnv1a(s);
    pthread_mutex_lock(&g_intern_mu);
    if ((g_cnt + 1) * 10 > g_cap * 7)
        intern_grow();
    size_t i = h & (g_cap - 1);
    while (g_tab[i]) {
        if (!strcmp(g_tab[i], s)) {
            const char *r = g_tab[i];
            pthread_mutex_unlock(&g_intern_mu);
            return r;
        }
        i = (i + 1) & (g_cap - 1);
    }
    g_tab[i] = strdup(s);
    g_cnt++;
    const char *r = g_tab[i];
    pthread_mutex_unlock(&g_intern_mu);
    return r;
}

void copy_str(char *dst, size_t n, const char *src)
{
    if (n == 0)
        return;
    if (!src)
        src = "";
    size_t l = strlen(src);
    if (l >= n)
        l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

void now_string(char out[24])
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, 24, "%Y-%m-%d %H:%M", &tmv);
}

/* Maps U+00C0..U+00FF to ASCII. '*' and '/' stand in for the odd symbols. */
static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

void fold_ascii(char *dst, size_t n, const char *s)
{
    size_t o = 0;
    if (n == 0)
        return;
    if (!s)
        s = "";
    while (*s && o + 1 < n) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            dst[o++] = c < 0x20 ? ' ' : (char)c;
            s++;
        } else if (c == 0xC3 && (unsigned char)s[1] >= 0x80 && (unsigned char)s[1] <= 0xBF) {
            dst[o++] = LATIN1[(unsigned char)s[1] - 0x80];
            s += 2;
        } else if (c == 0xE2 && (unsigned char)s[1] == 0x80 && s[2]) {
            unsigned char k = (unsigned char)s[2];
            dst[o++] = (k == 0x98 || k == 0x99) ? '\'' : (k == 0x9C || k == 0x9D) ? '"' : (k == 0x93 || k == 0x94) ? '-' : '?';
            s += 3;
        } else {
            dst[o++] = '?';
            s++; /* skip lead byte */
            while (((unsigned char)*s & 0xC0) == 0x80)
                s++; /* and its continuation bytes */
        }
    }
    dst[o] = 0;
}
