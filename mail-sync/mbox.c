/* mbox.c - mbox storage + sync state for mail-sync. See mbox.h. */
#include "mbox.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

void
imap_flags_to_status(const char *imap_flags, char *out, size_t outsz)
{
	size_t o = 0;
	if (outsz == 0)
		return;
	if (imap_flags) {
		if (strcasestr_(imap_flags, "\\Seen"))
			out[o++] = 'R';
		if (strcasestr_(imap_flags, "\\Flagged"))
			out[o++] = 'F';
		if (strcasestr_(imap_flags, "\\Answered"))
			out[o++] = 'A';
		if (strcasestr_(imap_flags, "\\Deleted"))
			out[o++] = 'D';
	}
	out[o] = 0;
}

int
mbox_mkdir_p(const char *path, char **err)
{
	char *tmp, *p;
	struct stat st;
	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			return 0;
		*err = xstrfmt("%s is not a directory", path);
		return -1;
	}
	tmp = xstrdup(path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0777) < 0 && errno != EEXIST) {
				*err = xstrfmt("mkdir %s: %s", tmp,
				    strerror(errno));
				free(tmp);
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, 0777) < 0 && errno != EEXIST) {
		*err = xstrfmt("mkdir %s: %s", tmp, strerror(errno));
		free(tmp);
		return -1;
	}
	free(tmp);
	return 0;
}

/* Sender for the "From " separator line: the address from the
 * message's From: header (between <...> when present, else the first
 * token), or "unknown" when the header is missing. */
static char *
from_sender(const char *body, size_t len)
{
	size_t i = 0;
	while (i < len) {
		size_t ls = i;
		while (i < len && body[i] != '\n')
			i++;
		size_t llen = i - ls;
		if (i < len)
			i++; /* skip \n */
		/* End of headers: blank line. */
		if (llen == 0 || (llen == 1 && body[ls] == '\r'))
			break;
		if (llen >= 5 && strncasecmp(body + ls, "From:", 5) == 0) {
			size_t j = ls + 5;
			while (j < ls + llen &&
			    (body[j] == ' ' || body[j] == '\t'))
				j++;
			/* Prefer <addr>. */
			size_t k;
			for (k = j; k < ls + llen; k++) {
				if (body[k] == '<') {
					size_t e = k + 1;
					while (e < ls + llen &&
					    body[e] != '>')
						e++;
					if (e < ls + llen && e > k + 1)
						return xstrndup(body + k + 1,
						    e - k - 1);
					break;
				}
			}
			/* Else first token, stopping at whitespace. */
			size_t e = j;
			while (e < ls + llen && body[e] != ' ' &&
			    body[e] != '\t' && body[e] != '\r')
				e++;
			if (e > j)
				return xstrndup(body + j, e - j);
			break;
		}
	}
	return xstrdup("unknown");
}

/* Write the body with mboxrd escaping: any line starting with "From "
 * gains a ">" prefix. Returns 0 on success, -1 on write error. */
static int
write_escaped(FILE *f, const char *body, size_t len)
{
	size_t i = 0;
	int at_line_start = 1;
	while (i < len) {
		if (at_line_start && len - i >= 5 &&
		    strncmp(body + i, "From ", 5) == 0) {
			if (fputc('>', f) == EOF)
				return -1;
		}
		if (fputc((unsigned char)body[i], f) == EOF)
			return -1;
		at_line_start = (body[i] == '\n');
		i++;
	}
	return 0;
}

int
mbox_append(const char *path, unsigned long uid, const char *imap_flags,
    const char *body, size_t len, char **err)
{
	char status[16], *sender, *parent;
	char datestr[64];
	time_t now;
	FILE *f;

	/* Create parent directories. */
	parent = xstrdup(path);
	{
		char *slash = strrchr(parent, '/');
		if (slash) {
			*slash = 0;
			if (mbox_mkdir_p(parent, err) < 0) {
				free(parent);
				return -1;
			}
		}
	}
	free(parent);

	f = fopen(path, "ab");
	if (!f) {
		*err = xstrfmt("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	imap_flags_to_status(imap_flags, status, sizeof status);
	sender = from_sender(body, len);
	now = time(NULL);
	{
		char *ct = ctime(&now); /* "Www Mmm dd hh:mm:ss yyyy\n" */
		size_t n = ct ? strlen(ct) : 0;
		if (n > 0 && ct[n - 1] == '\n')
			ct[n - 1] = 0;
		snprintf(datestr, sizeof datestr, "%s", ct ? ct : "unknown");
	}
	if (fprintf(f, "From %s %s\n", sender, datestr) < 0 ||
	    fprintf(f, "X-Mail-Sync-Uid: %lu\n", uid) < 0 ||
	    fprintf(f, "Status: %s\n", status) < 0 ||
	    write_escaped(f, body, len) < 0) {
		*err = xstrfmt("cannot write %s: %s", path, strerror(errno));
		fclose(f);
		free(sender);
		return -1;
	}
	free(sender);
	/* End with a blank line: one \n if the body already ends with
	 * one, two if it ends without, so the next separator starts on
	 * its own line and upas never merges two messages. */
	if (len == 0 || body[len - 1] != '\n') {
		if (fputc('\n', f) == EOF)
			goto werr;
	}
	if (fputc('\n', f) == EOF)
		goto werr;
	if (fclose(f) != 0) {
		*err = xstrfmt("cannot close %s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
werr:
	*err = xstrfmt("cannot write %s: %s", path, strerror(errno));
	fclose(f);
	return -1;
}

/* ---- reflag by whole-file rewrite ---- */

/* A parsed message: byte ranges into the file buffer. hdr_end is the
 * offset just past the header block's blank line (or the message end
 * when there is no blank line); blank_start is where that blank line
 * begins (== hdr_end when there is none); status_off/status_len
 * locate the existing "Status:" line within the header block, or -1. */
typedef struct {
	size_t start, end;	/* whole message incl. From_ line */
	size_t hdr_end, blank_start;
	long status_off, status_len;
	unsigned long uid;
	int has_uid;
} MsgSpan;

static int
parse_messages(const char *buf, size_t len, MsgSpan **out, int *nout)
{
	MsgSpan *spans = NULL;
	int n = 0, cap = 0;
	size_t i = 0;

	while (i < len) {
		MsgSpan m;
		size_t p;
		/* Each message starts with a "From " line. */
		if (len - i < 5 || strncmp(buf + i, "From ", 5) != 0)
			break; /* trailing garbage: ignore */
		m.start = i;
		m.uid = 0;
		m.has_uid = 0;
		m.status_off = -1;
		m.status_len = 0;
		/* Message ends at the next "From " line start, or EOF. */
		m.end = len;
		{
			size_t q = i;
			while (q < len) {
				if (buf[q] == '\n' && q + 1 < len &&
				    len - (q + 1) >= 5 &&
				    strncmp(buf + q + 1, "From ", 5) == 0) {
					m.end = q + 1;
					break;
				}
				q++;
			}
		}
		/* Header block: skip the From_ line, then lines until the
		 * first blank line, all within this message. */
		p = i;
		while (p < m.end && buf[p] != '\n')
			p++;
		if (p < m.end)
			p++;
		m.hdr_end = m.end;
		m.blank_start = m.end;
		while (p < m.end) {
			size_t ls = p;
			while (p < m.end && buf[p] != '\n')
				p++;
			size_t llen = p - ls;
			if (p < m.end)
				p++;
			if (llen == 0 || (llen == 1 && buf[ls] == '\r')) {
				m.blank_start = ls;
				m.hdr_end = p;
				break;
			}
			if (llen > 17 &&
			    strncasecmp(buf + ls, "X-Mail-Sync-Uid:", 16) == 0) {
				const char *s = buf + ls + 16;
				while (s < buf + ls + llen &&
				    (*s == ' ' || *s == '\t'))
					s++;
				m.uid = strtoul(s, NULL, 10);
				m.has_uid = 1;
			}
			if (llen >= 7 &&
			    strncasecmp(buf + ls, "Status:", 7) == 0) {
				m.status_off = (long)ls;
				m.status_len = (long)(p - ls);
			}
		}
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			spans = xrealloc(spans, sizeof(MsgSpan) * (size_t)cap);
		}
		spans[n++] = m;
		i = m.end;
	}
	*out = spans;
	*nout = n;
	return 0;
}

int
mbox_reflag(const char *path, unsigned long uid, const char *imap_flags,
    char **err)
{
	char status[16], *tmp, *buf = NULL;
	FILE *f;
	size_t len = 0, cap = 0;
	MsgSpan *spans = NULL;
	int nspans = 0, i, found = 0;

	f = fopen(path, "rb");
	if (!f) {
		*err = xstrfmt("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	{
		char chunk[8192];
		size_t r;
		while ((r = fread(chunk, 1, sizeof chunk, f)) > 0) {
			if (len + r > cap) {
				cap = (len + r) * 2 + 64;
				buf = xrealloc(buf, cap);
			}
			memcpy(buf + len, chunk, r);
			len += r;
		}
	}
	fclose(f);

	parse_messages(buf, len, &spans, &nspans);
	for (i = 0; i < nspans; i++)
		if (spans[i].has_uid && spans[i].uid == uid) {
			found = 1;
			break;
		}
	if (!found) {
		free(buf);
		free(spans);
		return 1;
	}

	imap_flags_to_status(imap_flags, status, sizeof status);
	tmp = xstrfmt("%s.tmp", path);
	f = fopen(tmp, "wb");
	if (!f) {
		*err = xstrfmt("cannot create %s: %s", tmp, strerror(errno));
		free(tmp);
		free(buf);
		free(spans);
		return -1;
	}
	for (i = 0; i < nspans; i++) {
		MsgSpan *m = &spans[i];
		if (m->has_uid && m->uid == uid) {
			if (m->status_off >= 0) {
				size_t head_end = (size_t)m->status_off;
				if (fwrite(buf + m->start, 1,
				    head_end - m->start, f) !=
				    head_end - m->start)
					goto werr;
				if (fprintf(f, "Status: %s\n", status) < 0)
					goto werr;
				size_t tail = (size_t)m->status_off +
				    (size_t)m->status_len;
				if (fwrite(buf + tail, 1, m->end - tail, f) !=
				    m->end - tail)
					goto werr;
			} else {
				/* Insert a Status line just before the
				 * header block's blank line (or at the
				 * message end when there is none). */
				size_t ins = m->blank_start;
				if (fwrite(buf + m->start, 1, ins - m->start,
				    f) != ins - m->start)
					goto werr;
				if (fprintf(f, "Status: %s\n", status) < 0)
					goto werr;
				if (fwrite(buf + ins, 1, m->end - ins, f) !=
				    m->end - ins)
					goto werr;
			}
		} else {
			if (fwrite(buf + m->start, 1, m->end - m->start,
			    f) != m->end - m->start)
				goto werr;
		}
	}
	/* Preserve any trailing bytes the parser ignored. */
	if (nspans > 0 && spans[nspans - 1].end < len) {
		if (fwrite(buf + spans[nspans - 1].end, 1,
		    len - spans[nspans - 1].end, f) !=
		    len - spans[nspans - 1].end)
			goto werr;
	}
	if (fclose(f) != 0) {
		*err = xstrfmt("cannot close %s: %s", tmp, strerror(errno));
		unlink(tmp);
		free(tmp);
		free(buf);
		free(spans);
		return -1;
	}
	if (rename(tmp, path) < 0) {
		*err = xstrfmt("cannot rename %s to %s: %s", tmp, path,
		    strerror(errno));
		unlink(tmp);
		free(tmp);
		free(buf);
		free(spans);
		return -1;
	}
	free(tmp);
	free(buf);
	free(spans);
	return 0;
werr:
	*err = xstrfmt("cannot write %s: %s", tmp, strerror(errno));
	fclose(f);
	unlink(tmp);
	free(tmp);
	free(buf);
	free(spans);
	return -1;
}

/* ---- state ---- */

int
mbox_state_load(const char *path, MboxState *st)
{
	FILE *f = fopen(path, "r");
	char line[1024];

	memset(st, 0, sizeof *st);
	if (!f)
		return 0; /* no state yet: fresh sync */
	if (!fgets(line, sizeof line, f)) {
		fclose(f);
		return 0;
	}
	{
		unsigned long v, n;
		if (sscanf(line, "v1 %lu %lu", &v, &n) == 2) {
			st->uidvalidity = v;
			st->uidnext = n;
		}
	}
	while (fgets(line, sizeof line, f)) {
		unsigned long uid;
		char flags[64];
		int got = sscanf(line, "%lu %63s", &uid, flags);
		if (got == 2)
			mbox_state_put(st, uid, flags);
		else if (got == 1)
			mbox_state_put(st, uid, "");
	}
	fclose(f);
	return 0;
}

int
mbox_state_save(const char *path, MboxState *st)
{
	char *tmp = xstrfmt("%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	int i;
	if (!f) {
		free(tmp);
		return -1;
	}
	fprintf(f, "v1 %lu %lu\n", st->uidvalidity, st->uidnext);
	for (i = 0; i < st->n; i++)
		fprintf(f, "%lu %s\n", st->ents[i].uid, st->ents[i].flags);
	if (fclose(f) != 0) {
		unlink(tmp);
		free(tmp);
		return -1;
	}
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		free(tmp);
		return -1;
	}
	free(tmp);
	return 0;
}

MboxStateEnt *
mbox_state_find(MboxState *st, unsigned long uid)
{
	int i;
	for (i = 0; i < st->n; i++)
		if (st->ents[i].uid == uid)
			return &st->ents[i];
	return NULL;
}

void
mbox_state_put(MboxState *st, unsigned long uid, const char *flags)
{
	MboxStateEnt *e = mbox_state_find(st, uid);
	if (e) {
		free(e->flags);
		e->flags = xstrdup(flags);
		return;
	}
	if (st->n == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 64;
		st->ents = xrealloc(st->ents, sizeof(MboxStateEnt) * (size_t)st->cap);
	}
	e = &st->ents[st->n++];
	e->uid = uid;
	e->flags = xstrdup(flags);
}

void
mbox_state_free(MboxState *st)
{
	int i;
	for (i = 0; i < st->n; i++)
		free(st->ents[i].flags);
	free(st->ents);
	memset(st, 0, sizeof *st);
}
