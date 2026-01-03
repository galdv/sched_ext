# Plan: Dumper Thread with Safe Synchronization

## Implementation Status

| Step | Task                                      | Status      |
|------|-------------------------------------------|-------------|
| 1    | Plan and flowchart                        | DONE        |
| 2    | User approval                             | DONE        |
| 3    | BPF map structure in scx_scheduler.bpf.c  | DONE        |
| 4    | BPF stopping/dispatch logic               | DONE        |
| 5    | Dumper thread in scx_loader.c             | DONE        |
| 6    | Testing & Verification                    | DONE        |

---

## Goal

On every context switch:
1. **Dumper** writes (seq, tgid, tid) of stopped thread to `X.txt`
2. **process_tree** records (tgid, tid) to shared memory when threads run
3. On Ctrl+C, process_tree dumps shared memory to `Y.txt`
4. **Verify**: Every Y.txt entry should exist in X.txt (in order)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         KERNEL (BPF)                            │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │              scx_scheduler.bpf.c                         │   │
│  │  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐  │   │
│  │  │  stopping() │───>│ dumper_state│───>│  dispatch() │  │   │
│  │  │  - save tid │    │    map      │    │  - check    │  │   │
│  │  │  - pending=1│    │             │    │    pending  │  │   │
│  │  └─────────────┘    └─────────────┘    └─────────────┘  │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ BPF map read/write
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                       USERSPACE                                  │
│  ┌──────────────────┐              ┌──────────────────────────┐ │
│  │   scx_loader     │              │     process_tree         │ │
│  │  ┌────────────┐  │              │  ┌────────────────────┐  │ │
│  │  │  Dumper    │  │              │  │  21 worker threads │  │ │
│  │  │  Thread    │  │              │  │  (7 proc × 3 thr)  │  │ │
│  │  │            │  │              │  │                    │  │ │
│  │  │ - read map │  │              │  │ - acquire mutex    │  │ │
│  │  │ - write    │  │              │  │ - record to shmem  │  │ │
│  │  │   X.txt    │  │              │  │ - release mutex    │  │ │
│  │  │ - pending=0│  │              │  │ - usleep(10ms)     │  │ │
│  │  └────────────┘  │              │  └────────────────────┘  │ │
│  └──────────────────┘              │  On Ctrl+C: dump Y.txt   │ │
│          │                         └──────────────────────────┘ │
│          ▼                                    │                  │
│      X.txt                                Y.txt                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Data Structures

### BPF Map (Kernel)
```
+----------------------------------------+
|        BPF Map: "dumper_state_map"     |
+----------------------------------------+
|  last_tgid   : u32  (process ID)       |
|  last_tid    : u32  (thread ID)        |
|  dumper_tid  : u32  (dumper's TID)     |
|  seq         : u64  (context switch #) |
|  pending     : u32  (1=dumper must run)|
+----------------------------------------+
```

### Dispatch Queues (BPF)
```
+----------------------------------------+
|              Two DSQs                  |
+----------------------------------------+
|  SHARED_DSQ (0) - regular tasks        |
|  DUMPER_DSQ (1) - dumper thread only   |
+----------------------------------------+
```

### Shared Memory (process_tree)
```
+----------------------------------------+
|        shared_data_t (mmap)            |
+----------------------------------------+
|  mutex        : pthread_mutex_t        |
|  record_idx   : unsigned long          |
|  records[100000] : (pid, tid) pairs    |
+----------------------------------------+
```

---

## Flowchart: BPF Scheduler (Kernel)

```
                    +---------------------+
                    |  Thread A running   |
                    +----------+----------+
                               |
                               v
                    +---------------------+
                    |  Context switch     |
                    |  (timer/usleep)     |
                    +----------+----------+
                               |
                               v
                    +---------------------+
                    |  BPF: stopping(A)   |
                    +----------+----------+
                               |
                               v
                      +---------------+
                      | A == dumper?  |
                      +-------+-------+
                              |
               +--------------+--------------+
               | YES                         | NO
               v                             v
    +------------------+          +------------------+
    |  Skip            |          |  last_tgid = A   |
    |  (don't update)  |          |  last_tid = A    |
    +--------+---------+          |  seq++           |
             |                    |  pending = 1     |
             |                    +--------+---------+
             |                             |
             +-------------+---------------+
                           |
                           v
             +---------------------+
             |  BPF: dispatch()    |
             +----------+----------+
                        |
                        v
               +---------------+
               | pending == 1? |
               +-------+-------+
                       |
            +----------+----------+
            | YES                 | NO
            v                     v
 +--------------------+  +--------------------+
 | Consume ONLY from  |  | Try DUMPER_DSQ     |
 | DUMPER_DSQ         |  | then SHARED_DSQ    |
 | (others blocked)   |  |                    |
 +--------------------+  +--------------------+
```

---

## Flowchart: Dumper Thread (scx_loader.c)

```
     +---------------------------+
     |  Dumper thread starts     |
     |  Pin to CPU (sched_ext)   |
     |  Register TID in BPF map  |
     +-----------+---------------+
                 |
                 v
     +---------------------------+
     |  Open X.txt for append    |
     +-----------+---------------+
                 |
                 v
        +--------+--------+<-----------------+
        | Read BPF map:   |                  |
        | seq, tgid, tid  |                  |
        +--------+--------+                  |
                 |                           |
                 v                           |
         +---------------+                   |
         | seq changed?  |                   |
         +-------+-------+                   |
                 |                           |
        +--------+--------+                  |
        | NO              | YES              |
        v                 v                  |
  +------------+  +------------------+       |
  |sched_yield |  | Write to X.txt:  |       |
  | (wait)     |  | "seq tgid tid"   |       |
  +-----+------+  +--------+---------+       |
        |                  |                 |
        |                  v                 |
        |         +------------------+       |
        |         | pending = 0      |       |
        |         | (unblock others) |       |
        |         +--------+---------+       |
        |                  |                 |
        +------------------+-----------------+
                      (loop)
```

---

## Flowchart: Worker Thread (process_tree.c)

```
     +---------------------------+
     |  Worker thread starts     |
     |  Get my pid, tid          |
     +-----------+---------------+
                 |
                 v
        +--------+--------+<-----------------+
        | Acquire mutex   |                  |
        +--------+--------+                  |
                 |                           |
                 v                           |
        +------------------+                 |
        | Record to shmem: |                 |
        | records[idx++] = |                 |
        |   (pid, tid)     |                 |
        +--------+---------+                 |
                 |                           |
                 v                           |
        +------------------+                 |
        | Release mutex    |                 |
        +--------+---------+                 |
                 |                           |
                 v                           |
        +------------------+                 |
        | usleep(10ms)     |                 |
        | (natural context |                 |
        |  switch here)    |                 |
        +--------+---------+                 |
                 |                           |
                 v                           |
         +---------------+                   |
         | keep_running? |---YES-------------+
         +-------+-------+
                 | NO (Ctrl+C)
                 v
        +------------------+
        | Thread exits     |
        +------------------+
```

---

## Flowchart: Main Process Cleanup (process_tree.c)

```
        +---------------------------+
        |  Ctrl+C received          |
        |  shared->running = 0      |
        +-----------+---------------+
                    |
                    v
        +---------------------------+
        |  Wait for all children    |
        +-----------+---------------+
                    |
                    v
        +---------------------------+
        |  Open Y.txt for write     |
        +-----------+---------------+
                    |
                    v
        +---------------------------+
        |  For i = 0 to record_idx: |
        |    Write "i pid tid"      |
        +-----------+---------------+
                    |
                    v
        +---------------------------+
        |  Close Y.txt              |
        |  Cleanup & exit           |
        +---------------------------+
```

---

## Sequence Diagram: Full Flow

```
  Worker A       BPF Scheduler      Dumper         X.txt    Shared Mem
     |                |                |              |          |
     |--[acquire mutex]----------------|--------------|--------->|
     |--[record (pid,tid)]-------------|--------------|--------->|
     |--[release mutex]----------------|--------------|--------->|
     |--[usleep 10ms]                  |              |          |
     |                |                |              |          |
  [timer]             |                |              |          |
     |                |                |              |          |
     |---[stop]------>|                |              |          |
     |                |--[stopping()]--|              |          |
     |                |  pending=1     |              |          |
     |                |  save tid      |              |          |
     |                |                |              |          |
     |                |--[dispatch()]->|              |          |
     |                |  (dumper only) |              |          |
     |                |                |              |          |
     |                |                |--[write]--->|          |
     |                |                | seq,tgid,tid |          |
     |                |                |              |          |
     |                |                |--[pending=0] |          |
     |                |                |              |          |
     |                |<-[yield]-------|              |          |
     |                |                |              |          |
     |                |--[dispatch()]->|              |          |
     |                |  (any task)    |              |          |
     |                |                |              |          |
  Worker B <----------|                |              |          |
     |                |                |              |          |
```

---

## Verification Method

### Files Produced
- **X.txt**: `seq tgid tid` (from dumper, every context switch)
- **Y.txt**: `seq pid tid` (from process_tree, when threads record)

### Verification Script
```bash
# Strip sequence numbers, compare (tgid,tid) pairs
awk '{print $2,$3}' X.txt > /tmp/x.tmp
awk '{print $2,$3}' Y.txt > /tmp/y.tmp

# Check: Is every Y entry found in X in order?
# Result: Y should be a perfect subsequence of X
```

### Expected Results
| Metric | Expected |
|--------|----------|
| Y entries in X | 100% |
| Order preserved | Yes |
| X has more entries | Yes (startup + usleep switches) |

---

## Usage

```bash
# Terminal 1: Load scheduler with CPU pinning
sudo ./build/scx_loader -c 1

# Terminal 2: Run workload on SAME CPU
./build/scx_run taskset -c 1 ./build/process_tree

# Press Ctrl+C to stop and dump Y.txt
# Compare: X.txt vs Y.txt
```

### Options
- `scx_loader -c <cpu>` : CPU to pin dumper thread (required)
- `process_tree --display` : Enable visual tree display

---

## Race Protection Summary

| Race                         | Protection                              |
|------------------------------|-----------------------------------------|
| Dumper context-switched      | Skip if `tid == dumper_tid` in BPF      |
| Multiple threads recording   | Single mutex protects shared memory     |
| BPF <-> Userspace visibility | Sequence number check                   |
| Multi-core interference      | Single core only (taskset)              |

---

## Verification Results

```
Y entries: 100,000
X entries: 110,695

Y matched in X (in order): 100,000/100,000 (100%)
X extra (interleaved): 6
X trailing (after Y full): 10,689

✓ Every Y record appears in X in exact order
```
