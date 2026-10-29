# desk — workspace startup script

One script that brings the Plan 9 desk up: it scans each desk
service, and starts the ones that are down. Idempotent — run it
whenever, it never starts a duplicate and never kills a stranger.

## Usage

```
desk          probe each service; start the ones that are down
desk status   probe only, start nothing
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
