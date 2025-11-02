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

Quick run (example on localhost ports 5000/6001/6002)

```bash
# Terminal 1
./bin/nm 5000

# Terminal 2
./bin/ss 127.0.0.1 5000 6001 6002 1
```

Then, in another terminal, try:

```bash
# Map or create a file in the directory (debug helper)
./bin/client 127.0.0.1 5000 dir-set demo.txt 1

# One-shot WRITE: begin->apply->end to replace/append a word
# Syntax: write <file> <sentenceIndex> <wordIndex> <content>
./bin/client 127.0.0.1 5000 write demo.txt 0 0 Hello
./bin/client 127.0.0.1 5000 write demo.txt 0 1 world.

# Read back
./bin/client 127.0.0.1 5000 read demo.txt

# Undo last committed change (single-level)
./bin/client 127.0.0.1 5000 undo demo.txt

# Folder and checkpoints (bonus)
# Create folder and a file inside
./bin/client 127.0.0.1 5000 mkdir docs
./bin/client 127.0.0.1 5000 write docs/note.txt 0 0 Hello
# Move file and folder
./bin/client 127.0.0.1 5000 mv docs/note.txt docs/notes.txt
./bin/client 127.0.0.1 5000 mv docs proj
# Checkpoints
./bin/client 127.0.0.1 5000 addaccess proj/notes.txt anonymous RW   # if needed for write-like ops
./bin/client 127.0.0.1 5000 checkpoint proj/notes.txt snap1
./bin/client 127.0.0.1 5000 list-checkpoints proj/notes.txt
./bin/client 127.0.0.1 5000 view-checkpoint proj/notes.txt snap1
./bin/client 127.0.0.1 5000 revertc proj/notes.txt snap1
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

Below commands use the uppercase forms and print outputs matching the examples in the spec. The client will first prompt for the username and registers it with the Name Server.

1) View files

```bash
./bin/client 127.0.0.1 5000 VIEW           # Lists files you can access
./bin/client 127.0.0.1 5000 VIEW -a        # Lists all files on the system
./bin/client 127.0.0.1 5000 VIEW -l        # Lists accessible files with details
./bin/client 127.0.0.1 5000 VIEW -al       # Lists all files with details
```

2) Read a file

```bash
./bin/client 127.0.0.1 5000 READ wowee.txt
```

3) Create a file

```bash
./bin/client 127.0.0.1 5000 CREATE mouse.txt
./bin/client 127.0.0.1 5000 VIEW
```

4) Write to a file (interactive, sentence locking)

```bash
# Start a write session on sentence index 0
./bin/client 127.0.0.1 5000 WRITE mouse.txt 0
# Now enter lines like below, finish with ETIRW
1 Im just a mouse.
ETIRW

# Append by writing to sentence 1
./bin/client 127.0.0.1 5000 WRITE mouse.txt 1
1 I dont like PNS
ETIRW

# Replace word in sentence 1
./bin/client 127.0.0.1 5000 WRITE mouse.txt 1
3 T-T
ETIRW

# Multiple applies in one session
./bin/client 127.0.0.1 5000 WRITE mouse.txt 0
4 deeply mistaken hollow lil gei-fwen
6 pocket-sized
ETIRW
```

5) Undo

```bash
./bin/client 127.0.0.1 5000 UNDO mouse.txt
```

6) Info

```bash
./bin/client 127.0.0.1 5000 INFO mouse.txt
```

7) Delete

```bash
./bin/client 127.0.0.1 5000 DELETE mouse.txt
```

8) Stream (0.1s between words, stops with STOP frame)

```bash
./bin/client 127.0.0.1 5000 STREAM wowee.txt
```

9) List users

```bash
./bin/client 127.0.0.1 5000 LIST
```

10) Access control

```bash
./bin/client 127.0.0.1 5000 ADDACCESS -R nuh_uh.txt user2
./bin/client 127.0.0.1 5000 ADDACCESS -W nuh_uh.txt user2 # implies RW
./bin/client 127.0.0.1 5000 REMACCESS nuh_uh.txt user2
```

11) Execute file (runs on NM and returns output)

```bash
./bin/client 127.0.0.1 5000 EXEC LMAAO.txt
```

Bonus (already implemented):

- Folders: CREATEFOLDER, VIEWFOLDER, MOVE. Use `mkdir`, `lsf`, `mv` subcommands as documented earlier.
- Checkpoints: CHECKPOINT, LISTCHECKPOINTS, VIEWCHECKPOINT, REVERT-by-name via `checkpoint`, `list-checkpoints`, `view-checkpoint`, `revertc`.

Error messages follow the spec format, e.g., `ERROR: not found`, `ERROR: permission denied`, etc.

- Add a `--data-dir` argument to SS to select the storage base path explicitly (no need to `cd`).
- Include an explicit numeric `code` field alongside `status` in all responses.

