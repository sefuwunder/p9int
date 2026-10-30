/* goa.h - GNOME Online Accounts access via busctl --json.
 *
 * Same protocol outpost's goa.ts uses: GetManagedObjects for discovery,
 * EnsureCredentials + GetAccessToken / GetPassword for credentials.
 * Credentials are fetched on demand and never written to disk by us. */
#ifndef WMAIL_GOA_H
#define WMAIL_GOA_H

typedef struct {
	char *path;		/* D-Bus object path */
	char *id;
	char *provider_type;
	char *provider_name;
	char *email;
	char *imap_host;
	char *imap_user;
	char *smtp_host;
	char *smtp_user;
	int imap_ssl, imap_tls;
	int smtp_ssl, smtp_tls, smtp_auth, smtp_xoauth2;
	int oauth2, password_based;
} GoaAccount;

typedef struct {
	char *user;		/* login identity (email) */
	int oauth2;
	char *token;		/* OAuth2 access token, or NULL */
	char *imap_password;	/* password-based accounts, or NULL */
	char *smtp_password;	/* password-based accounts, or NULL */
} GoaCreds;

/* List mail-enabled GOA accounts. Returns 0 on success, -1 with *err set. */
int goa_accounts(GoaAccount **out, int *nout, char **err);
void goa_free_accounts(GoaAccount *a, int n);

/* Fetch fresh credentials for one account (EnsureCredentials first). */
int goa_credentials(GoaAccount *a, GoaCreds *out, char **err);
void goa_free_creds(GoaCreds *c);

/* Test hook: extract accounts from an already-parsed GetManagedObjects
 * reply (the {"type":..., "data":[...]} document). */
struct Json;
int goa_accounts_from_json(struct Json *root, GoaAccount **out, int *nout,
    char **err);

#endif
