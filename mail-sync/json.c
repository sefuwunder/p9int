/* json.c - minimal JSON parser for busctl --json output. */
#include "json.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const char *p;
	char *err;
} P;

static Json *
newnode(JType t)
{
	Json *j = xmalloc(sizeof *j);
	memset(j, 0, sizeof *j);
	j->t = t;
	return j;
}

static void
push(Json *j, Json *item, const char *key)
{
	if (j->n == j->cap) {
		j->cap = j->cap ? j->cap * 2 : 8;
		j->items = xrealloc(j->items, sizeof(Json *) * (size_t)j->cap);
		if (j->t == J_OBJ)
			j->keys = xrealloc(j->keys, sizeof(char *) * (size_t)j->cap);
	}
	j->items[j->n] = item;
	if (j->t == J_OBJ)
		j->keys[j->n] = xstrdup(key ? key : "");
	j->n++;
}

void
json_free(Json *j)
{
	int i;
	if (!j)
		return;
	free(j->s);
	for (i = 0; i < j->n; i++) {
		json_free(j->items[i]);
		if (j->t == J_OBJ)
			free(j->keys[i]);
	}
	free(j->items);
	free(j->keys);
	free(j);
}

static void
skipws(P *p)
{
	while (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r')
		p->p++;
}

static Json *parse_value(P *p);

static void
utf8_encode(char **dst, unsigned cp)
{
	char *d = *dst;
	if (cp < 0x80) {
		*d++ = (char)cp;
	} else if (cp < 0x800) {
		*d++ = (char)(0xC0 | (cp >> 6));
		*d++ = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		*d++ = (char)(0xE0 | (cp >> 12));
		*d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*d++ = (char)(0x80 | (cp & 0x3F));
	} else {
		*d++ = (char)(0xF0 | (cp >> 18));
		*d++ = (char)(0x80 | ((cp >> 12) & 0x3F));
		*d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*d++ = (char)(0x80 | (cp & 0x3F));
	}
	*dst = d;
}

static char *
parse_string_raw(P *p)
{
	/* p->p points at the opening quote. Returns malloc'd UTF-8. */
	size_t cap = 32, len = 0;
	char *buf, *d;
	if (*p->p != '"') {
		p->err = xstrdup("expected string");
		return NULL;
	}
	p->p++;
	buf = xmalloc(cap);
	d = buf;
	for (;;) {
		unsigned char c = (unsigned char)*p->p;
		if (c == 0) {
			p->err = xstrdup("unterminated string");
			free(buf);
			return NULL;
		}
		if (c == '"') {
			p->p++;
			break;
		}
		if (c == '\\') {
			unsigned cp;
			p->p++;
			switch (*p->p) {
			case '"': c = '"'; p->p++; break;
			case '\\': c = '\\'; p->p++; break;
			case '/': c = '/'; p->p++; break;
			case 'b': c = '\b'; p->p++; break;
			case 'f': c = '\f'; p->p++; break;
			case 'n': c = '\n'; p->p++; break;
			case 'r': c = '\r'; p->p++; break;
			case 't': c = '\t'; p->p++; break;
			case 'u': {
				int i;
				cp = 0;
				p->p++;
				for (i = 0; i < 4; i++) {
					char h = *p->p++;
					cp <<= 4;
					if (h >= '0' && h <= '9')
						cp |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f')
						cp |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F')
						cp |= (unsigned)(h - 'A' + 10);
					else {
						p->err = xstrdup("bad \\u escape");
						free(buf);
						return NULL;
					}
				}
				/* Surrogate pair. */
				if (cp >= 0xD800 && cp <= 0xDBFF &&
				    p->p[0] == '\\' && p->p[1] == 'u') {
					unsigned lo = 0;
					p->p += 2;
					for (i = 0; i < 4; i++) {
						char h = *p->p++;
						lo <<= 4;
						if (h >= '0' && h <= '9')
							lo |= (unsigned)(h - '0');
						else if (h >= 'a' && h <= 'f')
							lo |= (unsigned)(h - 'a' + 10);
						else if (h >= 'A' && h <= 'F')
							lo |= (unsigned)(h - 'A' + 10);
						else {
							p->err = xstrdup("bad surrogate");
							free(buf);
							return NULL;
						}
					}
					if (lo >= 0xDC00 && lo <= 0xDFFF)
						cp = 0x10000 + ((cp - 0xD800) << 10) +
						    (lo - 0xDC00);
				}
				if (len + 4 >= cap) {
					cap *= 2;
					buf = xrealloc(buf, cap);
					d = buf + len;
				}
				utf8_encode(&d, cp);
				len = (size_t)(d - buf);
				continue;
			}
			default:
				p->err = xstrdup("bad escape");
				free(buf);
				return NULL;
			}
		} else {
			p->p++;
		}
		if (len + 1 >= cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
			d = buf + len;
		}
		*d++ = (char)c;
		len++;
	}
	*d = 0;
	return buf;
}

static Json *
parse_value(P *p)
{
	Json *j;
	skipws(p);
	switch (*p->p) {
	case '{': {
		p->p++;
		j = newnode(J_OBJ);
		skipws(p);
		if (*p->p == '}') {
			p->p++;
			return j;
		}
		for (;;) {
			char *key;
			Json *val;
			skipws(p);
			key = parse_string_raw(p);
			if (!key) {
				json_free(j);
				return NULL;
			}
			skipws(p);
			if (*p->p != ':') {
				p->err = xstrdup("expected ':' in object");
				free(key);
				json_free(j);
				return NULL;
			}
			p->p++;
			val = parse_value(p);
			if (!val) {
				free(key);
				json_free(j);
				return NULL;
			}
			push(j, val, key);
			free(key);
			skipws(p);
			if (*p->p == ',') {
				p->p++;
				continue;
			}
			if (*p->p == '}') {
				p->p++;
				return j;
			}
			p->err = xstrdup("expected ',' or '}' in object");
			json_free(j);
			return NULL;
		}
	}
	case '[': {
		p->p++;
		j = newnode(J_ARR);
		skipws(p);
		if (*p->p == ']') {
			p->p++;
			return j;
		}
		for (;;) {
			Json *val = parse_value(p);
			if (!val) {
				json_free(j);
				return NULL;
			}
			push(j, val, NULL);
			skipws(p);
			if (*p->p == ',') {
				p->p++;
				continue;
			}
			if (*p->p == ']') {
				p->p++;
				return j;
			}
			p->err = xstrdup("expected ',' or ']' in array");
			json_free(j);
			return NULL;
		}
	}
	case '"': {
		char *s = parse_string_raw(p);
		if (!s)
			return NULL;
		j = newnode(J_STR);
		j->s = s;
		return j;
	}
	case 't':
		if (strncmp(p->p, "true", 4) == 0) {
			p->p += 4;
			j = newnode(J_BOOL);
			j->b = 1;
			return j;
		}
		break;
	case 'f':
		if (strncmp(p->p, "false", 5) == 0) {
			p->p += 5;
			j = newnode(J_BOOL);
			j->b = 0;
			return j;
		}
		break;
	case 'n':
		if (strncmp(p->p, "null", 4) == 0) {
			p->p += 4;
			return newnode(J_NULL);
		}
		break;
	default:
		break;
	}
	if (*p->p == '-' || (*p->p >= '0' && *p->p <= '9')) {
		char *end;
		j = newnode(J_NUM);
		j->num = strtod(p->p, &end);
		if (end == p->p) {
			json_free(j);
			p->err = xstrdup("bad number");
			return NULL;
		}
		p->p = end;
		return j;
	}
	p->err = xstrdup("unexpected character in JSON");
	return NULL;
}

Json *
json_parse(const char *text, char **err)
{
	P p;
	Json *j;
	p.p = text;
	p.err = NULL;
	j = parse_value(&p);
	if (!j) {
		if (err)
			*err = p.err ? p.err : xstrdup("JSON parse error");
		else
			free(p.err);
		return NULL;
	}
	skipws(&p);
	if (*p.p) {
		json_free(j);
		if (err)
			*err = xstrdup("trailing data after JSON value");
		return NULL;
	}
	return j;
}

Json *
json_get(Json *j, const char *key)
{
	int i;
	if (!j || j->t != J_OBJ)
		return NULL;
	for (i = 0; i < j->n; i++)
		if (strcmp(j->keys[i], key) == 0)
			return j->items[i];
	return NULL;
}

Json *
json_unwrap(Json *j)
{
	while (j && j->t == J_OBJ && j->n == 2 &&
	    json_get(j, "type") && json_get(j, "data"))
		j = json_get(j, "data");
	return j;
}

const char *
json_as_str(Json *j)
{
	j = json_unwrap(j);
	if (j && j->t == J_STR)
		return j->s;
	return NULL;
}

int
json_as_bool(Json *j, int dflt)
{
	j = json_unwrap(j);
	if (j && j->t == J_BOOL)
		return j->b;
	return dflt;
}
