# Design Doc — Docs++ (CS3301 OSN Course Project)

## 1) Project overview

A simplified Google-Docs-like system with three binaries:

- Name Server (NM): single coordinator. Tracks storage servers, user registry, file→server mapping, ACLs; directly serves some commands and brokers others. If NM dies, the system is considered down. ([karthikv1392.github.io][1])
- Storage Servers (SS): hold files and metadata, handle concurrent client reads/writes, implement sentence-level write locks and single-level UNDO. SS can join later and (re)register with NM. ([karthikv1392.github.io][1])
- Client (CLI): terminal app for VIEW/READ/CREATE/WRITE/UNDO/INFO/DELETE/STREAM/LIST/ACCESS/EXEC. For READ/WRITE/STREAM, client talks directly to SS after an NM lookup; other operations are served via NM. EXEC always runs at NM. ([karthikv1392.github.io][1])

File model. Text only. A sentence ends at ., !, or ? (even in strings like “e.g.”). Words are ASCII sequences separated by spaces. Indices are based on the current state after each completed WRITE. ([karthikv1392.github.io][1])

## 2) Goals & non-goals

Goals (grading-relevant):

- All User Functionalities (150): view, read, create, write with sentence locks, single-step undo, info, delete, stream with 0.1 s delay, list users, access control, execute at NM. ([karthikv1392.github.io][1])
- System Requirements (40): persistence, access control, logging, unified error codes, and efficient search + caching on NM. ([karthikv1392.github.io][1])
- Specs (10): init & flows for NM/SS/Client registration and routing. ([karthikv1392.github.io][1])

Non-goals (base): NM high-availability/failover is explicitly out of scope; replication/fault-tolerance are optional bonus items. ([karthikv1392.github.io][1])

## 3) Assumptions

- POSIX C + TCP sockets (as suggested by TA tips). ([karthikv1392.github.io][1])
- NM address/port are “well-known” to clients and SS (provided at startup). ([karthikv1392.github.io][1])
- One global NM; many SS; many Clients. SS can join after NM boot. ([karthikv1392.github.io][1])
- UNDO is file-scoped, last change only, not per-user. ([karthikv1392.github.io][1])

## 4) High-level architecture

Process model

- NM: single process with a small thread pool (or select() loop) to handle concurrent client and SS control connections; in-memory indexes + periodic state persistence.
- SS: thread-per-client or thread-pool server; per-file mutex + per-sentence lightweight locks for writes; direct client port for data operations.
- Client: synchronous CLI; maintains two TCP channels: NM control and (when needed) a transient SS data channel.

Deployment topology

```
      +-------------------+
      |        NM         |  (well-known control port)
      |  Registry, Dir    |
      +-------------------+
        ^      ^      ^
        |      |      |
  SS-ctrl   client  SS-ctrl
        \     |     /
         \    |    /
     +-----------------+        +-----------------+
     |       SS1       |  ...   |       SSk       |
     | data port, fs   |        | data port, fs   |
     +-----------------+        +-----------------+
```

Command routing (from the spec):

| Command | Handled by | Notes |
| --- | --- | --- |
| VIEW, LIST, INFO, ADDACCESS/-R/-W, REMACCESS | NM | Served from NM’s state (may consult SS for details). ([karthikv1392.github.io][1]) |
| CREATE, DELETE | NM→SS | NM chooses SS, forwards op, waits for ACK, updates directory. ([karthikv1392.github.io][1]) |
| READ, WRITE, STREAM | Client↔SS | NM returns SS IP:port (and a short-lived ticket). Direct data path afterward. 0.1 s inter-word delay for STREAM. ([karthikv1392.github.io][1]) |
| EXEC | NM | NM fetches content from SS, then executes at NM and pipes output to client. ([karthikv1392.github.io][1]) |

## 5) Networking & protocol

- Transport: TCP everywhere (per TA pointers). ([karthikv1392.github.io][1])
- Framing: length-prefixed JSON messages to keep parsing simple and robust:

```
uint32_t len_be
{ "type": "LOOKUP", "reqId": "...", "user": "alice", "payload": {...} }
```

Key RPCs

- SS→NM on boot: SS_REGISTER{ ssId, ssCtrlPort, ssDataPort, files[] } (and on reconnect). NM replies OK.
- Client→NM on boot: CLIENT_HELLO{ user, clientAddr } → OK.
- Client→NM LOOKUP (e.g., READ/WRITE/STREAM): request with {op, file} → response {ssAddr, ssDataPort, ticket} (ticket = signed blob or nonce).
- Client→SS BEGIN_WRITE: {file, sentenceIndex, ticket} → lock acquisition or error.
- Client→SS APPLY: {wordIndex, content} repeated; END_WRITE commits atomically and releases lock.

Tickets vs re-auth: To enforce ACLs at SS without an NM round-trip on every call, NM issues a short-lived signed ticket embedding {user, file, op, exp}; SS verifies signature.

## 6) Data model & persistence

At NM (persist to nm_state.json):

- Users (active sessions).
- Directory: fileName → primarySS (and optional replicas, if doing bonus).
- ACLs: fileName → { owner, grants: {user → R|RW} }.
- File facts cache (optional): last access time, word/char counts (authoritative copy lives at SS).

At SS:

- FileData: tokenized representation:
  - sentences: [ [word, word, …], … ] (delimiter characters remain attached to trailing word when we re-compose).
- FileMeta: { owner, createdTs, modifiedTs, lastAccessTs, lastAccessUser, wordCount, charCount }.
- UndoRecord (single): pre-image of the last committed WRITE session (may touch multiple words/sentences) + who/when. Only one level per file. ([karthikv1392.github.io][1])

On-disk layout (example):

```
ss_data/
  files/<filename>.txt           # canonical text
  meta/<filename>.json           # FileMeta
  undo/<filename>.json           # UndoRecord (last change only)
```

Every commit uses write-to-temp + atomic rename; call fsync() before ACK to NM to satisfy persistence requirements. ([karthikv1392.github.io][1])

## 7) Concurrency, locking, and streaming

- Sentence tokenization: split on ., !, ? (every such occurrence is a delimiter, per requirement). Indices refer to the current post-write state. ([karthikv1392.github.io][1])
- Locks: locks[file][sentenceIndex] = {ownerUser, since}; a file-level mutex guards the structure + commit. A WRITE session spans BEGIN_WRITE…END_WRITE and is the atomic unit for UNDO. Conflict errors: locked sentence, invalid indices, or no write permission. (See example error cases.) ([karthikv1392.github.io][1])
- Streaming: STREAM sends words with ~0.1 s delay. Mid-stream SS failure must surface an appropriate error to the client. ([karthikv1392.github.io][1])

## 8) Access control

- Owner: always RW. ADDACCESS -R / -W adds R or RW; REMACCESS removes all access for a user. Enforced at NM on routing and at SS via ticket verification. INFO shows ACL. ([karthikv1392.github.io][1])

## 9) Efficient search & caching (NM)

- Requirement: faster than O(N) lookup for files and recent queries. Use a hash map for exact filename → SS and an LRU cache for repeated queries; optionally add a trie for prefix filters in VIEW -l/-al. ([karthikv1392.github.io][1])

## 10) Logging & error handling

- Unified error codes: OK, ERR_NOAUTH, ERR_NOTFOUND, ERR_LOCKED, ERR_BADREQ, ERR_CONFLICT, ERR_UNAVAILABLE, ERR_INTERNAL.
- What to log: at NM and SS, every request/ack/response with {ts, component, peer ip:port, user, op, file, status, details} as specified. NM should also print user-visible status messages. ([karthikv1392.github.io][1])

## 11) Initialization & lifecycle

1. NM starts (well-known addr). Loads nm_state.json (if present).
2. SS start: send SS_REGISTER (ctrl port, data port, current file list). NM updates directory. SS can join dynamically later as well. ([karthikv1392.github.io][1])
3. Client starts: asks for username; sends CLIENT_HELLO to NM; commands now flow per routing table. ([karthikv1392.github.io][1])

## 12) Security and execution notes

- Treat EXEC as explicitly unsafe by design (per spec); still, run under a constrained working dir and propagate raw stdout/stderr back to client as required. Execution happens at NM, not at client or SS. ([karthikv1392.github.io][1])
- Tickets include expiry and op scoping (READ/WRITE/STREAM) to minimize capability leakage.

## 13) Code organization (suggested)

```
/nm
  nm_main.c
  nm_net.c           // TCP server, framing, req dispatch
  nm_registry.c      // SS registry, user sessions
  nm_dir.c           // file→SS map, LRU cache
  nm_acl.c
  nm_exec.c          // EXEC pipeline
  nm_persist.c       // nm_state.json
  nm_log.c
/ss
  ss_main.c
  ss_net_ctrl.c      // NM control channel
  ss_net_data.c      // client data channel (READ/WRITE/STREAM)
  ss_store.c         // FileData/FileMeta load/save, fsync, atomic rename
  ss_tokenize.c      // sentence/word split & compose
  ss_lock.c          // per-sentence locks
  ss_undo.c
  ss_log.c
/client
  cli_main.c
  cli_shell.c        // REPL parser, command encoder
  cli_nm.c           // NM control channel
  cli_ss.c           // SS data channel
  cli_render.c       // VIEW tables, INFO print
/common
  net_proto.h/.c     // length-prefixed JSON, error codes
  models.h/.c        // structs shared across components
  json.h/.c
```

## 14) Milestones

- M0 – Project bootstrap: repo, three binaries, Makefile, JSON framing helpers, config (NM host/port).
- M1 – NM & SS registration: NM accepts SS_REGISTER; shows connected SS list; Client CLIENT_HELLO with username.
- M2 – Persistence groundwork: NM loads/saves nm_state.json; SS has on-disk layout and clean startup/shutdown.
- M3 – Directory & search (NM): file→SS map + LRU; implement VIEW (-a, -l, -al) and LIST from NM. ([karthikv1392.github.io][1])
- M4 – READ flow: LOOKUP(READ) returns SS endpoint (+ticket); Client fetches file from SS. ([karthikv1392.github.io][1])
- M5 – CREATE / DELETE: NM selects SS, forwards op, updates mapping; errors for dup/missing. ([karthikv1392.github.io][1])
- M6 – WRITE + sentence locks: tokenization, per-sentence lock, multi-edit per session, atomic commit; index updates after each WRITE completion. ([karthikv1392.github.io][1])
- M7 – Access control: ACL model at NM; ADDACCESS -R/-W, REMACCESS; SS side enforcement via tickets. ([karthikv1392.github.io][1])
- M8 – UNDO (single level): per-file last-operation undo at SS (file-scoped). ([karthikv1392.github.io][1])
- M9 – STREAM: word-wise streaming with ~0.1 s delay; graceful failure if SS dies mid-stream. ([karthikv1392.github.io][1])
- M10 – INFO: file facts (owner, size, created/modified, last access, ACL). ([karthikv1392.github.io][1])
- M11 – EXEC: NM fetches body from SS, executes per line at NM, pipes output to client. ([karthikv1392.github.io][1])
- M12 – Logging & error model: unified codes, structured logs at NM/SS, human-readable client messages. ([karthikv1392.github.io][1])
- (Bonus) M13 – Folders: CREATEFOLDER/MOVE/VIEWFOLDER. ([karthikv1392.github.io][1])
- (Bonus) M14 – Checkpoints: CHECKPOINT/VIEWCHECKPOINT/REVERT/LISTCHECKPOINTS. ([karthikv1392.github.io][1])
- (Bonus) M15 – Replication & FT: async replicate writes; NM detects SS failure; re-attach on recovery. ([karthikv1392.github.io][1])

---

[1]: https://karthikv1392.github.io/cs3301_osn/course_project/ "Course Project | Operating Systems and Networks"
