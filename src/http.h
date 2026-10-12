#ifndef HUDDLE_HTTP_H
#define HUDDLE_HTTP_H
#include <stddef.h>
#include <yyjson.h>

typedef struct {
    char *p;
    size_t n, cap;
} Buf;

void http_init(void);
/* Each worker thread keeps one curl handle; call before the thread exits. */
void http_thread_cleanup(void);
/* GET with retries. Returns HTTP status (200, 404, ...) or 0 on transport failure. */
long http_get(const char *url, Buf *out);
/* GET + parse. Returns NULL unless status is 200 and the body is valid JSON. */
yyjson_doc *http_get_json(const char *url, long *status);

/* Run fn(i, ctx) for i in [0,n) on up to nthreads threads. */
typedef void (*ParallelFn)(int i, void *ctx);
void parallel_for(int n, int nthreads, ParallelFn fn, void *ctx);

#endif
