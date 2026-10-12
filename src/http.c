#include "http.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local CURL *t_curl;

void http_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_thread_cleanup(void)
{
    if (t_curl) {
        curl_easy_cleanup(t_curl);
        t_curl = NULL;
    }
}

static size_t on_write(char *data, size_t sz, size_t nm, void *ud)
{
    Buf *b = ud;
    size_t n = sz * nm;
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 16384;
        while (nc < b->n + n + 1)
            nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np)
            return 0;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
    return n;
}

long http_get(const char *url, Buf *out)
{
    /* ESPN's $ref links are plain http; use https. */
    char fixed[1024];
    if (!strncmp(url, "http://", 7))
        snprintf(fixed, sizeof fixed, "https://%s", url + 7);
    else
        snprintf(fixed, sizeof fixed, "%s", url);

    if (!t_curl) {
        t_curl = curl_easy_init();
        if (!t_curl)
            return 0;
    }
    long status = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        out->n = 0;
        if (out->p)
            out->p[0] = 0;
        curl_easy_reset(t_curl);
        curl_easy_setopt(t_curl, CURLOPT_URL, fixed);
        curl_easy_setopt(t_curl, CURLOPT_WRITEFUNCTION, on_write);
        curl_easy_setopt(t_curl, CURLOPT_WRITEDATA, out);
        curl_easy_setopt(t_curl, CURLOPT_USERAGENT, "huddle/0.2 (+local NFL viewer)");
        curl_easy_setopt(t_curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(t_curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(t_curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(t_curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(t_curl, CURLOPT_NOSIGNAL, 1L);
        CURLcode rc = curl_easy_perform(t_curl);
        status = 0;
        if (rc == CURLE_OK)
            curl_easy_getinfo(t_curl, CURLINFO_RESPONSE_CODE, &status);
        if (rc == CURLE_OK && status < 500 && status != 429)
            return status; /* success, or a definitive 4xx such as 404 */
        usleep(400000u * (unsigned)(attempt + 1));
    }
    return status;
}

yyjson_doc *http_get_json(const char *url, long *status)
{
    Buf b = {0};
    long st = http_get(url, &b);
    if (status)
        *status = st;
    yyjson_doc *doc = NULL;
    if (st == 200 && b.n)
        doc = yyjson_read(b.p, b.n, 0);
    free(b.p);
    return doc;
}

typedef struct {
    atomic_int next;
    int n;
    ParallelFn fn;
    void *ctx;
} PJob;

static void *pworker(void *p)
{
    PJob *j = p;
    for (;;) {
        int i = atomic_fetch_add(&j->next, 1);
        if (i >= j->n)
            break;
        j->fn(i, j->ctx);
    }
    http_thread_cleanup();
    return NULL;
}

void parallel_for(int n, int nthreads, ParallelFn fn, void *ctx)
{
    if (n <= 0)
        return;
    if (nthreads > n)
        nthreads = n;
    PJob j = {.n = n, .fn = fn, .ctx = ctx};
    atomic_init(&j.next, 0);
    pthread_t th[32];
    if (nthreads > 32)
        nthreads = 32;
    int started = 0;
    for (int i = 0; i < nthreads; i++)
        if (pthread_create(&th[started], NULL, pworker, &j) == 0)
            started++;
    if (started == 0) { /* couldn't spawn: do it inline */
        pworker(&j);
        return;
    }
    for (int i = 0; i < started; i++)
        pthread_join(th[i], NULL);
}
