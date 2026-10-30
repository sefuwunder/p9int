/* mbox.h - mbox storage + sync state for mail-sync.
 * One mbox file per IMAP folder. Messages are appended in arrival
 * order, each starting with a "From " separator line, followed by an
 * X-Mail-Sync-Uid header (the IMAP UID, used to find the message
 * again for flag updates) and a Status header, then the raw message
 * with mboxrd escaping (body lines starting with "From " gain a ">"
 * prefix so they can never be mistaken for a separator).
 *
 * This is the form Plan 9's upas/fs reads: point it at the mbox with
 * `upas/fs -f <file>` and the folder appears as a file tree. */
#ifndef MAILSYNC_MBOX_H
#define MAILSYNC_MBOX_H

#include <stddef.h>

/* Append one message to an mbox file, creating parent directories and
 * the file itself as needed. The file is left ending with a blank
 * line. 0 = ok, -1 with *err set. */
int mbox_append(const char *path, unsigned long uid, const char *imap_flags,
    const char *body, size_t len, char **err);

/* Update the Status header of the message with the given uid,
 * matched by its X-Mail-Sync-Uid header. Rewrites the file atomically
 * (tmp + rename); every other byte is preserved.
 * Returns 0 = updated, 1 = uid not found, -1 with *err set. */
int mbox_reflag(const char *path, unsigned long uid, const char *imap_flags,
    char **err);

/* Map an IMAP flag string ("\\Seen \\Flagged") to mbox Status letters:
 * R = read (Seen), F = flagged, A = answered, D = deleted. */
void imap_flags_to_status(const char *imap_flags, char *out, size_t outsz);

/* Create a directory path, parents included. 0 = ok, -1 with *err set. */
int mbox_mkdir_p(const char *path, char **err);

typedef struct {
	unsigned long uid;
	char *flags;	/* Status letters, e.g. "RF" */
} MboxStateEnt;

typedef struct {
	unsigned long uidvalidity, uidnext;
	MboxStateEnt *ents;
	int n, cap;
} MboxState;

/* State file format, one line per entry after a "v1 <validity> <next>"
 * header: "<uid> <status-letters>". Missing file = fresh sync. */
int mbox_state_load(const char *path, MboxState *st);
int mbox_state_save(const char *path, MboxState *st);
MboxStateEnt *mbox_state_find(MboxState *st, unsigned long uid);
void mbox_state_put(MboxState *st, unsigned long uid, const char *flags);
void mbox_state_free(MboxState *st);

#endif
