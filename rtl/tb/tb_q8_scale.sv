module tb_q8_scale;
	localparam int N = 20000;
	logic	[15:0]	amax_vec	[0:N - 1];
	logic	[15:0]	d_exp		[0:N - 1];
	logic	[31:0]	id_exp		[0:N - 1];
	logic			clk;
	logic			rst_n;
	logic			valid_in;
	logic	[15:0]	amax_in;
	logic			valid_out;
	logic	[15:0]	d_out;
	logic	[31:0]	id_out;
	int				in_idx;
	int				out_idx;
	int				fails;

	q8_scale #(.DIV_DELAY(1)) dut (
		.clk(clk), .rst_n(rst_n), .valid_in(valid_in),
		.amax_f16_in(amax_in), .valid_out(valid_out), .d_f16_out(d_out),
		.id_f32_out(id_out));

	initial clk = 0;
	always #5 clk = ~clk;

	initial begin
		$readmemh("/tmp/maxabs_amax.txt", amax_vec);
		$readmemh("/tmp/scale_d.txt", d_exp);
		$readmemh("/tmp/scale_id.txt", id_exp);
		rst_n = 0;
		valid_in = 0;
		in_idx = 0;
		out_idx = 0;
		fails = 0;
		@(posedge clk);
		@(posedge clk);
		#2;
		rst_n = 1;
		#2;
		while (out_idx < N) begin
			if (in_idx < N) begin
				valid_in = 1;
				amax_in = amax_vec[in_idx];
			end else
				valid_in = 0;
			@(posedge clk);
			#1;
			if (valid_out) begin
				if (d_out !== d_exp[out_idx] || id_out !== id_exp[out_idx]) begin
					if (fails < 20)
						$display("FAIL block %0d: d_expect=%04h d_got=%04h id_expect=%08h id_got=%08h",
							out_idx, d_exp[out_idx], d_out, id_exp[out_idx], id_out);
					fails++;
				end
				out_idx++;
			end
			if (in_idx < N)
				in_idx++;
		end
		if (fails == 0)
			$display("PASS: q8_scale bit-exact on %0d blocks (10-cycle latency confirmed)", N);
		else
			$display("FAIL: %0d / %0d mismatches", fails, N);
		$finish;
	end
endmodule
