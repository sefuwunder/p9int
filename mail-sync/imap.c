/* imap.c - minimal IMAP4rev1 client over OpenSSL. */
#include "imap.h"
#include "util.h"

#include <ctype.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define RDBUF 8192

struct Imap {
	int fd;
	SSL *ssl;
	SSL_CTX *ctx;
	unsigned char rbuf[RDBUF];
	size_t rpos, rlen;
	int tagno;
	int failed; /* transport dead */
};

static int
im_fill(Imap *im)
{
	ssize_t n;
	if (im->failed)
		return -1;
	if (im->ssl) {
		n = SSL_read(im->ssl, im->rbuf, RDBUF);
	} else {
		n = read(im->fd, im->rbuf, RDBUF);
	}
	if (n <= 0) {
		im->failed = 1;
		return -1;
	}
	im->rpos = 0;
	im->rlen = (size_t)n;
	return 0;
}

static int
im_getc(Imap *im)
{
	if (im->rpos >= im->rlen && im_fill(im) < 0)
		return -1;
	return im->rbuf[im->rpos++];
}

/* Read exactly n bytes into buf (no CRLF handling). 0 = ok. */
static int
im_read_exact(Imap *im, char *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		if (im->rpos < im->rlen) {
			size_t avail = im->rlen - im->rpos;
			size_t take = avail < (n - got) ? avail : (n - got);
			memcpy(buf + got, im->rbuf + im->rpos, take);
			im->rpos += take;
			got += take;
		} else if (im_fill(im) < 0) {
			return -1;
		}
	}
	return 0;
}

char *
imap_read_line(Imap *im)
{
	size_t cap = 256, len = 0;
	char *buf = xmalloc(cap);
	int c;
	for (;;) {
		c = im_getc(im);
		if (c < 0) {
			free(buf);
			return NULL;
		}
		if (c == '\n')
			break;
		if (len + 1 >= cap) {
			cap *= 2;
			buf = xrealloc(buf, cap);
		}
		buf[len++] = (char)c;
	}
	/* Strip trailing CR. */
	if (len > 0 && buf[len - 1] == '\r')
		len--;
	buf[len] = 0;
	return buf;
}

static int
im_send(Imap *im, const char *s)
{
	size_t len = strlen(s), sent = 0;
	while (sent < len) {
		ssize_t n;
		if (im->ssl)
			n = SSL_write(im->ssl, s + sent, (int)(len - sent));
		else
			n = write(im->fd, s + sent, len - sent);
		if (n <= 0) {
			im->failed = 1;
			return -1;
		}
		sent += (size_t)n;
	}
	return 0;
}

static void
next_tag(Imap *im, char *buf, size_t bufsz)
{
	snprintf(buf, bufsz, "a%03d", ++im->tagno);
}

/* Send a command and read until its tagged response.
 * untagged lines go to cb (cb may be NULL). Returns 0 on tagged OK. */
typedef int (*LineCb)(Imap *im, void *ctx, char *line);
static int
im_cmd(Imap *im, const char *cmd, LineCb cb, void *ctx, char **err)
{
	char tag[16], *line, *full;
	next_tag(im, tag, sizeof tag);
	full = xstrfmt("%s %s\r\n", tag, cmd);
	if (im_send(im, full) < 0) {
		free(full);
		*err = xstrdup("failed to send command");
		return -1;
	}
	free(full);
	for (;;) {
		line = imap_read_line(im);
		if (!line) {
			*err = xstrdup("connection closed by server");
			return -1;
		}
		if (strncmp(line, tag, strlen(tag)) == 0 &&
		    line[strlen(tag)] == ' ') {
			/* Tagged response: "a001 OK ..." */
			char *p = line + strlen(tag) + 1;
			int ok = strncmp(p, "OK", 2) == 0 &&
			    (p[2] == ' ' || p[2] == 0);
			if (!ok)
				*err = xstrfmt("server error: %s", line);
			free(line);
			return ok ? 0 : -1;
		}
		if (line[0] == '+' && line[1] == 0) {
			/* Continuation: only expected by auth flows, which
			 * handle their own reads; treat as ignorable here. */
			free(line);
			continue;
		}
		if (cb && cb(im, ctx, line) < 0) {
			free(line);
			*err = xstrdup("response handler failed");
			return -1;
		}
		free(line);
	}
}

Imap *
imap_from_fd(int fd)
{
	Imap *im = xmalloc(sizeof *im);
	memset(im, 0, sizeof *im);
	im->fd = fd;
	return im;
}

static int
im_ssl_setup(Imap *im, const char *host, char **err)
{
	im->ctx = SSL_CTX_new(TLS_client_method());
	if (!im->ctx) {
		*err = xstrdup("SSL_CTX_new failed");
		return -1;
	}
	SSL_CTX_set_default_verify_paths(im->ctx);
	im->ssl = SSL_new(im->ctx);
	if (!im->ssl) {
		*err = xstrdup("SSL_new failed");
		return -1;
	}
	SSL_set_tlsext_host_name(im->ssl, host);
	SSL_set_fd(im->ssl, im->fd);
	if (SSL_connect(im->ssl) != 1) {
		*err = xstrfmt("TLS handshake failed: %s",
		    ERR_error_string(ERR_get_error(), NULL));
		return -1;
	}
	if (SSL_get_verify_result(im->ssl) != X509_V_OK) {
		*err = xstrdup("TLS certificate verification failed");
		return -1;
	}
	return 0;
}

Imap *
imap_connect(const char *host, int port, int ssl, int starttls, char **err)
{
	struct addrinfo hints, *res = NULL, *ai;
	char portstr[16];
	int fd = -1;
	Imap *im;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(portstr, sizeof portstr, "%d", port);
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
		*err = xstrfmt("cannot resolve %s", host);
		return NULL;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		*err = xstrfmt("cannot connect to %s:%d", host, port);
		return NULL;
	}
	im = imap_from_fd(fd);
	if (ssl) {
		if (im_ssl_setup(im, host, err) < 0) {
			imap_free(im);
			return NULL;
		}
		if (imap_greeting(im, err) < 0) {
			imap_free(im);
			return NULL;
		}
	} else if (starttls) {
		if (imap_greeting(im, err) < 0) {
			imap_free(im);
			return NULL;
		}
		if (im_cmd(im, "STARTTLS", NULL, NULL, err) < 0) {
			imap_free(im);
			return NULL;
		}
		if (im_ssl_setup(im, host, err) < 0) {
			imap_free(im);
			return NULL;
		}
	}
	return im;
}

void
imap_free(Imap *im)
{
	if (!im)
		return;
	if (im->ssl) {
		SSL_shutdown(im->ssl);
		SSL_free(im->ssl);
	}
	if (im->ctx)
		SSL_CTX_free(im->ctx);
	if (im->fd >= 0)
		close(im->fd);
	free(im);
}

int
imap_greeting(Imap *im, char **err)
{
	char *line = imap_read_line(im);
	if (!line) {
		*err = xstrdup("no greeting from server");
		return -1;
	}
	if (strncmp(line, "* OK", 4) != 0) {
		*err = xstrfmt("bad greeting: %s", line);
		free(line);
		return -1;
	}
	free(line);
	return 0;
}

/* Quote a string for IMAP, escaping " and \. */
static char *
im_quote(const char *s)
{
	size_t cap = strlen(s) * 2 + 3;
	char *out = xmalloc(cap), *d = out;
	*d++ = '"';
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			*d++ = '\\';
		*d++ = *s;
	}
	*d++ = '"';
	*d = 0;
	return out;
}

int
imap_login(Imap *im, const char *user, const char *pass, char **err)
{
	char *qu = im_quote(user), *qp = im_quote(pass);
	char *cmd = xstrfmt("LOGIN %s %s", qu, qp);
	int r;
	free(qu);
	free(qp);
	r = im_cmd(im, cmd, NULL, NULL, err);
	free(cmd);
	return r;
}

int
imap_oauth2(Imap *im, const char *user, const char *token, char **err)
{
	char *raw = xstrfmt("user=%s\x01auth=Bearer %s\x01\x01", user, token);
	char *blob = b64_encode((unsigned char *)raw, strlen(raw));
	char tag[16], *line;
	int ok = -1;
	free(raw);
	next_tag(im, tag, sizeof tag);
	{
		char *auth = xstrfmt("%s AUTHENTICATE XOAUTH2\r\n", tag);
		if (im_send(im, auth) < 0) {
			*err = xstrdup("failed to send AUTHENTICATE");
			free(auth);
			free(blob);
			return -1;
		}
		free(auth);
	}
	line = imap_read_line(im);
	if (!line || strcmp(line, "+") != 0) {
		*err = xstrfmt("unexpected XOAUTH2 challenge: %s",
		    line ? line : "(eof)");
		free(line);
		free(blob);
		return -1;
	}
	free(line);
	{
		char *b = xstrfmt("%s\r\n", blob);
		free(blob);
		if (im_send(im, b) < 0) {
			*err = xstrdup("failed to send XOAUTH2 blob");
			free(b);
			return -1;
		}
		free(b);
	}
	for (;;) {
		line = imap_read_line(im);
		if (!line) {
			*err = xstrdup("connection closed during XOAUTH2");
			return -1;
		}
		if (strncmp(line, tag, strlen(tag)) == 0) {
			char *p = line + strlen(tag) + 1;
			ok = (strncmp(p, "OK", 2) == 0 &&
			    (p[2] == ' ' || p[2] == 0)) ? 0 : -1;
			if (ok < 0)
				*err = xstrfmt("XOAUTH2 failed: %s", line);
			free(line);
			break;
		}
		free(line);
	}
	return ok;
}

int
imap_logout(Imap *im)
{
	char *err = NULL;
	int r = im_cmd(im, "LOGOUT", NULL, NULL, &err);
	free(err);
	return r;
}

typedef struct {
	unsigned long uidvalidity, uidnext;
} SelCtx;

static int
sel_cb(Imap *im, void *ctx, char *line)
{
	SelCtx *s = ctx;
	char *p;
	(void)im;
	if ((p = strstr(line, "[UIDVALIDITY ")) != NULL)
		s->uidvalidity = strtoul(p + 13, NULL, 10);
	else if ((p = strstr(line, "[UIDNEXT ")) != NULL)
		s->uidnext = strtoul(p + 9, NULL, 10);
	return 0;
}

int
imap_select(Imap *im, const char *folder, unsigned long *uidvalidity,
    unsigned long *uidnext, char **err)
{
	SelCtx s;
	char *q = im_quote(folder);
	char *cmd = xstrfmt("SELECT %s", q);
	int r;
	free(q);
	s.uidvalidity = 0;
	s.uidnext = 0;
	r = im_cmd(im, cmd, sel_cb, &s, err);
	free(cmd);
	if (r == 0) {
		*uidvalidity = s.uidvalidity;
		*uidnext = s.uidnext;
	}
	return r;
}

typedef struct {
	unsigned long **uids;
	char ***flags;
	int *n, cap;
} FlagsCtx;

static const char *
find_token(const char *line, const char *tok)
{
	const char *p = strstr(line, tok);
	if (!p)
		return NULL;
	return p + strlen(tok);
}

/* Parse one FETCH line's UID and FLAGS (no body). */
static int
parse_uid_flags(const char *line, unsigned long *uid, char *flagbuf,
    size_t flagbufsz)
{
	const char *p;
	*uid = 0;
	flagbuf[0] = 0;
	p = find_token(line, "UID ");
	if (p)
		*uid = strtoul(p, NULL, 10);
	p = find_token(line, "FLAGS (");
	if (p) {
		const char *e = strchr(p, ')');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n >= flagbufsz)
			n = flagbufsz - 1;
		memcpy(flagbuf, p, n);
		flagbuf[n] = 0;
	}
	return *uid != 0 ? 0 : -1;
}

static int
flags_cb(Imap *im, void *ctx, char *line)
{
	FlagsCtx *f = ctx;
	unsigned long uid;
	char flagbuf[512];
	(void)im;
	if (strncmp(line, "* ", 2) != 0 || !strstr(line, "FETCH"))
		return 0;
	if (parse_uid_flags(line, &uid, flagbuf, sizeof flagbuf) < 0)
		return 0;
	if (*f->n == f->cap) {
		f->cap = f->cap ? f->cap * 2 : 64;
		*f->uids = xrealloc(*f->uids, sizeof(unsigned long) * (size_t)f->cap);
		*f->flags = xrealloc(*f->flags, sizeof(char *) * (size_t)f->cap);
	}
	(*f->uids)[*f->n] = uid;
	(*f->flags)[*f->n] = xstrdup(flagbuf);
	(*f->n)++;
	return 0;
}

int
imap_uid_flags(Imap *im, unsigned long **uids, char ***flags, int *n,
    char **err)
{
	FlagsCtx f;
	int r;
	*uids = NULL;
	*flags = NULL;
	*n = 0;
	f.uids = uids;
	f.flags = flags;
	f.n = n;
	f.cap = 0;
	r = im_cmd(im, "UID FETCH 1:* (UID FLAGS)", flags_cb, &f, err);
	return r;
}

typedef struct {
	ImapMsgCb cb;
	void *ctx;
} FetchCtx;

static int
fetch_cb(Imap *im, void *ctx, char *line)
{
	FetchCtx *f = ctx;
	unsigned long uid;
	char flagbuf[512];
	const char *p;
	char *body = NULL;
	size_t bodylen = 0;

	if (strcmp(line, ")") == 0)
		return 0; /* trailing paren after a literal */
	if (strncmp(line, "* ", 2) != 0 || !strstr(line, "FETCH"))
		return 0;
	if (parse_uid_flags(line, &uid, flagbuf, sizeof flagbuf) < 0)
		return 0;
	p = strstr(line, "BODY[] {");
	if (p) {
		unsigned long litlen = strtoul(p + 8, NULL, 10);
		body = xmalloc(litlen + 1);
		if (im_read_exact(im, body, litlen) < 0) {
			free(body);
			return -1;
		}
		body[litlen] = 0;
		bodylen = litlen;
		/* Consume the CRLF after the literal. */
		{
			char crlf[2];
			if (im_read_exact(im, crlf, 2) < 0) {
				free(body);
				return -1;
			}
		}
	} else if (strstr(line, "BODY[] \"\"")) {
		body = xstrdup("");
		bodylen = 0;
	} else {
		/* No body in this response; skip. */
		return 0;
	}
	f->cb(f->ctx, uid, flagbuf, body, bodylen);
	free(body);
	return 0;
}

int
imap_uid_fetch(Imap *im, unsigned long *uids, int n, ImapMsgCb cb, void *ctx,
    char **err)
{
	FetchCtx f;
	int i, r = 0;

	f.cb = cb;
	f.ctx = ctx;
	/* Chunked uid sets: one command per 50 uids. */
	for (i = 0; i < n && r == 0;) {
		int j, chunk = n - i > 50 ? 50 : n - i;
		size_t cap = 64 + (size_t)chunk * 22;
		char *set = xmalloc(cap), *cmd;
		set[0] = 0;
		for (j = 0; j < chunk; j++) {
			char num[24];
			snprintf(num, sizeof num, "%lu", uids[i + j]);
			if (j > 0)
				strncat(set, ",", cap - strlen(set) - 1);
			strncat(set, num, cap - strlen(set) - 1);
		}
		cmd = xstrfmt("UID FETCH %s (UID FLAGS BODY[])", set);
		free(set);
		r = im_cmd(im, cmd, fetch_cb, &f, err);
		free(cmd);
		i += chunk;
	}
	return r;
}
