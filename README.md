# Gardener

A system supervisor: learns what "normal" looks like for a multi-channel system, flags anomalies, and (later) can act on them.

## Status

Phase D complete. Phases A-D are all in. The system ingests packets, keeps per-channel history, writes and reads binary logs, replays logs through ingest at any speed, learns per-channel per-regime statistics with exponential decay, persists those baselines across restarts, runs six anomaly detectors on every packet, and writes the resulting events to a JSONL file. Phases E (policy engine) and F (dashboard link) are next.

## What Gardener is

Gardener sits above a running system and watches it. Systems — a pump loop, an HVAC unit, a motor controller — produce a stream of values. Some are measurements, some are controller internals (a PID's setpoint and output), some are actuator commands. Gardener ingests all of it, builds a picture of what "normal" looks like for that system over time, and flags when the picture stops matching. It does not try to hold any value at a target; that is the controller's job. Gardener's job is to notice when something is wrong that the controller cannot see because the controller only looks at its own loop.

In its current state Gardener is a C library plus a set of small command-line tools, running on a PC. It ingests packets, keeps per-channel history in memory, writes and reads binary logs, replays recorded logs through the ingest path, learns what "normal" looks like per channel per operating regime, runs six detectors against that learned baseline on every packet, and emits anomaly events to stderr and a JSONL file. It does not yet act on anomalies — that is phase E.

## The problem it solves, concretely

A pump controller holds flow at 10 L/min. It succeeds: flow is 10 L/min, always. But the pump's current draw has crept from 2.1 A to 3.8 A over three weeks. The controller is happy — flow is right. The operator doesn't notice — nothing is broken. The pump is 90% through its bearing life and the failure is going to be sudden.

A PID loop holds coolant at 60°C. It succeeds: coolant is 60°C. But the PID's output has been pinned at 100% for six hours, and the coolant is only at 59.8°C. The loop is saturated; it can't deliver what the setpoint asks. The controller doesn't see this because it only knows its own error signal.

Gardener's job is to notice both of these. It watches the controller's internals (setpoint, PID output, actuator command) alongside the physical measurements. It builds a model of "normal" for each operating regime. It flags when a value drifts outside what normal looks like. It doesn't try to control anything; it tells the human.

## How it fits together

```
+------------------+      JSONL       +-----------------------------+
| gensim (Python)  | ---------------> |  gardener                   |
| simulated source |                  |  ingest_feed(packet_t)      |
+------------------+                  |                             |
                                      |  +--------------------+     |
+------------------+      JSONL       |  | registry_t         |     |
| any JSONL source | ---------------> |  |  id -> descriptor  |     |
+------------------+                  |  +--------------------+     |
                                      |            |                |
+------------------+                  |            v                |
| log_writer       | <--------------- |  +--------------------+     |
| gardener.bin     |   --log <path>   |  | per-channel state  |     |
| gardener.idx     |                  |  |  ring[1024]        |     |
+--------+---------+                  |  +--------------------+     |
         |                            |            |                |
         |                            |            v                |
         |                            |  +--------------------+     |
         |                            |  | baseline_t         |     |
         |                            |  |  Welford per       |     |
         |                            |  |  (channel,regime)  |     |
         |                            |  |  exponential decay |     |
         |                            |  +--------------------+     |
         |                            |            |                |
         |                            |            v                |
         |                            |  +--------------------+     |
         |                            |  | detect_t           |     |
         |                            |  |  range/zscore/     |     |
         |                            |  |  flatline/roc/     |     |
         |                            |  |  correlation/      |     |
         |                            |  |  mismatch          |     |
         |                            |  +--------------------+     |
         |                            |            |                |
         |                            |            v                |
         |                            |  events.jsonl              |
         |                            |  --events <path>           |
         |                            +-----------------------------+
         |                                          |
         v                                          v
+------------------+                    snapshot / history /
| log_reader       |                    baseline stats /
| gardener-log     |                    anomaly events
| gardener --replay|
+------------------+
```

### The five data structures to know

**`channel_desc_t`** — a static description of one signal, loaded at startup from `channels.toml`. Contains `id` (uint16, wire identifier), `name` (up to 31 chars), `unit`, `type` (f32/i32/u16/bool/enum), `role` (sensor/setpoint/ctrl_out/actuator/status/counter), `controllable` (may Gardener write to it?), `is_regime` (does it select the operating regime?), and optional `min_valid`/`max_valid` bounds.

The `role` field is the key insight that makes Gardener different from a generic data logger. A PID loop produces four distinct kinds of signals: measurement (sensor), desired value (setpoint), controller's effort (ctrl_out), what the actuator does (actuator). Each has different "normal" behavior. A supervisor that doesn't know the difference will confuse a saturated controller for a healthy one.

**`sample_t`** — one reading of one channel. 24 bytes in memory, 18 bytes on the wire. Contains `channel_id`, `type`, `role`, `flags` (VALID / STALE / SIMULATED), value (union of f32/i32/u16/bool), `timestamp_ns`. A sample is atomic: `VALID` means fresh, `STALE` means last-known-good and ignored by detectors, `SIMULATED` means test source.

**`packet_t`** — one tick's worth of samples from one source. Up to 64 channels. Two encodings: JSONL for dev feeds and simulated sources, binary (22 + 18×N bytes, CRC-16/MODBUS trailer) for logs and the eventual target link. A packet is a coherent snapshot: all its samples were taken at the same moment. This is why `baseline_regime_for_packet` derives the regime once per packet.

**`baseline_stat_t`** — the statistics for one channel in one regime. 40 bytes. Contains `count`, `mean`, `m2`, `min_val`, `max_val`, `first_ts_ns`, `last_ts_ns`, `state`, `flags`. Welford maintains `count`/`mean`/`m2` incrementally. Variance = `m2 / (count - 1)`. Numerically stable even for large means with small variances.

**`event_t`** — one anomaly event. 48 bytes, packed. Contains two timestamps (event and source), `channel_id` and `channel_id2` (for pair detectors), `regime`, `observed`, `baseline_mean`, `baseline_stddev`, `threshold`, `detector`, `severity`, `state` (STARTED/CLEARED), `version`, `crc`. One STARTED per episode, one CLEARED per deactivation. No ONGOING events.

## What exists right now

### Library

**Platform layer** (`platform.h`, `platform_posix.c`) — the only code that touches the OS directly. `plat_now_ns()`, `plat_wall_ns()`, `plat_sleep_ms()`, `plat_file_open/read/write/seek/tell/sync/close()`, `plat_log()`. Nothing outside `src/platform/` calls OS APIs; the port to an MCU will be a folder swap.

**Channel model** (`channel.h`) — `ch_type_t`, `ch_role_t`, `channel_desc_t`, `sample_t`, `sample_as_f32`. Header-only or inline.

**`ring_t`** (`ring.h`, `ring.c`) — fixed-capacity sample ring. Push appends, `ring_at(i)` indexes (0=oldest), wrap overwrites oldest. No allocation after `ring_init`.

**`registry_t`** (`registry.h`, `registry.c`) — loads channel descriptors from TOML. id→descriptor lookup, unknown ids get synthesized placeholders. String storage in a fixed arena; no heap.

**Packet codecs** (`packet.h`, `packet.c`) — JSONL (`packet_from_json`, `packet_to_json`) for the dev feed and binary (`packet_encode`, `packet_decode`, `packet_crc16`) with CRC-16/MODBUS for logs and the future wire. Both preserve all fields.

**`ingest_t`** (`ingest.h`, `ingest.c`) — the engine that turns packets into per-channel history. Embedded `registry_t`, one `ring_t` per channel (`INGEST_RING_CAP = 1024`), latest-value snapshot per channel, counters. `ingest_feed(packet)` normalizes each sample (fills type/role from descriptor if missing, inherits packet timestamp if per-sample is zero, propagates flags) and pushes into the ring. ~6.3 MB static. No heap.

**`log_writer_t`** (`log.h`, `log_writer.h`, `log_writer.c`) — writes binary logs. Preallocates 100 MB, wraps in place. Fixed 4096-byte header with magic, version, offsets, counters, CRC. JSONL registry snapshot after header. Variable-size records, each one `packet_encode` output. Sidecar index (`.idx`) with sparse timestamp→offset entries every 256 records. Refuses to open a file with a bad header unless `start_fresh`.

**`log_reader_t`** (`log_reader.h`, `log_reader.c`) — reads binary logs. Walks records in logical order (oldest-first, wrap-aware). Time-range queries via `log_reader_seek_ns` using the index fast path, linear scan fallback. Reconstructs the registry from the header snapshot.

**`replay`** (`replay.h`, `replay.c`) — reads a log and feeds packets through ingest at three speeds (`REPLAY_SPEED_MAX`, `REPLAY_SPEED_REAL`, `REPLAY_SPEED_SCALED`). Window filtering via `from_ts`/`to_ts`. Optional looping for soak testing.

**`baseline_t`** (`baseline.h`, `baseline.c`) — the model of "normal." Fixed 64-byte header with magic, version, halflife, regime channel ids. Per-channel table (`BASELINE_MAX_CHANNELS = 256`). Per-(channel, regime) statistics table (`BASELINE_MAX_REGIMES = 16` each). Operations: `baseline_init`, `baseline_update` (Welford accumulation per valid sample), `baseline_decay` (exponential aging), `baseline_get`, `baseline_save`/`baseline_load` (atomic file I/O with CRC). ~166 KB static.

**`event_t`, `event_to_json`** (`event.h`, `event.c`) — the anomaly event struct and its JSONL encoder. Enum-to-string helpers, timestamp split/join helpers. Events are 48 bytes packed, no variable-length fields.

**`detect_t`** (`detect.h`, `detect.c`) — the six detectors. `detect_init_default(det, source_id)` enables all. `detect_update(det, packet, ingest, baseline, registry, events, cap)` runs each enabled detector against each sample (and pair), appends events. Per-channel state (`range_in_violation`, `zscore_in_violation`, `flatline_in_violation`, `roc_in_violation`). Per-pair state (`pair_state[]` for correlation, `mismatch_state[]` for mismatch with `consecutive` counter). Each detector emits one STARTED per episode, one CLEARED per deactivation.

The six detectors:

- **range** — sample outside `[min,max]` from the registry. No baseline needed.
- **zscore** — `|observed - mean| > threshold * stddev` for the current regime. Requires READY baseline.
- **flatline** — last 32 samples for a channel are all identical within 1e-6.
- **roc** — mean absolute rate over last 16 sample pairs exceeds `roc_absolute_max`.
- **correlation** — declared pairs `(a,b)` expected to positively correlate; fires when Pearson r < threshold over 32 samples.
- **mismatch** — declared pairs `(ctrl_out, actuator)`; fires when `|ctrl - act| > threshold` for N consecutive samples.

### Executables and tools

**`gardener`** (`src/main.c`) — the main entry point. Three modes: stdin (default), replay (`--replay`), and any combination of optional state. Optional persistence: `--log` writes packets to a binary log, `--baseline` loads or creates a baseline and updates on every packet. Optional detection: `--detect` runs all six detectors, `--events` writes resulting events to JSONL, `--detect-pair` and `--detect-mismatch` configure pairs. If any persistence or detection sink fails to open, `gardener` logs a warning and continues.

**`gardener-dump`** (`tools/gardener_dump.c`) — `--config <path>` prints the registry as a table; `--feed <path>` feeds a JSONL file through ingest and prints the snapshot.

**`gardener-log`** (`tools/gardener_log.c`) — reads a binary log and prints records as a table or JSONL. `--from`/`--to` filter by timestamp using the index. `--header` shows the header and registry without records.

**`gardener-stats`** (`tools/gardener_stats.c`) — reads a baseline file and prints per-channel, per-regime statistics. `--registry` resolves channel names. `--channel`/`--regime` filter. `--ready-only` shows only READY slots. `--jsonl` for machine consumption.

**`gensim`** (`tools/gensim/`) — a Python simulated source. Emits JSONL packets on stdout driven by a scenario file with fault injection. Plant models (`generic`, `hvac`), time acceleration (`--speed 10`), and scripted events (`freeze`, `spike`, `drift`, `link_gap`). See `tools/gensim/README.md`.

## What does not exist yet

- **Policy engine** (Phase E) — no config-driven rules, no actions, no safety rails. Events are produced but nothing acts on them.
- **Dashboard** (Phase F) — no PC link, no UI, no real-time view.
- **Real Modbus link** (later) — targets are simulated via `gensim`.

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
echo '{"t":1,"v":{"1":20.5}}' | ./build/gardener
```

### Write a log

```
echo '{"t":1,"v":{"1":20.5}}' | ./build/gardener --log run.bin --log-fresh
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

The two rows for `coolant_temp` are the two regimes. Each has its own baseline. Detectors compare live samples against the baseline for the *current* regime.

### Run detection against a baseline

```
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/sensor_drift.yaml \
    --duration 300 --speed 10 |
    ./build/gardener --channels configs/channels.toml \
                     --baseline normal.bl \
                     --detect --events drift_events.jsonl
```

`drift_events.jsonl` now contains anomaly events, one JSON per line. `[event]` lines also print to stderr, and a `[detect]` summary at exit shows per-detector counts.

### Detection with correlation and mismatch pairs

```
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/pump_cavitation.yaml \
    --duration 300 --speed 10 |
    ./build/gardener --channels configs/channels.toml \
                     --baseline normal.bl \
                     --detect --events cav_events.jsonl \
                     --detect-pair 1,2 \
                     --detect-mismatch 11,20
```

### Replay a log with detection

```
./build/gardener --replay normal.bin \
    --channels configs/channels.toml \
    --baseline normal.bl \
    --detect --events replay_events.jsonl
```

### Windows note

PowerShell's `>` re-encodes stdout as UTF-16LE on PowerShell 5.x, which `gardener` cannot parse. Redirect through `cmd.exe`:

```
cmd /c "py tools\gensim\gensim.py --scenario ... --duration 300 > drift.jsonl"
```

Or use PowerShell 7 (`pwsh`), whose `>` defaults to UTF-8. Piping is not affected; only file redirection.

## Options reference

### gardener

```
--channels <path>          channel registry (default: configs/channels.toml)
--stats-every <n>          print counters every n packets
--verbose                  log at DEBUG level
--quiet                    log at WARN level
--version                  print version
--help                     print usage

--log <path>               write ingested packets to a binary log
--log-max <bytes>          log size, accepts K/M/G suffix (default 100M, min 1M)
--log-fresh                truncate an existing log instead of appending

--baseline <path>          load or create a baseline model
--baseline-fresh           start from empty, ignore existing file
--baseline-halflife <s>    decay half-life in seconds (default: 300)
--baseline-save-every <s>  periodic save interval (default: 0 = exit only)

--detect                   run detectors on every packet
--events <path>            write events to this JSONL file
--detect-pair <a,b>        correlation pair (repeatable)
--detect-mismatch <c,a>    mismatch pair (ctrl_out,actuator; repeatable)
--detect-zscore <n>        z-score threshold in sigma (default: 5.0)
--detect-roc <rate>        ROC threshold in units/second (default: 1000)
--detect-corr <r>          correlation threshold (default: 0.5)
--detect-mismatch-thresh <d>  mismatch abs-diff threshold (default: 5.0)

--replay <path>            replay a binary log instead of stdin
--replay-speed <mode>      max (default), real, or a positive factor
--replay-from <ts_ns>      start at this timestamp
--replay-to <ts_ns>        stop before this timestamp
--replay-loop              replay until interrupted
--replay-progress <n>      print progress every n packets
```

### gardener-log

```
--file <path>              binary log to read
--from <ts_ns>             start at this timestamp
--to <ts_ns>               stop before this timestamp
--limit <n>                print at most n records
--jsonl                    output as JSONL instead of a table
--header                   print header and registry, then exit
--quiet                    suppress warnings
--help                     print usage
```

### gardener-dump

```
--config <path>            channel registry to print (no feed)
--feed <path>              JSONL file to feed through ingest
--channels <path>          channel registry for feed mode
--counts                   include per-channel sample count column
--quiet                    suppress warnings
--help                     print usage
```

### gardener-stats

```
--file <path>              baseline file to read (required)
--registry <path>          optional; resolves channel names
--channel <id>             only show this channel id
--regime <n>               only show this regime index
--jsonl                    machine-readable output
--ready-only               only show READY slots
--quiet                    suppress warnings
--help                     print usage
```

## Test

```
cd build && ctest --output-on-failure
```

Twenty-six test suites currently. For a detailed breakdown of what each suite checks and why, see `tests/TESTS.md`.

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
| test_event_format | event | struct sizes, enum values, helpers |
| test_event_json | event | JSON encoding, escaping, sentinels |
| test_detect_range | detect | range detector, STARTED/CLEARED |
| test_detect_zscore | detect | z-score, READY gating, severity |
| test_detect_flatline | detect | flatline detection, clear |
| test_detect_roc | detect | rate-of-change threshold |
| test_detect_correlation | detect | pair decoupling, recovery |
| test_detect_mismatch | detect | sustained divergence, per-pair state |
| test_main_detect | main | CLI detection wiring, events file |

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

CRC-16/MODBUS: poly 0x8005 reflected as 0xA001, init 0xFFFF, refin/refout true, xorout 0x0000. Check value of "123456789" is 0x4B37.

## Log format

The binary log, locked as of B0:

- Preallocated at a fixed size (default 100 MB, min 1 MB).
- Fixed 4096-byte header with magic, version, offsets, counters, timestamps, CRC.
- JSONL registry snapshot after the header.
- Variable-size records, each one `packet_encode` output.
- Sidecar index (`.idx`) with sparse timestamp→offset entries every 256 records.
- Wraps to `data_start` when full, overwriting oldest. `wrap_count` tracks wraps.

## Baseline format

The baseline file, locked as of C4:

- Fixed size: 64-byte header + channel table (256 * 8) + stats table (256 * 16 * 40) + 4-byte CRC = 165,956 bytes.
- Header: magic (0x42415345 "BASE"), version (1), size, halflife, regime channel ids, timestamps.
- Channel table: `{u16 ch_id, u16 n_regimes_used, u32 _pad}` per slot.
- Stats table: `baseline_stat_t` per (channel, regime), 40 bytes each.
- CRC-16/MODBUS trailer over everything before it.
- Atomic write: temp file + rename.

## Event format

Events are written as JSONL, one per line:

```
{"t":<event_ns>,"src_t":<source_ns>,"ch":<id>,"ch2":<id|null>,"regime":<n|null>,
 "detector":"<name>","severity":"<info|warn|critical>","state":"<started|cleared>",
 "observed":<float>,"mean":<float>,"stddev":<float>,"threshold":<float>,
 "msg":"<human-readable>"}
```

Detector names: `range`, `zscore`, `flatline`, `roc`, `correlation`, `mismatch`. Severity: `info`, `warn`, `critical`. State: `started`, `cleared`. `ch2` and `regime` are `null` when not applicable (single-channel detector, no regime).

## Design notes

### Data model invariants

- `sample_t` is 24 bytes in memory, 18 bytes on the wire. Both sizes asserted by tests.
- `event_t` is 48 bytes packed. Size and field offsets asserted by tests.
- `ROLE_UNKNOWN = 0`. Zero-initialized descriptors mean "unknown", not "sensor".
- Unknown channel ids get synthesized into the registry on first sight and cached.
- Every sample has exactly one of `CH_FLAG_VALID` or `CH_FLAG_STALE`.
- Per-sample timestamps win over packet timestamps; zero means inherit.
- `PACKET_FLAG_SIMULATED` propagates to `CH_FLAG_SIMULATED`.

### Constants and bounds

- `REGISTRY_MAX_CHANNELS = 256`, arena 64 B/channel.
- `ingest_t` is ~6.3 MB static. Must be `static` or global, not on the stack.
- `baseline_t` is ~166 KB static. Same rule.
- `INGEST_RING_CAP = 1024` samples per channel (~102 s at 10 Hz).
- `PACKET_MAX_CH = 64`. Keeps `packet_t` around 1.5 KB.
- `BASELINE_MAX_CHANNELS = 256`, `BASELINE_MAX_REGIMES = 16`, `BASELINE_MIN_SAMPLES = 100`.
- `DETECT_MAX_CHANNELS = 256`, `DETECT_MAX_PAIRS = 8`, `DETECT_MAX_MISMATCH_PAIRS = 8`, `DETECT_EVENT_BUF = 32`.

### Codecs

- `packet_from_json` honors explicit `"type"` for both tag and union bytes. Inferred types pick i32 for whole numbers, f32 otherwise.
- `packet_to_json` returns needed length, even on truncation.
- `find_key` stops at the end of the current object — keys in later entries don't leak into earlier parses.
- Wire format is little-endian, byte-by-byte, independent of host endianness.
- `packet_decode` returns bytes consumed, allowing stream walking.
- CRC is CRC-16/MODBUS, not CRC-16/ARC (check value 0x4B37 pins this).

### Log

- Preallocated at fixed size, wraps in place.
- `log_writer_open` refuses to open a file with a bad header unless `start_fresh`.
- `log_reader_seek_ns` positions so a following `log_reader_next` returns the first record with `ts >= target`.
- Logging failures are non-fatal — ingest continues, failure reported at exit.

### Baseline

- Welford maintains `count`/`mean`/`m2` incrementally. Variance = `m2 / (count - 1)`.
- Regime derivation: FNV-1a hash of quantized regime channel values, mixed with MurmurHash3 finalizer, modulo `BASELINE_MAX_REGIMES`. Same values → same index. Different values may collide.
- Regime derived once per packet, not per sample.
- Missing regime channel → whole packet skipped.
- `baseline_update` is pure Welford accumulation. Decay is separate. `count` equals "samples fed" before any decay.
- `baseline_decay` shrinks `count` and `m2` by `2^(-dt/halflife)`. Mean preserved.
- READY → STALE when decayed count drops below `BASELINE_MIN_SAMPLES`. STALE → READY when new samples cross back over.
- `baseline_update` must use the packet's timestamp for decay, not `plat_wall_ns()`. Mixing epochs decays to zero silently.
- `baseline_load` zeroes `*b` on failure.

### Detection

- Six detectors: range, zscore, flatline, roc, correlation, mismatch.
- Each emits one STARTED per episode, one CLEARED per deactivation. No ONGOING.
- Per-channel state for single-channel detectors, per-pair state for correlation and mismatch.
- zscore requires READY baseline for the current regime. EMPTY/LEARNING/STALE baseline → no fire.
- flatline requires 32 samples in ring. roc requires 16 pairs. correlation requires 32 samples in each channel. No false positives during warm-up.
- mismatch requires N consecutive samples over threshold (`mismatch_sustained`, default 10). A single transient doesn't fire.
- Detector failures (buffer full) drop events and count them, but don't stop ingest.

### Platform

- No OS headers outside `src/platform/`.
- `plat_log` writes to stderr only. Stdout is for machine-readable output.

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
  event.h             event struct, enums, JSONL encoder
  detect.h            detector config, state, update API

src/                implementation
  main.c              entry point (stdin / replay / logging / baseline / detect)
  ring.c
  registry.c
  ingest.c
  packet.c
  log_writer.c
  log_reader.c
  replay.c
  baseline.c
  event.c
  detect.c
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

`configs/channels.toml` describes every channel. One `[[channel]]` block per signal:

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

- `id` — required, 0..65535. Wire identifier.
- `name` — required, up to 31 chars.
- `unit` — optional, up to 15 chars.
- `type` — f32, i32, u16, bool, enum (default f32).
- `role` — sensor, setpoint, ctrl_out, actuator, status, counter. Defaults to unknown.
- `min`, `max` — optional sanity range.
- `controllable` — true/false (default false). May Gardener write to it?
- `is_regime` — true/false (default false). Marks as regime selector.

The parser handles a small TOML subset: `[[table]]` headers and `key = value` lines with quoted strings, numbers, booleans. Trailing comments on value lines are not stripped. Nested tables and arrays are not supported.

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

Phase D — detection                          done
  D0 event format
  D1 range + z-score detectors
  D2 rate-of-change + flatline
  D3 correlation pairs
  D4 CTRL_OUT vs ACTUATOR mismatch
  D5 wire into gardener, events file

Phase E — policy engine                      next
  config-driven rules, notify actions

Phase F — loopback                           planned
  target stub, dashboard stub, closed loop

Later — real Modbus link, dashboard, MCU port
```