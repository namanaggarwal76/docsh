# Running Docs++ on Multiple Devices

This guide explains how to run the Docs++ distributed file system across multiple machines on a network.

## Architecture Overview

The system consists of three types of components:
1. **Name Manager (NM)**: Central coordination server - should run on one machine
2. **Storage Servers (SS)**: File storage nodes - can run on multiple machines
3. **Clients**: User interfaces - can run on any machine

All components communicate via TCP/IP, with no hardcoded localhost addresses.

## Prerequisites

- All machines must be on the same network (or have network connectivity)
- Firewall rules must allow TCP connections on your chosen ports
- The NM machine's IP address must be reachable from all SS and client machines

## Step-by-Step Setup

### 1. Determine the NM IP Address

On the machine that will run the Name Manager, find its IP address:

```bash
# Linux/Mac
hostname -I | awk '{print $1}'
# or
ip addr show | grep "inet " | grep -v 127.0.0.1
```

Let's assume the NM machine has IP: **192.168.1.10**

### 2. Start the Name Manager

On the NM machine (192.168.1.10):

```bash
./bin/nm 5000
```

This starts the NM listening on port 5000 for both SS and client connections.

### 3. Start Storage Servers

Storage servers can run on any machine (including the NM machine or separate machines).

**On Machine A (could be anywhere on the network):**
```bash
./bin/ss 192.168.1.10 5000 6001 6002 1
```
- `192.168.1.10` - NM's IP address
- `5000` - NM's control port
- `6001` - SS control port (unused currently)
- `6002` - SS data port (what clients connect to)
- `1` - SS ID (must be unique)

**On Machine B (different machine):**
```bash
./bin/ss 192.168.1.10 5000 6003 6004 2
```
- `6003` - SS control port (different from SS #1)
- `6004` - SS data port (different from SS #1)
- `2` - SS ID (must be unique)

**Important:** Each SS registers its IP address automatically with the NM based on the TCP connection source address. No manual IP configuration needed!

### 4. Start Clients

Clients can run on any machine that can reach the NM.

**On Machine C (laptop, desktop, anywhere):**
```bash
./bin/client 192.168.1.10 5000
```

You'll be prompted for a username. After logging in, you can use all Docs++ commands.

## How It Works

1. **SS Registration**: When a storage server connects to the NM, the NM extracts its IP address from the TCP socket (using `getpeername()`). This IP is stored in the `ss_entry_t` structure.

2. **Client File Operations**: When a client wants to read/write a file:
   - Client sends a LOOKUP request to the NM
   - NM checks ACLs and determines which SS stores the file
   - NM responds with `{"status":"OK","ssAddr":"<SS_IP>","ssDataPort":<port>,"ticket":"..."}`
   - Client connects directly to the SS using the provided address and port

3. **Replication**: When the NM needs to replicate data between storage servers, it uses the stored `ss_addr` field to connect to the correct SS on the network.

## Example: Three Machines Setup

### Machine 1: Server (192.168.1.100)
Runs both NM and one SS:
```bash
# Terminal 1
./bin/nm 5000

# Terminal 2
./bin/ss 192.168.1.100 5000 6001 6002 1
```

### Machine 2: Storage Node (192.168.1.101)
Runs another SS:
```bash
./bin/ss 192.168.1.100 5000 6003 6004 2
```

### Machine 3: Client (192.168.1.102)
Runs the client:
```bash
./bin/client 192.168.1.100 5000
```

## Testing the Setup

After starting all components:

1. On the client, create a file:
   ```
   CREATE test.txt -r -w
   WRITE test.txt 0
   Hello from a distributed system.
   ETIRW
   ```

2. Read it back:
   ```
   READ test.txt
   ```

3. Check the info to see which SS stores it:
   ```
   INFO test.txt
   ```

## Troubleshooting

### "Failed to connect to NM"
- Verify the NM machine's IP address is correct
- Check that port 5000 (or your chosen port) is open on the NM machine
- Test connectivity: `telnet 192.168.1.10 5000`

### "ERROR: service unavailable (no storage server reachable)"
- Ensure at least one SS is running and registered
- Check SS logs to verify successful registration
- Wait a few seconds for heartbeats to establish SS as "UP"

### "Connection refused" when accessing files
- Verify the SS data ports (6002, 6004, etc.) are accessible
- Check firewall rules on SS machines
- Ensure SS machines can accept incoming connections on their data ports

### Clients can't reach SS directly
- The client needs direct network access to SS machines
- If using NAT/firewall, ensure port forwarding is set up
- Alternatively, run all components on machines within the same subnet

## Port Summary

| Component | Port Type | Default | Configurable |
|-----------|-----------|---------|--------------|
| NM | Control | 5000 | Yes (argument to `./bin/nm`) |
| SS | Control | 6001, 6003... | Yes (2nd port argument) |
| SS | Data | 6002, 6004... | Yes (3rd port argument) |
| Client | N/A | N/A | Connects to NM and SS dynamically |

## Security Notes

- This implementation does not use encryption (traffic is plaintext)
- Authentication is username-based (no passwords)
- Suitable for trusted networks only
- For production use, consider adding TLS/SSL and proper authentication
