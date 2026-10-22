# Tests

One test executable per module. Each is a small C program with a
minimal CHECK/CHECK_EQ/CHECK_NEAR macro set and a `main()` that runs
every case. No test framework, no fixtures directory, no test runner
beyond CTest.

The tests exist to lock in behavior, not to prove the code works. When
the code and a test disagree, one of them is wrong, and the question
is which one is the source of truth. Usually it's the test — it
encodes what callers rely on. When the docstring and the test disagree
too, the test wins.

Running:

    cd build && ctest --output-on-failure

Or run a single suite:

    cd build
    ./test_ring

All tests are also runnable directly; the CTest registration just
invokes the same binary.

---

## test_platform

**What it tests:** the platform layer (`src/platform/platform_posix.c`
and `platform.h`), which is the only module that calls OS APIs
directly. Everything else in the codebase goes through it, so a bug
here looks like a bug everywhere.

**Why it exists:** to catch portability and infrastructure problems
before they propagate. Time going backward, sleeps returning instantly,
files not round-tripping — any of these would be extremely hard to
diagnose from a higher-level failure.

### Cases

**Monotonic time moves forward.**
`plat_now_ns()` is what every sample timestamp eventually traces back
to. If it is not strictly monotonic, ordering samples and computing
rates are both undefined. The test sleeps 20 ms and asserts the clock
advanced by at least 15 ms (allowing for scheduler jitter) and less
than 10 s (catching a clock that jumped to epoch).

**Wall clock is plausible.**
`plat_wall_ns()` is used in log headers where we want timestamps
comparable across reboots. Asserts the value is after 2020-01-01 in
nanoseconds. Fails loudly if the RTC is unset.

**Sleep returns on time.**
`plat_sleep_ms(30)` must actually sleep. If it returned immediately,
all polling loops in later phases would busy-spin.

**File write/read round-trip.**
Writes a known payload, reads it back, checks content, and confirms
the next read reports EOF.

**File seek and tell.**
Writes a 10-byte file, reads it in pieces, seeks to specific offsets,
and verifies both the returned data and the position reported by
`plat_file_tell`. Added in B1 when the log writer needed seek.

**Missing file returns NULL.**
Opening a nonexistent path returns NULL, not a half-initialized
handle.

**NULL safety.**
Every file function (`open`, `read`, `write`, `seek`, `tell`, `sync`,
`close`) handles NULL without crashing.

**Log levels filter.**
Setting the level to ERROR and calling `plat_log(LOG_DEBUG, ...)`
must not print.

---

## test_ring

**What it tests:** `ring_t` (`src/ring.c`), the fixed-capacity sample
ring that backs every channel in `ingest_t`.

**Why it exists:** the ring is where all historical data lives. If
its indexing or wrap behavior is wrong, every downstream observation
is wrong.

### Cases

**Bad arguments rejected at init.**

**Push under capacity preserves order.**

**Push exactly to capacity.**
Boundary condition.

**Wrap overwrites oldest.**
The core invariant.

**Wrap twice.**
Modular arithmetic after multiple laps.

**Clear.**
Reset without reallocation.

**Capacity 1.**
Degenerate case.

**`sample_as_f32` for every type.**
Detector contract.

**`sample_t` is 24 bytes with fixed field offsets.**
A static assertion. The ingest storage math is built into the type,
and the A6 wire format packs the same fields in a different order. If
someone adds a field and the struct grows to 32 bytes, this test fails
before the size change propagates into the storage array.

---

## test_registry

**What it tests:** `registry_t` (`src/registry.c`), the config loader
that turns `channels.toml` into the descriptor table everything else
reads.

**Why it exists:** the registry is the source of truth for what a
channel means. Every downstream decision reads from here.

### Cases

**Empty file loads cleanly.**

**Missing file is a hard error.**

**Full channel with every field.**

**Multiple channels.**

**Comments and blank lines.**
Also documents the trailing-comment limitation (not stripped).

**Unknown key warns and counts.**

**Unknown type and role.**
Falls back to defaults.

**Missing name drops the channel.**

**`registry_get_or_default` synthesizes and caches.**

**`registry_get_or_default` returns existing.**

**Capacity limit.**

---

## test_ingest

**What it tests:** `ingest_t` (`src/ingest.c`), the engine that turns
packets into per-channel history.

**Why it exists:** ingest is where the codec layer meets the storage
layer. It is the single entry point for all data, and every counter
it maintains (`packets_seen`, `samples_seen`, `samples_dropped`,
`unknown_channels`) is a metric the rest of the system will key on.

### Cases

**NULL arguments rejected.**

**Init without config.**

**Single-channel round-trip.**

**Three channels in one packet.**

**Ring accumulates.**

**Ring wraps.**
Feeds `INGEST_RING_CAP + 5` samples and confirms `ring_oldest` is
sample 5 and `latest` is sample 1028.

**Unknown channel synthesized.**

**Unknown channel cached.**
Counts feeds, not registrations. A decision, not an accident.

**Timestamp inheritance.**

**Simulated flag propagates.**

**Stale flag preserved, valid not set.**
The VALID/STALE invariant.

**Out-of-range id dropped.**

**Oversized packet rejected before counting.**

**Type/role fill-in from descriptor.**

**Mixed known and unknown in one packet.**

**`has_channel` false before any feed.**

**Accessors respect bounds.**

---

## test_packet_json

**What it tests:** the JSONL codec (`packet_from_json`,
`packet_to_json` in `src/packet.c`).

**Why it exists:** JSONL is the dev feed format. It is what
`gensim` produces, what hand-written test input looks like, and what
future external tools will use. It is deliberately tolerant, which
means it is deliberately easy to get subtly wrong.

### Cases

**Decode one f32.**

**Decode multiple channels.**
Caught the `find_key` scoping bug — with a `"type":"bool"` appearing
in entry 3, entries 1 and 2 must not pick it up.

**Explicit type wins.**
Caught a type/union mismatch bug: the parser was correctly setting
the type tag, but the union bytes had already been written by the
type-inference path.

**Shorthand `v` map.**

**Whitespace tolerance.**

**Unknown keys ignored.**

**Key order irrelevant.**

**Negative numbers.**

**Malformed input returns -1.**
Empty string, missing `t`, missing channels, empty arrays, non-JSON.

**Encode basics.**

**Encode bool.**

**Round-trip.**

**NULL and zero-cap.**

**Returns needed length.**
The "count even when truncated" contract. Caught a bug in the
`append` helper: the initial version returned -1 on overflow instead
of counting the needed size.

---

## test_packet_bin

**What it tests:** the binary codec (`packet_encode`, `packet_decode`,
`packet_crc16` in `src/packet.c`).

**Why it exists:** the binary format is the wire format for logs
(Phase B) and the eventual target link (Phase F). It has to be
byte-exact and stable.

### Cases

**Encoded size formula.** `22 + 18 × N`.

**Worst case fits in `PACKET_BIN_MAX`.**

**Round-trip empty.**

**Round-trip one f32.**

**Round-trip all types.** f32, i32, bool, u16, enum.

**Round-trip all flags.**

**Encode NULL and cap-too-small.**

**Encode too many channels.**

**Decode NULL and too-short buffer.**

**Bad magic.**

**Bad version.**

**Bad CRC.**
Bit rot protection.

**Channel count exceeds max.**
Checked independently of CRC.

**Buffer shorter than claimed.**

**Two packets concatenated.**
The stream-walking contract.

**CRC-16/MODBUS check vector.**
`packet_crc16("123456789", 9) == 0x4B37`. Also serves as a naming
check: CRC-16/ARC has a different check value (`0xBB3D`) and an
earlier draft used the wrong name.

---

## test_log_format

**What it tests:** the structural invariants declared in
`include/gardener/log.h` — header and index layout, record size
formula, and the relationship between the log format and
`packet_encode`.

**Why it exists:** the log format is on-disk state. Once written, it
has to be readable by every future version of Gardener.

### Cases

**Constants.**
`LOG_MAGIC`, `LOG_VERSION`, `LOG_HEADER_SIZE`, `LOG_DEFAULT_MAX`,
`LOG_MIN_MAX`, and the index constants.

**Header size and field offsets.**
Every field offset in `log_header_t` is asserted.

**Index header and entry sizes and offsets.**

**Record size formula matches `packet_encode`.**
For N in 0..5 and for `PACKET_MAX_CH`, `log_record_size(N)` must
equal what `packet_encode` actually produces. If `packet_encode` ever
changes its header size or sample size without `log_record_size`
changing too, this test fails immediately.

**Minimum log size holds enough records to be useful.**

**Registry snapshot fits within the default log's budget.**

---

## test_log_writer

**What it tests:** `log_writer_t` (`src/log_writer.c`).

**Why it exists:** the log writer is the first component with real
file I/O and mutable on-disk state. It has more failure modes than
anything before it: partial writes, wrap, header/mutation ordering,
index persistence, header validation on reopen.

### Cases

**Open/create/close.**

**Append one and read back.**

**Append many and read back.** 100 packets in order.

**Reopen appends.**
Two sessions, verifying continuity. **This test caught two bugs:**
- The log writer wrote the index file before opening it, causing
  every open to fail. (Sequencing bug.)
- `last_wall_ns` was being set *after* the header CRC was computed.
  Since `last_wall_ns` is inside the CRC-covered range, every header
  written had an invalid CRC. This wasn't visible until reopen,
  which validated the CRC and refused.

**Wrap overwrites oldest.**

**Refuse invalid header.**
A log with a bad magic must not be silently overwritten without
`start_fresh`.

**Min size refused.**

**NULL args.**

---

## test_log_reader

**What it tests:** `log_reader_t` (`src/log_reader.c`).

**Why it exists:** the reader is the other half of the log contract.
Failures here would silently produce wrong data.

### Cases

**Open missing file returns NULL.**

**Open NULL arg returns NULL.**

**Close NULL is safe.**

**Open empty and iterate.**

**Write/read five.**
Timestamps and values round-trip in order.

**Rewind.**

**Registry snapshot.**
The header's JSONL is reconstructed into a usable `registry_t` with
correct names, units, roles, types, and flags.

**Seek.**
`--from`/`--to` semantics:
- `seek_ns(250)` → next call returns ts=300.
- `seek_ns(500)` → next call returns ts=500.
- `seek_ns(0)` → next call returns ts=0.
- `seek_ns(9999)` → next call returns end-of-log.

**NULL safety.**

---

## test_replay

**What it tests:** the replay engine (`src/replay.c`).

**Why it exists:** replay is how the same recorded data can be fed
through ingest repeatedly. Detector tuning in Phase C depends on it.
Failures here would silently feed the wrong packets or drop others.

### Cases

**Options default.**
`replay_opts_default` resets all fields.

**NULL arguments rejected.**

**Missing file returns -1.**

**Empty log:** zero packets fed, no crash.

**Ten packets:** timestamps and count match, latest value is the last
packet's.

**From/to window:** `from_ts_ns = 250, to_ts_ns = 750` on a 10-packet
log at 100 ns intervals feeds exactly 5 packets (300..700). Verifies
both bounds are respected.

**From-only:** `from_ts_ns = 500` feeds 5 packets (500..900).

**To-only:** `to_ts_ns = 300` feeds 3 packets (0..200).

**Invalid speed factor:** `REPLAY_SPEED_SCALED` with `factor = 0.0`
returns -1.

**Max speed is fast:** 100 packets with 1-second intervals replay in
under 1 second. Confirms `REPLAY_SPEED_MAX` doesn't sleep.

**Not tested:** `REPLAY_SPEED_REAL` and `REPLAY_SPEED_SCALED` with
actual timing. Those introduce real sleeps and would make the test
slow and flaky. The delay computation is simple enough to trust by
inspection; the modes are exercised manually.

---

## test_main_args

**What it tests:** the CLI wiring in `src/main.c` — argument parsing,
the `--log` option's effect, and interaction with `--replay`.

**Why it exists:** the earlier suites test library functions in
isolation. `main` is the integration layer: how arguments map to
behavior, whether the log file is created when asked, whether
failures are reported cleanly. Bugs here would be invisible to the
library tests.

**How it works:** runs the `gardener` executable as a subprocess via
`system()`, checks exit codes and produced files. Not pretty, but it
is the only way to test CLI behavior without restructuring `main`
around an injectable argv parser.

**Platform note:** the paths are relative to `build/`, which is where
CTest runs the test binary:
- `gardener.exe` (the binary itself, in the same directory)
- `../configs/channels.toml` (one level up, not two)

### Cases

**`--version` exits 0.**

**No `--log` flag creates no file.**
Feed one packet via stdin, verify no `.bin` file is created in the
working directory. The opt-in design means `gardener` doesn't write
files unless asked.

**`--log` flag creates a file.**
Feed three packets with `--log <path> --log-fresh`, verify the file
exists, opens with `log_reader`, and contains exactly 3 records.

**`--log-max` below minimum doesn't create a log.**
`--log-max 1000` is below `LOG_MIN_MAX` (1 MB), so `log_writer_open`
fails. `gardener` continues without logging, still exits 0, and no
log file is created.

**Replay + log.**
Session 1: JSONL → log1. Session 2: replay log1 → log2. Both logs
contain 3 records with matching timestamps.

### A test infrastructure note

`test_main_args` uses `system()`, which spawns a shell. It is not
portable to environments without one, and it is slower than the
library-level tests. It is also the test that surfaces bugs the
library tests can't see (CLI argument handling, file creation
side-effects, exit codes).

On failure, `run_gardener` re-runs the command **without** output
redirection so the actual `gardener` stderr appears in the test log.
That is the difference between "test failed with rc=1" (unhelpful)
and "test failed with rc=1, and gardener printed 'cannot open
../configs/channels.toml'" (actionable). When `test_main_args` fails,
look for that re-run output above the FAIL line.

---

## Things not tested yet

Honest list of gaps, so they're not surprising later:

- **Log wrap followed by read.** `test_log_writer` verifies wrap
  writes, but `test_log_reader` doesn't verify reading from a wrapped
  log. The reader supports it (`log_reader_rewind` handles both
  cases) but no test exercises the circular walk.

- **Index seek with a real index.** `test_log_reader`'s seek test
  runs on a 10-record log, which won't have written any index
  entries (`LOG_INDEX_INTERVAL = 256`). So the seek exercises the
  linear-scan fallback, not the index fast path. A test with 300+
  records would exercise the index.

- **`log_reader_prev`.** Not implemented. No test because there's
  nothing to test.

- **Corrupted record mid-log.** The reader is expected to stop at
  the first undecodable record and return -1. No test exercises
  this.

- **`gensim`.** No automated test. It's exercised manually and via
  the pipeline checks in the README. A Python test harness that runs
  each scenario and checks the output against expected patterns
  would catch regressions as scenarios evolve.

- **Replay real-time pacing.** Not tested for timing accuracy.
  Manual verification only.

- **Concurrency.** Nothing is thread-safe today. When that changes,
  the invariants will need to be documented and tested.

---

## Test philosophy

Three rules, applied consistently:

1. **Tests encode caller-visible behavior, not implementation.**
   `test_ring` asserts that pushing six items into a ring of four
   leaves items 2, 3, 4, 5 readable. It does not assert that the
   internal `head` counter equals 2.

2. **Tests use the public API.**
   No test reaches into a struct and inspects a private field. If a
   piece of state matters to callers, it has an accessor.

3. **Every test names the invariant it protects.**
   `test_stale_flag_preserved` says what it's about.
   `CHECK_EQ(x, 5)` inside it says what the invariant is. When the
   test fails six months from now, the reader should be able to tell
   *what* changed without reading the source.

The corollary: when a test fails, the first question is not "how do
I fix the code?" but "which one — test or code — is the source of
truth here?" If the answer is the test, fix the code. If the answer
is the design and the test misstates it, fix the test. If it's
ambiguous, decide which one is right and make the other agree.