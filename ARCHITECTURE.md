# ITCH Feed Handler — Architecture & Production Gap Analysis

> A high-performance market data feed handler for NASDAQ ITCH 5.0 over MoldUDP64.
> Designed for HFT and fintech infrastructure in Singapore (SGX, NASDAQ connectivity).

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Data Flow: End to End](#2-data-flow-end-to-end)
3. [Component Reference](#3-component-reference)
   - 3.1 [Network Layer](#31-network-layer)
   - 3.2 [Protocol Layer](#32-protocol-layer)
   - 3.3 [Routing Layer](#33-routing-layer)
   - 3.4 [Order Book Layer](#34-order-book-layer)
   - 3.5 [Recovery Layer](#35-recovery-layer)
   - 3.6 [Worker Threads](#36-worker-threads)
   - 3.7 [Monitoring Layer](#37-monitoring-layer)
   - 3.8 [Concurrency Primitives](#38-concurrency-primitives)
   - 3.9 [System & Common Utilities](#39-system--common-utilities)
   - 3.10 [Session Layer](#310-session-layer)
4. [Memory Budget](#4-memory-budget)
5. [Threading Model](#5-threading-model)
6. [Configuration Reference](#6-configuration-reference)
7. [Production Gap Analysis](#7-production-gap-analysis)

---

## 1. System Overview

This is a single-process, multi-threaded C++20 feed handler that:

- Receives raw UDP multicast packets from **two independent A/B feed lines** simultaneously
- Merges, deduplicates, and fails over between feeds via a dedicated `FeedMerger` thread
- Parses MoldUDP64 transport framing and all 22 ITCH message types
- Routes parsed messages to one of 8 shard workers via lock-free queues
- Maintains a full Level 2 order book per symbol (up to 65,535 symbols)
- Publishes best bid/offer (BBO) snapshots via wait-free SeqLock for downstream consumers
- Detects sequence gaps, requests retransmission from a configurable recovery server (SoupBinTCP login), and **re-injects recovered packets** back into the parse pipeline
- Detects MoldUDP64 session name changes and orchestrates a cross-subsystem reset (books, orders, sequences, symbols)
- Persists the last confirmed sequence number to disk for warm restart without false gap declarations
- Reports latency histograms and throughput statistics every N seconds

**Design philosophy:** Every decision on the hot path optimises for latency. Zero heap allocation after startup. Lock-free inter-thread communication. CPU-pinned threads with real-time scheduling.

---

## 2. Data Flow: End to End

```
 A-feed (primary)                  B-feed (secondary, optional)
 233.54.12.111:26477                233.54.12.112:26477
         │                                  │
         ▼  recvmsg() into PacketSlot        ▼  recvmsg() into PacketSlot
┌──────────────────────────┐   ┌──────────────────────────────┐
│  ReceiverThread (primary)│   │  ReceiverThread (secondary)  │
│  core 1, SCHED_FIFO 80   │   │  core 13, SCHED_FIFO 80      │
│  → primary_ring SPSC     │   │  → secondary_ring SPSC       │
└──────────────┬───────────┘   └──────────────┬───────────────┘
               │ slot index                    │ slot index
               └──────────┬────────────────────┘
                           ▼
               ┌──────────────────────────────────┐
               │  FeedMerger  (core 12, FIFO 77)  │
               │  1. Drain recovery_queue first   │
               │  2. Consume primary + secondary  │
               │  3. Dedup by MoldUDP64 seq (128  │
               │     slot bitmask window)         │
               │  4. Detect primary silence >5s   │
               │  Push MergedSlot → merged_queue  │
               └──────────────┬───────────────────┘
                               │ MergedSlot (4B: slot_idx + FeedSource)
                               ▼
┌──────────────────────────────────────────────────┐
│  ParserThread  (core 2, SCHED_FIFO prio 79)      │
│  Pop MergedSlot → resolve ring pointer           │
│  Read MoldUDP64 header → check sequence numbers  │
│  Parse ITCH messages → stack buffer (≤128 msgs)  │
│  Record parse latency histogram                  │
│  on_system_event() → SessionManager              │
│  gap_detector_.mark_recovered(seq) on advance    │
│  Enqueue seq → PersistentSequence SPSC           │
│  route(msg) or broadcast(msg) to shard queues    │
└────┬────────┬────────┬──────── ... ──────────────┘
     │ shard0 │ shard1 │ ... shard7
     ▼        ▼        ▼
┌──────────────────────────────────────────────────┐
│  ShardWorker[0..7]  (cores 3–10, SCHED_FIFO 78)  │
│  Pop ItchMsg from SPSC queue                     │
│  Dispatch to Level2Book (one per symbol/shard)   │
│  Pool alloc/free + hash map insert/erase         │
│  Update sorted price-level arrays                │
│  Publish BboSnapshot via SeqLock                 │
│  Record wire-to-book latency histogram           │
└────────────────┬─────────────────────────────────┘
                 │  wait-free SeqLock reads
                 ▼
    Downstream consumers (strategies, risk, OMS)

RecoveryThread  (core 11, SCHED_OTHER)
  └─ Polls GapDetector → TCP retransmit request (primary → secondary fallback)
  └─ Non-blocking recv → inject_recovered_packet() → primary_ring.recovery_queue
  └─ FeedMerger drains recovery_queue at highest priority before live packets
  └─ gap_detector_.mark_recovery_sent() prevents re-requesting the same gap
  └─ ParserThread calls mark_recovered(seq) on every in-order advance
  └─ Periodic book snapshots to disk

PersistentSequence  (background, no pinning)
  └─ Drains 2-slot SPSC → atomic write last_sequence.bin

MetricsReporter  (background, no pinning)
  └─ Prints latency/throughput/recovery/merger stats every N sec
```

**Key invariants:**
- Primary Receiver → FeedMerger: SPSC (`primary_ring.queue`), lock-free
- Secondary Receiver → FeedMerger: SPSC (`secondary_ring.queue`), lock-free
- RecoveryThread → FeedMerger: separate SPSC (`primary_ring.recovery_queue`, 1024 deep); FeedMerger is sole consumer
- FeedMerger → ParserThread: SPSC (`merged_queue`, 65536 deep); FeedMerger is sole producer
- Parser → each Shard: SPSC, lock-free
- Parser → PersistentSequence: 2-slot SPSC, non-blocking (drop-on-full is correct)
- ShardWorker → BboPublisher: single writer per locate code, wait-free readers
- SessionManager reset path uses existing SPSC broadcast — no new synchronisation primitive
- No locks, no mutexes, no condition variables on any hot path

---

## 3. Component Reference

### 3.1 Network Layer

**Files:** `src/network/socket_receiver.cpp`, `include/network/socket_receiver.hpp`,
`include/network/packet_ring.hpp`, `include/network/packet.hpp`, `include/network/udp_receiver.hpp`

#### SocketReceiver

Opens an `AF_INET SOCK_DGRAM` socket configured for:
- `SO_REUSEADDR` / `SO_REUSEPORT` — multiple processes can bind the same port
- `SO_RCVBUF = 8MB` — large kernel receive buffer to absorb burst traffic
- `O_NONBLOCK` — non-blocking mode; returns `EAGAIN` when queue is empty
- Multicast join via `IP_ADD_MEMBERSHIP` on the specified interface
- **Linux only:** `SO_TIMESTAMPING` with `SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE` for NIC-level hardware timestamps. Falls back to `clock_gettime(CLOCK_REALTIME)` on macOS or unsupported NICs.

`recv()` uses `recvmsg()` with a control message buffer. On Linux it extracts the hardware timestamp from `SCM_TIMESTAMPING`; on other platforms it timestamps immediately after the syscall.

#### PacketRingBuffer / ZeroCopyRing

```
PacketSlot layout (cache-line aligned, 1536 bytes):
  [data: uint8_t[1500]]         ← UDP payload written directly by recvmsg
  [length: uint16_t]
  [padding: 6 bytes]
  [recv_timestamp_ns: uint64_t]

PacketRingBuffer = PacketSlot[PACKET_RING_DEPTH=65536]   (~98 MB)
SlotIndexQueue   = SpscQueue<uint16_t, 65536>             (128 KB)
RecoverySlotQueue= SpscQueue<uint16_t, 1024>              (2 KB, within ZeroCopyRing)
```

`ZeroCopyRing` wraps one `PacketRingBuffer*` (heap/huge-page allocated externally) plus:
- **`SlotIndexQueue queue`** — live packets from `ReceiverThread` to `FeedMerger`
- **`RecoverySlotQueue recovery_queue`** + **`PacketSlot recovery_slots[1024]`** — recovered packets from `RecoveryThread` to `FeedMerger`
  - `inject_recovered_packet(data, len, ts, drop_ctr, recovered_ctr)`: capacity check → `memcpy` into next `recovery_slots[write_ & 1023]` → `try_push` to `recovery_queue`. Drop-on-full increments `FeedStats::recovery_drops`.

Only 2-byte slot indices travel through the live SPSC queue. `MergedSlot` tokens (4 bytes) travel the `merged_queue` to `ParserThread`; the actual 1500-byte data never copies.

**Merge types (in `packet_ring.hpp`):**
```cpp
enum class FeedSource : uint8_t { Primary = 0, Secondary = 1, Recovery = 2 };
struct MergedSlot { uint16_t slot_idx; FeedSource source; uint8_t pad; }; // 4 bytes
using MergedSlotQueue = SpscQueue<MergedSlot, MERGED_QUEUE_DEPTH=65536>;
```

`ParserThread::resolve(ms)` dispatches on `ms.source` to `primary_ring_.slot()`, `secondary_ring_.slot()`, or `primary_ring_.recovery_slot()` — branch-free for the common `Primary` case.

**Allocation:**
- Linux: `mmap(MAP_HUGETLB)` for 2MB huge pages, falls back to `mmap + MADV_HUGEPAGE`
- macOS: `std::aligned_alloc(64, size)`
- Secondary ring buffer allocated only when `multicast_group_secondary` is set

Total live ring size: ~98MB per ring (65536 × 1536 bytes).

#### FeedMerger

**File:** `include/workers/feed_merger.hpp`, `src/workers/feed_merger.cpp`

Sole producer of `MergedSlotQueue`, consuming from `primary_ring`, `secondary_ring`, and `primary_ring.recovery_queue`.

**`DedupWindow`:** 16-byte sliding bitmask (two `uint64_t` words; width = `DEDUP_WINDOW_SIZE = 128`). `check_and_mark(seq)` is branch-free in the steady state (no window advance needed — A/B feeds are microseconds apart, well within 128 sequences). The window advances one 64-sequence block at a time on overflow.

**Hot-path loop (no allocation, no locking):**
1. `primary_ring_.consume_recovered(rec_idx)` — drain recovery packets at highest priority; `continue` until empty to ensure gaps heal without delay
2. `primary_ring_.consume(pidx)` + (if secondary enabled) `secondary_ring_.consume(sidx)`
3. `handle_live_slot()` per slot received:
   - Update `primary_last_ts_` on primary packets; clear `failed_over_` flag if primary returns
   - Increment `secondary_packets` counter for secondary slots
   - Peek MoldUDP64 sequence at byte offset 10 via `__builtin_memcpy` + `__builtin_bswap64`
   - Skip dedup for heartbeats (`msg_count == 0xFFFF`) and short headers (< 20 bytes)
   - `dedup_.check_and_mark(seq)` — drop duplicate, increment `dedup_drops`; else forward
4. Failover check every `FAILOVER_CHECK_INTERVAL = 4096` slots (amortises `steady_ns()` call):
   - If `now - primary_last_ts_ > failover_timeout_ns`: set `failed_over_`, increment `feed_failover_count`, log WARN
   - While failed over: re-log every 1s

**Startup/shutdown order:** FeedMerger starts after shard workers and parser, before secondary and primary receivers. Stops after both receivers, before parser.

---

### 3.2 Protocol Layer

**Files:** `src/protocol/itch_parser.cpp`, `src/protocol/moldudp64_parser.cpp`,
`src/protocol/sequence_tracker.cpp`, `include/protocol/itch_messages.hpp`,
`include/protocol/moldudp64.hpp`, `include/protocol/moldudp64_parser.hpp`,
`include/protocol/sequence_tracker.hpp`

#### MoldUDP64

Transport framing for ITCH. Wire format:

```
Offset  Size  Field
0       10    Session identifier (ASCII)
10      8     Sequence number (big-endian uint64)
18      2     Message count (big-endian uint16; 0xFFFF = heartbeat)
20+     var   Message blocks: [length:2 BE][payload:length]...
```

`MoldUDP64Parser::parse()` validates minimum length, detects heartbeats, and returns a `ParseResult` pointing into the original buffer (no copy).

#### SequenceTracker

Tracks the expected next sequence number per session.

| Received seq | Action |
|---|---|
| `expected` | Accept, advance expected |
| `> expected` | Gap detected; store range + timestamp; return false |
| `<= current` | Duplicate/reorder; increment counter; return false |

Counters are atomics; the tracker is written only by ParserThread (no CAS loops needed on the write side).

#### ITCH 5.0 Parser

Handles all 21 ITCH message types. Wire layout for all messages:

```
[type:1][locate:2 BE][tracking:2 BE][timestamp:6 BE][message-specific...]
```

Messages parsed into `ItchMsg` (a union, trivially copyable, ~96 bytes):

| Type | Code | Book Action |
|---|---|---|
| System Event | `S` | Informational |
| Stock Directory | `R` | Create book entry at SOD |
| Stock Trading Action | `H` | Update trading status |
| RegSHO | `Y` | Short-sale restriction flag |
| Market Participant | `L` | Informational (discarded) |
| MWCB Decline/Status | `V`/`W` | Circuit breaker (discarded) |
| IPO Quoting Period | `K` | Informational |
| LULD Auction Collar | `J` | Informational |
| Operational Halt | `h` | Update halt status |
| **Add Order** | `A` | Pool alloc + map insert + level update |
| **Add Order MPID** | `F` | Same + attribution |
| **Order Executed** | `E` | Reduce qty (remove if 0) |
| **Order Executed w/ Price** | `C` | Same + execution price |
| **Order Cancel** | `X` | Reduce qty (remove if 0) |
| **Order Delete** | `D` | Full removal |
| **Order Replace** | `U` | Delete + re-add at new price/qty |
| Trade | `P` | Non-order trade (informational) |
| Cross Trade | `Q` | Auction cross (informational) |
| Broken Trade | `B` | Execution reversal (informational) |
| NOII | `I` | Net order imbalance |
| RPI Indicator | `N` | Retail price improvement flag |

**Price encoding:** 32-bit fixed-point multiplied by 10,000. E.g., $12.3456 → `123456`. Conversion from 4-byte big-endian wire integer is a single `__builtin_bswap32`.

**Timestamp encoding:** 6-byte big-endian nanoseconds since midnight. Parsed to `uint64_t` via byte-by-byte shifts.

**Hot-path allocation:** Zero. All 128 parsed messages per packet are written into a stack-allocated `ItchMsg[128]` buffer.

---

### 3.3 Routing Layer

**Files:** `src/routing/shard_router.cpp`, `src/routing/symbol_directory.cpp`,
`include/routing/shard_router.hpp`, `include/routing/symbol_directory.hpp`

#### SymbolDirectory

Direct-indexed array `SymbolInfo[MAX_LOCATE=65536]` (~2MB). Populated by `StockDirectory` messages at start-of-day. Provides O(1) lookup by locate code and O(1) shard assignment (`locate % NUM_SHARDS`).

```cpp
struct SymbolInfo {
    char     ticker[9];          // null-terminated 8-char stock field
    LocateCode locate;
    ShardId    shard;            // locate % NUM_SHARDS
    uint8_t    market_category;
    uint32_t   round_lot_size;
    uint8_t    financial_status;
    bool       is_active;
};
```

Written once at SOD, read-only during session. Fits in L3 cache.

#### ShardRouter

Routes `ItchMsg` to one of 8 `ShardQueue` instances:

- **Unicast** (order messages): `locate % NUM_SHARDS` → single queue push
- **Broadcast** (admin messages): fan-out to all 8 queues

Admin message types that are broadcast: `StockDirectory`, `SystemEvent`, `StockTradingAction`, `RegSHO`, `MWCBDecline`, `MWCBStatus`, `OperationalHalt`.

If a shard queue is full (262,144 entries exhausted), the message is **dropped** with a warning log.

---

### 3.4 Order Book Layer

**Files:** `src/orderbook/order_pool.cpp`, `src/orderbook/order_map.cpp`,
`src/orderbook/price_level.cpp`, `src/orderbook/level2_book.cpp`,
`src/orderbook/bbo_publisher.cpp`, and corresponding headers.

#### Order (32 bytes, 32-byte aligned)

```cpp
struct alignas(32) Order {
    OrderId  id;           // 8 — ITCH order reference number
    Price    price;        // 8 — fixed-point × 10000
    Qty      qty;          // 4 — current remaining quantity
    uint32_t prev_idx;     // 4 — pool index of previous order in level
    uint32_t next_idx;     // 4 — pool index of next order in level
    uint16_t level_idx;    // 2 — index into bid_levels_[] or ask_levels_[]
    Side     side;         // 1 — 'B' or 'S'
    uint8_t  pad;          // 1
};
// 32 bytes = 2 orders per cache line
```

#### OrderPool (slab allocator)

Pre-allocated array of `MAX_ORDERS_PER_SHARD = 2,000,000` orders, backed by NUMA-local huge-page memory.

- `alloc()`: Pop index from LIFO freelist — O(1), no contention
- `free(idx)`: Push index onto freelist — O(1), no contention
- `index_of(ptr)`: Pointer arithmetic — O(1)
- `at(idx)`: Direct array index — O(1)

The freelist is stored immediately after the order array in the same allocation. LIFO order maximises cache reuse for temporally adjacent alloc/free pairs.

Memory: `2M × (32 + 4) = 72MB` per shard × 8 = **576MB total**.

#### OrderMap (Robin Hood hash table)

Maps `OrderId (uint64_t)` → pool index `(uint32_t)`.

```
Capacity: 4,194,304 (2^22, power-of-2 mandatory)
Load factor target: ~50% (2M active orders per shard)
Entry size: 16 bytes (key:8 + value:4 + probe_dist:2 + pad:2)
Total: 4M × 16 = 64MB per shard × 8 = 512MB total
```

**Fibonacci hashing:** `(id × 11400714819323198485ULL) >> (64 − log2(capacity))` — distributes better than modulo for sequential order IDs.

**Robin Hood invariant:** On insert, if the existing entry has a shorter probe sequence than the inserting entry, they swap. This keeps average probe length < 2 even at 50% load and allows tombstone-free deletion (backward shift).

#### PriceLevel (64 bytes = one cache line)

```cpp
struct PriceLevel {
    Price    price;             // 8
    Qty      total_qty;         // 4 — sum of all orders at this price
    uint32_t order_count;       // 4
    uint32_t head_order_idx;    // 4 — FIFO list head (pool index)
    uint32_t tail_order_idx;    // 4 — FIFO list tail (pool index)
    uint32_t pad[10];           // 40 — fills cache line
};
```

Orders at the same price form a doubly-linked FIFO list through the pool (time priority). Partial executions and cancellations consume from the head.

#### Level2Book

Maintains two sorted arrays of `PriceLevel`:
- `bid_levels_[256]` — descending order (best bid at index 0)
- `ask_levels_[256]` — ascending order (best ask at index 0)

Operations:
- **AddOrder/AddOrderMPID:** `pool.alloc()`, find-or-insert price level (linear scan), link at tail, `map.insert()`
- **OrderExecuted / OrderCancel:** `map.find()`, reduce qty, remove order if qty=0, remove level if empty
- **OrderDelete:** `map.find()`, unlink from level, `pool.free()`, `map.erase()`
- **OrderReplace:** Per NASDAQ ITCH 5.0 §4.6.2 time-priority rule — two cases:
  - **Case A** (in-place, time priority preserved): `new_price == old_price && new_shares ≤ old_shares` → qty updated in-place; order keeps its FIFO position. Increment `order_replaces_inplace` counter.
  - **Case B** (new time priority): price changed OR `new_shares > old_shares` → unlink old order, free pool slot, allocate new Order at `new_price`, append to tail of target level. Increasing qty is equivalent to a cancel + re-submit; the exchange sends the firm to the back of the queue.
- **StockTradingAction:** Update `status_` (Halted/Paused/Quotation/Trading)

After each book-affecting operation, `get_bbo()` extracts `bid_levels_[0]` and `ask_levels_[0]` into a `BboSnapshot`.

**Supports up to 256 price levels per side per symbol.** Levels beyond this are dropped with a warning.

#### BboPublisher

Array of `SeqLock<BboSnapshot>` indexed by locate code (up to 65,536 entries).

```
BboSnapshot (64 bytes, trivially copyable):
  locate:     uint16_t
  bid_price:  int64_t
  bid_qty:    uint32_t
  ask_price:  int64_t
  ask_qty:    uint32_t
  ts:         uint64_t   ← ITCH message timestamp
```

- `publish(loc, bbo)`: Single writer per locate; SeqLock store (atomic odd/even sequence + memcpy)
- `read(loc)`: Multi-reader, wait-free; spins only if writer is active (microseconds at worst)

Total memory: 65536 × 128 bytes = **8MB** (fits in L3 on modern CPUs).

#### OrderBookPool

**Files:** `include/orderbook/order_book_pool.hpp`, `src/orderbook/order_book_pool.cpp`

Pre-allocates all `Level2Book` objects for one shard into a single NUMA-local huge-page slab at startup, eliminating `new Level2Book(...)` on the SOD StockDirectory burst path.

```
BOOKS_PER_SHARD = MAX_LOCATE / NUM_SHARDS = 65536 / 8 = 8192

Slab index formula (O(1), branch-free):
  book(loc) = &slab_[loc / NUM_SHARDS]

Inverse (used in constructor placement-new loop):
  loc = slab_index * NUM_SHARDS + shard_id

Memory per shard: 8192 × sizeof(Level2Book) ≈ 257 MiB (NUMA-local huge-page)
```

- Constructor: `CpuAffinity::alloc_on_numa(BOOKS_PER_SHARD × sizeof(Level2Book), numa_node)` then placement-new loop with correct locate codes
- `book(loc)`: `ITCH_FORCE_INLINE`, returns `&slab_[loc / NUM_SHARDS]` — no allocation, no branch
- Destructor: `CpuAffinity::free_numa()` only — no explicit `Level2Book` dtors (trivially destructible)
- `ShardWorker` holds `bool book_active_[BOOKS_PER_SHARD]` to track which slab slots have received a `StockDirectory`; all order processing is gated on `book_active_[loc/NUM_SHARDS]`

---

### 3.5 Recovery Layer

**Files:** `src/recovery/gap_detector.cpp`, `src/recovery/retransmit_client.cpp`,
`src/recovery/book_snapshot.cpp`, `src/recovery/persistent_sequence.cpp`, and headers.

#### GapDetector

Holds one pending `GapRange` (start_seq, end_seq, detected_at_ns, recovery_sent). Called by ParserThread on gap detection.

- `on_gap(info)`: Store gap range, record detection timestamp
- `should_request_recovery(now_ns)`: Return true if gap pending AND `now_ns − detected_at_ns > gap_timeout_ms × 1e6`
- `mark_recovered(up_to_seq)`: Clear pending gap when retransmission is complete

Gap timeout (500ms default) prevents retransmit request storms during burst packet loss.

#### RetransmitClient

TCP client for MoldUDP64 gap recovery. Supports primary + optional secondary server with automatic fallback, and pluggable venue-specific login.

```cpp
struct RetransmitCredentials {
    std::string username;
    std::string password;
    bool        require_login{false};
};
```

**Login abstraction:**
- `LoginHandler` — abstract base with `virtual bool login(fd, username, password, session_10)`. Swap implementations via `set_login_handler()` without rebuilding RetransmitClient.
- `NasdaqLoginHandler` — implements SoupBinTCP protocol (NASDAQ ITCH retransmit servers):
  - Sends 64-byte Login Request packet: `[len:2 BE]['L'][user:6][pass:10][session:10][seq_ascii:20][reserved:15]`
  - Reads response: `'A'` (Accepted) or `'J'` (Rejected — logs reason then `exit(1)`)
  - Sets `SO_RCVTIMEO=5s` during login phase; cleared before `fcntl(O_NONBLOCK)`

**Connection flow:**
1. TCP connect to primary; on failure try secondary (if configured)
2. If `require_login`: call `login_handler_->login()` — FATAL exit on rejection
3. Switch socket to non-blocking for steady-state `recv()`

**Key methods:**
- `connect()`: primary → secondary fallback
- `request(session_10, start_seq, count)`: 20-byte MoldUDP64 retransmit request
- `recv(pkt)`: Non-blocking; returns bytes read or ≤0
- `raw_fd()`: Exposes socket fd for future TLS integration (no API change needed)

#### BookSnapshot

Serialises/deserialises `Level2Book` state to binary files for fast restart recovery.

**Configuration:** constructor takes `directory` + `filename_template`; template supports two substitution tokens:
- `{session}` — 10-byte MoldUDP64 session field with trailing ASCII spaces stripped
- `{timestamp}` — `steady_clock::now().time_since_epoch().count()` (monotonic nanoseconds, unique per run)

**Write path (atomic):**
1. Expand template → `<final_path>`
2. Write binary to `<final_path>.tmp`
3. `fsync` + `close`
4. `rename(.tmp → final_path)` — readers never see partial writes
5. Update `latest.ptr` sidecar via own tmp→rename cycle

**Load path:**
1. Read `latest.ptr` → get snapshot filename
2. Open and deserialise

**Binary format:**
```
uint32_t magic  = 0x49544348  ('ITCH')
size_t   count  — number of books
for each book:
  uint32_t len  — serialised byte length
  uint8_t  buf[len]
```

Directory is created via `std::filesystem::create_directories()` at construction (mode 0755).

#### PersistentSequence

Persists the last confirmed MoldUDP64 sequence number + session to disk for warm restart. On restart, `FeedHandler::init()` calls `load()` and seeds `SequenceTracker` before workers start — preventing false gap declarations on the first in-order packet.

**On-disk record (24 bytes, trivially copyable):**
```
char     magic[4]    = "ITCS"
char     session[10] — raw MoldUDP64 session, not null-terminated
uint16_t version     = 1
uint64_t seq         — last confirmed sequence (host byte order)
```

**Hot path — `enqueue(seq, session_10)` (`ITCH_FORCE_INLINE`):**
- `memcpy(10)` + `try_push()` on a 2-slot `SpscQueue<SeqRecord, 2>`
- Drop-on-full is correct: the background writer already holds a newer pending save
- ~6 instructions; zero allocation; non-blocking

**Background writer thread (`SCHED_OTHER`, unpinned):**
- Pop from queue → `save_sync()` → `sleep_for(1ms)` when idle
- `save_sync()`: `open(.tmp, O_CREAT|O_TRUNC)` → `write(24B)` → `fsync` → `close` → `rename(.tmp → last_sequence.bin)`

**Lifecycle:**
- `start()`: launches writer thread
- `stop()`: drain queue + one final `save_sync()` → join (called after parser stops, before process exits)
- `load()`: `open(O_RDONLY)` → validate magic → return `LoadedSeq{seq, session}`; ENOENT=INFO (first run); read error=WARN

File path: `<last_sequence_dir>/last_sequence.bin` (configurable, default `/var/lib/itch`).

---

### 3.6 Worker Threads

**Files:** `src/workers/receiver_thread.cpp`, `src/workers/feed_merger.cpp`,
`src/workers/parser_thread.cpp`, `src/workers/shard_worker.cpp`,
`src/workers/recovery_thread.cpp`, and headers.

#### Startup / Shutdown Order

Workers are started in **reverse data-flow order** (consumers before producers) and stopped in **forward data-flow order** (producers before consumers). This prevents queue overflow on startup and ensures in-flight messages drain cleanly on shutdown.

```
Start:  metrics → recovery → persist_seq → shard[0..7] → parser → feed_merger → secondary_receiver → primary_receiver
Stop:   primary_receiver → secondary_receiver → feed_merger → parser → persist_seq → shard[0..7] → recovery → metrics
```

#### ReceiverThread (primary: core 1 SCHED_FIFO 80; secondary: core 13 SCHED_FIFO 80)

Two independent instances — one per feed line. Tight spin loop:
1. Claim next write slot in the respective ring (`primary_ring_` or `secondary_ring_`)
2. `SocketReceiver::recv()` directly into slot (`recvmsg` with hardware timestamp)
3. If data received: push slot index to ring's SPSC queue; on ring-full: drop + increment `packets_dropped`
4. If `EAGAIN`: spin and retry

No allocation. No syscall beyond `recvmsg`. Secondary receiver is only created and started when `multicast_group_secondary` is configured.

#### FeedMerger (core 12, SCHED_FIFO prio 77)

See §3.1. Sits between both receivers and `ParserThread`. The sole producer of `merged_queue`.

#### ParserThread (core 2, SCHED_FIFO prio 79)

Tight spin loop:
1. `merged_queue_.try_pop(ms)` — pop `MergedSlot` (4 bytes) from FeedMerger
2. `resolve(ms)` — dispatch on `ms.source` to get `const PacketSlot*` (no copy)
3. `MoldUDP64Parser::parse()` — validate header, check heartbeat
4. `seq_tracker_.try_advance(seq)` — check sequence; on gap: notify `GapDetector`
   - If in-order: `persist_seq_.enqueue(seq, session)` — non-blocking SPSC push
   - If in-order: `gap_detector_.mark_recovered(seq)` — clears pending gap once sequence advances past `gap_.end_seq` (no-op when no gap pending)
5. `ItchParser::parse()` — parse up to 128 messages into stack buffer; record parse latency
6. For each `SystemEvent`: `session_mgr_.on_system_event(msg, mold_session, seq)`
7. For each `StockDirectory`: `session_mgr_.try_resolve_watched(locate, ticker8)` — set warmup bit before routing
8. For each message: `router_.route(msg)` or `router_.broadcast(msg)`

Stack buffer `ItchMsg[128]` means zero allocation per packet.

#### ShardWorker[0..7] (cores 3–10, SCHED_FIFO prio 78)

Tight spin loop:
1. `queue_.try_pop(msg)` — pop `ItchMsg` from shard SPSC queue
2. `dispatch(msg)`:
   - `InternalSessionReset` → `clear_all_books()` + `pool_.reset()` + `map_.reset()` (session boundary)
   - `StockDirectory` → `activate_book(locate)` if `session_mgr_.is_open()` (cold path, SOD only; O(1) slab lookup, zero allocation)
   - `StockTradingAction` → `book->on_trading_action(msg)` if `book_active_[loc/NUM_SHARDS]`
   - Order messages → `book->on_*` if `book_active_[loc/NUM_SHARDS]`; else discard
3. `bbo_pub_.publish(locate, book->get_bbo())` after any book-affecting message
4. `book_lat_.record(steady_ns() − msg.recv_timestamp)` — wire-to-book latency

Each shard holds a reference to its `OrderBookPool`. All 8,192 `Level2Book` objects are pre-allocated in a NUMA-local huge-page slab at startup. `book_active_[loc/NUM_SHARDS]` is the sole gate for all per-book dispatch; unregistered locate codes are silently discarded.

#### RecoveryThread (core 11, SCHED_OTHER)

Background loop (not latency-critical):
1. `handle_recovery()`: poll `GapDetector::should_request_recovery(now)`. If true:
   - TCP connect primary → secondary fallback if not already connected
   - `retransmit_client_.request(session_, gap.start_seq, count)` — 20-byte MoldUDP64 retransmit request (capped at 65535 sequences per request)
   - `gap_detector_.mark_recovery_sent()` — sets `gap_.recovery_sent = true` so `should_request_recovery()` returns false until a new gap is detected; prevents re-requesting the same gap every 10ms poll cycle
2. `recv_retransmitted()`: drain up to 64 packets per poll cycle from recovery TCP socket:
   - `retransmit_client_.recv(pkt)` (non-blocking)
   - `inject_ring_.inject_recovered_packet(pkt.data, pkt.length, pkt.recv_ts, stats_.recovery_drops, stats_.recovered_packets)`
   - FeedMerger drains `primary_ring_.recovery_queue` at the top of its loop (before live packets), so recovered packets are parsed ahead of new market data
   - `GapDetector::mark_recovered(seq)` is called by **ParserThread** on every in-order sequence advance; it clears `pending_` once `seq >= gap_.end_seq`
3. If `snapshot_interval_sec` elapsed: log snapshot trigger (actual save is driven by EOD callback)
4. `sleep(10ms)` — intentional yield to avoid starving other background tasks

#### MetricsReporter (no pinning, SCHED_OTHER)

Background loop:
1. `sleep(report_interval_sec)`
2. Print latency histograms (parse latency, wire-to-book latency per shard)
3. Print throughput counters (packets received, messages parsed, gaps, duplicates)

---

### 3.7 Monitoring Layer

**Files:** `src/monitoring/latency_histogram.cpp`, `src/monitoring/metrics_reporter.cpp`,
and headers.

#### LatencyHistogram

512-bucket histogram occupying 4KB (fits in L1 data cache):

- Buckets 0–255: linear, 1 nanosecond per bucket (0–255ns range)
- Buckets 256–511: logarithmic (16 sub-buckets per octave, covering ~256ns to ~100ms)

`record(latency_ns)`: Single `std::atomic<uint64_t>::fetch_add(1, relaxed)`. No locks.

Percentile queries (`p50`, `p95`, `p99`, `p999`): Linear scan, called only from reporter thread — not on hot path.

Two histograms tracked:
- `parse_lat_`: Packet receive timestamp → parser completion
- `book_lat_` (per shard): Packet receive timestamp → BBO published

#### FeedStats

Cache-line–isolated atomic counter groups (each `alignas(64)`):

| Counter group | Writers | Description |
|---|---|---|
| `packets_received`, `packets_dropped` | ReceiverThread | Ring-level packet accounting |
| `messages_parsed`, `sequence_gaps`, `sequence_dups` | ParserThread | Parse and sequence accounting |
| `recovered_packets`, `recovery_drops` | RecoveryThread | Recovery injection accounting |
| `shard_queue_drops[NUM_SHARDS]` | ParserThread (one counter) | Per-shard queue-full drops |
| `price_level_drops[NUM_SHARDS]` | ShardWorker[s] (one per shard) | Per-shard price level overflow drops |
| `order_replaces_inplace[NUM_SHARDS]` | ShardWorker[s] (one per shard) | OrderReplace Case A (time priority preserved) |
| `secondary_packets`, `dedup_drops`, `feed_failover_count`, `merged_drops` | FeedMerger | A/B feed merge accounting |
| `msg_type_count[256]` | ParserThread | Per ITCH message type counts |

`shard_queue_drops` has a single writer (ParserThread), so all 8 elements intentionally share one cache line. All other per-shard groups are written by different threads and live on separate `alignas(64)` lines to prevent false sharing.

---

### 3.8 Concurrency Primitives

**Files:** `include/concurrency/seqlock.hpp`, `include/concurrency/spsc_queue.hpp`,
`include/concurrency/cacheline.hpp`

#### SeqLock\<T\>

Wait-free multi-reader, exclusive single-writer synchronisation primitive.

```
Writer protocol:
  1. seq.fetch_add(1, acq_rel)   → seq is now ODD (writer active)
                                    acq_rel prevents data writes reordering
                                    before the increment on ARM64 (TSO on x86 is free)
  2. data = value                 (non-atomic struct copy)
  3. seq.store(seq+2, release)   → seq is now EVEN+2 (complete)

Reader protocol:
  1. seq1 = seq.load(acquire); if ODD: spin
  2. value = data                 (non-atomic struct copy)
  3. atomic_thread_fence(acquire) ← ARM64: dmb ish — prevents the plain loads
                                    of data from being reordered after seq2 load
                                    by the out-of-order CPU (x86: no-op)
  4. seq2 = seq.load(relaxed); if seq1 != seq2: retry
  5. return value
```

**ARM64 correctness note:** Without `fetch_add(acq_rel)` on the writer side, the CPU can reorder the `data = value` store before the `fetch_add`, making a torn write invisible to the consistency check. Without the `atomic_thread_fence(acquire)` on the reader side, the CPU can speculatively advance the `seq2` load before the data loads, making a torn read invisible. Both fences are no-ops on x86 (TSO model) — zero overhead on the primary deployment target.

Guarantees no torn reads. Reader spin rate is bounded by writer frequency (book updates arrive at ~50K/s per symbol — readers rarely spin).

#### SpscQueue\<T, Capacity\>

Lock-free single-producer single-consumer FIFO. Capacity must be a power of 2.

```
Memory layout (each region on its own cache line):
  [write_pos atomic]        ← written by producer, read by consumer
  [cached_read_pos]         ← producer's stale view of read_pos (avoids read_pos cache line bouncing)
  [buffer[Capacity]]
  [cached_write_pos]        ← consumer's stale view of write_pos
  [read_pos atomic]         ← written by consumer, read by producer
```

The cached positions reduce cache-line invalidation traffic: the producer only reloads `read_pos` when its cached copy suggests the queue is full (and vice versa). In steady-state operation most pushes/pops read only local cache-hot data.

`try_push` / `try_pop` return `false` immediately when full/empty (no blocking, no spinning).

#### CacheAligned\<T\>

Wrapper that pads `T` to a 64-byte cache line boundary, preventing false sharing between independently updated fields.

---

### 3.9 System & Common Utilities

#### CpuAffinity

- `pin_current_thread(cpu_id)`: Linux: `pthread_setaffinity_np`. macOS: `thread_policy_set` (hint only).
- `set_realtime(priority)`: Linux: `sched_setscheduler(SCHED_FIFO)` — requires `CAP_SYS_NICE`.
- `alloc_on_numa(size, node)`: Linux: `mmap + mbind` or libnuma `numa_alloc_onnode`. macOS: `std::aligned_alloc`.

---

### 3.10 Session Layer

**Files:** `include/session/session_manager.hpp`, `src/session/session_manager.cpp`

#### SessionManager

Detects MoldUDP64 session name changes and orchestrates a full cross-subsystem reset. Provides a lock-free gate (`is_open()`) for `StockDirectory` processing by ShardWorkers.

**Session state machine:**

```
Unknown ──('S' SystemEvent)──► PreOpen
                          ──('O' or 'C')──► Open
                          ──('Q','M','E')──► Closed
```

| Event code | New state |
|---|---|
| `'S'` (Start-of-Messages) | PreOpen |
| `'O'` (Market Open) / `'C'` (Market Close) | Open |
| `'Q'` (End-of-Messages) / `'M'` (Emergency Halt) / `'E'` (End-of-Trade) | Closed |

**Key design choices:**
- Session name: `char session_[11]` (10-byte wire field + null) — zero heap allocation
- State: `std::atomic<uint8_t>` — `is_open()` uses `memory_order_relaxed`, safe for lock-free read by ShardWorkers
- Auto-detect mode: `initial_session = "AUTO_DETECT"` — populated from the first received MoldUDP64 session field (no pre-configuration needed)

**`on_system_event(msg, raw_session_10, mold_seq)`** — called by ParserThread on every `SystemEvent`:
1. `memcmp(raw_session_10, session_, 10)` — if session name changed and new name is non-zero: call `reset_session(new_session_10)`
2. `event_code_to_state(msg.system_event.event_code)` → update `state_`

**`reset_session(new_session_10)`** — cold path, triggered at session boundary:
1. `memcpy(session_, new_session_10, 10)` — update stored session name
2. `seq_tracker_.reset(0)` — clear expected sequence
3. `sym_dir_.reset()` — mark all symbols inactive (repopulated from new `StockDirectory` burst)
4. `bbo_pub_.reset()` — zero all SeqLock BBO slots
5. `router_.broadcast(InternalSessionReset_msg)` — fan-out sentinel to all 8 shard SPSC queues
6. `state_.store(Unknown, relaxed)` — will be re-driven by incoming SystemEvents

The `InternalSessionReset` sentinel (`MessageType = '\x01'`) is an internal message type that never appears on the wire. ShardWorkers handle it by calling `clear_all_books()` + `pool_.reset()` + `map_.reset()`, then returning immediately. No new synchronisation primitive is needed — the existing SPSC broadcast mechanism is reused.

#### Timestamps

```
steady_ns()  — CLOCK_MONOTONIC; used for durations, timeouts, latency measurements
now_ns()     — wall clock, platform-optimised:
  x86_64     : calibrated RDTSC (CPUID leaf 0x15; fallback: 10ms calibration window)
  ARM64 macOS: mach_absolute_time() × timebase ratio
  Fallback   : clock_gettime(CLOCK_REALTIME)
```

#### Core Types and Constants

```
OrderId   = uint64_t         OrderMap capacity    = 4,194,304 (2^22)
Price     = int64_t          Shard queue depth    = 262,144   (2^18)
Qty       = uint32_t         Packet ring depth    = 65,536    (2^16)
LocateCode= uint16_t         Recovery ring depth  = 1,024     (2^10)
ShardId   = uint8_t          Merged queue depth   = 65,536    (2^16)
Timestamp = uint64_t         Dedup window size    = 128 sequences
NUM_SHARDS= 8                Max symbols          = 65,536
MAX_PRICE_LEVELS = 256       Max price levels     = 256 per side (uint16_t, recompile to change)
PRICE_SCALE      = 10,000    Orders per shard     = 2,000,000
```

---

## 4. Memory Budget

| Component | Per Shard | Total (8 shards) |
|---|---|---|
| OrderPool (orders + freelist) | 72 MB | 576 MB |
| OrderMap (hash table) | 64 MB | 512 MB |
| OrderBookPool (Level2Book slab) | ~257 MB | ~2,056 MB |
| **Subtotal per-shard** | **~393 MB** | **~3,144 MB** |
| Primary packet ring buffer | — | ~98 MB |
| Secondary packet ring buffer (optional) | — | ~98 MB |
| Recovery slot buffer (in ZeroCopyRing) | — | ~1.5 MB |
| MergedSlotQueue (FeedMerger → Parser) | — | ~0.25 MB |
| BboPublisher (SeqLock array) | — | 8 MB |
| SymbolDirectory | — | 2 MB |
| Latency histograms | ~4 KB | ~36 KB |
| **Total fixed (single feed)** | | **~3.3 GB** |
| **Total fixed (dual feed)** | | **~3.4 GB** |

`OrderBookPool` pre-allocates all 8,192 `Level2Book` objects per shard at startup into a NUMA-local huge-page slab. Each book holds two `PriceLevel[256]` arrays (`256 × 64 B × 2 sides = 32 KB`), giving `8,192 × ~32 KB ≈ 257 MiB` per shard. This replaces the previous lazy `new Level2Book(...)` on SOD StockDirectory bursts, eliminating heap allocation latency spikes on the hot path. The slab index formula is `loc / NUM_SHARDS` (O(1), branch-free).

---

## 5. Threading Model

| Thread | Core | Sched Policy | Priority | Role |
|---|---|---|---|---|
| ReceiverThread (primary) | 1 | SCHED_FIFO | 80 | A-feed UDP receive → primary_ring |
| ParserThread | 2 | SCHED_FIFO | 79 | Parse + route (reads merged_queue) |
| ShardWorker[0] | 3 | SCHED_FIFO | 78 | Book shard 0 |
| ShardWorker[1] | 4 | SCHED_FIFO | 78 | Book shard 1 |
| ... | ... | ... | ... | ... |
| ShardWorker[7] | 10 | SCHED_FIFO | 78 | Book shard 7 |
| RecoveryThread | 11 | SCHED_OTHER | 0 | Gap detection / TCP retransmit / injection |
| FeedMerger | 12 | SCHED_FIFO | 77 | A/B merge, dedup, failover → merged_queue |
| ReceiverThread (secondary) | 13 | SCHED_FIFO | 80 | B-feed UDP receive → secondary_ring |
| PersistentSequence writer | unset | SCHED_OTHER | 0 | Async sequence persistence |
| MetricsReporter | unset | SCHED_OTHER | 0 | Stats printing |
| main thread | unset | SCHED_OTHER | 0 | Signal wait loop |

Secondary `ReceiverThread` and `FeedMerger` are only created when `multicast_group_secondary` is configured. When secondary is disabled, `FeedMerger` still runs and acts as a thin pass-through forwarding shim (no dedup overhead — `secondary_enabled_ = false` skips all secondary-ring logic).

**SCHED_FIFO** requires `CAP_SYS_NICE` (Linux). On macOS the affinity calls are hints only and real-time scheduling is unavailable; the system still builds and runs but without latency guarantees.

Inter-thread communication uses only SPSC queues and SeqLock — no mutex, no condition variable, no `std::atomic` CAS loops on hot paths (only `fetch_add` and `store`).

---

## 6. Configuration Reference

File: `config/feed_handler.toml`

```toml
[network]
multicast_group = "233.54.12.111"   # A-feed (primary) multicast address
port            = 26477              # UDP port
interface       = "0.0.0.0"         # Bind interface (0.0.0.0 = any)
# B-feed (secondary) — leave multicast_group_secondary empty/absent to disable.
# When set, FeedMerger joins both groups, deduplicates by sequence number,
# and automatically fails over to secondary if primary is silent for failover_timeout_ms.
# multicast_group_secondary = "233.54.12.112"
# port_secondary = 26477
# interface_secondary = "0.0.0.0"
failover_timeout_ms = 5000          # ms of primary silence before failover log

[threads]
receiver_core = 1
parser_core   = 2
shard_cores   = [3, 4, 5, 6, 7, 8, 9, 10]
recovery_core = 11
merger_core   = 12  # FeedMerger (SCHED_FIFO 77); secondary receiver uses core merger_core+1

[monitoring]
report_interval_sec = 10            # Stats print interval

[recovery]
gap_timeout_ms        = 500         # Delay before retransmit request
snapshot_interval_sec = 60          # Book snapshot frequency

# Primary retransmit server (NASDAQ SoupTCP / SGX equivalent).
# REQUIRED — 198.51.100.1 is the RFC 5737 placeholder; startup validation
# rejects it with exit(1) if not overridden.
retransmit_address = "10.0.1.10"
retransmit_port    = 17001

# Optional secondary retransmit server (dual-feed / cross-connect path).
# Remove or leave address empty to disable.
# retransmit_address_secondary = "10.0.1.11"
# retransmit_port_secondary    = 17001

# SoupBinTCP login credentials.
# require_login = false → skip handshake (unauthenticated test/simulation feeds).
# On login rejection the process exits immediately (misconfiguration, not transient).
require_login        = false
retransmit_username  = ""
retransmit_password  = ""

# Snapshot storage. {session} and {timestamp} are expanded at save time.
# Directory is created (mode 0755) at startup if absent.
snapshot_directory         = "/var/lib/itch/snapshots"
snapshot_filename_template = "itch_{session}_{timestamp}.snap"

[session]
# "AUTO_DETECT": session name populated from the first received MoldUDP64
# session field. Set explicitly (e.g. "STRT00000T") to pre-seed the tracker.
initial_session = "AUTO_DETECT"

[persistence]
# Directory for last_sequence.bin — the durable warm-start state file.
# Process must have read+write access.
last_sequence_dir = "/var/lib/itch"
```

---

## 7. Production Gap Analysis

The following gaps must be addressed before deploying this system in a live Singapore HFT or fintech environment. They are grouped by severity.

---

### P0 — Blockers (must fix before any live use)

**1. Session management is hard-coded** ✅ RESOLVED
`SessionManager` auto-detects the MoldUDP64 session name from the first received packet (or accepts an explicit pre-seed). On session name change it executes a full cross-subsystem reset: `SequenceTracker`, `SymbolDirectory`, `BboPublisher`, and all shard books/pools/maps via a `InternalSessionReset` broadcast. A `SessionState` machine (`Unknown→PreOpen→Open→Closed`) driven by ITCH `SystemEvent` codes gates `StockDirectory` processing. See §3.10.

**2. Recovery server address is a placeholder** ✅ RESOLVED
`retransmit_address` is now a required config key (no default). Startup validates that it is not the RFC 5737 documentation placeholder `198.51.100.1` and calls `exit(1)` with a FATAL message if it is. An optional secondary server (`retransmit_address_secondary`) enables dual-feed / cross-connect failover. See §3.5.

**3. No sequence number persistence across restarts** ✅ RESOLVED
`PersistentSequence` persists the last confirmed MoldUDP64 sequence + session to `<last_sequence_dir>/last_sequence.bin` via an atomic tmp→fsync→rename write. `FeedHandler::init()` calls `load()` before starting workers and seeds `SequenceTracker` — warm restart proceeds from the last known position without declaring a false gap. See §3.5.

**4. Book snapshot path not configurable** ✅ RESOLVED
`BookSnapshot` now takes a configurable `directory` + `filename_template`. Template tokens `{session}` and `{timestamp}` are expanded at save time. The directory is created at startup if absent. A `latest.ptr` sidecar file enables O(1) snapshot discovery on restart. Write path is atomic (tmp→fsync→rename). See §3.5.

**5. No TLS / authentication on recovery TCP connection** ✅ RESOLVED
`RetransmitClient` now uses an abstract `LoginHandler` interface for pluggable venue authentication. `NasdaqLoginHandler` implements the SoupBinTCP handshake (64-byte Login Request → `'A'` Accepted / `'J'` Rejected). Rejected login causes `exit(1)`. `raw_fd()` exposes the socket for future TLS integration. See §3.5.

**6. `new Level2Book(...)` on hot path** ✅ RESOLVED
`OrderBookPool` pre-allocates all 8,192 `Level2Book` objects per shard at startup into a NUMA-local huge-page slab (`~257 MiB/shard`). `ShardWorker::activate_book()` performs an O(1) slab lookup (`loc / NUM_SHARDS`) with no allocation. SOD StockDirectory bursts no longer trigger heap activity. See §3.4.

---

### P1 — High Priority (required for production reliability)

**7. Shard queue drop is silent beyond a log warning** ✅ RESOLVED
`RouteResult` enum replaces `bool` in `ShardRouter::route()` / `broadcast()`. Per-shard drop counters (`FeedStats::shard_queue_drops[NUM_SHARDS]`) are incremented lock-free on every dropped message and reported by `MetricsReporter`. `SpscQueue::is_pressure_high(watermark)` enables soft back-pressure: `ParserThread` yields once every 100 packets when any shard queue exceeds the configured threshold (`[sharding] queue_full_alert_threshold`, default 0.80). No synchronous logging on the drop path.

**8. No end-of-day / start-of-day lifecycle** ✅ RESOLVED
Full SOD/EOD lifecycle is implemented end-to-end:
- **EOD archival**: `SessionManager::on_eod_archival()` fires on `'Q'` / `'M'` *before* state→Closed. One `EodCallback` per shard is registered by `FeedHandler`; each captures its shard's `OrderBookPool` and calls `BookSnapshot::save_eod()` with `"_sN"` per-shard suffix. Configurable via `[session] eod_archive_enabled`.
- **Order gating**: All order and book messages are discarded while `is_open()` returns false (PreOpen, Closed, Unknown states). `StockTradingAction` passes through; `StockDirectory` activates books in Open or pre-open warmup paths only.
- **Pre-open warmup**: `[session] watched_symbols` names up to 256 space-padded ITCH tickers. `ParserThread` calls `SessionManager::try_resolve_watched()` on every `StockDirectory` message, setting the locate-code bit before the message is routed. `ShardWorker` checks `is_warmup_symbol(loc)` during PreOpen state and activates the book early so it is ready when `'O'` arrives.
- **Session reset**: New MoldUDP64 session name triggers `reset_session()` — seq_tracker, sym_dir, bbo_pub reset + `InternalSessionReset` broadcast to all 8 SPSC queues → ShardWorkers clear books, pool, and map.

**9. No overflow handling for MAX_PRICE_LEVELS** ✅ RESOLVED
`Level2Book::insert_level()` now guards on `count >= MAX_PRICE_LEVELS`. On overflow it increments `FeedStats::price_level_drops[shard_id]` (relaxed atomic, zero log on hot path) and returns `MAX_PRICE_LEVELS` sentinel. The BBO is unaffected — existing levels are preserved and `get_bbo()` reads `bid_levels_[0]` / `ask_levels_[0]` as normal. `bid_count_` / `ask_count_` were promoted from `uint8_t` to `uint16_t` to eliminate a silent wrap-to-zero bug at the 256th level. `MAX_PRICE_LEVELS` is `constexpr uint16_t` (default 256, recompile to change). `MetricsReporter` prints per-shard drop totals with a suggestion to increase the constant if non-zero.

**10. RecoveryThread re-injection is not implemented** ✅ RESOLVED
`ZeroCopyRing` has a dedicated `recovery_queue` (SPSC, 1024 deep) plus `recovery_slots[1024]` buffer. `RecoveryThread::recv_retransmitted()` drains up to 64 packets per poll cycle from the TCP socket and calls `inject_recovered_packet()` — which does a capacity check, `memcpy` into the next recovery slot, and `try_push` to `recovery_queue` (drop-on-full increments `recovery_drops`). `FeedMerger` drains `recovery_queue` at the top of its loop before live packets, so recovered packets are parsed ahead of new market data. `ParserThread` calls `gap_detector_.mark_recovered(seq)` on every in-order sequence advance, clearing the pending gap once all missing sequences arrive. `GapDetector::mark_recovery_sent()` prevents `RecoveryThread` from re-requesting the same gap on every 10ms poll. See §3.1 and §3.6.

**11. No failover / dual-feed support** ✅ RESOLVED
`FeedMerger` joins and consumes both A-feed (`primary_ring`) and B-feed (`secondary_ring`) simultaneously. Deduplication uses `DedupWindow` — a 128-sequence sliding bitmask that suppresses the duplicate packet from the slower feed with a single bitwise test per slot (~3 instructions). Automatic failover: if primary has been silent for `failover_timeout_ms` (default 5000ms), `FeedMerger` logs a WARN, increments `feed_failover_count`, and sets `failed_over_ = true` (secondary packets continue to be forwarded as normal — there is no separate "failover mode"; secondary is always forwarded when unique). Recovery is automatic: when primary packets resume, `failed_over_` clears and an INFO log is emitted. Secondary feed is fully optional — if `multicast_group_secondary` is empty, `FeedMerger` acts as a zero-overhead pass-through. See §3.1 and §3.6.

**12. OrderReplace loses time priority** ✅ RESOLVED
`Level2Book::on_order_replace()` now correctly implements NASDAQ ITCH 5.0 §4.6.2:
- **Case A** (`new_price == old_price && new_shares ≤ old_shares`): in-place update — qty adjusted, order reference re-keyed, FIFO position preserved. `order_replaces_inplace[shard_id]` incremented.
- **Case B** (price changed OR qty increased): old order unlinked and freed; new order allocated and appended to tail of target level (new time priority). Qty increase is treated as cancel + resubmit — the exchange moves the firm to the back of the queue.
The previous code applied in-place update for all same-price replaces regardless of qty direction, incorrectly giving a larger replace the same queue position as a reduction. Four unit tests verify FIFO ordering by executing through the queue after a replace. See `tests/test_level2_book.cpp`.

---

### P2 — Medium Priority (required before co-location or MAS reporting)

**13. No heartbeat / liveness monitoring**
The system does not detect a feed going silent (no packets for N seconds). A stale feed that appears healthy is more dangerous than an obvious disconnect. A watchdog that checks packet receive timestamps and raises an alert after a configurable silence timeout is needed.

**14. No trade message processing**
`Trade ('P')`, `CrossTrade ('Q')`, and `BrokenTrade ('B')` are parsed but discarded. Last-trade price, daily VWAP, and imbalance indicators are essential for risk calculations required by Singapore MAS regulations (SFA, Securities and Futures Act). Trade messages must at minimum update a last-price / cumulative volume structure.

**15. Price fixed-point uses `int64_t` but wire format is `int32_t`**
ITCH price fields are 4-byte unsigned integers on the wire. The internal `Price` type is `int64_t` which is correct for arithmetic, but the conversion from the 4-byte wire value should be explicitly documented and checked for overflow in cross-currency or fractional-penny instruments.

**16. No order book validation / sanity checks**
There are no assertions that the book is internally consistent: bid ≤ ask, order quantities are positive, level sums match individual order quantities. These checks are essential for detecting parser bugs, message mis-sequencing, or venue feed anomalies. They should run in debug builds and be toggleable in production.

**17. Metrics are printed to stderr only**
The `MetricsReporter` writes to `stderr`. For production observability (Prometheus, Grafana, SGX regulatory reporting, MAS surveillance), metrics must be exported via a structured interface: Prometheus exposition format, StatsD UDP, or an in-process shared-memory interface for co-located consumers.

**18. Logging is synchronous on the hot path**
`ITCH_LOG_WARN` and `ITCH_LOG_INFO` in hot-path code (`cpu_affinity.cpp`, `shard_worker.cpp`) call `fprintf(stderr, ...)` which can block on a syscall. All logging on the hot path should be asynchronous (ring-buffer based, drained by a background logger thread).

**19. No graceful handling of `ITCH_UNLIKELY` pool exhaustion**
`OrderPool::alloc()` returns `nullptr` if the pool is exhausted. `Level2Book::on_add_order()` calls `pool_.alloc()` without checking the return value in all code paths. A null dereference under pool exhaustion will crash the process.

---

### P3 — Lower Priority (polish and operational readiness)

**20. Build is Debug by default**
`CMakeCache.txt` shows `CMAKE_BUILD_TYPE=Debug`. The `-g` flag disables `-O3` and `‑march=native`, making the binary 3–10× slower. Production builds must explicitly set `-DCMAKE_BUILD_TYPE=Release`.

**21. No CI/CD pipeline**
There is no `.github/workflows/`, `Jenkinsfile`, or equivalent. Automated build, test, and latency regression testing (via `bench_itch`) are required before each deployment at a co-location facility.

**22. No NUMA topology auto-detection**
`numa_node_of_cpu()` is available but the config requires manual core → NUMA mapping. On multi-socket servers, assigning the receiver core and NIC IRQ to the same NUMA node as the order pool memory is critical. This should be auto-detected from the system topology.

**23. Shard count is a compile-time constant**
`NUM_SHARDS = 8` is fixed at compile time. Changing it requires recompilation. For different server sizes (e.g., 4-core VMs vs. 64-core bare metal) this should be a runtime parameter.

**24. No venue-specific symbol filtering**
The system processes all 65,535 possible locate codes. Singapore-listed instruments on venues using ITCH protocol (or a compatible format) typically have a much smaller active set. A configurable symbol whitelist would reduce memory usage and shard load significantly.

**25. IPv6 not supported**
`SocketReceiver` uses `AF_INET` only. Some venue connectivity (particularly newer SGX infrastructure) may require IPv6.

---

### Summary Table

| # | Gap | Severity | Category |
|---|---|---|---|
| 1 | Hard-coded session identifier | P0 | Correctness |
| 2 | Recovery server is placeholder address | P0 | Correctness |
| 3 | No sequence number persistence across restarts | P0 | Correctness |
| 4 | Snapshot path not configurable | P0 | Operations |
| 5 | No auth on retransmit TCP connection | P0 | Security |
| 6 | `new` on SOD hot path | P0 | Latency |
| 7 | ~~Shard queue drop is silent~~ ✅ | P1 | Reliability |
| 8 | ~~No SOD/EOD lifecycle handling~~ ✅ | P1 | Correctness |
| 9 | ~~MAX_PRICE_LEVELS overflow is silent~~ ✅ | P1 | Correctness |
| 10 | ~~Gap recovery re-injection not implemented~~ ✅ | P1 | Reliability |
| 11 | ~~No dual-feed / failover support~~ ✅ | P1 | Reliability |
| 12 | ~~OrderReplace time priority inconsistency~~ ✅ | P1 | Correctness |
| 13 | No feed liveness / heartbeat watchdog | P2 | Reliability |
| 14 | Trade messages discarded (no last-trade / VWAP) | P2 | Completeness |
| 15 | Price conversion unsigned→signed not guarded | P2 | Correctness |
| 16 | No order book integrity validation | P2 | Correctness |
| 17 | No structured metrics export (Prometheus/StatsD) | P2 | Operations |
| 18 | Synchronous logging on hot path | P2 | Latency |
| 19 | No null check after pool exhaustion | P2 | Stability |
| 20 | Debug build by default | P3 | Performance |
| 21 | No CI/CD pipeline | P3 | Operations |
| 22 | NUMA topology requires manual config | P3 | Operations |
| 23 | Shard count is compile-time only | P3 | Flexibility |
| 24 | No symbol filtering / whitelist | P3 | Efficiency |
| 25 | IPv6 not supported | P3 | Compatibility |
