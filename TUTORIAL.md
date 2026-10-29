# desk — workflows you can run today

What the mounted namespaces let you do **in their present condition**.
No tag verbs, no plumber routing, no automatic follow-ups, no note
storage — those are proposed, not built. Everything below works with
what exists now: the `desk` script, the three 9P trees, the
translator, and the ninep-helper CLI.

## The shape of a session

```
desk acme     # start services, mount the trees, open acme on today
desk status   # probe only, start nothing
desk unmount  # clear mounts a crashed session left behind
```

While the session is open, the trees are ordinary files:

```
$n/cadence        templates, runs, steps, today, kpis, activity, contacts, staging
$n/comms/relay    contacts, threads
$n/comms/outpost  contacts, threads, inbox
```

`$n` is `$MOUNT_ROOT`, default `/n`. Closing acme unmounts
everything (trap on exit, interrupt, and terminate); the servers
and their data stay. So every workflow below happens **during a
session** — or, for the scripted ones, any time the servers are up,
through the `9p` CLI instead of the mount (see "Two ways in").

## Two ways in

**Mounted files** (during `desk acme`): read with `cat`/`grep` or
open in acme; write with shell redirection. Only three kinds of
write are accepted anywhere in the trees:

| Write | Command | Effect |
| ----- | ------- | ------ |
| step done | `printf '1' > /n/cadence/runs/<run>/steps/<task>/done` | marks done (`0` undoes it) |
| draft approve | `printf '\n' > /n/cadence/staging/<draft>/approve` | creates the run (blank data falls back to the draft's `template` file; writing a playbook id explicitly overrides it) |
| draft reject | `printf '\n' > /n/cadence/staging/<draft>/reject` | marks rejected, kept as audit |

Everything else is read-only, and writes to it fail with
"permission denied": step `title`/`role`/`week`, all `links/`
fields, thread messages, contacts, `today`, `activity`, and the
whole relay and outpost trees. In acme that means: read any window
you like, but Put only ever succeeds on a `done` file — and even
there, prefer the shell form above so the bytes are exactly `1`
or `0` (the server trims whitespace, so `echo 1` works too).

**The `9p` CLI** (any time the servers are up, no mount needed;
needs `$PLAN9/bin` on PATH):

```
9p -a tcp!127.0.0.1!9040 read /today
printf '1' | 9p -a tcp!127.0.0.1!9040 write /runs/<run>/steps/<task>/done
printf '\n' | 9p -a tcp!127.0.0.1!9040 write /staging/<draft>/approve
```

Ports: cadence 9040, relay 9006, outpost 9029. The write commands
in this tutorial were verified live against a scratch cadence
server through the `9p` CLI (done `0` → `1`, approve → run created
with an activity event, reject → `rejected`); the mount route
reaches the same 9P operations through 9pfuse.

## Morning: Today, then staging

### 1. Read Today

`desk acme` opens `/n/cadence/today` for you. It lists, per active
run, the open steps of that run's **current week** — the week
counted from the run's start date through its playbook's weeks —
in this form:

```
Run name | week N | role | title
```

"Nothing due today." means no active run has an open step in its
current week. It is a week view, not a calendar-day view: a step
due Friday shows on Monday too.

### 2. Review staging

```
ls /n/cadence/staging/
```

Each draft is a directory of field files, exactly what the
translator wrote: `template`, `thread`, `contact`, `excerpt`,
`created`. There is no `status` file yet — a draft has no status
until you decide it. Read them:

```
cat /n/cadence/staging/<draft>/excerpt
cat /n/cadence/staging/<draft>/contact
cat /n/cadence/staging/<draft>/thread
```

### 3. Check the source thread (the manual Look)

There is no plumber routing for desk ids yet, so you open the
thread yourself. The draft's `thread` file says `relay:t1` or
`outpost:t9`; the messages live under the matching tree:

```
cat /n/comms/relay/threads/t1/messages/*/body
cat /n/comms/outpost/threads/t9/messages/*/body
```

Confirm the excerpt matches a real first message, the contact is
not already in cadence (`ls /n/cadence/contacts/` — known contacts
are skipped by the translator, so a draft for one is suspect), and
the thread is not a group (`is_group` file: `1` means the
translator should have skipped it).

### 4. Approve the real ones

```
printf '\n' > /n/cadence/staging/<draft>/approve
```

Then verify — never assume the write took:

```
cat /n/cadence/staging/<draft>/status    # approved
cat /n/cadence/staging/<draft>/result    # the new run id
ls /n/cadence/runs/<run-id>/steps/
```

The run's name is `<contact> — <playbook name>`, its steps come
from the playbook's weeks, and an event is logged to `activity`.
The draft directory stays put: the decision is the audit record.

### 5. Reject the rest

```
printf '\n' > /n/cadence/staging/<draft>/reject
cat /n/cadence/staging/<draft>/status    # rejected
```

A rejected draft does **not** re-draft on the next message — the
translator only drafts first-seen threads — so rejection is final
unless you approve it later from the same directory. A draft with
no `status` file is undecided; end the morning with none of those.

## During the day: steps, proof, follow-ups

### 6. Mark a step done

You confirmed something by phone, or finished the work yourself —
no message for the desk to read. Write the flag, then read it back:

```
printf '1' > /n/cadence/runs/<run>/steps/<task>/done
cat /n/cadence/runs/<run>/steps/<task>/done    # 1
```

`printf '0'` undoes it. This is the only per-step write that
exists: you cannot re-title, re-time, or re-assign a step through
the trees. A step that needs re-timing waits for the underlying
app (or a future verb) — say so in your own notes (step 8) rather
than forcing the file.

### 7. Follow the proof

Every step can carry links: `links/<link>/{kind, ref, label}`.
Read them before trusting a step's state:

```
ls /n/cadence/runs/<run>/steps/<task>/links/
cat /n/cadence/runs/<run>/steps/<task>/links/*/kind
cat /n/cadence/runs/<run>/steps/<task>/links/*/ref
cat /n/cadence/runs/<run>/steps/<task>/links/*/label
```

A `ref` like `relay:t1` or `outpost:t5` is the tied thread; a
`message` link's ref names the exact message that completed the
step. Open it (step 3's paths) and the "why is this here?" question
answers itself. If a step claims a confirmation you cannot find in
its links, the tie is wrong — that is a data problem to fix in the
underlying app, not something the trees can repair.

### 8. Follow up on a quiet step (manual Fanout)

Nothing sends follow-ups for you yet, so this is a manual workflow
with the desk doing the finding:

1. Find the quiet step: it is in Today (current week, still open)
   or in a run's `steps/` with `done` reading `0`.
2. Find its thread: the step's `links/` entry with kind `thread`.
3. Read the thread's last message date:
   `cat /n/comms/outpost/threads/<id>/messages/*/created` — the
   newest sorts last.
4. Reply **in the thread's home app** (relay/outpost UI, email,
   wherever the conversation lives). The desk never sends; your
   reply is the follow-up.

Follow the guide's rules by hand, since nothing enforces them yet:
stagger follow-ups instead of sending three at once, and never
send between 10 p.m. and 7 a.m. When the reply comes back
confirming the work, the step still needs your hand (step 6) —
automatic completion on confirming replies is unbuilt too, unless
the translator's step-binding already covers it. Do not wait for a
completion that nothing is watching for.

### 9. Notes (the one verb with no equivalent)

There is no note storage in the trees yet: no step note file, no
activity write path. So notes live outside the desk for now — the
underlying app's own notes, or a scratch file of your own — and
they will **not** appear in `activity` or on the step. Keep them
anyway; a note you can find beats a note the system never
recorded. When Note is eventually built, its storage is still an
open design point (the example workspace proposes a step link of
kind `note`).

### 10. Ask the helper

ninep-helper reads the same trees over 9P and answers without a
model. From the helper checkout:

```
bun src/helper.ts status     # which trees answer, what is in staging
bun src/helper.ts drafts     # proposed runs awaiting a decision
bun src/helper.ts today      # today's steps, in words
bun src/helper.ts man <topic>  # plan9port man pages first, then bundled notes
bun src/helper.ts <question>   # arguments are the question
```

Because arguments are the question, it also runs from an acme tag:
type the question as the command and middle-click it. A tree that
is down makes the helper say so, never guess.

### 11. Search the desk with shell tools

Because the trees are files, the usual tools compose:

```
# every proposed run older than a week (stale drafts to decide)
find /n/cadence/staging -mindepth 1 -maxdepth 1 -type d -mtime +7

# drafts mentioning a name
grep -l "Harbor" /n/cadence/staging/*/excerpt

# every open step across all runs
for f in /n/cadence/runs/*/steps/*/done; do
  [ "$(cat "$f")" = "0" ] && echo "$f"
done
```

## Evening: Activity, close clean

### 12. Skim Activity

```
cat /n/cadence/activity
```

Newest first: runs approved, steps done, drafts decided. If a
write you made is missing from Activity, re-read the file you
wrote (steps 4–6) — the trees are the truth, Activity is the log.

### 13. Close the session

Check staging is decided (every draft has a `status` file), then
close acme: the trap unmounts all three trees. If a session
crashed and mounts linger, `desk unmount` clears them silently
when nothing is mounted, and exits 1 only if a mounted tree could
not be unmounted.

### 14. Back up the desk

Daily, outside any session: copy the three data directories, the
translator's state file, and `~/.enclave` (see the executive desk
guide). Databases, staging drafts, and state files are never
pushed to git — code and config in git, live data in backups.

## Troubleshooting

| Symptom | Cause / fix |
| ------- | ----------- |
| `fusermount: user has no write access to mountpoint` | mountpoint dirs are root-owned (often from desk's own sudo-mkdir line). `sudo chown -R $(id -un) $MOUNT_ROOT/cadence $MOUNT_ROOT/comms`, or `MOUNT_ROOT=$HOME/n ./desk acme`. Desk's precheck now refuses with this exact command instead of failing mid-mount. |
| a service reports `failed` | read its log under `$DESK_LOG_DIR` (default `logs/` beside the script). Outpost/relay with no database yet exit with a plain message, not a stack — create the database by running the app first. |
| a port reports `busy (not 9P)` | something else listens there. Desk leaves it alone and never kills it — move the stranger or point the service elsewhere. |
| a service reports `missing (no dir)` | no checkout found via `$X_DIR`, a subdirectory, a sibling, or `$HOME/<name>`. Set the env var or fix the layout. |
| a write fails with "permission denied" | the path is read-only by design (see "Two ways in"). Only `done`, `approve`, and `reject` accept writes. |
| `9p: mount: no name space` | the `9p` CLI without `-a` needs a namespace; pass `-a tcp!127.0.0.1!<port>` as in the examples. |
| translator not drafting | check its pidfile/log: desk only sees a translator it started itself. A hand-started translator is invisible to `desk status`. |

## What is deliberately not here

Tag verbs (Look, Approve, Done, Note, Fanout as tag words),
plumber routing for desk ids, automatic step completion on
confirming replies, automatic follow-ups, and note storage are all
proposed, not built. This tutorial covers only what the mounted
namespaces do today; the example workspace in
`sefuwunder/executive-desk-example` shows what those verbs would
look like when they exist.
