# Gardener

A system supervisor: learns what "normal" looks like for a multi-channel system, flags anomalies, executes policy actions, and streams live telemetry to a dashboard.

## Status

Phase F complete. All core phases (A through F) are done. 

The system ingests packets, keeps per-channel history, writes and reads binary logs, replays logs through ingest at any speed, learns per-channel per-regime statistics with exponential decay, persists baselines across restarts, runs six anomaly detectors on every packet, evaluates policy rules to execute actions (notifications, logs, webhooks), and serves a live JSONL stream over TCP for external dashboards.

## The problem it solves, concretely

A pump controller holds flow at 10 L/min. It succeeds: flow is 10 L/min, always. But the pump's current draw has crept from 2.1 A to 3.8 A over three weeks. The controller is happy — flow is right. The operator doesn't notice — nothing is broken. The pump is 90% through its bearing life and the failure is going to be sudden.

Gardener's job is to notice these things. It watches the controller's internals (setpoint, PID output, actuator command) alongside the physical measurements. It builds a model of "normal" for each operating regime. It flags when a value drifts outside what normal looks like, and it can immediately take action (like triggering a webhook, logging to a file, or notifying a live dashboard).

## How it fits together

```text
+------------------+      JSONL       +-----------------------------+
| gensim (Python)  | ---------------> |  gardener (C)               |
| simulated source |                  |                             |
+------------------+                  |  ingest_feed(packet_t)      |
                                      |                             |
+------------------+      JSONL       |  +--------------------+     |
| any JSONL source | ---------------> |  | registry_t         |     |
| (real or test)   |                  |  |  id -> descriptor  |     |
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
         |                            |  +--------------------+     |
         |                            |  | policy_engine_t    |     |
         |                            |  |  rules -> actions  |     |
         |                            |  +--------------------+     |
         |                            |            |                |
         |                            |            v                |
         |                            |  events.jsonl (disk)        |
         |                            |  JSONL TCP (port 5555)      |
         |                            +-----------------------------+
         |                                          |
         v                                          v
+------------------+                    +-----------------------+
| log_reader       |                    | Python Dashboard      |
| gardener-log     |                    | (FastAPI + htmx)      |
| gardener --replay|                    | http://localhost:8001 |
+------------------+                    +-----------------------+
```

### The five data structures to know

**`channel_desc_t`** — a static description of one signal, loaded at startup from `channels.toml`. Contains `id` (uint16, wire identifier), `name` (up to 31 chars), `unit`, `type` (f32/i32/u16/bool/enum), `role` (sensor/setpoint/ctrl_out/actuator/status/counter), `controllable` (may Gardener write to it?), `is_regime` (does it select the operating regime?), and optional `min_valid`/`max_valid` bounds.

The `role` field is the key insight that makes Gardener different from a generic data logger. A PID loop produces four distinct kinds of signals: measurement (sensor), desired value (setpoint), controller's effort (ctrl_out), what the actuator does (actuator). Each has different "normal" behavior. A supervisor that doesn't know the difference will confuse a saturated controller for a healthy one.

**`sample_t`** — one reading of one channel. 24 bytes in memory, 18 bytes on the wire. Contains `channel_id`, `type`, `role`, `flags` (VALID / STALE / SIMULATED), value (union of f32/i32/u16/bool), `timestamp_ns`. A sample is atomic: `VALID` means fresh, `STALE` means last-known-good and ignored by detectors, `SIMULATED` means test source.

**`packet_t`** — one tick's worth of samples from one source. Up to 64 channels. Two encodings: JSONL for dev feeds and simulated sources, binary (22 + 18×N bytes, CRC-16/MODBUS trailer) for logs and the eventual target link. A packet is a coherent snapshot: all its samples were taken at the same moment. This is why `baseline_regime_for_packet` derives the regime once per packet.

**`baseline_stat_t`** — the statistics for one channel in one regime. 40 bytes. Contains `count`, `mean`, `m2`, `min_val`, `max_val`, `first_ts_ns`, `last_ts_ns`, `state`, `flags`. Welford maintains `count`/`mean`/`m2` incrementally. Variance = `m2 / (count - 1)`. Numerically stable even for large means with small variances.

**`event_t`** — one anomaly event. 48 bytes, packed. Contains two timestamps (event and source), `channel_id` and `channel_id2` (for pair detectors), `regime`, `observed`, `baseline_mean`, `baseline_stddev`, `threshold`, `detector`, `severity`, `state` (STARTED/CLEARED), `version`, `crc`. One STARTED per episode, one CLEARED per deactivation. No ONGOING events.

**`policy_rule_t`** — a user-defined rule (loaded from TOML). Matches events based on detector type, severity, and state. Contains a cooldown timer and a list of actions (`notify`, `log`, `webhook`).

## What exists right now

### Library

**Platform layer** (`platform.h`, `platform_posix.c`) — the only code that touches the OS directly. `plat_now_ns()`, `plat_wall_ns()`, `plat_sleep_ms()`, `plat_file_open/read/write/seek/tell/sync/close()`, `plat_log()`. Nothing outside `src/platform/` calls OS APIs; the port to an MCU will be a folder swap.

**Channel model** (`channel.h`) — `ch_type_t`, `ch_role_t`, `channel_desc_t`, `sample_t`, `sample_as_f32`. Header-only or inline.

**`ring_t`** (`ring.h`, `ring.c`) — fixed-capacity sample ring. Push appends, `ring_at(i)` indexes (0=oldest), wrap overwrites oldest. No allocation after `ring_init`.

**`registry_t`** (`registry.h`, `registry.c`) — loads channel descriptors from TOML. id→descriptor lookup, unknown ids get synthesized placeholders. String storage in a fixed arena; no heap.

**Packet codecs** (`packet.h`, `packet.c`) — JSONL (`packet_from_json`, `packet_to_json`) for the dev feed and binary (`packet_encode`, `packet_decode`, `packet_crc16`) with CRC-16/MODBUS for logs and the future wire. Both preserve all fields.

**`ingest_t`** (`ingest.h`, `ingest.c`) — the engine that turns packets into per-channel history. Embedded `registry_t`, one `ring_t` per channel (`INGEST_RING_CAP = 1024`), latest-value snapshot per channel, counters. `ingest_feed(packet)` normalizes each sample and pushes into the ring. ~6.3 MB static. No heap.

**`log_writer_t`** (`log.h`, `log_writer.h`, `log_writer.c`) — writes binary logs. Preallocates 100 MB, wraps in place. Fixed 4096-byte header with magic, version, offsets, counters, CRC. JSONL registry snapshot after header. Variable-size records. Sidecar index (`.idx`) with sparse timestamp→offset entries. Refuses to open a file with a bad header unless `start_fresh`.

**`log_reader_t`** (`log_reader.h`, `log_reader.c`) — reads binary logs. Walks records in logical order (oldest-first, wrap-aware). Time-range queries via `log_reader_seek_ns`. Reconstructs the registry from the header snapshot.

**`replay`** (`replay.h`, `replay.c`) — reads a log and feeds packets through ingest at three speeds (`REPLAY_SPEED_MAX`, `REPLAY_SPEED_REAL`, `REPLAY_SPEED_SCALED`). Window filtering via `from_ts`/`to_ts`. Optional looping.

**`baseline_t`** (`baseline.h`, `baseline.c`) — the model of "normal." Fixed 64-byte header with magic, version, halflife, regime channel ids. Per-channel table (`BASELINE_MAX_CHANNELS = 256`). Per-(channel, regime) statistics table. Operations: `baseline_init`, `baseline_update`, `baseline_decay`, `baseline_get`, `baseline_save`/`baseline_load`. ~166 KB static.

**`event_t`, `event_to_json`** (`event.h`, `event.c`) — the anomaly event struct and its JSONL encoder. Enum-to-string helpers, timestamp split/join helpers. Events are 48 bytes packed.

**`detect_t`** (`detect.h`, `detect.c`) — the six detectors. `detect_init_default` enables all. `detect_update` runs each enabled detector, appends events. Detectors: `range`, `zscore`, `flatline`, `roc` (rate of change), `correlation`, `mismatch`. Each emits one STARTED per episode, one CLEARED per deactivation.

**`policy_engine_t`** (`policy.h`, `policy.c`) — parses a TOML file into rules. Evaluates incoming events against the rules (with cooldown debouncing). Executes actions (`notify`, `log`, `webhook`). Has a `dry_run` mode for safety.

**`dashboard_server_t`** (`dashboard.h`, `dashboard.c`) — non-blocking TCP server. Listens on a given port. Pushes the channel registry to newly connected clients, and streams JSONL events to all connected clients as they happen.

### Executables and tools

**`gardener`** (`src/main.c`) — the main entry point. Three modes: stdin (default), replay (`--replay`), and any combination of optional state. Optional persistence: `--log` writes packets to a binary log, `--baseline` maintains a model of normal. Optional detection: `--detect` runs all six detectors, `--events` writes resulting events to JSONL. Optional policy: `--policy` loads rules, `--policy-dry-run` tests them. Optional dashboard: `--dashboard <port>` serves live TCP data.

**`gardener-dump`** (`tools/gardener_dump.c`) — `--config <path>` prints the registry as a table; `--feed <path>` feeds a JSONL file through ingest and prints the snapshot.

**`gardener-log`** (`tools/gardener_log.c`) — reads a binary log and prints records as a table or JSONL. `--from`/`--to` filter by timestamp using the index.

**`gardener-stats`** (`tools/gardener_stats.c`) — reads a baseline file and prints per-channel, per-regime statistics. `--ready-only` shows only slots that crossed the threshold.

**`gensim`** (`tools/gensim/`) — a Python simulated source. Emits JSONL packets driven by a scenario file with fault injection. Plant models (`generic`, `hvac`), time acceleration, and scripted events (`freeze`, `spike`, `drift`, `link_gap`). 

**`dashboard`** (`tools/dashboard/`) — a Python FastAPI web application. Connects to the `--dashboard` TCP stream and renders live anomalies in a browser using Jinja2 and htmx.

## What does not exist yet

- **Real Modbus link** (later) — targets are simulated via `gensim` or standard JSONL feeds.
- **MCU Port** (V2) — while the C code is highly portable, the actual hardware deployment is a future phase.

## Build

Requires a C11 compiler and CMake 3.16+. Python 3.8+ for `gensim` and the dashboard.

```bash
cmake -B build
cmake --build build
```

## Run

### 1. Build a Baseline (Learn "Normal")

```bash
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/normal.yaml --duration 300 --speed 10 | 
    ./build/gardener --channels configs/channels.toml --baseline normal.bl --baseline-fresh
```

### 2. Run Detection (and write to a file)

```bash
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/sensor_drift.yaml --duration 300 --speed 10 |
    ./build/gardener --channels configs/channels.toml --baseline normal.bl --detect --events drift_events.jsonl
```

`drift_events.jsonl` now contains anomaly events. `[event]` lines also print to stderr, and a `[detect]` summary at exit shows per-detector counts.

### 3. Execute Policies

Create a `policy.toml`:

```toml
[[rule]]
name = "critical_coolant"
detector = "zscore"
severity = "critical"
state = "started"
cooldown = "30s"

[[rule.action]]
type = "notify"
target = "Coolant temperature is critically out of bounds!"

[[rule.action]]
type = "log"
target = "critical_events.log"
```

Run with the policy engine:

```bash
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/sensor_drift.yaml --duration 300 --speed 10 |
    ./build/gardener --channels configs/channels.toml --baseline normal.bl --detect --policy policy.toml
```
*(Add `--policy-dry-run` to see what would happen without executing webhooks or file logs).*

### 4. Live Dashboard

**Terminal 1 (Python):**
```bash
cd tools/dashboard
py -m pip install -r requirements.txt
py dashboard.py
```

**Terminal 2 (Gardener):**
```bash
py tools/gensim/gensim.py --scenario tools/gensim/scenarios/sensor_drift.yaml --duration 60 --speed 5 |
    ./build/gardener --channels configs/channels.toml --baseline normal.bl --detect --dashboard 5555
```
Open `http://localhost:8001` in your browser to watch the anomalies stream in.

### Windows note

PowerShell's `>` re-encodes stdout as UTF-16LE on PowerShell 5.x, which `gardener` cannot parse. Redirect through `cmd.exe`, use `Get-Content` for file piping, or use PowerShell 7 (`pwsh`). Piping (`|`) directly from `py` to `gardener` is not affected.

## Options Reference

### gardener

```text
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

--policy <path>            load policy rules from a TOML file
--policy-dry-run           evaluate rules but do not execute actions

--dashboard <port>         start a TCP JSONL server for live monitoring

--replay <path>            replay a binary log instead of stdin
--replay-speed <mode>      max (default), real, or a positive factor
--replay-from <ts_ns>      start at this timestamp
--replay-to <ts_ns>        stop before this timestamp
--replay-loop              replay until interrupted
--replay-progress <n>      print progress every n packets
```

### gardener-log

```text
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

```text
--config <path>            channel registry to print (no feed)
--feed <path>              JSONL file to feed through ingest
--channels <path>          channel registry for feed mode
--counts                   include per-channel sample count column
--quiet                    suppress warnings
--help                     print usage
```

### gardener-stats

```text
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

```bash
cd build && ctest --output-on-failure
```

There are 31 test suites currently, covering every module from A to F. For a detailed breakdown of what each suite checks and why, see `tests/TESTS.md`.

## Wire format

The binary packet format, locked as of A6:

```text
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

```json
{"t":<event_ns>,"src_t":<source_ns>,"ch":<id>,"ch2":<id|null>,"regime":<n|null>,
 "detector":"<name>","severity":"<info|warn|critical>","state":"<started|cleared>",
 "observed":<float>,"mean":<float>,"stddev":<float>,"threshold":<float>,
 "msg":"<human-readable>"}
```

Detector names: `range`, `zscore`, `flatline`, `roc`, `correlation`, `mismatch`. Severity: `info`, `warn`, `critical`. State: `started`, `cleared`. `ch2` and `regime` are `null` when not applicable.

## Design Notes

### Data Model
- `sample_t` is 24 bytes in memory, 18 bytes on the wire. Both sizes asserted by tests.
- `event_t` is 48 bytes packed. Size and field offsets asserted by tests.
- `ROLE_UNKNOWN = 0`. Zero-initialized descriptors mean "unknown", not "sensor".
- Unknown channel ids get synthesized into the registry on first sight and cached.

### Constants and Bounds
- `REGISTRY_MAX_CHANNELS = 256`, arena 64 B/channel.
- `ingest_t` is ~6.3 MB static. Must be `static` or global, not on the stack.
- `baseline_t` is ~166 KB static. Same rule.
- `INGEST_RING_CAP = 1024` samples per channel (~102 s at 10 Hz).
- `BASELINE_MAX_CHANNELS = 256`, `BASELINE_MAX_REGIMES = 16`, `BASELINE_MIN_SAMPLES = 100`.

### Codecs
- `packet_from_json` honors explicit `"type"` for both tag and union bytes. Inferred types pick i32 for whole numbers, f32 otherwise.
- Wire format is little-endian, byte-by-byte, independent of host endianness.
- CRC is CRC-16/MODBUS, not CRC-16/ARC.

### Logging & Replay
- Preallocated at fixed size, wraps in place.
- `log_writer_open` refuses to open a file with a bad header unless `start_fresh`.
- `log_reader_seek_ns` positions so a following read returns the first record with `ts >= target`.

### Baseline & Regimes
- Welford maintains `count`/`mean`/`m2` incrementally. Variance = `m2 / (count - 1)`.
- Regime derivation: FNV-1a hash of quantized regime channel values, mixed with MurmurHash3 finalizer, modulo `BASELINE_MAX_REGIMES`.
- `baseline_decay` shrinks `count` and `m2` by `2^(-dt/halflife)`. Mean preserved.
- `baseline_update` must use the packet's timestamp for decay, not `plat_wall_ns()`. Mixing epochs decays to zero silently.

### Detection & Policy
- Six detectors: range, zscore, flatline, roc, correlation, mismatch.
- Each emits one STARTED per episode, one CLEARED per deactivation.
- zscore requires READY baseline for the current regime.
- Policy engine evaluates rules synchronously in the ingest loop. Execution failures (e.g., webhook times out) are logged but do not block ingest.
- `--policy-dry-run` ensures safe evaluation before deploying to production.

### Dashboard
- The C server is non-blocking. If a client disconnects or falls behind, events are dropped from the socket buffer, but never from disk or the main loop.
- The Python UI polls the background socket thread every 1 second using htmx to update the DOM.

## Roadmap

```text
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

Phase E — policy engine                      done
  E0 rules format
  E1 matching & cooldowns
  E2 action execution
  E3 CLI integration
  E4 E2E testing

Phase F — dashboard                          done
  F0 TCP server
  F1 Python FastAPI + htmx UI

Later — real Modbus link, MCU port
```