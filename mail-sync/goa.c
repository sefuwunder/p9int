/* goa.c - GNOME Online Accounts via busctl --json. */
#include "goa.h"
#include "json.h"
#include "util.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define GOA_BUS "org.gnome.OnlineAccounts"
#define GOA_ROOT "/org/gnome/OnlineAccounts"
#define BUSCTL_TIMEOUT_MS 20000

/* Run busctl and capture stdout. Returns malloc'd stdout, or NULL + *err. */
static char *
run_busctl(char *const argv[], char **err)
{
	int outpipe[2], errpipe[2];
	pid_t pid;
	char *out = NULL, *errout = NULL;
	size_t outlen = 0, outcap = 0, errlen = 0, errcap = 0;
	int status, done = 0;

	if (pipe(outpipe) < 0 || pipe(errpipe) < 0) {
		*err = xstrdup("pipe failed");
		return NULL;
	}
	pid = fork();
	if (pid < 0) {
		*err = xstrdup("fork failed");
		return NULL;
	}
	if (pid == 0) {
		/* Child: wire pipes to stdout/stderr, fix up the bus address. */
		char sock[64];
		dup2(outpipe[1], 1);
		dup2(errpipe[1], 2);
		close(outpipe[0]);
		close(outpipe[1]);
		close(errpipe[0]);
		close(errpipe[1]);
		if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
			snprintf(sock, sizeof sock, "/run/user/%d/bus",
			    (int)getuid());
			if (access(sock, F_OK) == 0)
				setenv("DBUS_SESSION_BUS_ADDRESS", sock, 1);
		}
		execvp("busctl", argv);
		_exit(127);
	}
	close(outpipe[1]);
	close(errpipe[1]);

	/* Read both pipes with a timeout; kill on expiry. */
	{
		struct pollfd pf[2];
		int ms_left = BUSCTL_TIMEOUT_MS;
		pf[0].fd = outpipe[0];
		pf[0].events = POLLIN;
		pf[1].fd = errpipe[0];
		pf[1].events = POLLIN;
		while (!done) {
			int r = poll(pf, 2, ms_left);
			if (r < 0)
				break;
			if (r == 0) { /* timeout */
				kill(pid, SIGKILL);
				break;
			}
			for (int i = 0; i < 2; i++) {
				char buf[4096];
				ssize_t n;
				char **dst;
				size_t *len, *cap;
				if (!(pf[i].revents & (POLLIN | POLLHUP)))
					continue;
				n = read(pf[i].fd, buf, sizeof buf);
				if (n <= 0) {
					pf[i].events = 0;
					continue;
				}
				if (i == 0) {
					dst = &out;
					len = &outlen;
					cap = &outcap;
				} else {
					dst = &errout;
					len = &errlen;
					cap = &errcap;
				}
				if (*len + (size_t)n + 1 > *cap) {
					*cap = (*len + (size_t)n + 1) * 2;
					*dst = xrealloc(*dst, *cap);
				}
				memcpy(*dst + *len, buf, (size_t)n);
				*len += (size_t)n;
				(*dst)[*len] = 0;
			}
			if (pf[0].events == 0 && pf[1].events == 0)
				done = 1;
			/* Recompute remaining time crudely: one poll cycle is
			 * bounded by the timeout, so a second cycle means we
			 * already waited too long; keep it simple and break
			 * out to waitpid once both pipes are drained. */
			if (!done)
				ms_left = 1000;
		}
	}
	close(outpipe[0]);
	close(errpipe[0]);
	while (waitpid(pid, &status, 0) < 0)
		;
	if (!out)
		out = xstrdup("");
	if (!errout)
		errout = xstrdup("");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		char *msg = errout[0] ? errout : out;
		/* Trim to first 300 chars. */
		char tmp[301];
		size_t m = strlen(msg);
		if (m > 300)
			m = 300;
		memcpy(tmp, msg, m);
		tmp[m] = 0;
		if (strstr(tmp, "ServiceUnknown") ||
		    strstr(tmp, "Failed to connect") ||
		    strstr(tmp, "No such") ||
		    strstr(tmp, "not provided by any"))
			*err = xstrfmt("GNOME Online Accounts not reachable: %s",
			    tmp);
		else
			*err = xstrfmt("busctl failed: %s", tmp);
		free(out);
		free(errout);
		return NULL;
	}
	free(errout);
	return out;
}

/* One busctl call. Returns the out-args array (Json array), or NULL + *err.
 * The caller owns the returned tree. */
static Json *
bus_call(const char *path, const char *iface, const char *method,
    char *const extra[], int nextra, char **err)
{
	char *argv[16];
	int i, n = 0;
	char *stdout_s;
	Json *root, *data;

	argv[n++] = "busctl";
	argv[n++] = "--user";
	argv[n++] = "--json=short";
	argv[n++] = "call";
	argv[n++] = (char *)GOA_BUS;
	argv[n++] = (char *)path;
	argv[n++] = (char *)iface;
	argv[n++] = (char *)method;
	for (i = 0; i < nextra && n < 15; i++)
		argv[n++] = extra[i];
	argv[n] = NULL;

	stdout_s = run_busctl(argv, err);
	if (!stdout_s)
		return NULL;
	root = json_parse(stdout_s, err);
	free(stdout_s);
	if (!root) {
		if (err && !*err)
			*err = xstrdup("could not parse busctl output");
		return NULL;
	}
	data = json_get(root, "data");
	if (!data || data->t != J_ARR) {
		json_free(root);
		*err = xstrdup("unexpected busctl output shape");
		return NULL;
	}
	/* Detach the data array from the root so the caller owns it. */
	{
		Json *arr = data;
		/* Steal: null out the root's reference, then free the shell. */
		for (i = 0; i < root->n; i++) {
			if (root->items[i] == data)
				root->items[i] = NULL;
		}
		json_free(root);
		/* json_free(NULL) is safe, but the stolen slot is NULL now;
		 * the shell freed keys for it harmlessly. */
		return arr;
	}
}

static const char *
prop_str(Json *props, const char *name)
{
	Json *v = props ? json_get(props, name) : NULL;
	return v ? json_as_str(v) : NULL;
}

static int
prop_bool(Json *props, const char *name, int dflt)
{
	Json *v = props ? json_get(props, name) : NULL;
	return v ? json_as_bool(v, dflt) : dflt;
}

static void
set_str(char **dst, const char *src)
{
	free(*dst);
	*dst = src ? xstrdup(src) : NULL;
}

int
goa_accounts_from_json(Json *root, GoaAccount **out, int *nout, char **err)
{
	Json *data, *objs;
	GoaAccount *accs = NULL;
	int n = 0, cap = 0, i;

	data = json_get(root, "data");
	if (!data || data->t != J_ARR || data->n < 1) {
		*err = xstrdup("unexpected GetManagedObjects shape");
		return -1;
	}
	objs = data->items[0];
	if (!objs || objs->t != J_ARR) {
		*err = xstrdup("unexpected GetManagedObjects shape");
		return -1;
	}
	for (i = 0; i < objs->n; i++) {
		Json *pair = objs->items[i], *ifaces, *acc, *mail;
		GoaAccount *a;
		const char *path;
		int enabled;
		if (!pair || pair->t != J_ARR || pair->n < 2)
			continue;
		path = json_as_str(pair->items[0]);
		ifaces = pair->items[1];
		if (!path || !ifaces || ifaces->t != J_OBJ)
			continue;
		acc = json_get(ifaces, "org.gnome.OnlineAccounts.Account");
		mail = json_get(ifaces, "org.gnome.OnlineAccounts.Mail");
		if (!acc || !mail)
			continue;
		/* Newer GOA exposes MailDisabled instead of MailEnabled. */
		if (json_get(acc, "MailDisabled"))
			enabled = !prop_bool(acc, "MailDisabled", 0);
		else
			enabled = prop_bool(acc, "MailEnabled", 1);
		if (!enabled)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 4;
			accs = xrealloc(accs, sizeof(GoaAccount) * (size_t)cap);
		}
		a = &accs[n];
		memset(a, 0, sizeof *a);
		a->path = xstrdup(path);
		set_str(&a->id, prop_str(acc, "Id"));
		set_str(&a->provider_type, prop_str(acc, "ProviderType"));
		set_str(&a->provider_name, prop_str(acc, "ProviderName"));
		set_str(&a->email, prop_str(mail, "EmailAddress"));
		if (!a->email || !a->email[0])
			set_str(&a->email, prop_str(acc, "PresentationIdentity"));
		set_str(&a->imap_host, prop_str(mail, "ImapHost"));
		set_str(&a->imap_user, prop_str(mail, "ImapUserName"));
		set_str(&a->smtp_host, prop_str(mail, "SmtpHost"));
		set_str(&a->smtp_user, prop_str(mail, "SmtpUserName"));
		a->imap_ssl = prop_bool(mail, "ImapUseSsl", 0);
		a->imap_tls = prop_bool(mail, "ImapUseTls", 0);
		a->smtp_ssl = prop_bool(mail, "SmtpUseSsl", 0);
		a->smtp_tls = prop_bool(mail, "SmtpUseTls", 0);
		a->smtp_auth = prop_bool(mail, "SmtpUseAuth", 0);
		a->smtp_xoauth2 = prop_bool(mail, "SmtpAuthXoauth2", 0);
		a->oauth2 = json_get(ifaces,
		    "org.gnome.OnlineAccounts.OAuth2Based") != NULL;
		a->password_based = json_get(ifaces,
		    "org.gnome.OnlineAccounts.PasswordBased") != NULL;
		n++;
	}
	*out = accs;
	*nout = n;
	return 0;
}

int
goa_accounts(GoaAccount **out, int *nout, char **err)
{
	Json *args;
	char *stdout_s;
	char *argv[] = { "busctl", "--user", "--json=short", "call",
		GOA_BUS, GOA_ROOT,
		"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
		NULL };
	Json *root;
	int r;

	stdout_s = run_busctl(argv, err);
	if (!stdout_s)
		return -1;
	root = json_parse(stdout_s, err);
	free(stdout_s);
	if (!root)
		return -1;
	(void)args;
	r = goa_accounts_from_json(root, out, nout, err);
	json_free(root);
	return r;
}

void
goa_free_accounts(GoaAccount *a, int n)
{
	int i;
	if (!a)
		return;
	for (i = 0; i < n; i++) {
		free(a[i].path);
		free(a[i].id);
		free(a[i].provider_type);
		free(a[i].provider_name);
		free(a[i].email);
		free(a[i].imap_host);
		free(a[i].imap_user);
		free(a[i].smtp_host);
		free(a[i].smtp_user);
	}
	free(a);
}

int
goa_credentials(GoaAccount *a, GoaCreds *out, char **err)
{
	Json *args;
	const char *s;

	memset(out, 0, sizeof *out);
	out->user = xstrdup(a->email ? a->email : "");

	args = bus_call(a->path, "org.gnome.OnlineAccounts.Account",
	    "EnsureCredentials", NULL, 0, err);
	if (!args)
		return -1;
	json_free(args);

	if (a->oauth2) {
		out->oauth2 = 1;
		args = bus_call(a->path,
		    "org.gnome.OnlineAccounts.OAuth2Based",
		    "GetAccessToken", NULL, 0, err);
		if (!args)
			return -1;
		s = (args->n >= 1) ? json_as_str(args->items[0]) : NULL;
		out->token = s ? xstrdup(s) : NULL;
		json_free(args);
		if (!out->token) {
			*err = xstrdup("GetAccessToken returned no token");
			return -1;
		}
		return 0;
	}
	if (a->password_based) {
		char *extra[] = { "s", "imap-password" };
		args = bus_call(a->path,
		    "org.gnome.OnlineAccounts.PasswordBased",
		    "GetPassword", extra, 2, err);
		if (!args)
			return -1;
		s = (args->n >= 1) ? json_as_str(args->items[0]) : NULL;
		out->imap_password = s ? xstrdup(s) : NULL;
		json_free(args);
		extra[1] = "smtp-password";
		args = bus_call(a->path,
		    "org.gnome.OnlineAccounts.PasswordBased",
		    "GetPassword", extra, 2, err);
		if (!args)
			return -1;
		s = (args->n >= 1) ? json_as_str(args->items[0]) : NULL;
		out->smtp_password = s ? xstrdup(s) : NULL;
		json_free(args);
		if (!out->imap_password) {
			*err = xstrdup("GetPassword returned no imap password");
			return -1;
		}
		return 0;
	}
	*err = xstrdup("account is neither OAuth2 nor password based");
	return -1;
}

void
goa_free_creds(GoaCreds *c)
{
	if (!c)
		return;
	free(c->user);
	free(c->token);
	free(c->imap_password);
	free(c->smtp_password);
	memset(c, 0, sizeof *c);
}
