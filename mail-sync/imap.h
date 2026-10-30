/* imap.h - minimal IMAP4rev1 client: LOGIN / XOAUTH2, SELECT,
 * UID FETCH (UID FLAGS) and UID FETCH (UID FLAGS BODY[]).
 * TLS via OpenSSL. Plain fd mode exists for tests. */
#ifndef WMAIL_IMAP_H
#define WMAIL_IMAP_H

#include <stddef.h>

typedef struct Imap Imap;

/* Connect with TLS. ssl=1: implicit TLS; starttls=1: STARTTLS on plain.
 * Returns NULL with *err set on failure. */
Imap *imap_connect(const char *host, int port, int ssl, int starttls,
    char **err);

/* Wrap an already-connected plain fd (tests). Takes ownership of fd. */
Imap *imap_from_fd(int fd);

void imap_free(Imap *im);

/* Read the server greeting. 0 = OK. */
int imap_greeting(Imap *im, char **err);
int imap_login(Imap *im, const char *user, const char *pass, char **err);
int imap_oauth2(Imap *im, const char *user, const char *token, char **err);
int imap_logout(Imap *im);

/* SELECT a folder; reports UIDVALIDITY and UIDNEXT. */
int imap_select(Imap *im, const char *folder, unsigned long *uidvalidity,
    unsigned long *uidnext, char **err);

/* UID + FLAGS for every message in the selected folder. */
int imap_uid_flags(Imap *im, unsigned long **uids, char ***flags, int *n,
    char **err);

/* Fetch full bodies for a uid set. cb gets uid, IMAP flag string
 * (e.g. "\\Seen \\Flagged", without parens) and the raw RFC822 body. */
typedef void (*ImapMsgCb)(void *ctx, unsigned long uid, const char *flags,
    const char *body, size_t len);
int imap_uid_fetch(Imap *im, unsigned long *uids, int n, ImapMsgCb cb,
    void *ctx, char **err);

/* Low-level line reader, exposed for tests. Returns malloc'd line
 * without trailing CRLF, or NULL on EOF/error. */
char *imap_read_line(Imap *im);

#endif
