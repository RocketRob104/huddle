#ifndef HUDDLE_UTIL_H
#define HUDDLE_UTIL_H
#include <stddef.h>

/* Returns a permanent, de-duplicated copy of s (thread-safe). Never freed. */
const char *intern(const char *s);

/* strncpy that always terminates and tolerates NULL. */
void copy_str(char *dst, size_t n, const char *src);

/* Local time as "YYYY-MM-DD HH:MM" (needs 24 bytes). */
void now_string(char out[24]);

/* Fold UTF-8 to plain ASCII (accents stripped, quotes/dashes mapped) so it can be
 * drawn with SDL's built-in 8x8 font. */
void fold_ascii(char *dst, size_t n, const char *utf8);

#endif
