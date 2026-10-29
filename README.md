# desk — workspace startup script

One script that brings the Plan 9 desk up: it scans each desk
service, and starts the ones that are down. Idempotent — run it
whenever, it never starts a duplicate and never kills a stranger.

## Usage

```
desk          probe each service; start the ones that are down
desk status   probe only, start nothing
desk acme     start the services, mount the 9P trees, launch acme;
              closing acme unmounts (mounts live for the session)
desk unmount  unmount the trees (clears mounts a crashed session
              left behind)
```

One line per service, exit 0 when everything ends up, 1 otherwise:

```
cadence 9040 up
relay 9006 started
outpost 9029 failed (see logs/outpost.log)
translator pid 70779 running
```

## Services

| name | check | start |
| ---- | ----- | ----- |
| cadence | 9P handshake on 9040 | `bun src/ninep-server.ts` (`NINEP_PORT=9040`) |
| relay | 9P handshake on 9006 | `bun src/ninep-server.ts` (`NINEP_PORT=9006`) |
| outpost | 9P handshake on 9029 | `bun src/ninep-server.ts` (`NINEP_PORT=9029`) |
| translator | pidfile | `bun src/translator.ts` |

"Up" means the port answers a real 9P Tversion handshake, not
merely that something listens. A port that accepts TCP but does
not speak 9P is reported `busy (not 9P)` and left alone — desk
never kills an unknown process and never starts a second server
onto an occupied port. A service that exits on start (outpost
with no database yet, for example) is reported `failed` with its
log path; the log carries the cause.

The HTTP UIs (cadence 3040, relay 3006, outpost 3029) are not
desk services and are not started here; the 9P servers do not
need them. Relay and outpost 9P servers open their databases
read-only, so they need an existing database to start; cadence
creates its data directory.

## Directories and logs

Service directories resolve in order: `$CADENCE_DIR` (and
`$RELAY_DIR`, `$OUTPOST_DIR`, `$TRANSLATOR_DIR`), a subdirectory
of this script's directory, `$HOME/<name>`. A service with no
directory is reported `missing` and skipped.

Logs go to `$DESK_LOG_DIR`, default `logs/` beside the script
(gitignored here). The translator has no port to scan, so it is
detected by a pidfile in the log directory; a translator started
by hand, not through desk, is invisible to it — start the desk
through desk.

## desk acme — mounts and launch

`desk acme` runs the normal startup first, then mounts the three
9P trees with plan9port's `9pfuse` and launches acme with today's
file open. The translator is started but does not gate the
session: reading the desk works without it. If any 9P service is
not up after startup, acme is not launched.

```
$n/cadence        cadence tree (templates, runs, today, staging)
$n/comms/relay    relay tree (contacts, threads)
$n/comms/outpost  outpost tree (contacts, threads, inbox)
```

`$n` is `$MOUNT_ROOT`, default `/n` (the guide's paths). If `/n`
cannot be created, desk prints the one-time `sudo mkdir` command;
`MOUNT_ROOT=$HOME/n` works with no privileges. `PLAN9` defaults
to `$HOME/plan9port`; `NINEPFUSE_BIN` and `ACME_BIN` override the
two binaries.

Requirements, checked before anything starts: `9pfuse` and `acme`
present, `/dev/fuse` present (on Ubuntu: install fuse3 and load
the module with `sudo modprobe fuse`; a kernel with no fuse
module cannot run the mounts), an unmount tool (`fusermount`,
`fusermount3`, or `umount`), and `DISPLAY` set.

Mounts are verified before acme launches: each mountpoint must
appear mounted and its probe path must read (`today` for cadence,
`contacts` for relay and outpost). A mountpoint that is already
mounted is reused only if its probe path reads; a non-empty
unmounted directory is refused, never hidden. A mount that fails
unmounts the ones that succeeded, and acme never launches
half-mounted.

Closing acme unmounts (trap on exit, interrupt, and terminate).
`desk unmount` clears mounts left by a crashed session; it prints
nothing for mounts already gone and exits 1 only if a mounted
tree could not be unmounted.
