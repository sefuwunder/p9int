# mail-sync — GOA mail to mbox, for Plan 9

`mail-sync` syncs GNOME Online Accounts mail to local mbox files,
one way (IMAP -> disk), incrementally. It is derived from wily's
`tools/wmail/wmail-sync`: the GOA discovery, credential flow, and
IMAP client are reused verbatim; only the storage changes, from
maildir to one mbox file per folder — the form Plan 9's `upas/fs`
reads.

## What it does

- Discovers mail-enabled GOA accounts with `busctl --json` (the same
  calls outpost's `goa.ts` uses): `GetManagedObjects`, then
  `EnsureCredentials` + `GetAccessToken` (OAuth2) or
  `GetPassword("imap-password")` (password accounts). Credentials are
  fetched on demand and never written to disk by this tool.
- Connects IMAP with TLS: port 993 when the account says `ImapUseSsl`,
  port 143 + STARTTLS when it says `ImapUseTls`. Plaintext IMAP to a
  non-local host is refused. Certificates are verified against the
  system store.
- `SELECT`s each folder, compares `UIDVALIDITY`/`UIDNEXT` against
  the folder's `.state` file, and appends new messages verbatim
  (full RFC822) to the folder's `.mbox` file. Flag changes on
  messages already synced rewrite that message's `Status` header
  (atomic tmp + rename; every other byte preserved). Deleted
  messages are skipped, not downloaded.
- On a `UIDVALIDITY` change it rebuilds the mbox from scratch
  (deliberate divergence from wmail-sync, which leaves old maildir
  files in place: an mbox cannot drop stale messages without a
  rewrite, and stale UIDs would otherwise duplicate forever; the
  mbox is a mirror and the server is the source of truth).

## Usage

```
mail-sync [-a account] [-f folder]... [-d root] [-l] [-v]
```

- `-a` matches a GOA account by email substring (default: first).
- `-f` repeats for folders (default: `INBOX`). `a/b` becomes `a.b`
  on disk.
- `-d` sets the mbox root (default: `$MAILSYNC_DIR` or `~/Mail`).
  Layout: `root/<email>/<folder>.mbox` plus
  `root/<email>/<folder>.state`.
- `-l` lists GOA mail accounts and exits.

Build: `./build.sh` here (needs `libssl-dev`; it builds `mail-sync`
and the test binary and runs all checks). Install by copying
`mail-sync` to a directory on your `PATH`.

## The mbox format it writes

Each message starts with a `From ` separator line (`From <sender>
<date>`, sender parsed from the message's `From:` header), followed
by two headers this tool owns, then the raw message:

```
From maya@harborcafe.example Thu Oct 29 10:00:00 2026
X-Mail-Sync-Uid: 101
Status: R
From: Maya Chen <maya@harborcafe.example>
...
```

- `X-Mail-Sync-Uid` is the IMAP UID, used to find the message again
  for flag updates. It is the only header this tool adds.
- `Status` follows the classic mbox convention: `R` = read (Seen),
  `F` = flagged, `A` = answered, `D` = deleted.
- Body lines starting with `From ` gain a `>` prefix (mboxrd
  escaping), so a body line can never be mistaken for a separator.
- The file always ends with a blank line.

## Serving it with Plan 9 mail

Point `upas/fs` at a synced mbox and the folder appears as a file
tree (one entry per message, one file per field: `from`, `subject`,
`body`, ...):

```
upas/fs -s -f ~/Mail/alice@example.com/INBOX.mbox
```

then read it with the `9p` client against the posted `upasfs`
service (plan9port's `upas/fs` cannot mount directly; see the
plan9port notes). One `upas/fs` run serves one mbox; serving
several folders at once is not yet worked out, since every run
posts the same `upasfs` service name.

## Limitations (v1, honest)

- One-way only: no expunge propagation (a server-side expunge does
  not remove the message from the mbox; a later sync does not
  resurrect it either, since state only tracks what it downloaded).
- No MIME part extraction: messages are stored raw. HTML mail shows
  as source until a text-rendering step is added.
- No sending yet. `Send`/`Reply` is phase 2: a `mail-send` reusing
  the same GOA credential flow with SMTP (OAuth2 XOAUTH2 or
  password, per the account's SMTP flags).
- Whether `upas/fs` honors the `Status` headers for display is not
  verified; flag state lives authoritatively in the `.state` file.
- Live verification against a real GOA account + IMAP server is
  partial: a first live run surfaced a busctl encoding mismatch
  ("unexpected GetManagedObjects shape") — the parser assumed the
  reply dict arrives as an array of `[path, interfaces]` pairs, but
  busctl can encode it as a JSON object instead. Fixed: the parser
  now accepts both encodings, with regression tests for each, and
  its errors name the shape actually found. A second live run
  surfaced an XOAUTH2 handshake mismatch: Gmail pads the empty
  continuation ("+ " not "+"), which the old exact-match check
  rejected. Fixed: the challenge check accepts "+" plus
  whitespace, and a post-blob JSON error challenge is answered
  with the empty line the exchange requires (the old code waited
  forever for the tagged reply). A full live IMAP sync still
  awaits confirmation on a real machine: run `mail-sync -l`
  first, then a sync with `-v`.
