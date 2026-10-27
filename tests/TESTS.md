# Tests

One test executable per module. Each is a small C program with a minimal CHECK/CHECK_EQ/CHECK_NEAR macro set and a `main()` that runs every case. No test framework, no fixtures directory, no test runner beyond CTest.

The tests exist to lock in behavior, not to prove the code works. When the code and a test disagree, one of them is wrong, and the question is which one is the source of truth. Usually it's the test — it encodes what callers rely on. When the docstring and the test disagree too, the test wins.

Running:

    cd build && ctest --output-on-failure

Or a single suite:

    cd build
    ./test_ring

All tests are also runnable directly; CTest registration just invokes the same binary.

---

## test_platform

**What it tests:** the platform layer (`src/platform/platform_posix.c` and `platform.h`), which is the only module that calls OS APIs directly.

**Why it exists:** to catch portability and infrastructure problems before they propagate.

### Cases

**Monotonic time moves forward.** `plat_now_ns()` is what every sample timestamp traces back to. The test sleeps 20 ms and asserts the clock advanced by at least 10 ms and less than 10 s (the bound is generous because Windows timer coalescing can wake the process early).

**Wall clock is plausible.** `plat_wall_ns()` returns a value after 2020-01-01 in nanoseconds. Fails loudly if the RTC is unset.

**Sleep returns on time.** `plat_sleep_ms(30)` must actually sleep. Lower bound is 20 ms.

**File write/read round-trip.** Writes a known payload, reads it back, checks content, confirms EOF.

**File seek and tell.** Writes 10 bytes, reads in pieces, seeks, verifies `plat_file_tell` reports the right position.

**Missing file returns NULL.**

**NULL safety.** Every file function handles NULL without crashing.

**Log levels filter.** Setting level to ERROR suppresses DEBUG.

---

## test_ring

**What it tests:** `ring_t` (`src/ring.c`), the fixed-capacity sample ring that backs every channel.

**Why it exists:** the ring is where all historical data lives. If its indexing or wrap behavior is wrong, every downstream observation is wrong.

### Cases

**Bad arguments rejected at init.**

**Push under capacity preserves order.**

**Push exactly to capacity.** Boundary condition.

**Wrap overwrites oldest.** Core invariant.

**Wrap twice.** Modular arithmetic across multiple laps.

**Clear.** Reset without reallocation.

**Capacity 1.** Degenerate case.

**`sample_as_f32` for every type.** Detector contract.

**`sample_t` is 24 bytes with fixed field offsets.** Static assertion. The ingest storage math is built into the type. If someone adds a field and the struct grows, this test fails before the size change propagates into the storage array.

---

## test_registry

**What it tests:** `registry_t` (`src/registry.c`), the config loader.

**Why it exists:** the registry is the source of truth for what a channel means. Every downstream decision reads from here.

### Cases

**Empty file loads cleanly.**

**Missing file is a hard error.**

**Full channel with every field.**

**Multiple channels.**

**Comments and blank lines.** Documents the trailing-comment limitation (not stripped).

**Unknown key warns and counts.**

**Unknown type and role.** Falls back to defaults.

**Missing name drops the channel.**

**`registry_get_or_default` synthesizes and caches.**

**`registry_get_or_default` returns existing.**

**Capacity limit.**

---

## test_ingest

**What it tests:** `ingest_t` (`src/ingest.c`), packets → history.

**Why it exists:** ingest is where codec meets storage. Every counter it maintains is a metric the rest of the system keys on.

### Cases

**NULL arguments rejected.**

**Init without config.**

**Single-channel round-trip.**

**Three channels in one packet.**

**Ring accumulates.**

**Ring wraps.** `INGEST_RING_CAP + 5` samples; oldest is sample 5.

**Unknown channel synthesized.**

**Unknown channel cached.** Counts feeds, not registrations.

**Timestamp inheritance.**

**Simulated flag propagates.**

**Stale flag preserved, valid not set.** VALID/STALE invariant.

**Out-of-range id dropped.**

**Oversized packet rejected before counting.**

**Type/role fill-in from descriptor.**

**Mixed known and unknown in one packet.**

**`has_channel` false before any feed.**

**Accessors respect bounds.**

---

## test_packet_json

**What it tests:** the JSONL codec.

**Why it exists:** JSONL is the dev feed format. Deliberately tolerant = easy to get subtly wrong.

### Cases

**Decode one f32.**

**Decode multiple channels.** Caught the `find_key` scoping bug — a `"type":"bool"` in entry 3 must not leak into entries 1 and 2.

**Explicit type wins.** Caught a type/union mismatch: the parser set the type tag correctly, but the union bytes had already been written by type-inference.

**Shorthand `v` map.**

**Whitespace tolerance.**

**Unknown keys ignored.**

**Key order irrelevant.**

**Negative numbers.**

**Malformed input returns -1.**

**Encode basics.**

**Encode bool.**

**Round-trip.**

**NULL and zero-cap.**

**Returns needed length.** Caught a bug in `append`: initial version returned -1 on overflow instead of counting the needed size.

---

## test_packet_bin

**What it tests:** the binary codec.

**Why it exists:** wire format for logs and the future target link. Byte-exact and stable.

### Cases

**Encoded size formula** `22 + 18 * N`.

**Worst case fits in `PACKET_BIN_MAX`.**

**Round-trip empty.**

**Round-trip one f32.**

**Round-trip all types.**

**Round-trip all flags.**

**Encode NULL and cap-too-small.**

**Encode too many channels.**

**Decode NULL and too-short buffer.**

**Bad magic.**

**Bad version.**

**Bad CRC.**

**Channel count exceeds max.** Checked independently of CRC.

**Buffer shorter than claimed.**

**Two packets concatenated.** Stream-walking contract.

**CRC-16/MODBUS check vector.** 0x4B37. Also a naming check: CRC-16/ARC would be 0xBB3D.

---

## test_log_format

**What it tests:** structural invariants in `log.h`.

**Why it exists:** on-disk state. Once written, readable by every future version.

### Cases

**Constants.**

**Header size and field offsets.**

**Index header and entry sizes and offsets.**

**Record size formula matches `packet_encode`.** If `packet_encode` changes its size without `log_record_size` changing too, this fails.

**Minimum log size holds enough records.**

**Registry snapshot fits within budget.**

---

## test_log_writer

**What it tests:** `log_writer_t`.

**Why it exists:** the first component with real file I/O and mutable on-disk state. Many failure modes.

### Cases

**Open/create/close.**

**Append one and read back.**

**Append many and read back.** 100 packets in order.

**Reopen appends.** **This test caught two bugs:**
- The writer wrote the index file before opening it. Sequencing bug.
- `last_wall_ns` was being set after the header CRC was computed. Since `last_wall_ns` is inside the CRC-covered range, every header written had an invalid CRC. Not visible until reopen.

**Wrap overwrites oldest.**

**Refuse invalid header.** Don't silently overwrite.

**Min size refused.**

**NULL args.**

---

## test_log_reader

**What it tests:** `log_reader_t`.

**Why it exists:** the other half of the log contract. Failures here silently produce wrong data.

### Cases

**Open missing file returns NULL.**

**Open NULL arg returns NULL.**

**Close NULL is safe.**

**Open empty and iterate.**

**Write/read five.** Order preserved.

**Rewind.**

**Registry snapshot.** Header's JSONL reconstructed to usable `registry_t` with correct names, units, roles, types, flags.

**Seek.** `--from`/`--to` semantics:
- `seek_ns(250)` -> next call returns ts=300.
- `seek_ns(500)` -> next call returns ts=500.
- `seek_ns(0)` -> next call returns ts=0.
- `seek_ns(9999)` -> next call returns end-of-log.

**NULL safety.**

---

## test_replay

**What it tests:** the replay engine.

**Why it exists:** replay is how the same recorded data feeds through ingest repeatedly. Detector tuning depends on it.

### Cases

**Options default.**

**NULL arguments rejected.**

**Missing file returns -1.**

**Empty log:** zero packets fed.

**Ten packets:** timestamps and count match.

**From/to window:** feeds exactly 5 packets (300..700).

**From-only:** feeds 5 packets (500..900).

**To-only:** feeds 3 packets (0..200).

**Invalid speed factor:** returns -1.

**Max speed is fast:** 100 packets in under 1 second.

**Not tested:** real-time and scaled-timing accuracy. Real sleeps would make the test slow and flaky. Delay computation is simple enough to trust by inspection.

---

## test_main_args

**What it tests:** the CLI wiring in `src/main.c` — argument parsing, `--log` behavior, `--replay` interaction.

**Why it exists:** integration layer. Bugs here are invisible to library tests.

**How it works:** runs `gardener` as a subprocess via `system()`. Paths are relative to `build/`.

### Cases

**`--version` exits 0.**

**No `--log` creates no file.** Opt-in design.

**`--log` creates a file.** 3 records verified.

**`--log-max` below minimum doesn't create a log.** Writer fails, gardener continues.

**Replay + log.** Both logs have 3 records, matching timestamps.

### A test infrastructure note

`test_main_args` uses `system()`, which spawns a shell. Not portable to environments without one. Slower than library tests. But it surfaces bugs library tests can't see (CLI argument handling, file side-effects, exit codes). On failure, `run_gardener` re-runs without output redirection so the actual `gardener` stderr appears in the test log.

---

## test_baseline_format

**What it tests:** structural invariants in `baseline.h`.

**Why it exists:** on-disk state. Format drift silently corrupts saved files.

### Cases

**Constants.** `BASELINE_MAGIC`, `BASELINE_VERSION`, `BASELINE_HEADER_SIZE`, `BASELINE_STAT_SIZE`, all the limits.

**Header size and field offsets.** Locked.

**Channel entry size and offsets.**

**Stat size and offsets.**

**File layout math.** `BASELINE_FILE_SIZE = 64 + 256*8 + 256*16*40 + 4 = 165,956`.

**State enum values.**

**`baseline_t` size sanity** between 100 KB and 200 KB.

---

## test_baseline

**What it tests:** the Welford math and core baseline API.

**Why it exists:** numerical correctness. If Welford is wrong, every detector's z-score is wrong.

### Cases

**Init NULL.**

**Init empty.**

**Update NULL.**

**Welford known sequence.** 1,2,3,4,5 -> mean=3, m2=10, variance=2.5, stddev=sqrt(2.5), min=1, max=5.

**Welford constant sequence.** 100 x 42 -> mean=42, variance=0.

**Welford single sample.** mean=7.5, variance=0.

**State transitions.** EMPTY -> LEARNING on first sample. LEARNING -> READY at 100.

**Multiple channels.** Three channels, three independent stats.

**Multi-sample packet.**

**Sample timestamp used.** `first_ts_ns == sample.timestamp_ns`.

**Packet timestamp inherited.** Sample ts=0 -> `first_ts_ns == packet.timestamp_ns`.

**Stale samples skipped.**

**Invalid samples skipped.**

**Get bounds.**

**Variance null safety.**

**No negative variance.**

**Large mean small variance.** The test that justifies Welford. Values 1e6-0.5, 1e6, 1e6+0.5 -> variance=0.25, stddev=0.5. Naive sum-of-squares would lose precision here.

**Channel capacity limit.** 256 channels registered. 257th ignored.

---

## test_baseline_regimes

**What it tests:** regime derivation and per-regime stats.

**Why it exists:** a system with modes (`running`, `idle`, `warming`) needs separate baselines. If regime derivation is wrong, samples go to the wrong slot.

### Cases

**Init no regime channels.** Single regime 0.

**Init one regime channel.** `regime_ch_ids[0]` populated.

**Init multiple regime channels.**

**Init ignores extra regime channels.** Max 4.

**No regime channels is single regime.**

**One regime channel creates two regimes.** Feed both `running=false` and `running=true`. Both regimes get data independently.

**Regime is stable across time.** Same regime-channel value -> same regime index, 10 times in a row.

**Missing regime channel skips packet.** Whole packet dropped, not just the missing sample.

**Regime for packet unknown.**

**Regime for packet no regime channels.**

**Regime quantization.** Values within 0.001 hash the same.

**Regime with multiple channels.** Four combos -> at least 3 distinct regimes (one collision allowed; with 16 slots and 4 inputs, collision probability is ~20%).

**Get NULL for unseen regime.**

---

## test_baseline_decay

**What it tests:** exponential decay.

**Why it exists:** without decay, an old baseline never adapts. With wrong decay, it adapts too fast (and blurs real faults) or too slow (and rejects changed-normal states).

### Cases

**Decay null safe.**

**Decay empty model no-op.**

**Decay half-life.** 100 samples, decay 1 halflife -> count=50, m2 halved, mean unchanged.

**Decay two half-lives.** 100 -> 25.

**Decay noop for zero dt.** Decay to same time = no change.

**Decay ready to stale.** 100 samples = READY. Decay 2 halflives -> count=25, state=STALE.

**Decay learning stays learning.** 50 samples, still LEARNING. Decay doesn't change state.

**Update does not decay.** Feeding 100 samples gives count=100 regardless of timing. Adding one sample after 10 halflives gives count=101.

**Decay preserves mean.**

**Decay stale to ready.** After decay to STALE, feed 50 samples -> count crosses 100 -> READY.

**Decay to zero.** 100 halflives -> count approaches 0, never negative.

---

## test_baseline_persist

**What it tests:** save/load.

**Why it exists:** baselines survive restarts. Corruption in the file format would silently reset weeks of learning.

### Cases

**Save NULL args.**

**Load nonexistent.**

**Round-trip empty.**

**Round-trip one channel.** Count, mean, m2, min, max, state, timestamps all preserved to float precision.

**Round-trip multiple channels.**

**Round-trip after decay.** Decay state survives save/load.

**Corrupt magic rejected.**

**Corrupt CRC rejected.** Bit flip in stats area caught.

**Truncated file rejected.**

**Load leaves `*b` zeroed on failure.** Callers get a known state after any load attempt.

---

## test_main_baseline

**What it tests:** `gardener --baseline` CLI wiring.

**Why it exists:** the baseline exists as a library; the CLI is where it becomes part of a running system.

**How it works:** subprocess via `system()`, same as `test_main_args`.

### Cases

**No flag, no file.** Without `--baseline`, no `.bin` created.

**Fresh creates file.** Feed 3 packets, verify baseline file exists, load it back, verify each channel has data in some regime.

**Resume across runs.** Run 1: 3 packets. Run 2 (no `--baseline-fresh`): 2 more. Final count = 5, mean = 12.0.

**Fresh ignores existing.** Run 1: 3 packets. Run 2 (`--baseline-fresh`): 1 packet. Final count = 1 (not 4), mean = 50.0 (not 25.0).

**Replay mode.** JSONL -> log -> replay -> baseline. 3 samples preserved.

**Corrupt file starts fresh.** Garbage file -> warning, fresh init, save works.

---

## test_event_format

**What it tests:** structural invariants in `event.h`.

**Why it exists:** the event struct is the interface between detection and everything downstream (events file, dashboard, policy engine in phase E). Changing it requires bumping `EVENT_VERSION`.

### Cases

**Constants.** `EVENT_MAGIC`, `EVENT_VERSION`, `EVENT_SIZE=48`, sentinels, `EVENT_MSG_MAX`.

**`event_t` is exactly 48 bytes.** Field offsets locked.

**Detector enum values.** range=1 through mismatch=6.

**Severity enum values.** info=0, warn=1, critical=2.

**Event state enum values.** started=0, cleared=1.

**Naming helpers.** `detector_name`, `severity_name`, `event_state_name` return correct strings and "?" for unknowns.

**Timestamp split/join round-trip.** NULL safety on helpers.

---

## test_event_json

**What it tests:** `event_to_json` — the JSONL encoder.

**Why it exists:** events are written to `events.jsonl` and consumed by external tools. The JSON format must be stable and parseable.

### Cases

**Basic encoding.** All fields present.

**`EVENT_CHANNEL_NONE` and `EVENT_REGIME_NONE` render as `null`.**

**Quote and backslash escaping in `msg`.**

**NULL message renders as empty string.**

**NULL args return -1.**

---

## test_detect_range

**What it tests:** the range detector.

**Why it exists:** the simplest detector, no baseline needed. Fires when a sample is outside `[min, max]` from the registry.

### Cases

**Init with defaults.**

**In-range value → no event.**

**Out-of-range → one STARTED.**

**Ongoing violation → no further events.**

**Return to range → one CLEARED.**

**Below-min → STARTED with lower bound as threshold.**

**No range configured → skip.**

**Stale sample → skip.**

**NULL args → -1.**

---

## test_detect_zscore

**What it tests:** the z-score detector.

**Why it exists:** the primary detector. Compares live samples to the learned baseline mean/stddev for the current regime.

### Cases

**READY baseline fires on far value.**

**Normal value → no event.**

**Not-ready baseline doesn't fire.** LEARNING baseline with a wild sample still doesn't fire.

**Clears when value returns to normal.**

**No baseline → no zscore.**

**Critical severity for very far.** A sample more than 2× threshold away gets `SEVERITY_CRITICAL`.

---

## test_detect_flatline

**What it tests:** the flatline detector.

**Why it exists:** catches frozen sensors. A channel that reports the same value for too long has failed somewhere upstream.

### Cases

**Flatline fires after window fills (32 samples).**

**Varying values → no fire.**

**Recovery clears.** Varied samples break the flatline.

Uses `g_ing` shared ingest for history.

---

## test_detect_roc

**What it tests:** the rate-of-change detector.

**Why it exists:** catches fast-moving values even when the value itself is in range.

### Cases

**Slope below threshold → no event.**

**Slope above threshold → fires once window fills.**

**Slow down after fire → clears.**

---

## test_detect_correlation

**What it tests:** the correlation pair detector.

**Why it exists:** two channels that normally co-vary can decouple while each is individually "normal." Correlation catches this.

### Cases

**Correlated pair → no event.**

**Decoupling fires.**

**Recovery clears.**

**No pairs configured → no events.**

**Insufficient history (only 10 samples) → no fire.**

Uses deterministic periodic patterns for correlation and decorrelation.

---

## test_detect_mismatch

**What it tests:** the `(ctrl_out, actuator)` mismatch detector.

**Why it exists:** a controller asking for 100% while the actuator is at 50% means something is broken (saturation, stuck valve, wiring). Both channels individually normal.

### Cases

**Agreeing → no event.**

**Small diff → no event.**

**Brief divergence (3 samples, sustained=5) → no event.**

**Sustained divergence (10 samples) → one STARTED.**

**Ongoing divergence → no more events.**

**Recovery clears.**

**`ctrl < act` also fires (INFO severity).**

**Two pairs independent.**

**No pairs → no events.**

---

## test_main_detect

**What it tests:** `gardener --detect` CLI wiring.

**Why it exists:** the detectors work individually in unit tests. This is the integration test: does the CLI actually produce events?

### Cases

**No `--detect` and no `--events` → no file created.**

**`--detect --events <path>` creates the events file.**

**Range violation emits 2 events** (STARTED + CLEARED) — file has 2 lines.

**`--detect-pair a,b` accepted** (doesn't crash, doesn't fire on too-short input).

**`--detect-mismatch c,d` accepted.**

**Bad `--detect-pair` syntax rejected** (nonzero exit code).

---

## Things not tested yet

Honest list of gaps:

- **Log wrap followed by read.** `test_log_writer` writes wrapped logs; `test_log_reader` doesn't read them back. Reader supports it but no test exercises the circular walk.

- **Index seek with a real index.** `test_log_reader`'s seek test runs on 10 records, which doesn't trigger index writes. Exercises the linear-scan fallback only.

- **`log_reader_prev`.** Not implemented.

- **Corrupted record mid-log.** Reader stops at first bad record; no test.

- **`gensim`.** Manual only. A Python test harness that runs each scenario and checks the output against expected patterns would catch regressions as scenarios evolve.

- **Replay real-time pacing accuracy.** Manual only.

- **Concurrency.** Nothing is thread-safe.

- **Baseline behavior with registry changes.** Load a baseline saved with one `channels.toml` against a different one. Not tested.

- **Baseline decay across restart.** Save at T1, load at T2, decay to T3. Not tested end-to-end.

- **`gardener-stats` output format stability.** No test.

- **Correlation and mismatch event content.** Tests check that events fire and clear, but don't verify observed values, threshold field, or severity escalation in detail.

- **Event JSON schema stability across versions.** No test that says "the format is exactly this." Deferred until a real consumer exists (phase F dashboard).

- **Detector behavior on drift vs. normal baselines.** Tested at the unit level, and the drift scenario runs end-to-end, but "the z-score detector fires on the drift scenario's coolant_temp and doesn't fire on the normal scenario's" is not asserted. That would be a nice integration test with a real scenario file and a real baseline.

---

## Test philosophy

Three rules, applied consistently:

**1. Tests encode caller-visible behavior, not implementation.**
`test_ring` asserts that pushing six items into a ring of four leaves items 2, 3, 4, 5 readable. It does not assert that the internal `head` counter equals 2.

**2. Tests use the public API.**
No test reaches into a struct and inspects a private field. If a piece of state matters to callers, it has an accessor.

**3. Every test names the invariant it protects.**
`test_stale_flag_preserved` says what it's about. `CHECK_EQ(x, 5)` inside it says what the invariant is. When the test fails six months from now, the reader should be able to tell *what* changed without reading the source.

The corollary: when a test fails, the first question is not "how do I fix the code?" but "which one — test or code — is the source of truth here?" If the answer is the test, fix the code. If the answer is the design and the test misstates it, fix the test. If it's ambiguous, decide which one is right and make the other agree.