# DOCS+

**Course Project for OSN (Monsoon 2025)**

**Team members: Naman Aggarwal(2024101018), Manav Beriwal(2024101105)**

## 1️⃣ Project Overview

**Docs++** is a distributed, multi-user document editing system built from scratch in C. It provides **sentence-level collaborative editing** with concurrent writes, access control, undo/redo via checkpoints, and transparent replication across multiple storage servers.

### Key Design Decisions

- **Sentence-Level Editing**: Documents split on sentence delimiters (`.`, `!`, `?`). Each sentence can be locked and edited independently.
- **Ticket-Based Authorization**: NM issues short-lived, signed tickets (file + operation + ssID). SS validates tickets to prevent unauthorized access.
- **Replication**: Each file is assigned a primary SS and one replicas. Commits trigger async replication via NM.
- **Merge-on-Commit**: When committing a sentence, SS re-reads the current file and merges only the edited sentence, preserving concurrent writes to other sentences.

---

## 2️⃣ System Architecture

### 2.1 Components

#### Name Server (NM)
- **Role**: Routing, ACL enforcement, user session management, replication orchestration, persistence.
- **State**: `nm_state.json` stores:
  - File → primary SS mapping
  - File → replica SS list
  - File → owner + ACL (user permissions: R, W, RW)
  - Active/inactive user sessions
  - Access requests (pending approval)
  - Logical folder hierarchy
  - Trash metadata (soft-deleted files)
- **Threading**: One pthread per client connection; shared state protected by mutexes.
- **Heartbeat Monitor**: Background thread marks SS down after 6s without heartbeat; promotes replicas on primary failure.

#### Storage Server (SS)
- **Role**: Persistent storage, sentence tokenization, write locks, UNDO snapshots, checkpoints.
- **Data Layout** (per SS instance):
  ```
  ss_data/ss<ID>/
    files/         ← current file contents
    undo/          ← single-level undo snapshots
    checkpoints/   ← named checkpoint files per file
  ```
- **Threading**: One pthread per client connection; global lock table protected by mutex.
- **Write Session**: Stateful; holds lock + in-memory doc until END_WRITE.

#### Client (CLI)
- **Interactive Shell**: Command history (arrow keys), tab-free user prompt.
- **Features**: Human-readable output (no raw JSON); colorized OK/ERROR (green/red); table formatting for lists.

### 2.2 Communication

- **Protocol**: Length-prefixed JSON over TCP.
  - Each message: `[4-byte big-endian length][JSON payload]`
  - Blocking I/O with `send_msg` / `recv_msg` wrappers.
- **Message Types**:
  - Client ↔ NM: `CREATE`, `DELETE`, `LOOKUP`, `RENAME`, `VIEWFOLDER`, `ADDACCESS`, `LISTTRASH`, etc.
  - NM ↔ SS: `SS_REGISTER`, `SS_HEARTBEAT`, `SS_COMMIT`, `SS_CHECKPOINT`, replication commands (`PUT`, `PUT_CHECKPOINT`).
  - Client ↔ SS (after LOOKUP): `READ`, `WRITE`, `UNDO`, `CHECKPOINT`, `REVERT`, `STREAM`, `INFO`.
- **Error Codes**: Standardized across NM/SS:
  - `OK`, `ERR_NOAUTH`, `ERR_NOTFOUND`, `ERR_LOCKED`, `ERR_CONFLICT`, `ERR_UNAVAILABLE`, `ERR_BADREQ`, `ERR_INTERNAL`.

### 2.3 High-Level Flow Diagram

```
Client                 Name Server (NM)          SS (Primary)              SS (Replica)
   │                           │                      │                         │
   │── CREATE ───────────────> │                      │                         │
   │                           │── pick least-loaded ─────────────────────────> │
   │                           │<────────────── OK ─────────────────────────────│
   │                           ├─ schedule CREATE replica ─────────────────────>│
   │<─────────────── OK ───────│                      │                         │
   │                           │                      │                         │
   │───────── WRITE ─────────> │                      │                         │
   │                           ├─ ACL check + issue ticket ────────────────────>│
   │<── ticket + SS address ───│                      │                         │
   │                           │                      │                         │
   │── BEGIN_WRITE(ticket) ─────────────────────────> │                         │
   │<────────── OK (lock acquired) ───────────────────│                         │
   │── APPLY (word/sentence updates) ────────────────>│                         │
   │── END_WRITE ────────────────────────────────────>│                         │
   │                           │<── SS_COMMIT notify ───────────────────────────│
   │                           ├─ schedule PUT replica ────────────────────────>│
   │<────────── OK (committed) ───────────────────────│                         │
   │                           │                      │                         │

```

---

## 3️⃣ Design Decisions

### 3.1 Sentence Representation

- **Tokenization**:
  - Sentences end at `.`, `!`, `?` (delimiter attached to last word).
  - Words separated by whitespace.
  - In-memory: `ss_doc_tokens_t` → array of sentence arrays of word strings.
  - On-disk: plain text. 

### 3.2 Locking for Concurrency

- **Sentence-Level Locks** (SS-side):
  - Lock entry: `(file, sentenceIndex)`.
  - Lock lifecycle: `BEGIN_WRITE` acquires → `APPLY` modifies → `END_WRITE` commits & releases.
  - Multiple readers: allowed concurrently (READ doesn't lock, readers see the latest snapshot of the file)
  - Exclusive writes: only one writer per sentence at a time; others get `ERR_LOCKED`.
- **Global Lock Table**: Protected by mutex; no deadlocks (single resource per session).

### 3.3 Undo Implementation

- **Single-Level UNDO** per file:
  - Stored: `ss_data/ss<ID>/undo/<file>.undo`
  - Captured: Before first commit after file creation or prior UNDO.
  - Mechanism: At `END_WRITE`, SS saves the *pre-image* from the session's initial snapshot to `.undo`.
  - Consumed: `UNDO` command swaps current file with undo snapshot and deletes `.undo`.
- **Limitation**: Only one UNDO available; successive UNDOs won't revert further back (unless using checkpoints).

### 3.4 Metadata Storage

- **NM State** (`nm_state.json`):
  - Directory: `file → (ss_id, owner, replicas, last_modified_user, last_accessed_user, timestamps)`
  - ACLs: `file → user → perms`
  - Folders: logical list of folder paths
  - Trash: `file → (trash_path, ss_id, owner, when)`
  - Requests: `file → [(user, mode), ...]`
  - Users: active/inactive lists
- **Persistence**: NM saves state after every mutation; SS uses atomic file ops.

### 3.5 Threading Model

- **NM**:
  - Main thread: accepts client connections.
  - Per-client thread: handles requests.
  - Heartbeat monitor thread: checks SS liveness; promotes replicas on failure.
  - Replication threads: PUT/PUT_CHECKPOINT to replicas.
- **SS**:
  - Main thread: binds data port.
  - Data server thread: accepts connections.
  - Per-connection thread: handles WRITE sessions.
  - Heartbeat thread: periodic ping to NM.
---

## 4️⃣ Directory Structure

```
course-project-osint/
├── client/
│   └── cli_main.c              # Interactive CLI with REPL, command parsing, human-readable output
├── nm/
│   ├── nm_main.c               # Main server loop, routing, replication orchestration
│   ├── nm_persist.c / .h       # JSON state save/load, ACL logic
│   └── nm_dir.c / .h           # File-to-SS mapping, folder management
├── ss/
│   ├── ss_main.c               # Data server, WRITE sessions, locks, UNDO, checkpoints
│   └── ss_tokenize.c / .h      # Sentence/word tokenization helpers
├── common/
│   ├── net_proto.c / .h        # send_msg/recv_msg, tcp_listen/tcp_connect, JSON helpers
│   └── tickets.c / .h          # Ticket build/validate (HMAC-like signing)
├── build/                      # .o object files (gitignored)
├── bin/                        # Compiled binaries: nm, ss, client (gitignored)
├── ss_data/                    # Per-SS data directories (created at runtime)
│   ├── ss1/                    # Storage Server ID=1
│   │   ├── files/
│   │   ├── undo/
│   │   └── checkpoints/
│   └── ss2/                    # Storage Server ID=2
│       └── ...
│   └── ss3/                    # Storage Server ID=3
│       └── ...
├── nm_state.json               # NM persistent state (created at runtime)
├── Makefile                    # Build system
└── README.md                   # This file
```

**Explanation**:
- **client/**: User-facing CLI; no state.
- **nm/**: Name Server logic; state in `nm_state.json`.
- **ss/**: Storage Server logic; state in `ss_data/ss<ID>/`.
- **common/**: Shared networking, JSON, and ticket utilities.
- **build/** and **bin/**: Generated during compilation.

---

## 5️⃣ Setup

### Compilation

```bash
make all
```

This builds:
- `bin/nm` (Name Server)
- `bin/ss` (Storage Server)
- `bin/client` (Client CLI)

Clean build artifacts:
```bash
make clean
```

### Running the System

#### Single Machine Setup

**Terminal 1: Name Server**
```bash
./bin/nm 5000
```

**Terminal 2: Storage Server #1**
```bash
./bin/ss 127.0.0.1 5000 6001 7001 1
```
- Args: `<nm_host> <nm_port> <ss_ctrl_port> <ss_data_port> [ss_id]`
- `ss_id` defaults to `ss_ctrl_port` if omitted.

**Terminal 3: Storage Server #2**
```bash
./bin/ss 127.0.0.1 5000 6002 7002 2
```

**Terminal 4: Client**
```bash
./bin/client 127.0.0.1 5000
```
- Connects to `127.0.0.1:5000` by default.
- Prompts for username; enter a unique name. 

#### Multiple Devices Setup

Ensure all machines are on the same network or have routable IPs.

**On NM machine (e.g., IP `a.b.c.d`)**:
```bash
./bin/nm 5000
```

**On Storage Server machine 1**:
```bash
./bin/ss a.b.c.d 5000 6001 7001 1
```

**On Storage Server machine 2**:
```bash
./bin/ss a.b.c.d 5000 6002 7002 2
```

**On any client machine**:
```bash
./bin/client a.b.c.d 5000
```
---

## 6️⃣ How the System Works (Step-by-Step)

### 6.1 Client Startup & Registration

1. Client connects to NM on TCP port (default 5000).
2. Sends `CLIENT_HELLO {user: "<username>"}`.
3. NM checks if user is already active:
   - If yes → `ERR_CONFLICT`.
   - If no → mark user active, save state, reply `OK`.
4. Client enters REPL loop; sends commands to NM.

### 6.2 SS Startup & Registration

1. SS binds data port (e.g., 7001).
2. Connects to NM and sends `SS_REGISTER {ssId: 1, ssCtrlPort: 6001, ssDataPort: 7001}`.
3. NM extracts SS IP from socket peer address, registers entry.
4. SS starts heartbeat thread → sends `SS_HEARTBEAT {ssId: 1}` every 2s.
5. NM marks SS `is_up=1` if heartbeat within last 6s.

### 6.3 File Read Pipeline

**Command**: `READ demo.txt`

1. Client → NM: `LOOKUP {op: "READ", file: "demo.txt", user: "alice"}`
2. NM:
   - Finds `demo.txt → ss_id=1`.
   - Checks ACL: `alice` needs R permission.
   - Builds ticket: `ticket_build("demo.txt", "READ", 1, 600, ...)` → signed string.
   - Returns: `{status: "OK", ssAddr: "127.0.0.1", ssDataPort: 7001, ticket: "..."}`.
3. Client → SS (7001): `READ {file: "demo.txt", ticket: "..."}`
4. SS:
   - Validates ticket: `ticket_validate(...)`.
   - Reads `ss_data/ss1/files/demo.txt`.
   - Returns: `{status: "OK", body: "Hello world. This is a demo."}`.
5. Client prints body.

**Metadata Update**: NM records `last_accessed_user="alice"` and `last_accessed_time=now`.

### 6.4 File Write Pipeline (Sentence Locking)

**Command**: `WRITE demo.txt 0`

1. Client → NM: `LOOKUP {op: "WRITE", file: "demo.txt", user: "alice"}`
2. NM:
   - Checks ACL: `alice` needs W permission.
   - Issues ticket for `WRITE` on SS 1.
   - Returns ticket + SS address.
3. Client → SS: `BEGIN_WRITE {file: "demo.txt", sentenceIndex: 0, ticket: "..."}`
4. SS:
   - Validates ticket.
   - Tries to acquire lock on `(demo.txt, 0)`.
   - If locked → `ERR_LOCKED`.
   - If free → acquire lock, load file, tokenize, allocate in-memory session.
   - Returns: `{status: "OK"}`.
5. Client enters interactive edit mode:
   - Prompts: `Enter <word_index> <content> lines; finish with ETIRW on its own line`
   - User enters:
     ```
     0 Hello
     1 world.
     ETIRW
     ```
6. For each line (except `ETIRW`):
   - Client → SS: `APPLY {wordIndex: 0, content: "Hello"}`
   - SS: updates in-memory token array, returns `OK`.
7. On `ETIRW`:
   - Client → SS: `END_WRITE {}`
8. SS:
   - **Merge-on-commit**: Re-reads current file, tokenizes, replaces only sentence 0 with session's sentence 0.
   - Saves undo snapshot (pre-image from session start) to `undo/demo.txt.undo`.
   - Composes merged doc, writes to temp file, renames to `files/demo.txt`.
   - Releases lock.
   - Sends `SS_COMMIT {file: "demo.txt", ssId: 1}` to NM.
   - Returns `OK` to client.
9. NM (async):
   - Fetches replicas for `demo.txt`.
   - Spawns thread: reads file from primary via `READ` ticket, sends `PUT` to each replica.
10. Client prints `OK`.

**Metadata Update**: NM records `last_modified_user="alice"` and `last_modified_time=now`.

**Concurrency**: If Bob tries `BEGIN_WRITE` on sentence 0 while Alice's session is active → `ERR_LOCKED`. Bob can edit sentence 1 concurrently.

### 6.5 Undo Mechanism

**Command**: `UNDO demo.txt`

1. Client → NM: `LOOKUP {op: "UNDO", file: "demo.txt", user: "alice"}`
2. NM: issues ticket for `UNDO` on primary SS.
3. Client → SS: `UNDO {file: "demo.txt", ticket: "..."}`
4. SS:
   - Reads `undo/demo.txt.undo` (pre-commit snapshot).
   - Writes snapshot to temp file, renames to `files/demo.txt`.
   - Deletes `undo/demo.txt.undo` (consumes undo).
   - Sends `SS_COMMIT` to NM (triggers replication).
   - Returns `OK`.
5. Client prints `OK`.

**Limitation**: Only one undo level. After UNDO, the undo snapshot is gone. `CHECKPOINT` has multi-level history.

### 6.6 Streaming (Word-by-Word)

**Command**: `STREAM demo.txt`

1. Client → NM: `LOOKUP {op: "READ", ...}` → get ticket.
2. Client → SS: `STREAM {file: "demo.txt", ticket: "..."}`
3. SS:
   - Reads file, splits on whitespace into words.
   - Sends frames: `{status: "OK", word: "Hello"}`, delay 0.1s, repeat.
   - Final frame: `{status: "STOP"}`.
4. Client prints each word as received, then newline on STOP.

**When Server stops mid-stream, we get an error message specifying the same**.

### 6.7 Execute Operation (NM-side Execution)

**Command**: `EXEC script.sh`

1. Client → NM: `LOOKUP {op: "READ", file: "script.sh", user: "alice"}`.
2. NM → SS: issues ticket, fetches file content via `READ`.
3. NM:
   - Unescapes JSON body (converts `\n` to real newlines).
   - Forks `/bin/sh -s`, pipes script to stdin.
   - Reads stdout+stderr in chunks, sends to client as `{status: "OK", chunk: "..."}`.
   - Final frame: `{status: "STOP", exit: <code>}`.
4. Client prints streaming output, shows exit code if non-zero.

**Security Note**: Script runs on NM machine, in NM's working directory (or optional chdir to `ss_data/ss1/files/`). No sandboxing. For production, restrict EXEC to trusted users.

### 6.8 How NM Chooses SS

- **File Creation**: NM picks the least-loaded SS (counts mappings per SS, chooses min).
- **File Access**: NM looks up file → SS mapping in directory.
- **Replication**: NM assigns one replica per file (picks next available SS != primary).

**Simple least-loaded strategy.**

---

## 7️⃣ Supported Commands

### Basic File Operations

#### `VIEW [-a] [-l]`
List files. Flags:
- `-a`: Show all files (admin mode).
- `-l`: Detailed view (table with words, chars, last access time, owner).

**Example**:
```bash
VIEW
VIEW -l
```

#### `READ <file>`
Print file contents.

**Example**:
```bash
READ demo.txt
```

#### `CREATE <file> [-r] [-w]`
Create empty file. Flags:
- `-r`: Public read (anyone can READ without ACL).
- `-w`: Public write (anyone can WRITE).

**Example**:
```bash
CREATE notes.txt
CREATE public.txt -r -w
```

#### `DELETE <file>`
Soft-delete file (moves to trash). Owner-only.

**Example**:
```bash
DELETE notes.txt
```

#### `INFO <file>`
Show file metadata: owner, size, words, last modified/accessed (with user/time).

**Example**:
```bash
INFO notes.txt
```

### Writing & Editing

#### `WRITE <file> <sentenceIndex>`
Start interactive write session on a sentence. Prompts:
```
Enter <word_index> <content> lines; finish with ETIRW on its own line
```

**Example**:
```bash
WRITE notes.txt 0
# Enter:
0 This is the first sentence.
ETIRW
```

**Notes**:
- `word_index` starts at 0 within the sentence.
- Session holds a lock until `ETIRW`.

#### `UNDO <file>`
Revert to last snapshot before most recent write. Single-level.

**Example**:
```bash
UNDO notes.txt
```

### Checkpoints & Versioning

#### `CHECKPOINT <file> <name>`
Save current file as a named checkpoint.

**Example**:
```bash
CHECKPOINT notes.txt v1
```

#### `LISTCHECKPOINTS <file>`
List all checkpoint tags for file.

**Example**:
```bash
LISTCHECKPOINTS notes.txt
```

#### `VIEWCHECKPOINT <file> <name>`
Print checkpoint content (read-only).

**Example**:
```bash
VIEWCHECKPOINT notes.txt v1
```

#### `REVERT <file> <checkpointName>`
Restore file to checkpoint (overwrites current).

**Example**:
```bash
REVERT notes.txt v1
```

### Folders & Rename/Move

#### `CREATEFOLDER <path>`
Create logical folder (no files yet).

**Example**:
```bash
CREATEFOLDER docs
CREATEFOLDER docs/projects
```

#### `VIEWFOLDER <path>`
List files and folders in path.

**Example**:
```bash
VIEWFOLDER folder
```

#### `RENAME <old> <new>`
Rename file (NM updates mapping; SS renames file + undo + checkpoints).

**Example**:
```bash
RENAME old.txt new.txt
```

#### `MOVE <src> <dst>`
Move file to folder or rename.

**Example**:
```bash
MOVE notes.txt docs/notes.txt
```

### Trash Management

#### `LISTTRASH`
List all soft-deleted files (owner, trash path, timestamp).

**Example**:
```bash
LISTTRASH
```

#### `RESTORE <file>`
Restore file from trash (owner-only).

**Example**:
```bash
RESTORE notes.txt
```

#### `EMPTYTRASH [<file>]`
Permanently delete trashed files. Without arg: purge all owned by you. With arg: purge specific file.

**Example**:
```bash
EMPTYTRASH
EMPTYTRASH notes.txt
```

### Access Control

#### `ADDACCESS -r|-w <file> <user>`
Grant access. Modes:
- `-r`: Read-only.
- `-w`: Read+Write.

**Example**:
```bash
ADDACCESS -r notes.txt bob
ADDACCESS -w notes.txt alice
```

#### `REMACCESS <file> <user>`
Revoke all access for user.

**Example**:
```bash
REMACCESS notes.txt bob
```

#### `REQUEST_ACCESS <file> [-r|-w]`
Submit access request (non-owners). Owner sees via `VIEWREQUESTS`.

**Example**:
```bash
REQUEST_ACCESS notes.txt -r
```

#### `VIEWREQUESTS <file>`
List pending access requests (owner-only).

**Example**:
```bash
VIEWREQUESTS notes.txt
```

### User Management

#### `LIST`
Show active and inactive users.

**Example**:
```bash
LIST
```

### Streaming & Execution

#### `STREAM <file>`
Stream file word-by-word (0.1s delay between words).

**Example**:
```bash
STREAM story.txt
```

#### `EXEC <file>`
Execute file as a shell script on NM. Output streams live; final line shows exit code.

**Example**:
```bash
EXEC tools/diag.sh
```

### Utility

#### `CLEAR`
Clear terminal screen (ANSI escape).

#### `EXIT` 
Log out and exit client.

---

## 8️⃣ Persistence

### Name Server State

- **File**: `nm_state.json` (JSON format, human-readable).
- **Saved**: After every mutation (create, delete, ACL change, user login/logout, replication metadata).
- **Loaded**: On startup; if missing, starts with empty state.

### Storage Server Data

- **Files**: `ss_data/ss<ID>/files/<path>` (plain text).
- **Undo**: `ss_data/ss<ID>/undo/<path>.undo` (single snapshot).
- **Checkpoints**: `ss_data/ss<ID>/checkpoints/<path>/<name>.chk` (multiple named snapshots).
- **Atomic Writes**: Temp file → rename.
- **Survives Restart**: All data persists; SS does not reload state from NM (stateless for directory mapping).

### Replication

- **NM tracks replicas** in `nm_state.json`.
- **Async replication**: NM spawns thread on `SS_COMMIT` notification; fetches file from primary, sends `PUT` to replicas.
- **Checkpoint replication**: On `SS_CHECKPOINT` notification, NM fetches checkpoint from primary via `VIEWCHECKPOINT`, sends `PUT_CHECKPOINT` to replicas.
- **Resync on SS UP**: When SS heartbeat transitions from down to up, NM:
  - Replicates current file content (for files where SS is a replica).
  - Replicates undo snapshot (if exists on primary).
  - Fetches checkpoint list from primary and replicates each checkpoint.

---

## 9️⃣ Caching & Efficient Search

#### Hash Map Implementation

All NM data structures use **custom hash maps** with chaining for **O(1) average-case lookups**:

- **Hash Function**: DJB2 algorithm (`hash = ((hash << 5) + hash) + c`) with 256 buckets
- **Collision Resolution**: Separate chaining with linked lists
- **Data Structures Optimized**:
  1. **Users**: `user_hash_node_t` - maps username → active status (O(1) login check)
  2. **ACLs**: `acl_hash_node_t` - maps filename → ACL entry index (O(1) permission check)
  3. **Folders**: `folder_hash_node_t` - maps folder path → index (O(1) existence check)
  4. **Requests**: `req_hash_node_t` - maps filename → request entry index (O(1) request lookup)
  5. **Trash**: `trash_hash_node_t` - maps filename → trash entry index (O(1) restore/purge)
  6. **Directory**: `nm_dir.c` with 257-bucket hash map + 64-entry LRU cache (O(1) for LRU and hash access)

#### Complexity Analysis
- File lookup: **O(1)** average, O(n) worst (hash collision chain)
- ACL check: **O(1)** average - hash map to ACL index, then O(g)
- User active check: **O(1)** average - direct hash map lookup
- Folder exists: **O(1)** average - hash map lookup
- Request lookup: **O(1)** average - hash map to request index
- Trash find: **O(1)** average - hash map to trash index

**Trade-offs**:
- Memory overhead: ~8KB for hash map buckets (256 buckets × 5 maps × 8 bytes/pointer)
- Index maintenance: On delete, must update hash maps when swapping array elements
- Collision handling: Uses chaining; average chain length < 2 for typical workloads (< 1000 entries)

#### LRU Cache for Directory Lookups

The directory mapping (`nm_dir.c`) implements a **2-tier lookup**:
1. **LRU Cache** (64 entries, doubly-linked list): Stores most recently accessed file mappings
2. **Hash Map** (257 buckets): Full directory index

**Workflow**:
- Lookup: Check LRU → if miss, check hash map → promote to LRU head
- Insert: Update hash map → update/insert LRU (evict tail if > 64 entries)
- Delete: Remove from both hash map and LRU

### SS Lookup

- **Lock Table**: Linked list; **O(n)** scan (n = number of active locks, typically < 100)
  - Trade-off: Simple implementation; locks are short-lived (milliseconds to seconds)

---

## 1️⃣0️⃣ Logging

### Log Format

**NM**:
```
[NM] Registered SS id=1 ctrl=6001 data=7001 addr=127.0.0.1
[NM] LOOKUP op=READ file=demo.txt have_op=1 have_file=1
[NM] Replicated PUT demo.txt -> ss2
[NM] Replicated CHECKPOINT demo.txt@v1 -> ss2
```

**SS**:
```
[SS] accept cfd=4
[SS] recv 42 bytes: {"type":"READ","file":"demo.txt","ticket":"..."}
[SS] type=READ
[SS] CREATE file=demo.txt path=ss_data/ss1/files/demo.txt
[SS] BEGIN_WRITE file=demo.txt okf=1 idxrc=0 sidx=0
[SS] lock_acquire rc=0
[SS] END_WRITE composing: Hello world.
[SS] END_WRITE commit OK
```

### Colorized Output

- **Client**:
  - Green: `OK`
  - Red: `ERROR`
  - Cyan: Section headers (e.g., "Access Requests:")
- **Disable**: `export NO_COLOR=1`

---

## 1️⃣1️⃣ Error Handling

### Error Codes (Unified NM/SS)

| Code                | Meaning                                      | Example Causes                                                                 |
|---------------------|----------------------------------------------|-------------------------------------------------------------------------------|
| `OK`                | Success                                      | -                                                                             |
| `ERR_NOAUTH`        | Permission denied                            | ACL check failed; invalid/expired ticket                                       |
| `ERR_NOTFOUND`      | Resource not found                           | File doesn't exist; checkpoint tag missing; undo snapshot missing              |
| `ERR_LOCKED`        | Sentence locked by another writer            | Concurrent WRITE to same sentence                                              |
| `ERR_CONFLICT`      | Name/state conflict                          | CREATE on existing file; RENAME to existing target; duplicate user login       |
| `ERR_UNAVAILABLE`   | Service unavailable                          | No SS reachable; primary down and no replica; NM connection failed             |
| `ERR_BADREQ`        | Bad request                                  | Missing fields; invalid indices; APPLY without active session; malformed JSON  |
| `ERR_INTERNAL`      | Internal server error                        | I/O failure (permissions, disk full); unexpected state                         |


---

## 1️⃣2️⃣ Bonus Features

### ✅ Folders

- **Logical Hierarchy**: NM stores folder paths; no physical directories (except in SS data).
- **Commands**: `CREATEFOLDER`, `VIEWFOLDER`, `MOVE` (supports folder destinations).
- **Implementation**: NM maintains vector of folder strings; prefix-based filtering for children.

### ✅ Checkpoints

- **Named Snapshots**: Save current file as `<name>.chk` in `checkpoints/<file>/`.
- **Commands**: `CHECKPOINT`, `LISTCHECKPOINTS`, `VIEWCHECKPOINT`, `REVERT`.
- **Storage**: SS stores full file copy per checkpoint (no delta/diff).
- **Replication**: Checkpoints replicated to replicas on creation; resynced on SS UP.

### ✅ Access Request System

- **Workflow**:
  1. Non-owner runs `REQUEST_ACCESS <file> [-r|-w]`.
  2. NM stores pending request.
  3. Owner runs `VIEWREQUESTS <file>` → sees list.
  4. Owner grants via `ADDACCESS` → removes request.
- **No Auto-Approval**: Manual owner action required.

### ✅ Replication

- **1+1 Setup**: Each file assigned one primary + one replica (if available).
- **Async PUT**: NM spawns thread on `SS_COMMIT`, fetches from primary, sends to replica(s).
- **Failover**: Heartbeat monitor promotes replica on primary down.
- **Checkpoint Replication**: On `SS_CHECKPOINT`, NM replicates checkpoint file to replicas.

### ✅ Trash Can (Soft Delete)

- **DELETE** moves file to `.trash/<timestamp>_<escaped_name>` on SS; NM records metadata.
- **LISTTRASH** shows all trashed items (owner, timestamp).
- **RESTORE** renames back from trash (owner-only).
- **EMPTYTRASH** permanently deletes (hard unlink).