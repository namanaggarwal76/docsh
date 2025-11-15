[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/0ek2UV58)

# Docs++ (Course Project)

This repository implements a Docs++ system with three components:
- Name Manager (NM): routing, ACL enforcement, replication orchestration, persistence
- Storage Server (SS): file storage, sentence-level WRITE with atomic commits, UNDO, checkpoints
- Client (CLI): interactive shell with user-aware authorization

Build

```bash
make -s all
```

## Run (one NM, two SS)

```bash
# Terminal 1: Name Manager (NM)
./bin/nm 5000

# Terminal 2: Storage Server #1 (id=1)
./bin/ss 127.0.0.1 5000 6001 6002 1

# Terminal 3: Storage Server #2 (id=2)
./bin/ss 127.0.0.1 5000 6003 6004 2
```

Then start a client shell (you’ll be prompted for username):

```bash
./bin/client 127.0.0.1 5000
```

## Final feature set (CLI)

- VIEW [-a] [-l]
- READ <file>
- CREATE <file> [-r] [-w] 
- WRITE <file> <sentenceIndex>
- UNDO <file>
- INFO <file>
- DELETE <file>
- LISTTRASH
- RESTORE <file>
- EMPTYTRASH [<file>]
- STREAM <file>
- LIST (active users)
- ADDACCESS -R|-W <file> <user>
- REMACCESS <file> <user>
- REQUEST_ACCESS <file> [-R|-W]
- VIEWREQUESTS <file>
- EXEC <file>
- CREATEFOLDER <path>
- VIEWFOLDER <path>
- MOVE <src> <dst>
- RENAME <old> <new>
- CHECKPOINT <file> <name>
- VIEWCHECKPOINT <file> <name>
- LISTCHECKPOINTS <file>
- REVERT <file> <checkpointName>
- CLEAR
- EXIT

Notes
- Sentence ends at '.', '!' or '?'. The delimiter stays attached to the last word.
- WRITE uses sentence-level locks on the SS; concurrent writes to the same sentence are rejected with ERR_LOCKED.
- Tickets are enforced end-to-end; NM performs ACL checks before issuing tickets.
- UNDO is single-level per file: SS snapshots the pre-commit state on the first write after an UNDO (or creation) and consumes it when UNDO is executed.
- Writes commit atomically via write-to-temp + rename.

## Quick test flows

Notes
- Commands are case-insensitive; the canonical names above are what the client prints in `help`.
- Sentence ends at '.', '!' or '?'. The delimiter stays attached to the last word.
- WRITE uses sentence-level locks on the SS; concurrent writes to the same sentence are rejected with ERR_LOCKED.
- Tickets are enforced end-to-end; NM performs ACL checks before issuing tickets.
- UNDO is single-level per file: SS snapshots the pre-commit state on the first write after an UNDO (or creation) and consumes it when UNDO is executed.
- Writes commit atomically via write-to-temp + rename.

1) View and create
```bash
VIEW
CREATE demo.txt -r
READ demo.txt
INFO demo.txt
```

2) Write with locking
```bash
WRITE demo.txt 0
# enter lines like: 0 Hello world.
ETIRW
READ demo.txt
```

3) Access control
```bash
ADDACCESS -R demo.txt bob
REMACCESS demo.txt bob
REQUEST_ACCESS demo.txt -R  # as a non-owner
VIEWREQUESTS demo.txt       # as owner
```

4) Checkpoints and revert
```bash
CHECKPOINT demo.txt snap1
LISTCHECKPOINTS demo.txt
VIEWCHECKPOINT demo.txt snap1
REVERT demo.txt snap1      # name
REVERT demo.txt snap1      # revert again (checkpoint only)
```

5) Folders and moves
```bash
CREATEFOLDER docs
WRITE docs/note.txt 0
ETIRW
VIEWFOLDER docs
MOVE docs/note.txt docs/notes.txt
RENAME docs/notes.txt docs/todo.txt
```

6) Execute scripts via NM (streaming output)
```bash
# Save a bash script into a file (via WRITE) and then execute it at the NM
WRITE tools/diag.sh 0
# enter lines:
# echo "Running diagnostics..."
# uname -a
# ls -la
# echo "Done!"
ETIRW

EXEC tools/diag.sh
# Output streams live (stdout+stderr mixed) until completion; final line shows exit code if non-zero.
```

7) Trash can (soft delete)
```bash
# Delete moves the file to a hidden trash namespace on the SS and records it in NM state
DELETE demo.txt

# List trashed items
LISTTRASH

# Restore a specific file (only the owner may restore)
RESTORE demo.txt

# Empty trash
# - Without an argument: purge all trashed items owned by you
# - With a file: purge only that file from trash
EMPTYTRASH           # purge all your trashed files
EMPTYTRASH demo.txt  # purge one file
```

## Data locations

- Name Manager (NM) state: `nm_state.json` at repo root. It stores users, directory mapping, ACLs, replicas, folders, and pending requests.
- Storage Server (SS) data: each SS writes under `ss_data/ss<ID>/` inside the repo:
  - `files/` current file contents
  - `undo/` single-level undo snapshots
  - `checkpoints/` named checkpoints per file
  - `meta/` reserved

You can run multiple SS from the same working directory; their data is kept separate by `ss<ID>`.

## Error messages and common causes

The client prints human-readable errors and does not dump raw JSON. Some errors include extra details from the server (msg).

- ERROR: resource not found
  - Causes: file doesn’t exist; checkpoint/tag not found; undo snapshot missing.
- ERROR: permission denied
  - Causes: you don’t have R/W access; request access or ask the owner; invalid/expired ticket.
- ERROR: conflict
  - Causes: creating a file that already exists; renaming to an existing target; conflicting operation.
- ERROR: sentence locked by another writer; try again later
  - Causes: another WRITE session holds a lock on the same sentence index.
- ERROR: service unavailable (no storage server reachable)
  - Causes: no SS registered or reachable; NM couldn’t map the file to a live SS.
- ERROR: bad request (invalid arguments/index or wrong sequence)
  - Causes: missing fields, non-numeric or out-of-range indices, APPLY without an active session, or other invalid input.
  - Details may include: missing-fields, invalid-index-or-content, session-active.
- ERROR: internal server error (I/O failure or unexpected state)
  - Causes: filesystem errors (permissions, disk), rename/write failures, or unhandled server-side conditions.
- Network/transport errors
  - Examples: failed to connect/send/receive to NM/SS. Usually indicates the service is down or a transient network issue.

## Colorized terminal output

The client prints colored output when stdout is a TTY:
- Green OK on success
- Red ERROR on failures
- Cyan section headers (e.g., requests)

Set NO_COLOR=1 to disable colors (or when piping output).

## Replication and failover (overview)

- NM tracks SS heartbeats. If a primary goes down, a live replica is promoted and the old primary is kept as a replica for later resync.
- On commit, SS notifies NM; NM replicates the update asynchronously (non-blocking) via PUT to replicas.
- When an SS comes back up, NM resynchronizes files where it is a replica.