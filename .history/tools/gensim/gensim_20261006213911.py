#!/usr/bin/env python3
"""
gensim — simulated source for Gardener.

Emits JSONL packets on stdout, one per tick, matching the packet format
that Gardener's JSONL parser consumes. Reads channel definitions from
the same configs/channels.toml that Gardener uses, so the two stay in
sync.

Usage:
    python3 gensim.py --scenario scenarios/normal.yaml [options]

Options:
    --channels <path>       channel registry (default: ../../configs/channels.toml)
    --scenario <path>       scenario file (required)
    --rate <hz>             packets per second (default: 10)
    --duration <sec>        stop after this many simulated seconds
    --speed <factor>        run faster than real time (default: 1.0)
    --seed <n>              random seed for reproducibility
    --deterministic         timestamps are sim_start + i*interval, not wall clock
    --quiet                 no stderr output
    --version               print version and exit

Exit codes:
    0   ran to completion (or --duration reached)
    1   error (bad scenario, missing channel, bad config)

Requires: Python 3.8+. PyYAML optional; falls back to a subset parser
if not installed.
"""

import argparse
import json
import math
import os
import random
import signal
import sys
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

VERSION = "0.1.0"

# ---------------------------------------------------------------------------
# Channel model — a small mirror of the C side's channel_desc_t
# ---------------------------------------------------------------------------

@dataclass
class Channel:
    id: int
    name: str
    type: str          # "f32" | "i32" | "u16" | "bool" | "enum"
    role: str          # "sensor" | "setpoint" | "ctrl_out" | "actuator"
                       # | "status" | "counter" | "unknown"
    unit: str = ""
    min_valid: float = 0.0
    max_valid: float = 0.0
    has_range: bool = False
    controllable: bool = False
    is_regime: bool = False

# ---------------------------------------------------------------------------
# Tiny TOML subset reader for channels.toml
#
# Handles exactly the shape that configs/channels.toml uses: [[channel]]
# headers and key = value lines with quoted strings, numbers, and
# booleans. Not a general TOML parser.
# ---------------------------------------------------------------------------

def parse_channels_toml(path: str) -> List[Channel]:
    channels: List[Channel] = []
    cur: Optional[Dict[str, Any]] = None
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()  # naive comment strip
            if not line:
                continue
            if line.startswith("[[") and line.endswith("]]"):
                table = line[2:-2].strip()
                if table != "channel":
                    continue
                if cur is not None:
                    channels.append(_channel_from_dict(cur))
                cur = {}
                continue
            if cur is None:
                continue
            if "=" not in line:
                raise ValueError(f"{path}:{lineno}: not a key=value line: {line!r}")
            key, val = line.split("=", 1)
            key = key.strip()
            val = val.strip()
            if val.startswith('"') and val.endswith('"') and len(val) >= 2:
                parsed: Any = val[1:-1]
            elif val == "true":
                parsed = True
            elif val == "false":
                parsed = False
            else:
                try:
                    parsed = float(val) if "." in val or "e" in val else int(val)
                except ValueError:
                    parsed = val
            cur[key] = parsed
    if cur is not None:
        channels.append(_channel_from_dict(cur))
    return channels

def _channel_from_dict(d: Dict[str, Any]) -> Channel:
    return Channel(
        id=int(d.get("id", 0)),
        name=str(d.get("name", "")),
        type=str(d.get("type", "f32")),
        role=str(d.get("role", "unknown")),
        unit=str(d.get("unit", "")),
        min_valid=float(d.get("min", 0.0)),
        max_valid=float(d.get("max", 0.0)),
        has_range=("min" in d or "max" in d),
        controllable=bool(d.get("controllable", False)),
        is_regime=bool(d.get("is_regime", False)),
    )

# ---------------------------------------------------------------------------
# Scenario model
# ---------------------------------------------------------------------------

@dataclass
class Event:
    at: float                # seconds since scenario start
    action: str              # "freeze" | "spike" | "drift" | "reset"
                             # | "link_drop" | "link_gap"
    ch: Optional[str] = None # channel name (for value actions)
    duration: float = 0.0    # seconds (for link_gap)
    magnitude: float = 0.0   # for spike
    rate: float = 0.0        # for drift (units per second)
    offset: float = 0.0      # for spike/drift (added to value)

@dataclass
class Scenario:
    name: str
    plant: str
    duration: float = 0.0    # 0 = no limit
    events: List[Event] = field(default_factory=list)

def load_scenario(path: str) -> Scenario:
    """Load a scenario from YAML. Uses PyYAML if available; falls back
    to a small parser for the specific shape our scenarios use."""
    text = open(path, "r", encoding="utf-8").read()
    try:
        import yaml  # type: ignore
        data = yaml.safe_load(text)
    except ImportError:
        data = _parse_simple_yaml(text)
    return _scenario_from_dict(data, path)

def _parse_simple_yaml(text: str) -> Dict[str, Any]:
    """
    Fallback YAML subset parser for scenarios. Handles:
      - top-level key: value
      - a list under `events:`
      - list items as `- key: value` with further `key: value` lines
    Does not handle nesting, anchors, multi-line strings, or flow
    style. Our scenario files use only the above.
    """
    result: Dict[str, Any] = {}
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        raw = lines[i]
        i += 1
        stripped = raw.split("#", 1)[0].rstrip()
        if not stripped.strip():
            continue
        if stripped.startswith(" ") or stripped.startswith("-"):
            continue  # belongs to a previous block, handled below
        if ":" not in stripped:
            continue
        key, val = stripped.split(":", 1)
        key = key.strip()
        val = val.strip()
        if val == "":
            # Either a nested block or a list. Check the next lines.
            items = []
            while i < len(lines):
                look = lines[i]
                if look.startswith("  -") or look.startswith("-"):
                    # gather this item's key-value pairs
                    item: Dict[str, Any] = {}
                    item_line = look.lstrip()[1:].strip()
                    if ":" in item_line:
                        k, v = item_line.split(":", 1)
                        item[k.strip()] = _scalar(v.strip())
                    i += 1
                    while i < len(lines):
                        cont = lines[i]
                        if cont.startswith("    ") or cont.startswith("  "):
                            cont_s = cont.strip()
                            if not cont_s or cont_s.startswith("#"):
                                i += 1
                                continue
                            if ":" in cont_s:
                                k, v = cont_s.split(":", 1)
                                item[k.strip()] = _scalar(v.strip())
                                i += 1
                                continue
                            i += 1
                            break
                        else:
                            break
                    items.append(item)
                else:
                    break
            if items:
                result[key] = items
            continue
        result[key] = _scalar(val)
    return result

def _scalar(v: str) -> Any:
    if v.startswith('"') and v.endswith('"'):
        return v[1:-1]
    if v == "true":  return True
    if v == "false": return False
    try:
        if "." in v or "e" in v.lower():
            return float(v)
        return int(v)
    except ValueError:
        # Try to strip trailing units like "10s", "2.5s"
        if v.endswith("s"):
            try:
                return float(v[:-1])
            except ValueError:
                pass
        return v

def _scenario_from_dict(d: Dict[str, Any], path: str) -> Scenario:
    if not isinstance(d, dict):
        raise ValueError(f"{path}: scenario must be a mapping")
    name = str(d.get("name", "unnamed"))
    plant = str(d.get("plant", "generic"))
    duration = float(d.get("duration", 0.0) or 0.0)

    raw_events = d.get("events", [])
    if not isinstance(raw_events, list):
        raise ValueError(f"{path}: 'events' must be a list")

    events: List[Event] = []
    for i, ev in enumerate(raw_events):
        if not isinstance(ev, dict):
            raise ValueError(f"{path}: event {i} must be a mapping")
        try:
            at = float(ev.get("at", 0.0))
            action = str(ev["action"])
        except KeyError as e:
            raise ValueError(f"{path}: event {i} missing {e}")
        events.append(Event(
            at=at,
            action=action,
            ch=str(ev["ch"]) if "ch" in ev else None,
            duration=float(ev.get("duration", 0.0)),
            magnitude=float(ev.get("magnitude", 0.0)),
            rate=float(ev.get("rate", 0.0)),
            offset=float(ev.get("offset", 0.0)),
        ))
    events.sort(key=lambda e: e.at)
    return Scenario(name=name, plant=plant, duration=duration, events=events)

# ---------------------------------------------------------------------------
# Plants — behavior generators for channels
# ---------------------------------------------------------------------------

class Plant:
    """
    Base class for scenario behaviors. A plant owns the current value of
    every channel and produces a new value each tick. Subclasses
    implement `step()` and can use the injected fault state.
    """

    def __init__(self, channels: List[Channel], seed: int = 0):
        self.channels = channels
        self.rng = random.Random(seed)
        self.t = 0.0                    # simulated time (seconds)
        # Fault state per channel name
        self.frozen: Dict[str, float] = {}    # ch -> frozen value
        self.drift_rate: Dict[str, float] = {} # ch -> rate per second
        self.spike_until: Dict[str, float] = {} # ch -> expiry time
        self.spike_value: Dict[str, float] = {} # ch -> offset magnitude

    # ---- fault API ----

    def freeze(self, ch_name: str) -> None:
        # Take the current value and hold it
        self.frozen[ch_name] = self._current(ch_name)

    def unfreeze(self, ch_name: str) -> None:
        self.frozen.pop(ch_name, None)

    def start_drift(self, ch_name: str, rate: float) -> None:
        self.drift_rate[ch_name] = rate

    def stop_drift(self, ch_name: str) -> None:
        self.drift_rate.pop(ch_name, None)

    def spike(self, ch_name: str, offset: float, duration: float) -> None:
        self.spike_value[ch_name] = offset
        self.spike_until[ch_name] = self.t + duration

    # ---- value lookup for the fault helpers ----

    def _current(self, ch_name: str) -> float:
        v = self._raw_value(ch_name)
        return 0.0 if v is None else float(v)

    def _raw_value(self, ch_name: str) -> Optional[float]:
        # Subclasses override. Returning None means "no value this tick".
        return None

    # ---- per-tick API ----

    def step(self, dt: float) -> Dict[str, float]:
        self.t += dt
        vals = self._generate(dt)

        # Apply faults in a fixed order: drift, then spike, then freeze.
        for ch, rate in list(self.drift_rate.items()):
            if ch in vals:
                vals[ch] += rate * dt
        for ch, until in list(self.spike_until.items()):
            if self.t < until and ch in vals:
                vals[ch] += self.spike_value.get(ch, 0.0)
            elif self.t >= until:
                self.spike_until.pop(ch, None)
                self.spike_value.pop(ch, None)
        for ch, val in list(self.frozen.items()):
            if ch in vals:
                vals[ch] = val
        return vals

    # Subclasses override this. Base returns zeros.
    def _generate(self, dt: float) -> Dict[str, float]:
        return {c.name: 0.0 for c in self.channels}


class GenericPlant(Plant):
    """
    Default plant: each channel gets a slow sine + noise, scaled by its
    range if it has one, or by a fixed amplitude otherwise. Regime and
    status channels are static.
    """

    def __init__(self, channels, seed=0):
        super().__init__(channels, seed)
        self._phase = {c.name: self.rng.uniform(0, 2 * math.pi) for c in channels}

    def _generate(self, dt):
        vals: Dict[str, float] = {}
        for c in self.channels:
            if c.role == "status" or c.type == "bool":
                # Status channels: boolean, default false.
                vals[c.name] = 0.0
                continue
            if c.role == "counter":
                vals[c.name] = float(int(self.t))
                continue
            # Sine + noise, centered at midpoint of range if defined.
            if c.has_range:
                mid = (c.min_valid + c.max_valid) / 2.0
                amp = (c.max_valid - c.min_valid) / 4.0  # 1/4 of range
            else:
                mid = 0.0
                amp = 10.0
            ph = self._phase[c.name]
            vals[c.name] = mid + amp * math.sin(2 * math.pi * self.t / 60.0 + ph) \
                          + self.rng.gauss(0, amp * 0.05)
        return vals


class HvacPlant(Plant):
    """
    Simple thermal model: an ambient temperature, a coolant temperature
    that drifts toward the setpoint, a pump whose duty follows the
    error, and a running flag. Channels are matched by name; unmatched
    channels get the generic sine behavior.
    """

    def __init__(self, channels, seed=0):
        super().__init__(channels, seed)
        self.coolant = 20.0
        self.ambient = 20.0
        self.setpoint = 60.0
        self.pump = 0.0

    def _generate(self, dt):
        # Ambient slowly oscillates.
        self.ambient = 20.0 + 5.0 * math.sin(2 * math.pi * self.t / 3600.0)
        # Coolant moves toward setpoint, faster when the pump is on.
        error = self.setpoint - self.coolant
        alpha = 0.05 + 0.5 * (self.pump / 100.0)
        self.coolant += error * alpha * dt
        # Pump duty follows the error.
        self.pump = max(0.0, min(100.0, 50.0 + error * 2.0))

        vals: Dict[str, float] = {}
        for c in self.channels:
            if c.name == "coolant_temp":
                vals[c.name] = self.coolant + self.rng.gauss(0, 0.1)
            elif c.name == "coolant_flow":
                vals[c.name] = 20.0 * (self.pump / 100.0) + self.rng.gauss(0, 0.2)
            elif c.name == "ambient_temp":
                vals[c.name] = self.ambient + self.rng.gauss(0, 0.1)
            elif c.name == "coolant_setpoint":
                vals[c.name] = self.setpoint
            elif c.name == "coolant_pid_out":
                vals[c.name] = self.pump
            elif c.name == "coolant_pump":
                vals[c.name] = self.pump
            elif c.name == "running":
                vals[c.name] = 1.0 if self.t > 1.0 else 0.0
            elif c.name == "fault_code":
                vals[c.name] = 0.0
            else:
                # Generic for others
                ph = hash(c.name) % 628 / 100.0
                vals[c.name] = 20.0 + 5.0 * math.sin(2 * math.pi * self.t / 60.0 + ph)
        return vals


PLANTS = {
    "generic": GenericPlant,
    "hvac": HvacPlant,
}

# ---------------------------------------------------------------------------
# Event scheduling
# ---------------------------------------------------------------------------

class EventQueue:
    def __init__(self, events: List[Event], plant: Plant):
        self.events = list(events)
        self.idx = 0
        self.plant = plant
        self.link_gaps: List[Tuple[float, float]] = []  # (start_t, end_t)

    def process(self, t: float) -> None:
        while self.idx < len(self.events) and self.events[self.idx].at <= t:
            ev = self.events[self.idx]
            self.idx += 1
            self._apply(ev)

    def _apply(self, ev: Event) -> None:
        if ev.action == "freeze" and ev.ch:
            self.plant.freeze(ev.ch)
        elif ev.action == "unfreeze" and ev.ch:
            self.plant.unfreeze(ev.ch)
        elif ev.action == "spike" and ev.ch:
            self.plant.spike(ev.ch, ev.offset, ev.duration or 5.0)
        elif ev.action == "drift" and ev.ch:
            self.plant.start_drift(ev.ch, ev.rate)
        elif ev.action == "reset" and ev.ch:
            self.plant.stop_drift(ev.ch)
            self.plant.unfreeze(ev.ch)
        elif ev.action == "link_gap":
            self.link_gaps.append((ev.at, ev.at + ev.duration))
        else:
            # Unknown action: warn but continue
            print(f"gensim: unknown action {ev.action!r}", file=sys.stderr)

    def link_down(self, t: float) -> bool:
        for start, end in self.link_gaps:
            if start <= t < end:
                return True
        return False

# ---------------------------------------------------------------------------
# JSONL packet emission
# ---------------------------------------------------------------------------

def format_value(v: float, ch_type: str) -> Any:
    if ch_type == "bool":
        return bool(round(v))
    if ch_type in ("i32", "u16", "enum"):
        return int(round(v))
    return round(v, 6)

def emit_packet(out, ts_ns: int, source_id: int,
                channels: List[Channel],
                values: Dict[str, float]) -> None:
    parts: List[str] = []
    for c in channels:
        if c.name not in values:
            continue
        v = format_value(values[c.name], c.type)
        # Role is emitted as an integer, matching the C enum.
        role_num = _role_to_int(c.role)
        item: Dict[str, Any] = {"id": c.id, "role": role_num}
        if c.type == "bool":
            item["type"] = "bool"
            item["v"] = bool(v)
        elif c.type in ("i32", "u16", "enum"):
            item["type"] = c.type
            item["v"] = int(v)
        else:
            item["v"] = float(v)
        parts.append(json.dumps(item, separators=(",", ":")))

    line = '{"t":%d,"src":%d,"ch":[%s]}\n' % (ts_ns, source_id, ",".join(parts))
    out.write(line)

def _role_to_int(role: str) -> int:
    return {
        "unknown": 0, "sensor": 1, "setpoint": 2, "ctrl_out": 3,
        "actuator": 4, "status": 5, "counter": 6,
    }.get(role, 0)

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

def run(args) -> int:
    channels = parse_channels_toml(args.channels)
    if not channels:
        print(f"gensim: no channels in {args.channels}", file=sys.stderr)
        return 1

    scenario = load_scenario(args.scenario)
    plant_cls = PLANTS.get(scenario.plant, GenericPlant)
    plant = plant_cls(channels, seed=args.seed)

    # Merge scenario duration with --duration (whichever is smaller if both set)
    dur = args.duration
    if scenario.duration > 0:
        dur = min(dur, scenario.duration) if dur > 0 else scenario.duration

    dt = 1.0 / args.rate
    sim_interval_ns = int(dt * 1e9)

    events = EventQueue(scenario.events, plant)

    if not args.quiet:
        print(
            f"gensim: scenario={scenario.name!r} plant={scenario.plant!r} "
            f"channels={len(channels)} rate={args.rate}Hz "
            f"duration={'unlimited' if dur == 0 else f'{dur}s'}",
            file=sys.stderr,
        )

    # Signal handling: Ctrl-C flushes and exits cleanly.
    stop = {"flag": False}
    def on_signal(signum, frame):
        stop["flag"] = True
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    start_wall = time.monotonic()
    start_ns = time.time_ns() if not args.deterministic else 1_000_000_000
    tick = 0
    packets_sent = 0

    try:
        while not stop["flag"]:
            sim_t = tick * dt
            if dur > 0 and sim_t >= dur:
                break

            events.process(sim_t)

            if events.link_down(sim_t):
                # Link is down: emit nothing this tick.
                pass
            else:
                values = plant.step(dt)
                if args.deterministic:
                    ts_ns = start_ns + tick * sim_interval_ns
                else:
                    ts_ns = start_ns + int((time.monotonic() - start_wall)
                                           * 1e9 * args.speed)
                emit_packet(sys.stdout, ts_ns, 1, channels, values)
                packets_sent += 1
                sys.stdout.flush()

            tick += 1

            # Wall-clock pacing. With speed=1, sleep until the next tick
            # would fire in real time. With speed=10, sleep 1/10th.
            if args.speed > 0:
                target = start_wall + (tick * dt) / args.speed
                now = time.monotonic()
                if target > now:
                    time.sleep(target - now)

    finally:
        if not args.quiet:
            print(f"\ngensim: sent {packets_sent} packets in {tick} ticks",
                  file=sys.stderr)
    return 0

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv):
    p = argparse.ArgumentParser(
        prog="gensim",
        description="Simulated source for Gardener.",
        add_help=True,
    )
    p.add_argument("--version", action="version", version=f"gensim {VERSION}")
    p.add_argument("--channels", default=_default_channels_path(),
                   help="channel registry TOML (default: %(default)s)")
    p.add_argument("--scenario", required=True,
                   help="scenario file (YAML or our subset)")
    p.add_argument("--rate", type=float, default=10.0,
                   help="packets per second (default: %(default)s)")
    p.add_argument("--duration", type=float, default=0.0,
                   help="stop after N simulated seconds (0 = no limit)")
    p.add_argument("--speed", type=float, default=1.0,
                   help="run faster than real time (default: %(default)s)")
    p.add_argument("--seed", type=int, default=0,
                   help="random seed (default: %(default)s)")
    p.add_argument("--deterministic", action="store_true",
                   help="use sim time for timestamps, not wall clock")
    p.add_argument("--quiet", action="store_true",
                   help="suppress stderr output")
    return p.parse_args(argv)

def _default_channels_path() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    # tools/gensim/gensim.py -> ../../configs/channels.toml
    return os.path.normpath(os.path.join(here, "..", "..", "configs",
                                         "channels.toml"))

def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        return run(args)
    except FileNotFoundError as e:
        print(f"gensim: {e}", file=sys.stderr)
        return 1
    except ValueError as e:
        print(f"gensim: {e}", file=sys.stderr)
        return 1
    except BrokenPipeError:
        # Emitting to a closed pipe (e.g. `| head`) is fine.
        return 0

if __name__ == "__main__":
    sys.exit(main())