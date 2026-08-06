#!/usr/bin/env python3
"""
EXP-FPGA-DIV-002 Phase B3, task item 1: head-of-line (HOL) blocking
quantification for the Phase B2 scheduler.

Methodology (disclosed, SIMULATED): a compact discrete-event SOFTWARE
REFERENCE MODEL of Phase B2's own scheduling rules (rtl/experimental/
q8_div/membrane_quant_stream_top_q8_dual_radix4_b2.sv), not literal
per-cycle RTL-internal-signal instrumentation (which would require
modifying B2's committed RTL to add debug ports, out of scope -- B1/B2
stay unmodified). Per-transaction service-time distributions are drawn
from this project's own REAL measured numbers (results/
b1-differential.json's own standalone-divider latency distribution).
shadow_reserved_count is tracked exactly as the real RTL does (incremented
at ADMISSION/issue time, not at tail-capture time) -- an earlier draft of
this model used tail-capture occupancy only and reproduced, in software,
the exact class of shadow-admission race the real RTL's own design
deliberately avoids; fixed here to match the real RTL's own logic before
producing any numbers.
"""
import random
import csv

random.seed(0xC0FFEE)

MODE_Q8_ENC, MODE_Q8_DEC, MODE_Q4_ENC, MODE_Q4_DEC = 0, 1, 2, 3
MODE_NAMES = {MODE_Q8_ENC: "Q8_ENC", MODE_Q8_DEC: "Q8_DEC", MODE_Q4_ENC: "Q4_ENC", MODE_Q4_DEC: "Q4_DEC"}

IN_FIFO_DEPTH = 16
L_MAX = 7
SHADOW_DEPTH = 1


def q8enc_service_time():
    return 5 + 1 + random.randint(2, 34)


def q4enc_service_time():
    return 2 + random.randint(2, 15)


class Txn:
    __slots__ = ("mode", "seq", "arrival_cycle", "done_cycle", "is_extra")

    def __init__(self, mode, arrival_cycle):
        self.mode = mode
        self.seq = None
        self.arrival_cycle = arrival_cycle
        self.done_cycle = None
        self.is_extra = False


def run_sim(n_cycles, mode_weights, warmup=2000):
    modes = list(mode_weights.keys())
    weights = list(mode_weights.values())

    fifo = []
    next_seq = 0
    next_retire_seq = 0

    q8_engine = None
    q8_done_at = None
    q8_hold = None

    q4_engine = None
    q4_done_at = None
    q4_hold = None

    tag_pipe = []          # list of [ready_cycle, Txn]
    shadow_hold = []       # list of Txn, at most SHADOW_DEPTH
    shadow_reserved_count = 0   # matches real RTL: reserved at admission, freed at final drain

    stall = {"fifo_empty": 0, "head_busy_q8enc": 0, "head_busy_q4enc": 0,
             "head_shadow_full": 0, "issued": 0}
    bypass_count_hist = {}
    distance_hist = {}
    blocked_head_mode = {m: 0 for m in (MODE_Q8_ENC, MODE_Q8_DEC, MODE_Q4_ENC, MODE_Q4_DEC)}
    bypassable_mode = {m: 0 for m in (MODE_Q8_ENC, MODE_Q8_DEC, MODE_Q4_ENC, MODE_Q4_DEC)}
    occ_hist = {}
    latencies = {m: [] for m in (MODE_Q8_ENC, MODE_Q8_DEC, MODE_Q4_ENC, MODE_Q4_DEC)}
    accepted = 0
    retired = 0
    overflow_events = 0

    for cycle in range(n_cycles):
        record = cycle >= warmup
        if record:
            occ_hist[len(fifo)] = occ_hist.get(len(fifo), 0) + 1

        if len(fifo) < IN_FIFO_DEPTH and random.random() < 0.7:
            m = random.choices(modes, weights=weights)[0]
            fifo.append(Txn(m, cycle))
            accepted += 1

        q8_pending = (q8_engine is not None) or (q8_hold is not None)
        q4_pending = (q4_engine is not None) or (q4_hold is not None)
        primary_pending = q8_pending or q4_pending
        # matches real RTL's tagpipe_can_issue: gate on RESERVED count
        # (admitted-but-not-yet-fully-drained), not on tail-capture
        # occupancy alone.
        shadow_room = (not primary_pending) or (shadow_reserved_count < SHADOW_DEPTH)

        def issuable(m):
            if m == MODE_Q8_ENC:
                return not q8_pending
            if m == MODE_Q4_ENC:
                return not q4_pending
            return shadow_room

        if not fifo:
            if record:
                stall["fifo_empty"] += 1
        else:
            head = fifo[0]
            if issuable(head.mode):
                head.seq = next_seq
                next_seq += 1
                fifo.pop(0)
                if record:
                    stall["issued"] += 1
                if head.mode == MODE_Q8_ENC:
                    q8_engine = head
                    q8_done_at = cycle + q8enc_service_time()
                elif head.mode == MODE_Q4_ENC:
                    q4_engine = head
                    q4_done_at = cycle + q4enc_service_time()
                else:
                    if primary_pending:
                        head.is_extra = True
                        shadow_reserved_count += 1
                    tag_pipe.append([cycle + L_MAX, head])
            else:
                if record:
                    if head.mode == MODE_Q8_ENC:
                        stall["head_busy_q8enc"] += 1
                    elif head.mode == MODE_Q4_ENC:
                        stall["head_busy_q4enc"] += 1
                    else:
                        stall["head_shadow_full"] += 1
                    blocked_head_mode[head.mode] += 1
                    n_bypass = 0
                    first_dist = None
                    for idx in range(1, len(fifo)):
                        if issuable(fifo[idx].mode):
                            n_bypass += 1
                            bypassable_mode[fifo[idx].mode] += 1
                            if first_dist is None:
                                first_dist = idx
                    bypass_count_hist[n_bypass] = bypass_count_hist.get(n_bypass, 0) + 1
                    if first_dist is not None:
                        distance_hist[first_dist] = distance_hist.get(first_dist, 0) + 1

        if q8_engine is not None and cycle >= q8_done_at:
            q8_hold = q8_engine
            q8_hold.done_cycle = cycle
            q8_engine = None
        if q4_engine is not None and cycle >= q4_done_at:
            q4_hold = q4_engine
            q4_hold.done_cycle = cycle
            q4_engine = None

        tagpipe_direct = None
        shadow_freed_direct = False
        if tag_pipe and tag_pipe[0][0] <= cycle:
            ready_cycle, txn = tag_pipe.pop(0)
            if txn.seq == next_retire_seq:
                tagpipe_direct = txn
                txn.done_cycle = cycle
                if txn.is_extra:
                    shadow_freed_direct = True
            else:
                if len(shadow_hold) < SHADOW_DEPTH:
                    txn.done_cycle = cycle
                    shadow_hold.append(txn)
                else:
                    overflow_events += 1  # should never happen given correct gating

        retired_this_cycle = None
        shadow_freed_from_hold = False
        if q8_hold is not None and q8_hold.seq == next_retire_seq:
            retired_this_cycle = q8_hold
            q8_hold = None
        elif q4_hold is not None and q4_hold.seq == next_retire_seq:
            retired_this_cycle = q4_hold
            q4_hold = None
        elif shadow_hold and shadow_hold[0].seq == next_retire_seq:
            retired_this_cycle = shadow_hold.pop(0)
            shadow_freed_from_hold = True
        elif tagpipe_direct is not None:
            retired_this_cycle = tagpipe_direct

        if shadow_freed_direct or shadow_freed_from_hold:
            shadow_reserved_count -= 1

        if retired_this_cycle is not None:
            next_retire_seq += 1
            retired += 1
            if record:
                latencies[retired_this_cycle.mode].append(retired_this_cycle.done_cycle - retired_this_cycle.arrival_cycle)

    total_stall_cycles = sum(v for k, v in stall.items() if k != "issued")
    return {
        "stall": stall, "total_stall_cycles": total_stall_cycles,
        "bypass_count_hist": bypass_count_hist, "distance_hist": distance_hist,
        "blocked_head_mode": blocked_head_mode, "bypassable_mode": bypassable_mode,
        "occ_hist": occ_hist, "latencies": latencies,
        "accepted": accepted, "retired": retired, "overflow_events": overflow_events,
    }


def mean(xs):
    return sum(xs) / len(xs) if xs else 0.0


if __name__ == "__main__":
    N = 400000
    profiles = {
        "10pct_Q8ENC": 0.10, "20pct_Q8ENC": 0.20, "25pct_Q8ENC": 0.25, "40pct_Q8ENC": 0.40,
    }
    rows = []
    for name, dens in profiles.items():
        other = (1.0 - dens) / 3.0
        weights = {MODE_Q8_ENC: dens, MODE_Q8_DEC: other, MODE_Q4_ENC: other, MODE_Q4_DEC: other}
        r = run_sim(N, weights)
        assert r["overflow_events"] == 0, f"{name}: overflow_events={r['overflow_events']} -- gating bug"
        s = r["stall"]
        total = r["total_stall_cycles"]
        frac_busy_encode = (s["head_busy_q8enc"] + s["head_busy_q4enc"]) / total if total else 0
        n_measured = sum(r["bypass_count_hist"].values())
        mean_bypass = sum(k * v for k, v in r["bypass_count_hist"].items()) / n_measured if n_measured else 0
        max_bypass = max(r["bypass_count_hist"].keys()) if r["bypass_count_hist"] else 0
        n_dist = sum(r["distance_hist"].values())
        mean_dist = sum(k * v for k, v in r["distance_hist"].items()) / n_dist if n_dist else 0
        within2 = sum(v for k, v in r["distance_hist"].items() if k <= 2) / n_dist if n_dist else 0
        within4 = sum(v for k, v in r["distance_hist"].items() if k <= 4) / n_dist if n_dist else 0
        rows.append({
            "profile": name, "q8_enc_density": dens,
            "stall_fifo_empty_cyc": s["fifo_empty"], "stall_head_busy_q8enc_cyc": s["head_busy_q8enc"],
            "stall_head_busy_q4enc_cyc": s["head_busy_q4enc"], "stall_head_shadow_full_cyc": s["head_shadow_full"],
            "issued_cyc": s["issued"], "total_stall_cyc": total,
            "frac_stall_from_busy_encode_head": round(frac_busy_encode, 4),
            "mean_bypassable_younger_behind_blocked_head": round(mean_bypass, 3),
            "max_bypassable_younger_observed": max_bypass,
            "mean_distance_to_first_executable_younger": round(mean_dist, 3),
            "frac_blocked_cycles_first_executable_within_lookahead2": round(within2, 4),
            "frac_blocked_cycles_first_executable_within_lookahead4": round(within4, 4),
            "blocked_head_mode_Q8ENC": r["blocked_head_mode"][MODE_Q8_ENC],
            "blocked_head_mode_Q8DEC": r["blocked_head_mode"][MODE_Q8_DEC],
            "blocked_head_mode_Q4ENC": r["blocked_head_mode"][MODE_Q4_ENC],
            "blocked_head_mode_Q4DEC": r["blocked_head_mode"][MODE_Q4_DEC],
            "bypassable_mode_Q8ENC": r["bypassable_mode"][MODE_Q8_ENC],
            "bypassable_mode_Q8DEC": r["bypassable_mode"][MODE_Q8_DEC],
            "bypassable_mode_Q4ENC": r["bypassable_mode"][MODE_Q4_ENC],
            "bypassable_mode_Q4DEC": r["bypassable_mode"][MODE_Q4_DEC],
            "mean_latency_Q8_ENC": round(mean(r["latencies"][MODE_Q8_ENC]), 3),
            "mean_latency_Q8_DEC": round(mean(r["latencies"][MODE_Q8_DEC]), 3),
            "mean_latency_Q4_ENC": round(mean(r["latencies"][MODE_Q4_ENC]), 3),
            "mean_latency_Q4_DEC": round(mean(r["latencies"][MODE_Q4_DEC]), 3),
            "accepted": r["accepted"], "retired": r["retired"],
        })
        print(name, "accepted=", r["accepted"], "retired=", r["retired"],
              "mean_lat_Q8DEC=", round(mean(r["latencies"][MODE_Q8_DEC]), 2),
              "mean_lat_Q4DEC=", round(mean(r["latencies"][MODE_Q4_DEC]), 2),
              "mean_lat_Q4ENC=", round(mean(r["latencies"][MODE_Q4_ENC]), 2))

    import os
    import sys
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        repo_root, "experiments/EXP-FPGA-DIV-002/results/b3-hol-profile.csv")
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {out_path}")
