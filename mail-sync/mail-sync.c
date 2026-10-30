/* mail-sync - sync GOA mail accounts to local mbox files for Plan 9.
 *
 * Derived from wily's tools/wmail/wmail-sync: the GOA discovery,
 * credential flow, and IMAP client are reused verbatim; only the
 * storage changes, from maildir to one mbox file per folder — the
 * form Plan 9's upas/fs reads (`upas/fs -f <file>`).
 *
 * v1: one-way IMAP -> mbox, incremental via a state file.
 * Credentials come from GNOME Online Accounts on demand and are never
 * stored. No expunge propagation, no MIME part extraction, no sending
 * yet. See README.md. */
#include "goa.h"
#include "imap.h"
#include "mbox.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int verbose = 0;

static void
usage(void)
{
	fprintf(stderr,
	    "usage: mail-sync [-a account] [-f folder]... [-d root] [-l] [-v]\n"
	    "  -a account  match a GOA account by email substring (default: first)\n"
	    "  -f folder   IMAP folder to sync (default: INBOX; repeatable)\n"
	    "  -d root     mbox root (default: $MAILSYNC_DIR or ~/Mail)\n"
	    "  -l          list GOA mail accounts and exit\n"
	    "  -v          verbose\n");
	exit(2);
}

typedef struct {
	const char *mboxpath;
	MboxState *st;
	int nnew;
	char *err;
} FetchCtx;

static void
fetch_cb(void *ctx, unsigned long uid, const char *flags, const char *body,
    size_t len)
{
	FetchCtx *f = ctx;
	char status[16];
	char *err = NULL;

	imap_flags_to_status(flags, status, sizeof status);
	if (mbox_append(f->mboxpath, uid, flags, body, len, &err) < 0) {
		if (!f->err)
			f->err = err;
		else
			free(err);
		return;
	}
	mbox_state_put(f->st, uid, status);
	f->nnew++;
	if (verbose)
		fprintf(stderr, "  new uid %lu (%s)\n", uid, status);
}

static char *
sanitize_folder(const char *folder)
{
	char *s = xstrdup(folder), *p;
	for (p = s; *p; p++)
		if (*p == '/')
			*p = '.';
	return s;
}

static int
sync_folder(GoaAccount *a, GoaCreds *creds, const char *folder,
    const char *root)
{
	char *san = sanitize_folder(folder);
	char *emaildir = xstrfmt("%s/%s", root, a->email);
	char *mboxpath = xstrfmt("%s/%s.mbox", emaildir, san);
	char *statepath = xstrfmt("%s/%s.state", emaildir, san);
	char *err = NULL;
	MboxState st;
	Imap *im;
	unsigned long uidvalidity = 0, uidnext = 0;
	unsigned long *uids = NULL;
	char **flags = NULL;
	int n = 0, i, nnew = 0, nreflag = 0;
	int port, rc = 1;

	free(san);
	if (mbox_mkdir_p(emaildir, &err) < 0) {
		fprintf(stderr, "mail-sync: %s\n", err);
		free(err);
		goto out;
	}
	mbox_state_load(statepath, &st);

	if (a->imap_ssl)
		port = 993;
	else if (a->imap_tls)
		port = 143;
	else {
		fprintf(stderr,
		    "mail-sync: account %s offers neither IMAP SSL nor STARTTLS; refusing plaintext\n",
		    a->email);
		mbox_state_free(&st);
		goto out;
	}
	if (verbose)
		fprintf(stderr, "connecting to %s:%d...\n", a->imap_host, port);
	im = imap_connect(a->imap_host, port, a->imap_ssl, a->imap_tls, &err);
	if (!im) {
		fprintf(stderr, "mail-sync: %s\n", err);
		free(err);
		mbox_state_free(&st);
		goto out;
	}
	if (creds->oauth2) {
		if (imap_oauth2(im, creds->user, creds->token, &err) < 0) {
			fprintf(stderr,
			    "mail-sync: XOAUTH2 failed: %s\n", err);
			free(err);
			imap_free(im);
			mbox_state_free(&st);
			goto out;
		}
	} else {
		if (imap_login(im, creds->user, creds->imap_password,
			&err) < 0) {
			fprintf(stderr,
			    "mail-sync: LOGIN failed: %s\n", err);
			free(err);
			imap_free(im);
			mbox_state_free(&st);
			goto out;
		}
	}
	if (imap_select(im, folder, &uidvalidity, &uidnext, &err) < 0) {
		fprintf(stderr, "mail-sync: SELECT %s failed: %s\n", folder,
		    err);
		free(err);
		imap_free(im);
		mbox_state_free(&st);
		goto out;
	}
	if (verbose)
		fprintf(stderr,
		    "  %s: uidvalidity %lu uidnext %lu\n", folder,
		    uidvalidity, uidnext);
	if (st.uidvalidity != 0 && st.uidvalidity != uidvalidity) {
		/* Unlike wmail-sync (which leaves old maildir files in
		 * place), an mbox cannot drop stale messages without a
		 * rewrite, and stale UIDs would otherwise duplicate
		 * forever. The mbox is a mirror and the server is the
		 * source of truth, so start it fresh. */
		fprintf(stderr,
		    "mail-sync: %s: UIDVALIDITY changed (%lu -> %lu); rebuilding mbox\n",
		    folder, st.uidvalidity, uidvalidity);
		unlink(mboxpath);
		mbox_state_free(&st);
		memset(&st, 0, sizeof st);
	}
	if (imap_uid_flags(im, &uids, &flags, &n, &err) < 0) {
		fprintf(stderr, "mail-sync: UID FETCH failed: %s\n", err);
		free(err);
		imap_free(im);
		mbox_state_free(&st);
		goto out;
	}
	/* New messages (skip \Deleted). */
	{
		unsigned long *newuids = xmalloc(sizeof(unsigned long) *
		    (size_t)(n > 0 ? n : 1));
		int nnewuids = 0;
		FetchCtx fctx;
		for (i = 0; i < n; i++) {
			if (mbox_state_find(&st, uids[i]))
				continue;
			if (strcasestr_(flags[i], "\\Deleted"))
				continue;
			newuids[nnewuids++] = uids[i];
		}
		fctx.mboxpath = mboxpath;
		fctx.st = &st;
		fctx.nnew = 0;
		fctx.err = NULL;
		if (nnewuids > 0) {
			if (verbose)
				fprintf(stderr, "  fetching %d new\n",
				    nnewuids);
			if (imap_uid_fetch(im, newuids, nnewuids, fetch_cb,
				&fctx, &err) < 0) {
				fprintf(stderr,
				    "mail-sync: fetch failed: %s\n", err);
				free(err);
				free(newuids);
				if (fctx.err)
					free(fctx.err);
				imap_free(im);
				mbox_state_free(&st);
				goto out_free;
			}
		}
		free(newuids);
		nnew = fctx.nnew;
		if (fctx.err) {
			fprintf(stderr, "mail-sync: %s\n", fctx.err);
			free(fctx.err);
			imap_free(im);
			mbox_state_free(&st);
			goto out_free;
		}
	}
	/* Flag changes on messages we already have. */
	for (i = 0; i < n; i++) {
		MboxStateEnt *e = mbox_state_find(&st, uids[i]);
		char status[16];
		int r;
		if (!e)
			continue;
		imap_flags_to_status(flags[i], status, sizeof status);
		if (strcmp(status, e->flags) == 0)
			continue;
		r = mbox_reflag(mboxpath, uids[i], flags[i], &err);
		if (r < 0) {
			fprintf(stderr, "mail-sync: reflag failed: %s\n",
			    err);
			free(err);
			err = NULL;
			continue;
		}
		if (r == 1) {
			fprintf(stderr,
			    "mail-sync: warning: uid %lu not in mbox, skipping reflag\n",
			    uids[i]);
			continue;
		}
		mbox_state_put(&st, uids[i], status);
		nreflag++;
	}
	st.uidvalidity = uidvalidity;
	st.uidnext = uidnext;
	if (mbox_state_save(statepath, &st) < 0)
		fprintf(stderr, "mail-sync: warning: cannot save %s\n",
		    statepath);
	printf("%s: %d new, %d reflagged\n", folder, nnew, nreflag);
	imap_logout(im);
	imap_free(im);
	mbox_state_free(&st);
	rc = 0;
out_free:
	for (i = 0; i < n; i++)
		free(flags[i]);
	free(flags);
	free(uids);
out:
	free(emaildir);
	free(mboxpath);
	free(statepath);
	return rc;
}

int
main(int argc, char **argv)
{
	GoaAccount *accs = NULL;
	int naccs = 0, i, opt;
	char *err = NULL;
	const char *account_match = NULL, *root = NULL;
	const char **folders = NULL;
	int nfolders = 0, list_only = 0;
	GoaAccount *acc = NULL;
	GoaCreds creds;
	int rc = 0;

	while ((opt = getopt(argc, argv, "a:f:d:lv")) != -1) {
		switch (opt) {
		case 'a':
			account_match = optarg;
			break;
		case 'f':
			folders = xrealloc(folders,
			    sizeof(char *) * (size_t)(nfolders + 1));
			folders[nfolders++] = optarg;
			break;
		case 'd':
			root = optarg;
			break;
		case 'l':
			list_only = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
		}
	}

	if (goa_accounts(&accs, &naccs, &err) < 0) {
		fprintf(stderr, "mail-sync: %s\n", err);
		return 1;
	}
	if (naccs == 0) {
		fprintf(stderr, "mail-sync: no mail-enabled GOA accounts\n");
		return 1;
	}
	if (list_only) {
		for (i = 0; i < naccs; i++)
			printf("%s\t%s\t%s (%s)\n", accs[i].email,
			    accs[i].provider_name, accs[i].imap_host,
			    accs[i].oauth2 ? "oauth2" : "password");
		goa_free_accounts(accs, naccs);
		return 0;
	}
	if (account_match) {
		for (i = 0; i < naccs; i++) {
			if (strcasestr_(accs[i].email, account_match)) {
				acc = &accs[i];
				break;
			}
		}
		if (!acc) {
			fprintf(stderr,
			    "mail-sync: no account matching '%s'; available:\n",
			    account_match);
			for (i = 0; i < naccs; i++)
				fprintf(stderr, "  %s\n", accs[i].email);
			goa_free_accounts(accs, naccs);
			return 1;
		}
	} else {
		acc = &accs[0];
	}
	if (!root) {
		root = getenv("MAILSYNC_DIR");
		if (!root || !root[0]) {
			const char *home = getenv("HOME");
			if (!home) {
				fprintf(stderr,
				    "mail-sync: no -d, $MAILSYNC_DIR or $HOME\n");
				goa_free_accounts(accs, naccs);
				return 1;
			}
			root = xstrfmt("%s/Mail", home);
		}
	}
	if (goa_credentials(acc, &creds, &err) < 0) {
		fprintf(stderr, "mail-sync: %s\n", err);
		goa_free_accounts(accs, naccs);
		return 1;
	}
	if (nfolders == 0) {
		rc = sync_folder(acc, &creds, "INBOX", root);
	} else {
		for (i = 0; i < nfolders; i++) {
			if (sync_folder(acc, &creds, folders[i], root) != 0)
				rc = 1;
		}
	}
	goa_free_creds(&creds);
	goa_free_accounts(accs, naccs);
	free(folders);
	return rc;
}
