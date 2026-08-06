// EXP-FPGA-DIV-001 Phase B3: a small, bounded completion reorder buffer.
//
// Problem this solves (full derivation in
// experiments/EXP-FPGA-DIV-001/phase-b3-root-cause.md): Phase B2's top
// level has exactly two completion sources (the shared fixed-latency
// `tag_pipe` and Q4_0 encode's own direct path) that must retire into a
// single-word-per-cycle output port in GLOBAL issue order, even though
// Phase B2's iterative Q4_0 divider makes one of those two sources' latency
// variable and usually much longer than the other's. Phase B2's own fix was
// to prevent issuance of anything while a Q4_0 encode transaction is in
// flight, so no "early" completion can ever occur -- correct, but it stalls
// two compute chains that share no resource with the divider at all.
//
// This module instead lets completions arrive in ANY order (up to
// `DEPTH` of them may be "ahead of turn" at once) and drains them to a
// single output port strictly in the order they were issued, using a
// small direct-mapped buffer indexed by a sequence number assigned at
// issue. It has exactly two completion input ports because this design
// has exactly two completion sources -- not a general N-way scheduler.
//
// ---- how correctness holds with a DIRECT-MAPPED (not associative) buffer ----
// The caller must never allow more than `DEPTH` transactions to be
// outstanding (issued, not yet drained) at once -- this module exposes
// `outstanding` and `issue_allow` (`outstanding < DEPTH`) for exactly that
// purpose; the caller is expected to gate its own issuance on
// `issue_allow` (see membrane_quant_stream_top_b3.sv). Given that
// invariant, any two simultaneously-outstanding transactions have
// distinct `seq mod DEPTH` (their sequence numbers are consecutive
// integers spanning fewer than `DEPTH` values), so indexing the buffer by
// `seq mod DEPTH` can never alias two live entries onto the same slot --
// checked at runtime by the `` `ifndef SYNTHESIS `` assertions below, not
// just argued here. `DEPTH` must be a power of two (1, 2, 4, or 8 in this
// experiment) so `mod DEPTH` is a cheap bitmask, not a divider.
//
// ---- latency cost of this module ----
// A completion is written into the buffer the cycle it arrives and can
// only be observed (and drained) starting the following cycle -- i.e.
// this module adds a flat +1 cycle of latency to EVERY mode versus a
// hypothetical direct (bypass) connection, even when a completion arrives
// exactly in turn. This is a deliberate simplicity choice: a same-cycle
// bypass path would need to arbitrate "did a fresh completion arrive that
// happens to be next AND is the buffer's own registered head also valid
// this cycle" combinationally, which is extra logic to save exactly one
// cycle out of (for Q4_0 encode) an already-hundreds-of-cycles latency --
// not worth the added complexity or the extra combinational path this
// close to two independent completion sources.
//
// ---- reset ----
// Synchronous to `rst_n` deasserting (same async-reset convention as
// every other module in this experiment): all buffer entries invalidated,
// `outstanding` and both sequence pointers return to 0. Any transaction
// that was mid-flight through the caller's own compute chains at reset
// time is the CALLER's responsibility to have already discarded (exactly
// as Phase B2 disclosses for `fp32_div_iterative_exact` -- reset there is
// unconditional and safe because no external state is written
// mid-computation); this module only ever holds already-COMPLETED
// payloads, which reset is equally safe to simply drop, since a reset
// flushes the entire datapath (out_valid must never be asserted with
// stale data after reset -- checked by the existing top-level testbench's
// own reset-mid-stream stage, unchanged for B3).
module membrane_completion_reorder #(
	parameter int DEPTH = 4,
	parameter int SEQ_WIDTH = 8,
	parameter int PAYLOAD_WIDTH = 531
) (
	input	logic				clk,
	input	logic				rst_n,

	// completion port A (e.g. the shared fixed-latency tag_pipe path)
	input	logic				a_valid,
	input	logic	[SEQ_WIDTH-1:0]		a_seq,
	input	logic	[PAYLOAD_WIDTH-1:0]	a_payload,

	// completion port B (e.g. Q4_0 encode's own direct path)
	input	logic				b_valid,
	input	logic	[SEQ_WIDTH-1:0]		b_seq,
	input	logic	[PAYLOAD_WIDTH-1:0]	b_payload,

	// issue side: caller asks "may I issue a new transaction this cycle"
	// via issue_allow (combinational), and if it does, must tag that
	// transaction with issue_seq (combinational, valid only when
	// issue_allow is 1) and pulse issue_fire the same cycle.
	input	logic				issue_fire,
	output	logic	[SEQ_WIDTH-1:0]		issue_seq,
	output	logic				issue_allow,

	// drain side: single in-order output port
	output	logic				out_valid,
	input	logic				out_ready,
	output	logic	[PAYLOAD_WIDTH-1:0]	out_payload,

	output	logic	[$clog2(DEPTH+1)-1:0]	outstanding	/* verilator public */
);

	localparam int IDX_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH);
	localparam logic [IDX_W-1:0] IDX_MASK = (DEPTH == 1) ? '0 : (IDX_W)'(DEPTH - 1);

	function automatic logic [IDX_W-1:0] idx_of(input logic [SEQ_WIDTH-1:0] s);
		idx_of = s[IDX_W-1:0] & IDX_MASK;
	endfunction

	logic				buf_valid	[0:DEPTH-1];
	logic	[PAYLOAD_WIDTH-1:0]	buf_payload	[0:DEPTH-1];

	logic	[SEQ_WIDTH-1:0]	seq_issue_next;
	logic	[SEQ_WIDTH-1:0]	seq_retire_next;

	assign issue_seq   = seq_issue_next;
	assign issue_allow = (outstanding < DEPTH);

	logic	head_valid;
	logic	pop_fire;

	assign head_valid   = buf_valid[idx_of(seq_retire_next)];
	assign out_valid    = head_valid;
	assign out_payload  = buf_payload[idx_of(seq_retire_next)];
	assign pop_fire     = head_valid && out_ready;

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			for (int k = 0; k < DEPTH; k++)
				buf_valid[k] <= 1'b0;
		end else begin
			// Clear the slot being drained this cycle, then (independently)
			// accept up to two fresh completions -- safe as one block
			// because the outstanding<DEPTH invariant guarantees a_seq,
			// b_seq, and the just-popped seq_retire_next never share an
			// index while all three conditions can be true simultaneously
			// (asserted below, not just assumed).
			if (pop_fire)
				buf_valid[idx_of(seq_retire_next)] <= 1'b0;
			if (a_valid) begin
				buf_valid[idx_of(a_seq)]   <= 1'b1;
				buf_payload[idx_of(a_seq)] <= a_payload;
			end
			if (b_valid) begin
				buf_valid[idx_of(b_seq)]   <= 1'b1;
				buf_payload[idx_of(b_seq)] <= b_payload;
			end
		end
	end

	always_ff @(posedge clk or negedge rst_n) begin
		if (!rst_n) begin
			seq_issue_next  <= '0;
			seq_retire_next <= '0;
			outstanding     <= '0;
		end else begin
			if (issue_fire)
				seq_issue_next <= seq_issue_next + (SEQ_WIDTH)'(1);
			if (pop_fire)
				seq_retire_next <= seq_retire_next + (SEQ_WIDTH)'(1);
			outstanding <= outstanding
				+ ($clog2(DEPTH+1))'(issue_fire ? 1 : 0)
				- ($clog2(DEPTH+1))'(pop_fire ? 1 : 0);
		end
	end

`ifndef SYNTHESIS
	initial begin
		assert ((DEPTH & (DEPTH - 1)) == 0)
			else $error("membrane_completion_reorder: DEPTH=%0d is not a power of two", DEPTH);
		assert (DEPTH >= 1)
			else $error("membrane_completion_reorder: DEPTH must be >= 1");
	end

	always_ff @(posedge clk) begin
		if (rst_n) begin
			assert (!(a_valid && b_valid && idx_of(a_seq) == idx_of(b_seq)))
				else $error("membrane_completion_reorder: two completions collided on the same buffer index this cycle -- outstanding<DEPTH invariant violated by caller");
			assert (!(a_valid && buf_valid[idx_of(a_seq)] && !(pop_fire && idx_of(seq_retire_next) == idx_of(a_seq))))
				else $error("membrane_completion_reorder: port A completion landed on an already-occupied, not-draining-this-cycle slot -- duplicate completion or index aliasing");
			assert (!(b_valid && buf_valid[idx_of(b_seq)] && !(pop_fire && idx_of(seq_retire_next) == idx_of(b_seq))))
				else $error("membrane_completion_reorder: port B completion landed on an already-occupied, not-draining-this-cycle slot -- duplicate completion or index aliasing");
			assert (outstanding <= DEPTH)
				else $error("membrane_completion_reorder: outstanding exceeded DEPTH -- issue_allow was not honored by caller");
		end
	end
`endif
endmodule
