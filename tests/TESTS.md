# Tests

One test executable per module. Each is a small C program with a
minimal CHECK/CHECK_EQ/CHECK_NEAR macro set and a `main()` that runs
every case. No test framework, no fixtures directory, no test
runner beyond CTest.

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
nanoseconds. Fails loudly if the RTC is unset, rather than letting
every log line read `1970-01-01`.

**Sleep returns on time.**
`plat_sleep_ms(30)` must actually sleep. If it returned immediately,
all polling loops in later phases would busy-spin and burn CPU.

**File write/read round-trip.**
Writes a known payload, reads it back, checks content, and confirms
the next read reports EOF. Any `plat_file_*` bug would propagate into
the log writer.

**File seek and tell.**
Writes a 10-byte file, reads it in pieces, seeks to specific offsets,
and verifies both the returned data and the position reported by
`plat_file_tell`. Added in B1 when the log writer needed seek.

**Missing file returns NULL.**
Opening a nonexistent path returns NULL, not a half-initialized
handle. The dump tool relies on this to report a clean error.

**NULL safety.**
Every file function (`open`, `read`, `write`, `seek`, `tell`, `sync`,
`close`) handles NULL without crashing. Cheaper to test once here
than to chase a segfault in a later sub-phase.

**Log levels filter.**
Setting the level to ERROR and calling `plat_log(LOG_DEBUG, ...)`
must not print. We do not capture stderr here; the test just verifies
the call returns and does not crash.

---

## test_ring

**What it tests:** `ring_t` (`src/ring.c`), the fixed-capacity sample
ring that backs every channel in `ingest_t`.

**Why it exists:** the ring is where all historical data lives. If
its indexing or wrap behavior is wrong, every downstream observation
is wrong. The test locks the invariants so a refactor cannot silently
change them.

### Cases

**Bad arguments rejected at init.**
`ring_init(NULL, storage, cap)`, `ring_init(&r, NULL, cap)`, and
`ring_init(&r, storage, 0)` all return -1. Callers can rely on a
predictable failure mode.

**Push under capacity preserves order.**
Three pushes, read back 0, 1, 2. If indexing is wrong when the ring
is not yet full, every accumulation is suspect.

**Push exactly to capacity.**
Four pushes into cap 4, read back 0, 1, 2, 3. The boundary between
"not full" and "full" is where off-by-one bugs live.

**Wrap overwrites oldest.**
Six pushes into cap 4, read back 2, 3, 4, 5. This is the invariant
the whole design rests on: capacity is fixed, oldest data ages out,
newest is always available.

**Wrap twice.**
Ten pushes into cap 3, read back 7, 8, 9. Ensures the modular
arithmetic is right after more than one lap.

**Clear.**
After `ring_clear`, count is 0, full is false, and the ring accepts
fresh pushes cleanly. Phase B's replay tool may want to reset rings
between runs.

**Capacity 1.**
The degenerate case. Every push is both the first and the newest.

**`sample_as_f32` for every type.**
Bool → 0/1, i32 → cast, u16 → cast, enum → cast. Every detector will
read samples through this function, so its behavior per type is a
contract.

**`sample_t` is 24 bytes with fixed field offsets.**
A static assertion. The ingest storage math (`256 × 1024 × 24` ≈ 6 MB)
is built into the type, and the A6 wire format packs the same fields
in a different order. If someone adds a field and the struct grows to
32 bytes, this test fails before the size change propagates into the
storage array. Field offset assertions catch reordering.

---

## test_registry

**What it tests:** `registry_t` (`src/registry.c`), the config loader
that turns `channels.toml` into the descriptor table everything else
reads.

**Why it exists:** the registry is the source of truth for what a
channel means. Every downstream decision — which detector applies,
whether Gardener may write to it, whether it selects a regime — reads
from here. The test locks the parser's tolerance and its failure
modes.

### Cases

**Empty file loads cleanly.**
Zero channels, zero warnings, exit 0. A registry that failed on an
empty input would break any pipeline that generates config
dynamically.

**Missing file is a hard error.**
Returns -1. The dump tool exits with code 1. Distinguishing this
from "empty file" matters because the operator needs to know whether
their config was read.

**Full channel with every field.**
Writes a config with every field set, verifies every field is stored
correctly. The reference case: if this fails, nothing else can be
trusted.

**Multiple channels.**
Two blocks load as two entries, lookups by id return the right one,
lookup of an unknown id returns NULL.

**Comments and blank lines.**
Leading `#` comments and empty lines are skipped. Also documents a
real limitation: trailing comments on a value line are *not*
stripped, because doing so requires knowing whether `#` is inside a
quoted string. The channel still loads, just with the comment text
in the value. This test pins that behavior so it is not accidentally
relied on.

**Unknown key warns and counts.**
Adding a key the parser does not recognize bumps `warnings` and does
not fail the load. The rest of the channel is kept.

**Unknown type and role.**
`type = "float64"` warns, and the type falls back to the default
(f32). Same for an unrecognized role. Hand-edited configs with typos
should load as much as possible.

**Missing name drops the channel.**
A `[[channel]]` with no name is unusable — it has no human
identifier for logs or events — so it is removed and a warning is
logged. The registry is compacted so `count` reflects only usable
channels.

**`registry_get_or_default` synthesizes.**
Looking up an unknown id creates a placeholder with
`role = ROLE_UNKNOWN`, `type = f32`, and a generated name. Calling it
twice returns the same entry, not a second one. This is the path
ingest uses for channels that arrive on the wire but are not in the
config.

**`registry_get_or_default` returns existing.**
If the id is already loaded, the call returns the existing descriptor
unchanged.

**Capacity limit.**
Loads `REGISTRY_MAX_CHANNELS + 5` channels and verifies `count`
stops at the max, with at least one warning. The registry is bounded;
a runaway config does not overrun the array.

---

## test_ingest

**What it tests:** `ingest_t` (`src/ingest.c`), the engine that turns
packets into per-channel history.

**Why it exists:** ingest is where the codec layer meets the storage
layer. It is the single entry point for all data, and every counter
it maintains (`packets_seen`, `samples_seen`, `samples_dropped`,
`unknown_channels`) is a metric the rest of the system will key on.
The test locks the semantics of each counter and each transformation.

### Cases

**NULL arguments rejected.**
`ingest_init(NULL, ...)` and `ingest_feed(NULL, ...)` return -1.
Callers can rely on this rather than checking for NULL themselves.

**Init without config.**
`ingest_init(&ing, NULL)` succeeds with an empty registry. Useful for
tests and for a future "auto-register everything on first sight"
mode.

**Single-channel round-trip.**
One f32 sample in, `ingest_latest` returns the same value, the
timestamp was inherited from the packet, and `CH_FLAG_VALID` is set.
The most basic path.

**Three channels in one packet.**
All three stored, all three retrievable. Confirms per-channel
indexing.

**Ring accumulates.**
Ten samples, `ring_count` is 10, values in order. The ring
integration with ingest works.

**Ring wraps.**
Feeds `INGEST_RING_CAP + 5` samples and confirms `ring_oldest` is
sample 5 and `latest` is sample 1028. Ties ingest to the ring's wrap
behavior end to end.

**Unknown channel synthesized.**
Feeding id 99 (not in config) bumps `unknown_channels`, grows the
registry by one, and stores the sample. The synthesized descriptor
has `role = ROLE_UNKNOWN`. This is the intended behavior for stray
channels.

**Unknown channel cached.**
Two feeds of the same unknown id produce one registry entry, not two.
`unknown_channels` counts feeds, not registrations, so it is 2 while
the registry grew by 1. This is a decision, not an accident: the
counter measures traffic from unknown channels, not unique unknown
channels.

**Timestamp inheritance.**
Sample with `timestamp_ns = 0` inherits the packet's. Sample with its
own timestamp wins. Both directions checked because both are common
(single-tick packets vs. batched samples).

**Simulated flag propagates.**
`PACKET_FLAG_SIMULATED` on the packet becomes `CH_FLAG_SIMULATED` on
every sample. Phase D detectors will want to know which samples came
from a simulation.

**Stale flag preserved, valid not set.**
A sample marked stale keeps `CH_FLAG_STALE` and does *not* get
`CH_FLAG_VALID`. This invariant — exactly one of VALID or STALE — is
what detectors rely on.

**Out-of-range id dropped.**
id ≥ 256 fails, `samples_dropped` bumps, no storage. The static array
is bounded.

**Oversized packet rejected before counting.**
A packet with more than `PACKET_MAX_CH` channels returns -1 and does
*not* bump `packets_seen`. "Seen" means "accepted", not "received",
and that distinction has to be tested or it will drift.

**Type/role fill-in from descriptor.**
A packet with `type = 0` and `role = 0` gets them filled from the
registry entry. This is how a minimal packet — just ids and values —
can still arrive correctly typed.

**Mixed known and unknown in one packet.**
Both stored. `unknown_channels` counts 1 for the unknown one only.

**`has_channel` false before any feed.**
`ingest_latest` returns NULL, `ingest_state` returns non-NULL with
`seen = false`. The asymmetry is deliberate: state exists for every
in-range id, but the sample is only valid once a sample has arrived.

**Accessors respect bounds.**
id ≥ `REGISTRY_MAX_CHANNELS` returns NULL from `ingest_latest`,
`ingest_state`, and `has_channel`.

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
The canonical shape: `{"t":...,"ch":[{"id":1,"role":1,"v":20.5}]}`.
Value, id, and role all present.

**Decode multiple channels.**
A packet with three entries in different types. This is the test
that caught the `find_key` scoping bug — with a `"type":"bool"`
appearing in entry 3, entries 1 and 2 must not pick it up.

**Explicit type wins.**
`{"type":"f32","v":20}` must store the value as f32 with the union
holding the float bit pattern, not as i32 with the union holding the
integer. This caught a type/union mismatch bug: the parser was
correctly setting the type tag, but the union bytes had already been
written by the type-inference path.

**Shorthand `v` map.**
`{"t":1,"v":{"1":20.5,"3":true}}` parses into two samples with
inferred types. Used for hand-typed input.

**Whitespace tolerance.**
Spaces and tabs around keys, colons, and values are ignored.
Hand-written test input is rarely compact.

**Unknown keys ignored.**
`"extra"` and `"note"` do not break the parse. Tolerant of `gensim`
growing extra fields.

**Key order irrelevant.**
`"ch"` before `"t"` or `"src"` in a later position parses the same.

**Negative numbers.**
`-5.5` becomes `f32(-5.5)`. A common source of parser bugs if sign
handling is wrong.

**Malformed input returns -1.**
Empty string, missing `t`, missing channels, empty arrays, non-JSON.
Failures are the correct default for malformed input.

**Encode basics.**
Output contains `"t":1000`, `"id":1`, `"v":20.5`. Locks the output
shape so the decode tests have a counterpart.

**Encode bool.**
Value prints as `true`, type tag is `"bool"`, role is numeric. The
encoder must round-trip what the decoder accepts.

**Round-trip.**
Encode a three-channel packet, decode it, compare every field.
Catches asymmetric changes to either side.

**NULL and zero-cap.**
`packet_to_json(NULL, ...)` and `(..., NULL, 0)` both return -1.

**Returns needed length.**
Passing a 4-byte buffer for a larger packet returns a value greater
than 4 and does not crash. This is the "count even when truncated"
contract, and it is tested explicitly because it was the second bug
this sub-phase caught: the initial `append` helper returned -1 on
overflow instead of counting the needed size.

---

## test_packet_bin

**What it tests:** the binary codec (`packet_encode`, `packet_decode`,
`packet_crc16` in `src/packet.c`).

**Why it exists:** the binary format is the wire format for logs
(Phase B) and the eventual target link (Phase F). It has to be
byte-exact and stable. The test pins both the format and the
failure modes.

### Cases

**Encoded size formula.**
Zero-channel packet is 22 bytes; a 3-channel packet is 76. The
formula `22 + 18 × N` is what the log reader uses to size its buffer,
so it has to be exact.

**Worst case fits in `PACKET_BIN_MAX`.**
A full 64-channel packet encodes to exactly `PACKET_BIN_MAX` bytes,
no more. If a future field creeps in, this fails before callers
allocate the wrong amount.

**Round-trip empty.**
A packet with zero channels encodes and decodes, preserving
timestamp, source id, and count. The empty case is legal; it happens
when a source has nothing to report but still wants to signal
"alive".

**Round-trip one f32.**
Full field-level comparison of a single-sample packet.

**Round-trip all types.**
f32, i32, bool, u16, enum in one packet, values and type tags
checked. Catches union corruption.

**Round-trip all flags.**
Packet flag and sample flags both survive.

**Encode NULL and cap-too-small.**
Both return -1 without writing.

**Encode too many channels.**
A packet claiming more than `PACKET_MAX_CH` channels returns -1
before writing anything.

**Decode NULL and too-short buffer.**
Both return -1. A buffer shorter than the fixed header is rejected
outright.

**Bad magic.**
Flipping a bit in the first four bytes returns -1. Catches reading
a random file as a packet.

**Bad version.**
Bumping the version field returns -1. Version is a hard gate.

**Bad CRC.**
Flipping a bit in the payload (and leaving the CRC unchanged)
returns -1. This is the check that protects logs from bit rot and
truncated writes.

**Channel count exceeds max.**
Encoding a valid small packet, then overwriting the count field to
0xFFFF and recomputing the CRC so it passes, still returns -1.
Proves the count is checked independently of the CRC.

**Buffer shorter than claimed.**
A packet that claims 3 channels but is handed only 30 bytes fails.
The decoder does not read past what the caller says is available.

**Two packets concatenated.**
Encode two packets back to back, decode them one at a time by
advancing past the first decode's return value. This is the contract
the log reader needs: decode returns bytes consumed, not total
buffer size.

**CRC-16/MODBUS check vector.**
`packet_crc16("123456789", 9)` must equal `0x4B37`. This is the
standard check value for CRC-16/MODBUS and pins polynomial, init,
reflection, and xorout in one assertion. Also serves as a naming
check: CRC-16/ARC has a *different* check value (`0xBB3D`) and an
earlier draft of this test used the wrong name.

---

## test_log_format

**What it tests:** the structural invariants declared in
`include/gardener/log.h` — header and index layout, record size
formula, and the relationship between the log format and
`packet_encode`.

**Why it exists:** the log format is on-disk state. Once written, it
has to be readable by every future version of Gardener. Locking the
sizes and field offsets in a test means any change to the format is
deliberate: you have to update the test *and* the writer *and* the
reader *and* the version field, or the build fails.

### Cases

**Constants.**
`LOG_MAGIC`, `LOG_VERSION`, `LOG_HEADER_SIZE`, `LOG_DEFAULT_MAX`,
`LOG_MIN_MAX`, and the index constants. Cheap, catches typos in the
header.

**Header size and field offsets.**
Every field offset in `log_header_t` is asserted. Reordering fields
requires updating these and bumping `LOG_VERSION`.

**Index header and entry sizes and offsets.**
Same treatment for the index file.

**Record size formula matches `packet_encode`.**
For N in 0..5 and for `PACKET_MAX_CH`, `log_record_size(N)` must
equal what `packet_encode` actually produces. This is the invariant
the log writer and log reader both depend on. If `packet_encode`
ever changes its header size or sample size without
`log_record_size` changing too, this test fails immediately.

**Minimum log size holds enough records to be useful.**
`LOG_MIN_MAX = 1 MB` minus the header must be at least 100 worst-case
records. Sanity check on the constant.

**Registry snapshot fits within the default log's budget.**
Worst-case 256-channel registry JSONL at ~128 bytes each is under
1% of the default log size. Sanity check on the constant.

---

## test_log_writer

**What it tests:** `log_writer_t` (`src/log_writer.c`).

**Why it exists:** the log writer is the first component with real
file I/O and mutable on-disk state. It has more failure modes than
anything before it: partial writes, wrap, header/mutation ordering,
index persistence, header validation on reopen. The test exercises
each of those end to end by writing records and reading them back.

### Cases

**Open/create/close.**
Opening a nonexistent path creates a valid log file with a
correct-looking header. Readable back with `plat_file_read`.

**Append one and read back.**
A single-packet log round-trips: timestamp, source id, channel
count, and sample fields all match.

**Append many and read back.**
100 packets with increasing timestamps are written and read back in
order. Verifies that sequential appends don't corrupt the stream.

**Reopen appends.**
Two sessions: session 1 writes 10 packets, closes. Session 2 opens
the same file, sees `record_count = 10`, appends 10 more. Result is
20 packets with correct timestamps. This test caught two bugs:
- The log writer wrote the index file before opening it, causing
  every open to fail. (Sequencing bug.)
- `last_wall_ns` was being set *after* the header CRC was computed.
  Since `last_wall_ns` is inside the CRC-covered range, every header
  written had an invalid CRC. This wasn't visible until reopen,
  which validated the CRC and refused.

**Wrap overwrites oldest.**
Fill a minimum-size log (1 MB) with 1500 64-channel packets. Each
packet is 1174 bytes; total ~1.7 MB written to a 1 MB file. Asserts
`wrap_count >= 1` and `record_count == 1500`. Exercises the wrap
path and the wrap-counter increment.

**Refuse invalid header.**
Create a valid log, corrupt the magic, and reopen without
`start_fresh`. Must return NULL — the writer refuses to overwrite a
file whose header it can't validate, since that file might contain
recoverable data. With `start_fresh`, the same file opens
successfully and is truncated.

**Min size refused.**
`log_writer_open(path, 100, ...)` returns NULL because 100 <
`LOG_MIN_MAX`. Prevents an absurdly small ring.

**NULL args.**
`log_writer_open(NULL, ...)`, `log_writer_append(NULL, NULL)`,
`log_writer_flush(NULL)`, and `log_writer_close(NULL)` all handle
their arguments without crashing.

---

## test_log_reader

**What it tests:** `log_reader_t` (`src/log_reader.c`).

**Why it exists:** the reader is the other half of the log
contract. It has to walk what the writer produced, reconstruct the
registry from the header snapshot, and answer time-range queries
using the sidecar index. Failures here would silently produce wrong
data — the worst kind.

### Cases

**Open missing file returns NULL.**
`log_reader_open("nonexistent")` returns NULL. Same as the writer's
policy: missing inputs are explicit failures, not zero-valued
successes.

**Open NULL arg returns NULL.**

**Close NULL is safe.**
`log_reader_close(NULL)` does not crash.

**Open empty and iterate.**
Create a log with zero records. `log_reader_next` returns 0
immediately (end of log). `total_records == 0`.

**Write/read five.**
Five packets with timestamps 1000, 1100, 1200, 1300, 1400. Iterate
and confirm each is returned in order with the right timestamp,
channel count, and value.

**Rewind.**
After iterating to end, call `log_reader_rewind`, iterate again,
confirm the same sequence. Also confirms a fresh reader returns the
first record.

**Registry snapshot.**
Write a log with a 3-channel registry (sensor, actuator, status) and
confirm `log_reader_registry` reconstructs it: correct ids, names,
units, types, roles, and flags (`has_range`, `controllable`,
`is_regime`). This exercises the JSONL snapshot parser in the
reader.

**Seek.**
Write 10 packets at timestamps 0, 100, 200, ..., 900. Then:
- `seek_ns(250)` → next call returns ts=300.
- `seek_ns(500)` → next call returns ts=500.
- `seek_ns(0)` → next call returns ts=0.
- `seek_ns(9999)` → next call returns end-of-log.

The contract: seek positions the reader such that a following
`log_reader_next` returns the first record with `ts >= target`.

**NULL safety.**
Every accessor (`total_records`, `wrap_count`, `header`,
`registry`, `path`) handles NULL. `next` and `seek_ns` handle NULL
arguments.

---

## Things not tested yet

Honest list of gaps, so they're not surprising later:

- **Log wrap followed by read.** `test_log_writer` verifies wrap
  writes, but `test_log_reader` doesn't verify reading from a wrapped
  log. The reader supports it (`log_reader_rewind` handles both
  cases) but no test exercises the circular walk. Should be added
  when B4's replay tool needs it, or now if you want the coverage.

- **Index seek with a real index.** `test_log_reader`'s seek test
  runs on a 10-record log, which won't have written any index
  entries (`LOG_INDEX_INTERVAL = 256`). So the seek is exercising the
  linear-scan fallback, not the index fast path. To test the index,
  write 300+ records. Worth adding.

- **`log_reader_prev`.** Not implemented. No test because there's
  nothing to test.

- **Corrupted record mid-log.** The reader is expected to stop at
  the first undecodable record and return -1. No test exercises
  this. Would require writing a valid log, corrupting a byte in the
  middle of the payload, and confirming iteration stops cleanly.

- **`gensim`.** No automated test. It's exercised manually and via
  the pipeline checks in the README. A Python test harness that
  runs each scenario and checks the output against expected patterns
  would catch regressions as scenarios evolve.

- **Concurrency.** Nothing is thread-safe today. When that changes,
  the invariants will need to be documented and tested.

---

## Test philosophy

Three rules, applied consistently:

1. **Tests encode caller-visible behavior, not implementation.**
   `test_ring` asserts that pushing six items into a ring of four
   leaves items 2, 3, 4, 5 readable. It does not assert that the
   internal `head` counter equals 2. If `ring.c` is refactored to
   use a different internal representation, the test should still
   pass.

2. **Tests use the public API.**
   No test reaches into a struct and inspects a private field. If a
   piece of state matters to callers, it has an accessor. If it
   doesn't, no test depends on it.

3. **Every test names the invariant it protects.**
   `test_stale_flag_preserved` says what it's about. `CHECK_EQ(x, 5)`
   inside it says what the invariant is. When the test fails six
   months from now, the reader should be able to tell *what*
   changed without reading the source.

The corollary: when a test fails, the first question is not "how do
I fix the code?" but "which one — test or code — is the source of
truth here?" If the answer is the test, fix the code. If the answer
is the design and the test misstates it, fix the test. If it's
ambiguous, decide which one is right and make the other agree.