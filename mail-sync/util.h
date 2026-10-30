/* util.h - small shared helpers for wmail-sync. */
#ifndef WMAIL_UTIL_H
#define WMAIL_UTIL_H

#include <stddef.h>

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xstrfmt(const char *fmt, ...); /* malloc'd printf */

/* Base64 encode, no line breaks. Returns malloc'd string. */
char *b64_encode(const unsigned char *in, size_t len);

/* Case-insensitive substring search; NULL-safe. */
int strcasestr_(const char *hay, const char *needle);

#endif
