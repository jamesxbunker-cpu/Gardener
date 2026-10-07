# gensim

Simulated source for Gardener. Emits JSONL packets on stdout in the
same format Gardener's JSONL parser consumes.

## Requirements

Python 3.8+. No mandatory dependencies.

PyYAML is optional. If installed, scenarios are parsed by PyYAML. If
not, a small subset parser handles the specific shape our scenario
files use. For simplicity, install PyYAML:

    pip install -r requirements.txt

## Usage

    python tools/gensim/gensim.py --scenario <path> [options]

Options:

- `--channels <path>` — channel registry TOML. Defaults to
  `../../configs/channels.toml` relative to this directory.
- `--scenario <path>` — required. YAML or subset.
- `--rate <hz>` — packets per second. Default 10.
- `--duration <sec>` — stop after this many simulated seconds.
- `--speed <factor>` — run faster than real time. `--speed 10` runs
  10× faster.
- `--seed <n>` — RNG seed for reproducibility.
- `--deterministic` — use sim time for timestamps, not wall clock.
  Needed for repeatable replay.
- `--quiet` — suppress stderr output.

## Examples

Run 10 minutes of normal operation, pipe to gardener and capture a log:

    python tools/gensim/gensim.py --scenario scenarios/normal.yaml \
        --duration 600 | ./build/gardener --log build/normal.bin

(This requires gardener to have log writing, which arrives in B5.)

Run at 10× speed to generate a lot of data quickly:

    python tools/gensim/gensim.py --scenario scenarios/normal.yaml \
        --duration 600 --speed 10 > /tmp/normal.jsonl

Feed a scenario into gardener-dump for a snapshot:

    python tools/gensim/gensim.py --scenario scenarios/sensor_drift.yaml \
        --duration 300 | ./build/gardener-dump --feed /dev/stdin

## Scenarios

Each scenario is a YAML file with:

    name: <string>           # human-readable
    plant: <string>          # one of: generic, hvac
    duration: <seconds>      # optional; 0 = unlimited
    events:
      - at: <seconds>        # when this event fires
        action: <string>     # see below
        ch: <channel name>   # for per-channel actions
        ...                  # action-specific fields

### Actions

- `freeze ch=<name>` — hold the channel's current value until
  `unfreeze`.
- `unfreeze ch=<name>` — resume normal generation.
- `spike ch=<name> offset=<float> duration=<sec>` — add `offset` to
  the channel's value for `duration` seconds.
- `drift ch=<name> rate=<float>` — add `rate` per second to the
  channel. Use `reset` to stop.
- `reset ch=<name>` — cancel any drift or freeze on this channel.
- `link_gap duration=<sec>` — emit no packets for `duration` seconds.
  Simulates a dropped serial link.

### Plants

- `generic` — every channel oscillates as a slow sine + noise.
  Channels with a min/max oscillate within that range; others use a
  default amplitude.
- `hvac` — a small thermal model: coolant temperature chases a
  setpoint, coolant flow follows the pump, pump duty follows the
  error. Channels are matched by name; anything not recognized falls
  back to the generic behavior.

The `hvac` plant recognizes these channel names:

    coolant_temp, coolant_flow, ambient_temp,
    coolant_setpoint, coolant_pid_out, coolant_pump,
    running, fault_code

## Deterministic mode

For detector tuning, `--deterministic --seed 42` gives byte-identical
output on every run (given the same scenario). Timestamps start at
1 second and advance by exactly `1/rate` seconds per tick.

## Timestamps

By default, packet timestamps are wall-clock nanoseconds. In
deterministic mode they are `1_000_000_000 + tick * (1e9 / rate)`,
which is what you want when replaying or when comparing runs.

## Notes

- Values are rounded to 6 decimal places before emission. This keeps
  output diffable across runs.
- Booleans are emitted as JSON `true`/`false`. Integer-typed channels
  emit integers. Everything else is a float.
- Role is emitted as an integer matching the C `ch_role_t` enum, so
  `packet_from_json` on the C side fills the correct role without
  needing the registry.