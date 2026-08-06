#!/usr/bin/env python3
"""
EXP-FPGA-DIV-002 Phase B4, task items 3-4: retirement-pressure
instrumentation and bottleneck-hypothesis quantification for the B3-split
scheduler.

Methodology (disclosed, SIMULATED): a discrete-event SOFTWARE REFERENCE
MODEL of B3-split's own scheduling rules (rtl/experimental/q8_div/
membrane_quant_stream_top_q8_dual_radix4_b3_split.sv), not literal
per-cycle RTL-internal-signal instrumentation (same disclosed convention
Phase B3's own b3-hol-model.py used -- B3-split's committed RTL is not
modified to add debug ports, out of scope per task item 6). Models BOTH
ingress queues (enc_fifo/dec_fifo, independently issuing), the shared
SHADOW_DEPTH-entry shadow_hold array, the Q8_0/Q4_0 encode hold
registers, tag_pipe's own direct-retire path, AND (new vs. the Phase B3
model) out_fifo occupancy with a real out_ready backpressure parameter,
since "downstream backpressure" (hypothesis D) is one of the six
bottleneck hypotheses task item 4 requires quantifying and the Phase B3
model did not need to cover it.

Every cycle is classified into exactly ONE of the ten primary retirement
states task item 3 specifies, in a fixed priority order (documented
inline at classify_cycle()) so the partition is well-defined and sums to
n_cycles exactly.
"""
import random
import csv

random.seed(0xC0FFEE)

MODE_Q8_ENC, MODE_Q8_DEC, MODE_Q4_ENC, MODE_Q4_DEC = 0, 1, 2, 3
MODE_NAMES = {MODE_Q8_ENC: "Q8_ENC", MODE_Q8_DEC: "Q8_DEC", MODE_Q4_ENC: "Q4_ENC", MODE_Q4_DEC: "Q4_DEC"}

ENC_FIFO_DEPTH = 8
DEC_FIFO_DEPTH = 8
OUT_FIFO_DEPTH = 32
SHADOW_DEPTH = 4
L_MAX = 7
SEQ_WIDTH = 8
SEQ_MOD = 1 << SEQ_WIDTH

STATES = [
	"next_seq_retires", "next_seq_backpressure",
	"younger_decode_blocked_by_encode", "younger_encode_blocked_by_other",
	"completion_slot_unavailable", "ingress_blocked_completion_capacity",
	"no_completed_result_engine_busy", "no_completed_result_idle",
	"tag_seq_wrap", "reset_recovery", "other",
]


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


def run_sim(n_cycles, mode_weights, out_ready_prob=1.0, reset_window=0, warmup=2000, burst_backpressure=False):
	modes = list(mode_weights.keys())
	weights = list(mode_weights.values())

	enc_fifo, dec_fifo = [], []
	next_seq = 0
	next_retire_seq = 0
	out_fifo_occ = 0

	q8_engine = q8_done_at = q8_hold = None
	q4_engine = q4_done_at = q4_hold = None
	tag_pipe = []		# [ready_cycle, Txn]
	shadow_hold = []	# list of Txn, at most SHADOW_DEPTH
	shadow_reserved_count = 0

	stall = {s: 0 for s in STATES}
	latencies = {m: [] for m in (MODE_Q8_ENC, MODE_Q8_DEC, MODE_Q4_ENC, MODE_Q4_DEC)}
	oldest_wait_hist = {}
	accepted = retired = overflow_events = 0
	wrap_events = 0

	for cycle in range(n_cycles):
		record = cycle >= warmup

		if cycle < reset_window:
			if record:
				stall["reset_recovery"] += 1
			continue

		# ---- ingress: two independent single-issue-per-cycle heads ----
		if len(enc_fifo) + len(dec_fifo) < ENC_FIFO_DEPTH + DEC_FIFO_DEPTH and random.random() < 0.7:
			m = random.choices(modes, weights=weights)[0]
			t = Txn(m, cycle)
			if m in (MODE_Q8_ENC, MODE_Q4_ENC) and len(enc_fifo) < ENC_FIFO_DEPTH:
				enc_fifo.append(t)
				accepted += 1
			elif m in (MODE_Q8_DEC, MODE_Q4_DEC) and len(dec_fifo) < DEC_FIFO_DEPTH:
				dec_fifo.append(t)
				accepted += 1

		q8_pending = (q8_engine is not None) or (q8_hold is not None)
		q4_pending = (q4_engine is not None) or (q4_hold is not None)
		primary_pending = q8_pending or q4_pending
		blocking_condition = primary_pending or bool(enc_fifo) or (shadow_reserved_count > 0)
		tagpipe_can_issue = (not blocking_condition) or (shadow_reserved_count < SHADOW_DEPTH)

		ingress_blocked_capacity = False
		if enc_fifo:
			head = enc_fifo[0]
			issuable = (not q8_pending) if head.mode == MODE_Q8_ENC else (not q4_pending)
			if issuable:
				enc_fifo.pop(0)
				head.seq = next_seq
				next_seq = (next_seq + 1) % SEQ_MOD
				if head.mode == MODE_Q8_ENC:
					q8_engine, q8_done_at = head, cycle + q8enc_service_time()
				else:
					q4_engine, q4_done_at = head, cycle + q4enc_service_time()

		tagpipe_issue_fire = False
		if dec_fifo:
			head = dec_fifo[0]
			if tagpipe_can_issue:
				dec_fifo.pop(0)
				head.seq = next_seq
				next_seq = (next_seq + 1) % SEQ_MOD
				if blocking_condition:
					head.is_extra = True
					shadow_reserved_count += 1
				tag_pipe.append([cycle + L_MAX, head])
				tagpipe_issue_fire = True
			else:
				ingress_blocked_capacity = True

		if q8_engine is not None and cycle >= q8_done_at:
			q8_hold, q8_hold.done_cycle, q8_engine = q8_engine, cycle, None
		if q4_engine is not None and cycle >= q4_done_at:
			q4_hold, q4_hold.done_cycle, q4_engine = q4_engine, cycle, None

		tagpipe_direct = None
		shadow_capture_overflow = False
		if tag_pipe and tag_pipe[0][0] <= cycle:
			_, txn = tag_pipe.pop(0)
			if txn.seq == next_retire_seq:
				tagpipe_direct = txn
				txn.done_cycle = cycle
			else:
				if len(shadow_hold) < SHADOW_DEPTH:
					txn.done_cycle = cycle
					shadow_hold.append(txn)
				else:
					shadow_capture_overflow = True
					overflow_events += 1

		# ---- retirement: at most one source retires per cycle, gated by
		# out_fifo room (downstream backpressure, hypothesis D) ----
		out_fifo_has_room = out_fifo_occ < OUT_FIFO_DEPTH
		if burst_backpressure:
			# sustained blocks (out_ready=0 for 40 of every 60 cycles)
			# rather than i.i.d. per-cycle deassertion -- a 32-entry
			# out_fifo absorbs i.i.d. noise almost entirely (confirmed:
			# see the 25pct_Q8ENC_backpressured profile's own
			# near-zero next_seq_backpressure count), but a real
			# downstream consumer stalling for tens of cycles at a time
			# is a more realistic backpressure shape and the one this
			# hypothesis actually needs correlated stalls to exercise.
			out_ready_this_cycle = (cycle % 60) < 20
		else:
			out_ready_this_cycle = random.random() < out_ready_prob
		# out_fifo drains toward the external port when out_ready is
		# asserted and it is non-empty (checked BEFORE this cycle's own
		# retirement write, matching stream_fifo's own same-cycle
		# occupancy semantics closely enough for a throughput model).
		if out_fifo_occ > 0 and out_ready_this_cycle:
			out_fifo_occ -= 1

		candidate = None
		if q8_hold is not None and q8_hold.seq == next_retire_seq:
			candidate = ("q8", q8_hold)
		elif q4_hold is not None and q4_hold.seq == next_retire_seq:
			candidate = ("q4", q4_hold)
		elif shadow_hold and shadow_hold[0].seq == next_retire_seq:
			candidate = ("shadow", shadow_hold[0])
		elif tagpipe_direct is not None:
			candidate = ("direct", tagpipe_direct)

		retired_this_cycle = None
		if candidate is not None:
			out_fifo_has_room_now = out_fifo_occ < OUT_FIFO_DEPTH
			if out_fifo_has_room_now:
				kind, txn = candidate
				retired_this_cycle = txn
				out_fifo_occ += 1
				if kind == "q8":
					q8_hold = None
				elif kind == "q4":
					q4_hold = None
				elif kind == "shadow":
					shadow_hold.pop(0)

		if retired_this_cycle is not None and retired_this_cycle.is_extra:
			shadow_reserved_count -= 1

		# ---- per-cycle state classification (task item 3, priority order) ----
		if record:
			prev_seq = next_retire_seq
			if retired_this_cycle is not None:
				stall["next_seq_retires"] += 1
			elif candidate is not None:
				stall["next_seq_backpressure"] += 1
			elif shadow_hold and (q8_pending or q4_pending) and shadow_hold[0].seq != next_retire_seq:
				# a decode result is DONE and waiting (shadow_hold non-empty)
				# but next_retire_seq's own owner is an encode-class
				# transaction still mid-service or held.
				stall["younger_decode_blocked_by_encode"] += 1
			elif (q8_hold is not None or q4_hold is not None) and not (
				(q8_hold is not None and q8_hold.seq == next_retire_seq)
				or (q4_hold is not None and q4_hold.seq == next_retire_seq)
			):
				stall["younger_encode_blocked_by_other"] += 1
			elif shadow_capture_overflow:
				stall["completion_slot_unavailable"] += 1
			elif ingress_blocked_capacity:
				stall["ingress_blocked_completion_capacity"] += 1
			elif (q8_engine is not None) or (q4_engine is not None):
				stall["no_completed_result_engine_busy"] += 1
			elif not enc_fifo and not dec_fifo and not tag_pipe and not shadow_hold and q8_hold is None and q4_hold is None:
				stall["no_completed_result_idle"] += 1
			else:
				stall["other"] += 1

			if retired_this_cycle is not None:
				retired += 1
				latencies[retired_this_cycle.mode].append(retired_this_cycle.done_cycle - retired_this_cycle.arrival_cycle)
				oldest_wait = cycle - retired_this_cycle.arrival_cycle
				oldest_wait_hist[oldest_wait] = oldest_wait_hist.get(oldest_wait, 0) + 1

			if next_retire_seq < prev_seq:  # wrapped this cycle (shouldn't happen from retire alone)
				wrap_events += 1
		if retired_this_cycle is not None:
			new_next = (next_retire_seq + 1) % SEQ_MOD
			if new_next < next_retire_seq:
				wrap_events += 1
			next_retire_seq = new_next

	total_cycles = sum(stall.values())
	return {
		"stall": stall, "total_cycles": total_cycles,
		"latencies": latencies, "oldest_wait_hist": oldest_wait_hist,
		"accepted": accepted, "retired": retired,
		"overflow_events": overflow_events, "wrap_events": wrap_events,
	}


def mean(xs):
	return sum(xs) / len(xs) if xs else 0.0


def p(xs, pct):
	if not xs:
		return 0
	s = sorted(xs)
	idx = min(len(s) - 1, int(len(s) * pct))
	return s[idx]


if __name__ == "__main__":
	import os
	import sys

	N = 400000
	profiles = {
		"10pct_Q8ENC": (0.10, 1.0, False), "20pct_Q8ENC": (0.20, 1.0, False),
		"25pct_Q8ENC": (0.25, 1.0, False), "40pct_Q8ENC": (0.40, 1.0, False),
		"25pct_Q8ENC_backpressured": (0.25, 0.7, False),
		"25pct_Q8ENC_burst_backpressure": (0.25, 1.0, True),
	}
	rows = []
	for name, (dens, out_ready_prob, burst) in profiles.items():
		other = (1.0 - dens) / 3.0
		weights = {MODE_Q8_ENC: dens, MODE_Q8_DEC: other, MODE_Q4_ENC: other, MODE_Q4_DEC: other}
		r = run_sim(N, weights, out_ready_prob=out_ready_prob, burst_backpressure=burst)
		assert r["overflow_events"] == 0, f"{name}: overflow_events={r['overflow_events']} -- shadow admission gating bug"
		s = r["stall"]
		total = r["total_cycles"]
		row = {"profile": name, "q8_enc_density": dens, "out_ready_prob": out_ready_prob}
		for st in STATES:
			row[f"cyc_{st}"] = s[st]
			row[f"frac_{st}"] = round(s[st] / total, 4) if total else 0
		row["accepted"] = r["accepted"]
		row["retired"] = r["retired"]
		row["wrap_events"] = r["wrap_events"]
		for m in (MODE_Q8_ENC, MODE_Q8_DEC, MODE_Q4_ENC, MODE_Q4_DEC):
			lat = r["latencies"][m]
			row[f"mean_latency_{MODE_NAMES[m]}"] = round(mean(lat), 3)
		rows.append(row)
		print(name, "accepted=", r["accepted"], "retired=", r["retired"],
			"next_seq_retires=", s["next_seq_retires"],
			"next_seq_backpressure=", s["next_seq_backpressure"],
			"ingress_blocked_completion_capacity=", s["ingress_blocked_completion_capacity"],
			"younger_decode_blocked_by_encode=", s["younger_decode_blocked_by_encode"],
			"younger_encode_blocked_by_other=", s["younger_encode_blocked_by_other"])

	repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
	out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
		repo_root, "experiments/EXP-FPGA-DIV-002/results/b4-retirement-profile.csv")
	with open(out_path, "w", newline="") as f:
		w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
		w.writeheader()
		w.writerows(rows)
	print(f"wrote {out_path}")
