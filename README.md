[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/0ek2UV58)

# Docs++ (Course Project)

Milestone recap:
- M1: NM/SS registration and CLIENT_HELLO
- M2: NM persistence (users, directory), SS on-disk layout
- M3: Directory + LRU, VIEW/LIST endpoints
- M4: READ: NM LOOKUP and SS data server
- M5: CREATE/DELETE through NM routed to SS
- M6: WRITE with sentence-level locks and atomic commit
- M7: Tickets enforced end-to-end + NM-side ACL checks (owner/grants)
- M8: Single-level UNDO (SS keeps one snapshot per file and restores atomically)
- M9: Version history (HISTORY/REVERT) with per-commit snapshots
- M10: Rename/move across NM+SS (file, undo, and history)
- M11: Migration and placement (least-loaded auto-provision; NM-orchestrated MIGRATE using SS PUT)
- M12: Logging & error model (human-friendly CLI, structured server logs)
- (Bonus) M13: Folders (CREATEFOLDER/VIEWFOLDER/MOVE). Nested paths supported on SS.
- (Bonus) M14: Checkpoints (CHECKPOINT/LISTCHECKPOINTS/VIEWCHECKPOINT/REVERT by name)

Build

```bash
make -s all
```

## How to run and verify all features (single and multiple users)

1) Start the servers (one NM and two SS)

```bash
# Terminal 1: Name Manager (NM)
./bin/nm 5000

# Terminal 2: Storage Server #1 (id=1)
./bin/ss 127.0.0.1 5000 6001 6002 1

# Terminal 3: Storage Server #2 (id=2)
./bin/ss 127.0.0.1 5000 6003 6004 2
```

2) Launch two clients (multi-user)

```bash
# Terminal 4 (User alice)
./bin/client 127.0.0.1 5000
# When prompted: alice

# Terminal 5 (User bob)
./bin/client 127.0.0.1 5000
# When prompted: bob
```

3) Verify core commands

- In either client: `VIEW`, `VIEW -l`, `VIEW -a`, `VIEW -al`
- As alice: `CREATE demo.txt`
- `READ demo.txt`, `INFO demo.txt`, `STREAM demo.txt`
- `DELETE demo.txt`

4) Verify writing with sentence-level locking (concurrency)

- Alice: `CREATE demo.txt` then `WRITE demo.txt 0` → enter `0 Hello world.` then `ETIRW`
- Bob in parallel: `WRITE demo.txt 0` while alice is writing → should see `ERROR: locked`
- After alice finishes, Bob retries `WRITE demo.txt 0` → now succeeds
- Bob can `READ demo.txt` at any time; it returns the last committed content

5) Verify UNDO, HISTORY, REVERT

- `UNDO demo.txt`
- `HISTORY demo.txt`
- `REVERT demo.txt 1`

6) Verify checkpoints (bonus)

- `checkpoint demo.txt snap1`
- `list-checkpoints demo.txt`
- `view-checkpoint demo.txt snap1`
- `revertc demo.txt snap1`

7) Verify folders (bonus)

- `mkdir docs`
- `WRITE docs/note.txt 0` → provide lines, then `ETIRW`
- `mv docs/note.txt docs/notes.txt`
- `lsf docs`

8) Verify access control (grants and requests)

- Owner grants:
	- As alice: `ADDACCESS -R demo.txt bob` (R) or `ADDACCESS -W demo.txt bob` (RW)
	- Remove: `REMACCESS demo.txt bob`
- Requests workflow:
	- As bob: `REQUEST_ACCESS demo.txt -R`
	- As alice: `VIEWREQUESTS demo.txt` → shows pending users
	- As alice: `APPROVE_ACCESS demo.txt -R bob` (or `DENY_ACCESS demo.txt bob`)
	- As bob: `READ demo.txt` → should now work after approval

9) Verify STATS and replication/failover (bonus)

- Run `STATS` to see summary: files, activeLocks (stub), replicationQueue
- With two SSs running, perform a write, then stop the primary SS process → `READ demo.txt` should still work via a replica (NM promotes replicas automatically)
Tip: A scripted demo is available:

```bash
./run_test.sh
```
Quick run (example on localhost ports 5000/6001/6002)

```bash
# Terminal 1: Name Manager
./bin/nm 5000

# Terminal 2: Storage Server #1 (id=1)
./bin/ss 127.0.0.1 5000 6001 6002 1

# Terminal 3: Storage Server #2 (id=2)
./bin/ss 127.0.0.1 5000 6003 6004 2
```

Then, in another terminal, try:

```bash
# Interactive client (shell mode)
./bin/client
# (You will be prompted for a username; defaults to NM 127.0.0.1:5000)
# At the docs> prompt, try:
#   VIEW
#   VIEW -l
#   CREATE mouse.txt
#   WRITE mouse.txt 0   # then enter lines and finish with ETIRW
#   READ mouse.txt
#   INFO mouse.txt
#   EXEC some.sh        # if the file contains shell commands
#   LIST
#   STATS
#   REQUEST_ACCESS wowee.txt -R   # as a non-owner user
#   VIEWREQUESTS wowee.txt        # as owner
#   APPROVE_ACCESS wowee.txt -R user2
#   DENY_ACCESS wowee.txt user2
#   exit

# Map or create a file in the directory (debug helper)
# At the docs> prompt:
#   dir-set demo.txt 1

# Folder and checkpoints (bonus)
# At the docs> prompt:
#   mkdir docs
#   WRITE docs/note.txt 0
#   0 Hello
#   ETIRW
#   mv docs/note.txt docs/notes.txt
#   mv docs proj
#   addaccess proj/notes.txt anonymous RW   # if needed for write-like ops
#   checkpoint proj/notes.txt snap1
#   list-checkpoints proj/notes.txt
#   view-checkpoint proj/notes.txt snap1
#   revertc proj/notes.txt snap1
```

Notes
- Sentence ends at '.', '!' or '?'. The delimiter stays attached to the last word.
- WRITE uses sentence-level locks on the SS; concurrent writes to the same sentence are rejected with ERR_LOCKED.
- Tickets are implemented and enforced via short-lived tickets (M7). NM performs ACL checks before issuing tickets.
- UNDO is single-level per file: the SS snapshots the pre-commit state on first write after an UNDO (or creation) and consumes it when UNDO is executed.
- Writes commit atomically via write-to-temp + rename.

## M12 – Logging & error model

What’s new:

- Human-friendly CLI output: the client now renders clear messages instead of raw JSON.
	- For READ, it prints just the file content on success.
	- For errors, it maps status to messages like “not found”, “permission denied”, etc.
- Structured server logs: SS/NM logs include explicit operation tags and additional context to aid debugging. The SS PUT path now logs temp/final paths and commit confirmation.

Multi-SS note (M11 + M12):

- Run each Storage Server (SS) from its own working directory so their `ss_data` folders don’t overlap. Example:
	- SS1: `cd /tmp/ss1_run && /path/to/bin/ss 127.0.0.1 5000 6001 6002 1`
	- SS2: `cd /tmp/ss2_run && /path/to/bin/ss 127.0.0.1 5000 6003 6004 2`
- If two SS share the same `ss_data`, a MIGRATE that deletes the source file could also delete the destination copy. Separate working dirs avoid this.

Next steps (optional):

## How to test each feature (spec I/O)

All commands are issued inside the interactive client shell (docs> prompt). The client will first prompt for the username and register it with the Name Server.

1) View files

At the docs> prompt:

```
VIEW            # Lists files you can access
VIEW -a         # Lists all files on the system
VIEW -l         # Lists accessible files with details
VIEW -al        # Lists all files with details
```

2) Read a file

```
READ wowee.txt
```

3) Create a file

```
# Private by default (owner RW only)
CREATE mouse.txt
VIEW

# Public read at creation
CREATE public.txt -r

# Public write at creation (implies read)
CREATE scratch.txt -w
```

4) Write to a file (interactive, sentence locking)

```
# Start a write session on sentence index 0
WRITE mouse.txt 0
# Now enter lines like below, finish with ETIRW
1 Im just a mouse.
ETIRW

# Append by writing to sentence 1
WRITE mouse.txt 1
1 I dont like PNS
ETIRW

# Replace word in sentence 1
WRITE mouse.txt 1
3 T-T
ETIRW

# Multiple applies in one session
WRITE mouse.txt 0
4 deeply mistaken hollow lil gei-fwen
6 pocket-sized
ETIRW
```

5) Undo

```
UNDO mouse.txt
```

6) Info

```
INFO mouse.txt
```

7) Delete

```
DELETE mouse.txt
```

8) Stream (0.1s between words, stops with STOP frame)

```
STREAM wowee.txt
```

9) List users

```
LIST
```

10) Access control

```
ADDACCESS -R nuh_uh.txt user2
ADDACCESS -W nuh_uh.txt user2   # implies RW
REMACCESS nuh_uh.txt user2
```

Access requests workflow (NM-managed):

```
# As a non-owner
REQUEST_ACCESS file.txt -R      # or -W

# As the owner
VIEWREQUESTS file.txt
APPROVE_ACCESS file.txt -R user2
DENY_ACCESS file.txt user2
```

Default visibility and CREATE flags:

- Files are private by default: only the owner has RW.
- You can make files public at creation time using flags:
	- `-r` grants READ to `anonymous` (all users).
	- `-w` grants WRITE (and READ) to `anonymous`.
	- You can combine them; `-w` implies read.
	- Example: `CREATE notes.txt -r` or `CREATE scratch.txt -w`.

11) Execute file (runs on NM and returns output)

```
EXEC LMAAO.txt
```

Bonus (already implemented):

- Folders: CREATEFOLDER, VIEWFOLDER, MOVE. Use `mkdir`, `lsf`, `mv` subcommands as documented earlier.
- Checkpoints: CHECKPOINT, LISTCHECKPOINTS, VIEWCHECKPOINT, REVERT-by-name via `checkpoint`, `list-checkpoints`, `view-checkpoint`, `revertc`.

Replication & fault tolerance (bonus):

- Two SSs can be registered; NM maintains heartbeats (every ~2s) to detect failures.
- NM promotes replicas automatically on primary failure and continues serving LOOKUP via a live replica.
- SS sends a commit notification after END_WRITE; NM schedules async replication (PUT) to replicas.
- On SS recovery, NM resynchronizes file data opportunistically.

STATS shows a quick system summary:

```
STATS           # prints files, activeLocks (stub), replicationQueue length
```

Run a quick end-to-end demo:

```
./run_test.sh
```

## Where data is stored (files and metadata)

- Name Manager (NM) persistent state:
	- File: `nm_state.json` at the repository root.
	- Contents include:
		- `users`: list of known usernames
		- `directory`: map file → primary SS id
		- `acls`: per-file `owner` and `grants` (user → R/W/RW)
		- `folders`: folder structure
		- `replicas`: map file → [ssIds] (present when replicas are configured)
		- `requests`: map file → [usernames] (pending access requests; present when used)
	- The file is backward-compatible: fields appear as features are used.

- Storage Server (SS) on-disk layout:
	- Each SS stores data under its working directory, inside `ss_data/`:
		- `ss_data/files/<path>`: the current contents of user files
		- `ss_data/history/<file>.<version>.snap`: versioned snapshots
		- `ss_data/undo/<file>.undo`: single-level undo snapshot
		- `ss_data/checkpoints/<file>.<name>.snap`: named checkpoints
		- `ss_data/meta/`: miscellaneous metadata (reserved)
	- Tip: Run each SS from its own working directory to keep data isolated. The provided `run_test.sh` uses separate temporary directories for SS1 and SS2.

Error messages follow the spec format, e.g., `ERROR: not found`, `ERROR: permission denied`, etc.

- Add a `--data-dir` argument to SS to select the storage base path explicitly (no need to `cd`).
- Include an explicit numeric `code` field alongside `status` in all responses.