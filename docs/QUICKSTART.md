# Docs++ Quickstart

## Build

```bash
make
```

Binaries are produced in `bin/`:

- `bin/nm` — Name Server
- `bin/ss` — Storage Server
- `bin/client` — Client

## Local smoke test

Open three terminals (or use background processes):

1) Start NM (control port 5000):

```bash
./bin/nm 5000
```

2) Start SS and register with NM (control port 6001, data port 6002, id 1):

```bash
./bin/ss 127.0.0.1 5000 6001 6002 1
```

Expected NM log: `Registered SS id=1 ctrl=6001 data=6002`.

3) Say hello from a client user:

```bash
./bin/client 127.0.0.1 5000 alice
```

Expected outputs:

- Client prints: `{ "status": "OK" }`
- NM logs: `Client hello from user=alice`

4) Add a file mapping (temporary debug until CREATE) and READ it:

```bash
./bin/client 127.0.0.1 5000 dir-set report.txt 1
echo "Hello world." > ss_data/files/report.txt
./bin/client 127.0.0.1 5000 read report.txt
```

Expected response includes the file content from SS as JSON: `{ "status":"OK", "body":"Hello world." }`.

## Notes

- No external JSON library is required yet; a tiny helper extracts simple fields for bootstrap.
- All network messages use a 4-byte big-endian length prefix, then UTF-8 JSON.
