#ifndef HUDDLE_STORE_H
#define HUDDLE_STORE_H
#include <stdbool.h>
#include <stddef.h>

#include "model.h"

/* Season <-> JSON snapshot. The same format is used for the data compiled into
 * the program and for the on-disk cache written by the Update button. */
bool store_save(const Season *s, const char *path);
Season *store_load_mem(const char *buf, size_t len);
Season *store_load_file(const char *path);

#endif
