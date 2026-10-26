# Gardener

A system supervisor: learns what "normal" looks like for a
multi-channel system, flags anomalies, and (later) can act on them.

## Status

Phase C complete. Phases A, B, and C are all in. The library ingests
packets, keeps per-channel history, writes and reads binary logs,
replays logs through ingest at any speed, learns per-channel
per-regime statistics with exponential decay, persists those
baselines across restarts, and comes with a Python simulated source
and a set of CLI tools to inspect what it has learned.

Detection, control, and the dashboard are phase D and later.

## What Gardener is

Gardener sits above a running system and watches it. Systems — a pump
loop, an HVAC unit, a motor controller, anything with sensors and
actuators — produce a stream of values. Some are measurements (temp,
flow, RPM). Some are controller internals (a PID's setpoint and
output). Some are commands to actuators (pump duty, valve position).
Gardener ingests all of it, builds a picture of what "normal" looks
like for that system over time, and flags when the picture stops
matching. It does not try to hold any value at a target; that is the
controller's job. Gardener's job is to notice when something is
wrong that the controller cannot see because the controller only
looks at its own loop.

In its current state Gardener is a C library plus a set of small
command-line tools, running on a PC. It ingests packets, keeps
per-channel history in memory, writes and reads binary logs, replays
recorded logs through the ingest path, learns what "normal" looks
like per channel per operating regime, and can be driven by a Python
simulated source. It does not yet act on anomalies — that is phase D.

## The problem it solves, concretely

A pump controller holds flow at 10 L/min. It succeeds: flow is
10 L/min, always. But the pump's current draw has crept from 2.1 A
to 3.8 A over three weeks. The controller is happy — flow is right.
The operator doesn't notice — nothing is broken. The pump is 90%
through its bearing life and the failure is going to be sudden.

A PID loop holds coolant at 60°C. It succeeds: coolant is 60°C. But
the PID's output has been pinned at 100% for six hours, and the
coolant is only at 59.8°C. The loop is saturated; it can't deliver
what the setpoint asks. The controller doesn't see this because it
only knows its own error signal.

Gardener's job is to notice both of these. It watches the
controller's internals (setpoint, PID output, actuator command)
alongside the physical measurements. It builds a model of "normal"
for each operating regime. It flags when a value drifts outside what
normal looks like. It doesn't try to control anything; it tells the
human.

## How it fits together

```
┌──────────────────┐      JSONL       ┌─────────────────────────────┐
│ gensim (Python)  │ ───────────────► │  gardener                   │
│ simulated source │                  │                             │
└──────────────────┘                  │  ingest_feed(packet_t)      │
                                      │                             │
┌──────────────────┐      JSONL       │  ┌────────────────────┐     │
│ any JSONL source │ ───────────────► │  │ registry_t         │     │
│ (real or test)   │                  │  │  id -> descriptor  │     │
└──────────────────┘                  │  └────────────────────┘     │
                                      │            │                │
                                      │            ▼                │
┌──────────────────┐                  │  ┌────────────────────┐     │
│ log_writer       │ ◄─────────────── │  │ per-channel state  │     │
│ gardener.bin     │   --log <path>   │  │  ring[1024]        │     │
│ gardener.idx     │                  │  │  latest sample     │     │
└────────┬─────────┘                  │  └────────────────────┘     │
         │                            │            │                │
         │                            │            ▼                │
         │                            │  ┌────────────────────┐     │
         │                            │  │ baseline_t         │     │
         │                            │  │  Welford per       │     │
         │                            │  │  (channel,regime)  │     │
         │                            │  │  exponential decay │     │
         │                            │  └────────────────────┘     │
         │                            │            │                │
         │                            │            ▼                │
         │                            │  baseline_save             │
         │                            │  --baseline <path>         │
         │                            └─────────────────────────────┘
         │                                          │
         ▼                                          ▼
┌──────────────────┐                    snapshot / history /
│ log_reader       │                    baseline stats
│ gardener-log     │                    (gardener-stats)
│ gardener --replay│
└──────────────────┘
```

### The four data structures to know

**`channel_desc_t`** — a static description of one signal. Set once
at startup from `channels.toml`. Never changes while running.
Contains:

- `id` (uint16) — the wire identifier, 0..65535
- `name` (string) — human-readable, up to 31 chars
- `unit` (string) — for display, up to 15 chars
- `type` — f32 / i32 / u16 / bool / enum
- `role` — sensor / setpoint / ctrl_out / actuator / status / counter
- `controllable` (bool) — may Gardener write to this channel?
- `is_regime` (bool) — does this channel select the operating regime?
- `min_valid`, `max_valid` (floats, optional) — sanity range

The `role` field is the key insight that makes Gardener different
from a generic data logger. A PID loop produces four distinct kinds
of signals: the measurement (sensor), the desired value (setpoint),
the controller's effort (ctrl_out), and what the actuator actually
does (actuator). Each has different "normal" behavior. A supervisor
that doesn't know the difference will confuse a saturated controller
for a healthy one.

**`sample_t`** — one reading of one channel. 24 bytes in memory,
18 bytes on the wire. Contains:

- `channel_id` (uint16)
- `type` (uint8, mirrors `channel_desc_t.type`)
- `role` (uint8, mirrors `channel_desc_t.role`)
- `flags` (uint8) — VALID / STALE / SIMULATED bit flags
- value (union of f32 / i32 / u16 / bool)
- `timestamp_ns` (uint64)

A sample is atomic. If it has `CH_FLAG_VALID`, its value is a fresh
reading from the source. If it has `CH_FLAG_STALE`, the value is the
last known good one and should be ignored by detectors. If it has
`CH_FLAG_SIMULATED`, it came from a test source.

**`packet_t`** — one tick's worth of samples from one source. Up to
64 channels. Two encodings:

- JSONL for the dev feed and simulated sources: one line,
  human-readable.
- Binary for logs and (eventually) the target link:
  22 + 18×N bytes, CRC-16/MODBUS trailer.

A packet is a coherent snapshot: all its samples were taken at the
same moment by the same system. Different packets are different
moments. This is why `baseline_regime_for_packet` derives the regime
once per packet — the whole packet belongs to one regime.

**`baseline_stat_t`** — the statistics for one channel in one
regime. 40 bytes. Contains:

- `count` (float) — effective sample count, may be fractional after
  decay
- `mean` (float) — running mean
- `m2` (float) — sum of squared deviations from the mean
- `min_val`, `max_val` (floats) — observed extremes
- `first_ts_ns`, `last_ts_ns` (uint64s) — sample timestamp range
- `state` (enum) — EMPTY / LEARNING / READY / STALE
- `flags` (bit field) — informational

The Welford algorithm maintains `count`, `mean`, and `m2`
incrementally. Variance is `m2 / (count - 1)`, standard deviation is
`sqrt(variance)`. This is numerically stable even for large means
with small variances (a pressure sensor reading 101325 Pa that
varies by 5 Pa).

## What exists right now

### Library

**Platform layer** (`platform.h`, `platform_posix.c`) — the only
code that touches the OS directly. Provides:

- `plat_now_ns()`, `plat_wall_ns()` — monotonic and wall clock
- `plat_sleep_ms(n)` — sleep, retrying on signal
- `plat_file_open/read/write/seek/tell/sync/close()` — file ops
- `plat_log(level, fmt, ...)` — formatted logging to stderr

Nothing outside `src/platform/` calls OS APIs. The port to an MCU
will be a folder swap.

**Channel model** (`channel.h`) — `ch_type_t`, `ch_role_t`,
`channel_desc_t`, `sample_t`, `sample_as_f32`. All header-only or
inline.

**`ring_t`** (`ring.h`, `ring.c`) — fixed-capacity sample ring. Push
appends; `ring_at(i)` indexes (0=oldest); wrap overwrites oldest.
No allocation after `ring_init`.

**`registry_t`** (`registry.h`, `registry.c`) — loads channel
descriptors from a TOML file. Provides id → descriptor lookup.
Unknown ids get synthesized placeholders with `role = ROLE_UNKNOWN`.
String storage lives in a fixed arena inside the struct; no heap.

**Packet codecs** (`packet.h`, `packet.c`) — two encoders/decoders:

- `packet_from_json`, `packet_to_json` — one packet per line.
- `packet_encode`, `packet_decode`, `packet_crc16` — packed binary
  format, byte-exact, CRC-16/MODBUS.

Both preserve all fields. JSONL is the dev feed; binary is the log
and future wire.

**`ingest_t`** (`ingest.h`, `ingest.c`) — the engine that turns
packets into per-channel history. Contains:

- Embedded `registry_t`
- One `ring_t` per channel (`INGEST_RING_CAP = 1024` samples)
- Latest-value snapshot per channel
- Counters: `packets_seen`, `samples_seen`, `samples_dropped`,
  `unknown_channels`

`ingest_feed(packet)` normalizes each sample (fills type/role from
the descriptor if missing, inherits packet timestamp if per-sample
timestamp is zero, propagates flags) and pushes into the ring.
~6.3 MB static. No heap.

**`log_writer_t`** (`log.h`, `log_writer.h`, `log_writer.c`) —
writes binary logs. Preallocates a file at `LOG_DEFAULT_MAX`
(100 MB), wraps in place when full. Fixed 4096-byte header with
magic, version, offsets, counters, CRC. JSONL registry snapshot
after the header. Variable-size records, each exactly one
`packet_encode` output. Sidecar index (`X.bin` → `X.idx`) with
sparse timestamp → offset entries every 256 records. Refuses to open
a file with a bad header unless `start_fresh`.

**`log_reader_t`** (`log_reader.h`, `log_reader.c`) — reads binary
logs. Walks records in logical order (oldest-first, wrap-aware).
Time-range queries via `log_reader_seek_ns` use the sidecar index
for the fast path, linear scan as fallback. Reconstructs the
registry from the header snapshot.

**`replay`** (`replay.h`, `replay.c`) — reads a log and feeds
packets through ingest at three speeds: `REPLAY_SPEED_MAX` (as fast
as possible), `REPLAY_SPEED_REAL` (as recorded), `REPLAY_SPEED_SCALED`
(factor times real time). Window filtering via `from_ts` / `to_ts`.
Optional looping for soak testing.

**`baseline_t`** (`baseline.h`, `baseline.c`) — the model of
"normal." Contains:

- Fixed 64-byte header with magic, version, halflife, regime
  channel ids
- Per-channel table (`BASELINE_MAX_CHANNELS = 256` entries)
- Per-(channel, regime) statistics table
  (`BASELINE_MAX_REGIMES = 16` slots each)
- Derived fields: `slot_to_ch_id`, `next_slot`, `regime_ch_ids`,
  `regime_ch_count`

Key operations:

- `baseline_init(b, registry, halflife_ns)` — initialize, populate
  regime channels from the registry's `is_regime` channels
- `baseline_update(b, packet)` — for each valid, non-stale sample:
  derive the packet's regime, find-or-register the channel's slot,
  apply Welford's update
- `baseline_decay(b, now_ns)` — age all slots by `dt = now -
  last_ts`, shrinking `count` and `m2` by `2^(-dt/halflife)`.
  READY → STALE when count falls below threshold.
- `baseline_get(b, ch_id, regime)` — lookup stats, NULL if EMPTY
- `baseline_save`, `baseline_load` — atomic file I/O with
  CRC-16/MODBUS trailer, version check, size check

~166 KB static. Welford statistics per slot: `count`, `mean`, `m2`,
`min`, `max`, `first_ts`, `last_ts`, `state`, `flags`.

### Executables and tools

**`gardener`** (`src/main.c`) — the main entry point. Three modes:

- **stdin** (default): read JSONL packets from stdin, feed to
  ingest
- **replay** (`--replay <path>`): replay a binary log through
  ingest
- **baseline** (`--baseline <path>`): maintain a model of "normal"

Optional persistent state:

- `--log <path>` writes every accepted packet to a binary log
- `--baseline <path>` loads or creates a baseline, updates on every
  packet, saves on exit (and optionally every N seconds)

If `--log` or `--baseline` fails to open, `gardener` logs a warning
and continues without it. Ingest is never blocked by persistence
failures.

**`gardener-dump`** (`tools/gardener_dump.c`) — two modes:

- `--config <path>` prints the channel registry as a table
- `--feed <path>` feeds a JSONL file through ingest and prints the
  resulting snapshot (latest value per channel, sample count,
  flags)

**`gardener-log`** (`tools/gardener_log.c`) — reads a binary log and
prints records as a table or JSONL. `--from` / `--to` filter by
timestamp using the index. `--header` shows the header and registry
without records. `--limit N` stops after N records.

**`gardener-stats`** (`tools/gardener_stats.c`) — reads a baseline
file and prints per-channel, per-regime statistics: state, count,
mean, stddev, min, max. `--registry` resolves channel names.
`--channel` / `--regime` filter. `--ready-only` shows only slots
that crossed the threshold. `--jsonl` for machine consumption.

**`gensim`** (`tools/gensim/`) — a Python simulated source. Emits
JSONL packets on stdout, driven by a scenario file with fault
injection. Supports plant models (`generic`, `hvac`), time
acceleration (`--speed 10` runs 10× faster than real time), and
scripted events (freeze, spike, drift, link_gap). See
`tools/gensim/README.md`.

## What does not exist yet

- **Detection** (Phase D) — the baseline exists, but nothing
  compares live samples to it. Every number a detector needs is
  computed; the comparison logic isn't written.
- **Policy engine** (Phase E) — no config-driven rules, no actions,
  no safety rails.
- **Dashboard** (Phase F) — no PC link, no UI, no real-time view.

## Build

Requires a C11 compiler and CMake 3.16+. Python 3.8+ for `gensim`.

```
cmake -B build
cmake --build build
```

## Run

### Ingest from stdin

```
./build/gardener --version
./build/gardener --verbose
./build/gardener --quiet
echo '{"t":1,"v":{"1":20.5}}' | ./build/gardener
```

### Ingest from stdin and write a log

```
echo '{"t":1,"v":{"1":20.5}}' | ./build/gardener \
    --log run.bin --log-fresh

./build/gardener-log --file run.bin --header
./build/gardener-log --file run.bin
```

### Build a baseline

```
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/normal.yaml \
    --duration 300 --speed 10 |
    ./build/gardener --channels configs/channels.toml \
                     --log normal.bin --log-fresh \
                     --baseline normal.bl --baseline-fresh
```

After this, `normal.bl` contains per-channel, per-regime Welford
statistics from 300 seconds of simulated normal operation.

### Inspect the baseline

```
./build/gardener-stats --file normal.bl --registry configs/channels.toml
```

Output:

```
ch     name                 regime  state     count    mean       stddev     min        max
------ -------------------- ------- --------- -------- ---------- ---------- ---------- ----------
1      coolant_temp         0       READY     876      61.234     0.842      58.912     63.847
1      coolant_temp         5       READY     2891     72.451     1.123      69.311     75.102
2      coolant_flow         0       READY     876      9.781      2.013      4.012      18.234
```

The two rows for `coolant_temp` are the two regimes
(running=false and running=true). Each has its own baseline. When
Phase D's detectors fire, they compare live samples to the baseline
for the current regime.

### Add samples to an existing baseline

```
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/normal.yaml \
    --duration 60 --speed 10 |
    ./build/gardener --channels configs/channels.toml \
                     --baseline normal.bl
```

No `--baseline-fresh`, so the existing baseline is loaded and new
samples are added. The model continues to adapt.

### Compare normal vs fault

```
# Normal run
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/normal.yaml \
    --duration 300 --speed 10 |
    ./build/gardener --channels configs/channels.toml \
                     --baseline normal.bl --baseline-fresh

# Fault run (coolant temp drifts upward after 15s)
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/sensor_drift.yaml \
    --duration 300 --speed 10 |
    ./build/gardener --channels configs/channels.toml \
                     --baseline drift.bl --baseline-fresh

# Compare
./build/gardener-stats --file normal.bl --registry configs/channels.toml --channel 1
./build/gardener-stats --file drift.bl  --registry configs/channels.toml --channel 1
```

The `coolant_temp` stddev in `drift.bl` will be larger than in
`normal.bl` — the drift makes the value wander. That's the signal
Phase D's z-score detector will use.

### Replay a log

```
./build/gardener --replay run.bin
./build/gardener --replay run.bin --replay-speed real
./build/gardener --replay run.bin --replay-speed 10
./build/gardener --replay run.bin --replay-from 5000000000 --replay-to 8000000000
./build/gardener --replay run.bin --log run_copy.bin --log-fresh
./build/gardener --replay run.bin --baseline replay.bl --baseline-fresh
```

### Windows note

PowerShell's `>` re-encodes stdout as UTF-16LE on PowerShell 5.x,
which `gardener` cannot parse. Redirect through `cmd.exe` instead:

```
cmd /c "py tools\gensim\gensim.py --scenario ... --duration 300 > drift.jsonl"
```

Or use PowerShell 7 (`pwsh`), whose `>` defaults to UTF-8. Piping
(`|`) is not affected; only file redirection.

## Options reference

### gardener

```
--channels <path>         channel registry (default: configs/channels.toml)
--stats-every <n>         print counters every n packets
--verbose                 log at DEBUG level
--quiet                   log at WARN level
--version                 print version
--help                    print usage

--log <path>              write ingested packets to a binary log
--log-max <bytes>         log size, accepts K/M/G suffix (default 100M, min 1M)
--log-fresh               truncate an existing log instead of appending

--baseline <path>         load or create a baseline model
--baseline-fresh          start from empty, ignore existing file
--baseline-halflife <s>   decay half-life in seconds (default: 300)
--baseline-save-every <s> periodic save interval (default: 0 = exit only)

--replay <path>           replay a binary log instead of stdin
--replay-speed <mode>     max (default), real, or a positive factor
--replay-from <ts_ns>     start at this timestamp
--replay-to <ts_ns>       stop before this timestamp
--replay-loop             replay until interrupted
--replay-progress <n>     print progress every n packets
```

### gardener-log

```
--file <path>             binary log to read
--from <ts_ns>            start at this timestamp
--to <ts_ns>              stop before this timestamp
--limit <n>               print at most n records
--jsonl                   output as JSONL instead of a table
--header                  print header and registry, then exit
--quiet                   suppress warnings
--help                    print usage
```

### gardener-dump

```
--config <path>           channel registry to print (no feed)
--feed <path>             JSONL file to feed through ingest
--channels <path>         channel registry for feed mode
--counts                  include per-channel sample count column
--quiet                   suppress warnings
--help                    print usage
```

### gardener-stats

```
--file <path>             baseline file to read (required)
--registry <path>         optional; resolves channel names
--channel <id>            only show this channel id
--regime <n>              only show this regime index
--jsonl                   machine-readable output
--ready-only              only show READY slots
--quiet                   suppress warnings
--help                    print usage
```

## Test

```
cd build && ctest --output-on-failure
```

Seventeen test suites currently. For a detailed breakdown of what
each suite checks and why, see `tests/TESTS.md`.

Summary:

| Suite | Module | Purpose |
|---|---|---|
| test_platform | platform | time, sleep, files |
| test_ring | ring | sample ring, wrap, layout |
| test_registry | registry | TOML parsing, lookup |
| test_ingest | ingest | packet feed, counters, flags |
| test_packet_json | packet | JSONL encode/decode |
| test_packet_bin | packet | binary encode/decode, CRC |
| test_log_format | log | header layout, size formulas |
| test_log_writer | log_writer | append, wrap, reopen, corruption |
| test_log_reader | log_reader | iteration, seek, registry snapshot |
| test_replay | replay | timing modes, window, looping |
| test_main_args | main | CLI parsing, log creation |
| test_baseline_format | baseline | struct sizes, offsets, layout |
| test_baseline | baseline | Welford math, state transitions |
| test_baseline_regimes | baseline | regime derivation, hashing |
| test_baseline_decay | baseline | exponential decay, state transitions |
| test_baseline_persist | baseline | save/load, corruption |
| test_main_baseline | main | CLI baseline wiring |

## Wire format

The binary packet format, locked as of A6:

```
Packet (little-endian):
+----------+---------+---------------+---------------+----------+----------+
| u32 magic| u16 ver | u16 source_id | u64 t_ns      | u16 ch_n | u16 flags|
| "GRDN"   |    1    |               |               |          |          |
+----------+---------+---------------+---------------+----------+----------+
  4 bytes    2          2              8               2          2

Followed by ch_n x packed sample (18 bytes each), then u16 crc16.

Packed sample (little-endian):
+---------+--------+--------+--------+------+----------+-----------------+
| u16 id  | u8 type| u8 role| u8 flag| u8 _ | u32 value| u64 timestamp_ns|
| 2 bytes | 1      | 1      | 1      | 1    | 4        | 8               |
+---------+--------+--------+--------+------+----------+-----------------+
```

Total: 22 + 18 * channel_count bytes.

CRC-16/MODBUS: poly 0x8005 reflected as 0xA001, init 0xFFFF,
refin/refout true, xorout 0x0000. Check value of "123456789" is
0x4B37.

## Log format

The binary log, locked as of B0:

- Preallocated at a fixed size (default 100 MB, minimum 1 MB).
- Fixed 4096-byte header with magic, version, offsets, counters,
  timestamps, and CRC.
- A JSONL registry snapshot after the header.
- Variable-size records, each one `packet_encode` output.
- Sidecar index (X.bin -> X.idx) with sparse timestamp -> offset
  entries every 256 records.
- Wraps to `data_start` when full, overwriting oldest. `wrap_count`
  tracks wraps.

## Baseline format

The baseline file, locked as of C4:

- Fixed size: 64-byte header + channel table (256 * 8) + stats
  table (256 * 16 * 40) + 4-byte CRC = 165,956 bytes.
- Header: magic (0x42415345 "BASE"), version (1), size, halflife,
  regime channel ids, timestamps.
- Channel table: `{u16 ch_id, u16 n_regimes_used, u32 _pad}` per
  slot.
- Stats table: `baseline_stat_t` per (channel, regime) — 40 bytes
  each.
- CRC-16/MODBUS trailer over everything before it.
- Atomic write: temp file + rename.

## Design notes

### Data model invariants

- `sample_t` is 24 bytes in memory, 18 bytes on the wire. Both
  sizes are asserted by tests.
- `ROLE_UNKNOWN = 0` — zero-initialized descriptors mean "unknown",
  not "sensor". Unknown channels get logged and are easy to spot.
- Unknown channel ids are synthesized into the registry on first
  sight and cached. Samples are stored normally.
- Every sample in a ring has exactly one of `CH_FLAG_VALID` or
  `CH_FLAG_STALE` set.
- Per-sample timestamps win over packet timestamps; zero means
  "inherit".
- `PACKET_FLAG_SIMULATED` propagates to `CH_FLAG_SIMULATED` on
  every sample.

### Constants and bounds

- `REGISTRY_MAX_CHANNELS = 256`, arena 64 B/channel.
- `ingest_t` is ~6.3 MB static. Must be `static` or global — not on
  the stack. Windows thread stacks default to 1 MB; a stack-local
  `ingest_t` segfaults on function entry.
- `baseline_t` is ~166 KB static. Same rule: declare it static.
- `INGEST_RING_CAP = 1024` samples per channel (~102 s at 10 Hz).
- `PACKET_MAX_CH = 64`. Keeps `packet_t` around 1.5 KB.
- `BASELINE_MAX_CHANNELS = 256`, `BASELINE_MAX_REGIMES = 16`,
  `BASELINE_MIN_SAMPLES = 100`.

### Registry

- String storage in a fixed arena inside the struct. No heap.
- Loads are append-only.
- No name -> channel dropped with warning. No role -> kept with
  `ROLE_UNKNOWN` and warning.

### Packet codecs

- `packet_from_json` honors an explicit `"type"` for both the type
  tag and the union bytes. Inferred types pick i32 for whole
  numbers, f32 otherwise.
- `packet_to_json` returns the length the full encoding needs, even
  if truncated. Callers detect truncation by comparing to `cap`.
- `find_key` stops at the end of the current object — keys in later
  entries of the same packet don't leak into earlier parses.
- Wire format is little-endian, byte-by-byte, independent of host
  endianness.
- `packet_decode` returns bytes consumed, so callers can walk a
  stream of concatenated packets.
- CRC is CRC-16/MODBUS, not CRC-16/ARC. The check value in the test
  (0x4B37 vs 0xBB3D) pins which is which.

### Log

- Preallocated at fixed size, wraps in place.
- `log_writer_open` refuses to overwrite a file with a bad header
  unless `start_fresh` is set. Prevents destroying a
  possibly-valuable file.
- `log_reader_seek_ns` positions so a following `log_reader_next`
  returns the first record with ts >= target.
- Logging failures are non-fatal to `gardener`. Ingest continues;
  the failure is reported at exit.

### Baseline

- Welford algorithm maintains `count`, `mean`, `m2` incrementally.
  Variance = `m2 / (count - 1)`. Numerically stable for large means
  with small variances.
- Regime derivation: FNV-1a hash of each regime channel's quantized
  value (3 decimals), mixed with MurmurHash3 finalizer, modulo
  `BASELINE_MAX_REGIMES`. Same regime-channel values -> same regime
  index, always. Different values may collide (16 slots is small).
- Regime is derived once per packet, not per sample. All samples in
  a packet share a regime.
- Missing regime channel -> whole packet skipped. Better than
  corrupting the wrong regime's baseline.
- `baseline_update` is pure Welford accumulation. Decay is separate.
  This makes `count` equal to "number of samples fed" (before any
  decay), which is intuitive.
- `baseline_decay` ages all slots by `dt = now - last_ts`. Shrinks
  `count` and `m2` by `2^(-dt/halflife)`. Mean is preserved (it's a
  ratio; scaling all weights equally preserves it).
- READY -> STALE when decayed count falls below
  `BASELINE_MIN_SAMPLES`. STALE -> READY when new samples bring it
  back over.
- `baseline_update` must use the packet's timestamp as "now" for
  decay, not `plat_wall_ns()`. Samples carry timestamps in the
  source's epoch, which may be monotonic-boot-time or wall-clock.
  Mixing epochs decays to zero (or not at all) silently.
- `baseline_load` zeroes `*b` on failure. Callers get a known state
  after any load attempt.

### Platform

- No OS headers outside `src/platform/`. Port to MCU is a folder
  swap.
- `plat_log` writes to stderr only. Stdout is for machine-readable
  output.

## Layout

```
include/gardener/   public headers
  platform.h          the OS boundary
  channel.h           channel types, roles, sample_t
  ring.h              fixed-capacity sample ring
  registry.h          channel descriptor registry
  packet.h            in-memory packet struct, JSONL and binary codecs
  ingest.h            per-channel rings and latest-value snapshot
  log.h               log format constants and structs
  log_writer.h        log writer API
  log_reader.h        log reader API
  replay.h            replay engine API
  baseline.h          baseline model API

src/                implementation
  main.c              entry point (stdin / replay / logging / baseline)
  ring.c
  registry.c
  ingest.c
  packet.c
  log_writer.c
  log_reader.c
  replay.c
  baseline.c
  platform/           the only place OS APIs are called

tools/              developer tools
  gardener_dump.c     JSONL feed + snapshot table
  gardener_log.c      binary log dump
  gardener_stats.c    baseline stats table
  gensim/             Python simulated source
    gensim.py
    scenarios/
    README.md

tests/              unit tests, one per module
configs/            channels.toml — one [[channel]] block per signal
```

## Configuration

`configs/channels.toml` describes every channel Gardener knows
about. One `[[channel]]` block per signal:

```
[[channel]]
id           = 1
name         = "coolant_temp"
unit         = "C"
type         = "f32"
role         = "sensor"
min          = 0
max          = 150
```

Fields:

- `id` — required, 0..65535. The wire identifier.
- `name` — required, up to 31 chars. For humans and logs.
- `unit` — optional, up to 15 chars.
- `type` — one of f32, i32, u16, bool, enum (default f32).
- `role` — one of sensor, setpoint, ctrl_out, actuator, status,
  counter. Defaults to unknown and logs a warning.
- `min`, `max` — optional, sanity range.
- `controllable` — true/false (default false). May Gardener write to
  this channel?
- `is_regime` — true/false (default false). Marks the channel as a
  regime selector for baseline segmentation.

The parser handles a small TOML subset: `[[table]]` headers and
`key = value` lines with quoted strings, numbers, and booleans.
Trailing comments on a value line are not stripped — put comments
on their own line. Nested tables and arrays are not supported.

## Roadmap

```
Phase A — core library                       done
  A0 skeleton . A1 ring . A2 registry
  A3 ingest . A4 JSONL . A5 dump . A6 binary

Phase B — logging and simulation             done
  B0 log format . B1 log writer . B2 log reader
  B3 gensim . B4 replay . B5 wire log into gardener

Phase C — baseline model                     done
  C0 format . C1 Welford . C2 regimes
  C3 decay . C4 persistence . C5 wire into gardener
  C6 gardener-stats tool

Phase D — detection                          next
  D0 event format
  D1 range + z-score detectors
  D2 rate-of-change + flatline
  D3 correlation pairs
  D4 CTRL_OUT vs ACTUATOR mismatch
  D5 wire into gardener, events file
  D6 detector tuning via replay

Phase E — policy engine                      planned
  config-driven rules, notify actions

Phase F — loopback                           planned
  target stub, dashboard stub, closed loop

Later — real Modbus link, dashboard, MCU port
```