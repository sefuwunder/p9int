/* tests/test.c - mail-sync unit tests (mbox + state only).
 *
 * No network, no D-Bus: the GOA/IMAP code is reused verbatim from
 * wmail-sync and covered by its own suite upstream. These tests cover
 * the new mbox layer: append format, mboxrd escaping, reflag
 * rewrites, and state save/load. */
#include "../mbox.h"
#include "../util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do { \
	checks++; \
	if (cond) { printf("ok %d - ", checks); printf(__VA_ARGS__); printf("\n"); } \
	else { failures++; printf("not ok %d - ", checks); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static char *
read_all(const char *path, size_t *len_out)
{
	FILE *f = fopen(path, "rb");
	char *buf = NULL;
	size_t len = 0, cap = 0;
	if (!f)
		return NULL;
	for (;;) {
		char chunk[4096];
		size_t r = fread(chunk, 1, sizeof chunk, f);
		if (r == 0)
			break;
		if (len + r > cap) {
			cap = (len + r) * 2 + 64;
			buf = xrealloc(buf, cap);
		}
		memcpy(buf + len, chunk, r);
		len += r;
	}
	fclose(f);
	if (len_out)
		*len_out = len;
	return buf;
}

static int
count_occurrences(const char *buf, size_t len, const char *needle)
{
	size_t nlen = strlen(needle), i;
	int c = 0;
	if (nlen == 0)
		return 0;
	for (i = 0; i + nlen <= len; i++)
		if (memcmp(buf + i, needle, nlen) == 0)
			c++;
	return c;
}

static int
contains(const char *buf, size_t len, const char *needle)
{
	return count_occurrences(buf, len, needle) > 0;
}

int
main(void)
{
	char dir_template[] = "/tmp/mail-sync-test-XXXXXX";
	char *dir = mkdtemp(dir_template);
	char *mbox, *state;
	char *err = NULL;
	char status[16];
	const char *msg1 =
	    "From: Maya Chen <maya@harborcafe.example>\n"
	    "To: you@example.com\n"
	    "Subject: Second location\n"
	    "\n"
	    "Hi — we just signed the lease.\n"
	    "From the new shop floor.\n";
	const char *msg2 =
	    "From: priya@northwind.example\n"
	    "Subject: Renewal\n"
	    "\n"
	    "Draft attached.\n";
	const char *msg3 = "Subject: No sender here\n\nBody only.\n";
	char *content;
	size_t clen;

	if (!dir) {
		fprintf(stderr, "mkdtemp failed\n");
		return 1;
	}
	mbox = xstrfmt("%s/alice@example.com/INBOX.mbox", dir);
	state = xstrfmt("%s/alice@example.com/INBOX.state", dir);

	/* Status mapping. */
	imap_flags_to_status("\\Seen \\Flagged", status, sizeof status);
	CHECK(strcmp(status, "RF") == 0, "status Seen+Flagged -> RF");
	imap_flags_to_status("\\Answered", status, sizeof status);
	CHECK(strcmp(status, "A") == 0, "status Answered -> A");
	imap_flags_to_status("\\Deleted", status, sizeof status);
	CHECK(strcmp(status, "D") == 0, "status Deleted -> D");
	imap_flags_to_status("", status, sizeof status);
	CHECK(strcmp(status, "") == 0, "status empty -> empty");
	imap_flags_to_status(NULL, status, sizeof status);
	CHECK(strcmp(status, "") == 0, "status NULL -> empty");

	/* Append: format, sender, escaping, trailing blank line. */
	CHECK(mbox_append(mbox, 101, "\\Seen", msg1, strlen(msg1),
	    &err) == 0, "append msg1");
	if (err) {
		fprintf(stderr, "append err: %s\n", err);
		free(err);
		err = NULL;
	}
	content = read_all(mbox, &clen);
	CHECK(content != NULL, "mbox readable after append");
	CHECK(contains(content, clen, "From maya@harborcafe.example "),
	    "From_ line carries parsed sender");
	CHECK(contains(content, clen, "X-Mail-Sync-Uid: 101\n"),
	    "X-Mail-Sync-Uid header present");
	CHECK(contains(content, clen, "Status: R\n"),
	    "Status header reflects Seen");
	CHECK(contains(content, clen, ">From the new shop floor.\n"),
	    "body From line escaped to >From");
	CHECK(count_occurrences(content, clen, "\nFrom ") == 0,
	    "no unescaped body From line (only separator at start)");
	CHECK(clen >= 2 && content[clen - 2] == '\n' &&
	    content[clen - 1] == '\n', "mbox ends with blank line");
	free(content);

	/* Second message: separators stay distinct. */
	CHECK(mbox_append(mbox, 102, "", msg2, strlen(msg2), &err) == 0,
	    "append msg2");
	content = read_all(mbox, &clen);
	CHECK(count_occurrences(content, clen, "X-Mail-Sync-Uid: ") == 2,
	    "two messages in mbox");
	CHECK(contains(content, clen, "From priya@northwind.example "),
	    "bare-token sender parsed");
	CHECK(contains(content, clen, "X-Mail-Sync-Uid: 102\n"),
	    "second uid present");
	free(content);

	/* Third message: no From: header -> unknown sender. */
	CHECK(mbox_append(mbox, 103, "\\Flagged", msg3, strlen(msg3),
	    &err) == 0, "append msg3");
	content = read_all(mbox, &clen);
	CHECK(contains(content, clen, "From unknown "),
	    "missing From: header -> unknown sender");
	CHECK(contains(content, clen, "Status: F\n"),
	    "Flagged -> Status F");
	free(content);

	/* Reflag: first message's Status changes, second untouched. */
	{
		char *before = read_all(mbox, &clen);
		/* Snapshot message 2's bytes: between its separator and
		 * message 3's separator. */
		char *m2start = strstr(before, "X-Mail-Sync-Uid: 102\n");
		char *m3start = strstr(before, "X-Mail-Sync-Uid: 103\n");
		size_t m2len = 0;
		char *m2copy = NULL;
		if (m2start && m3start && m3start > m2start) {
			/* Back up to message 2's From_ line. */
			char *p = m2start;
			while (p > before && p[-1] != '\n')
				p--;
			m2len = (size_t)(m3start - p);
			/* m3start points at its X- header, not its From_
			 * line; back up the same way. */
			{
				char *q = m3start;
				while (q > before && q[-1] != '\n')
					q--;
				m2len = (size_t)(q - p);
			}
			m2copy = xstrndup(p, m2len);
		}
		CHECK(mbox_reflag(mbox, 101, "\\Seen \\Flagged", &err) == 0,
		    "reflag uid 101 ok");
		content = read_all(mbox, &clen);
		CHECK(contains(content, clen, "X-Mail-Sync-Uid: 101\n"),
		    "uid 101 still present after reflag");
		/* Status RF must appear before uid 102's block. */
		{
			char *u101 = strstr(content, "X-Mail-Sync-Uid: 101\n");
			char *u102 = strstr(content, "X-Mail-Sync-Uid: 102\n");
			char *st = u101 ? strstr(u101, "Status: ") : NULL;
			CHECK(st && u102 && st < u102 &&
			    strncmp(st, "Status: RF\n", 11) == 0,
			    "uid 101 Status updated to RF");
		}
		if (m2copy) {
			char *u102 = strstr(content, "X-Mail-Sync-Uid: 102\n");
			char *p = u102;
			while (p > content && p[-1] != '\n')
				p--;
			CHECK((size_t)(content + clen - p) >= m2len &&
			    memcmp(p, m2copy, m2len) == 0,
			    "uid 102 bytes untouched by reflag");
			free(m2copy);
		}
		free(before);
		free(content);
	}
	CHECK(mbox_reflag(mbox, 999, "\\Seen", &err) == 1,
	    "reflag missing uid -> 1");

	/* Reflag inserts a Status line when the message has none. */
	{
		char *raw = xstrfmt("%s/raw.mbox", dir);
		FILE *f = fopen(raw, "wb");
		fprintf(f, "From a@b.c Thu Oct 29 10:00:00 2026\n"
		    "X-Mail-Sync-Uid: 7\n"
		    "Subject: no status\n"
		    "\n"
		    "Body.\n"
		    "\n");
		fclose(f);
		CHECK(mbox_reflag(raw, 7, "\\Answered", &err) == 0,
		    "reflag inserts Status when missing");
		content = read_all(raw, &clen);
		CHECK(contains(content, clen, "Status: A\n"),
		    "inserted Status A");
		CHECK(contains(content, clen, "Body.\n"),
		    "body preserved on insert");
		free(content);
		free(raw);
	}

	/* State save/load round-trip. */
	{
		MboxState st;
		memset(&st, 0, sizeof st);
		mbox_state_put(&st, 101, "RF");
		mbox_state_put(&st, 102, "");
		mbox_state_put(&st, 103, "F");
		st.uidvalidity = 12345;
		st.uidnext = 200;
		CHECK(mbox_state_save(state, &st) == 0, "state save");
		mbox_state_free(&st);
		CHECK(mbox_state_load(state, &st) == 0, "state load");
		CHECK(st.uidvalidity == 12345 && st.uidnext == 200,
		    "state validity/next round-trip");
		CHECK(mbox_state_find(&st, 101) &&
		    strcmp(mbox_state_find(&st, 101)->flags, "RF") == 0,
		    "state entry 101 flags round-trip");
		CHECK(mbox_state_find(&st, 999) == NULL,
		    "state find missing -> NULL");
		mbox_state_free(&st);
	}
	{
		MboxState st;
		char *nopath = xstrfmt("%s/nope.state", dir);
		CHECK(mbox_state_load(nopath, &st) == 0 &&
		    st.uidvalidity == 0 && st.n == 0,
		    "missing state file -> fresh state");
		mbox_state_free(&st);
		free(nopath);
	}

	free(mbox);
	free(state);
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
