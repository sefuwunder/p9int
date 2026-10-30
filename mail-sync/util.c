/* util.c */
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *
xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) {
		fprintf(stderr, "mail-sync: out of memory\n");
		exit(1);
	}
	return p;
}

void *
xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q) {
		fprintf(stderr, "mail-sync: out of memory\n");
		exit(1);
	}
	return q;
}

char *
xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *
xstrndup(const char *s, size_t n)
{
	char *d = xmalloc(n + 1);
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

char *
xstrfmt(const char *fmt, ...)
{
	va_list ap, ap2;
	int n;
	char *buf;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) {
		va_end(ap2);
		return xstrdup("");
	}
	buf = xmalloc((size_t)n + 1);
	vsnprintf(buf, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	return buf;
}

static const char b64tab[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *
b64_encode(const unsigned char *in, size_t len)
{
	size_t outlen = ((len + 2) / 3) * 4;
	char *out = xmalloc(outlen + 1);
	size_t i, o = 0;

	for (i = 0; i < len; i += 3) {
		unsigned v = in[i] << 16;
		int rem = (int)(len - i);
		if (rem > 1)
			v |= in[i + 1] << 8;
		if (rem > 2)
			v |= in[i + 2];
		out[o++] = b64tab[(v >> 18) & 63];
		out[o++] = b64tab[(v >> 12) & 63];
		out[o++] = rem > 1 ? b64tab[(v >> 6) & 63] : '=';
		out[o++] = rem > 2 ? b64tab[v & 63] : '=';
	}
	out[o] = 0;
	return out;
}

int
strcasestr_(const char *hay, const char *needle)
{
	size_t nl;
	if (!hay || !needle)
		return 0;
	nl = strlen(needle);
	if (nl == 0)
		return 1;
	for (; *hay; hay++) {
		size_t i;
		for (i = 0; i < nl; i++) {
			if (tolower((unsigned char)hay[i]) !=
			    tolower((unsigned char)needle[i]))
				break;
		}
		if (i == nl)
			return 1;
	}
	return 0;
}
