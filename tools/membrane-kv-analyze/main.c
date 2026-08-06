#define _DEFAULT_SOURCE

#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "membrane/codec.h"
#include "membrane/kvdump.h"
#include "membrane/kvmetrics.h"
#include "membrane/kvpredict.h"
#include "membrane/kvquant.h"

# define MAX_META 8
# define N_BLOCK_SIZES 4
# define MAX_LAYERS 64
# define SUMMARY_BLOCK 65536
# define NPRED MEMBRANE_PRED_COUNT
# define N_GROUP_SIZES 4
# define N_Q8_MODES 2
# define Q8_REF_GROUP 0	/* index into g_group_sizes used for the headline
						 * per-layer/per-prompt/RAW-vs-codec comparisons:
						 * 32 elements, symmetric mode, the closest analogue
						 * to common inference-engine int8 KV quantization. */
# define Q8_REF_MODE MEMBRANE_Q8_SYMMETRIC

static const size_t	g_block_sizes[N_BLOCK_SIZES] = {
	4096, 16384, 65536, 262144
};

static const size_t	g_group_sizes[N_GROUP_SIZES] = {32, 64, 128, 256};

typedef struct s_kva_opts
{
	const char	*jsonl_path;
	const char	*csv_path;
	const char	*pred_csv_path;
	const char	*highplane_path;
	const char	*quant_csv_path;
	const char	*meta[MAX_META];
	int			meta_count;
	char		**inputs;
	int			input_count;
}	kva_opts_t;

/* Aggregates for the human K-vs-V summary at 64 KiB blocks. Index 0 = K,
 * 1 = V for the per-tensor arrays. */
typedef struct s_kva_totals
{
	uint64_t	raw[2];
	uint64_t	rle[2];
	uint64_t	adaptive[2];
	uint64_t	byteplane[2];
	uint64_t	byteplane_adaptive[2];
	int			integrity_ok;
	int			byteplane_integrity_ok;
}	kva_totals_t;

/* Per-layer byteplane aggregate at 64 KiB blocks, across all prompts. */
typedef struct s_kva_layer
{
	uint64_t	raw;
	uint64_t	byteplane_adaptive;
	int			seen;
}	kva_layer_t;

/* Per-prompt (one input file) aggregate at 64 KiB blocks. */
typedef struct s_kva_file_sum
{
	uint64_t	raw;
	uint64_t	rle;
	uint64_t	adaptive;
	uint64_t	byteplane;
	uint64_t	byteplane_adaptive;
	uint64_t	huffman;
	uint64_t	q8_raw;			/* reference config: 32 elems, symmetric */
	uint64_t	q8_encoded;
	uint64_t	q8_elements;
	double		q8_sum_sq_err;
}	kva_file_sum_t;

/* Per-predictor residual aggregate over whole tensor payloads, split by
 * tensor (index 0 = K, 1 = V). ent_bytes / q_* are byte-weighted sums. */
typedef struct s_pred_acc
{
	uint64_t	raw[2];
	uint64_t	ideal[2];
	uint64_t	bytes[2];
	double		ent_bytes[2];
	double		q_first[2];
	double		q_last[2];
}	pred_acc_t;

/* Per-block-size high-plane Huffman aggregate (Phase 2.4). enc/dec_ns and
 * timed_bytes come from a dedicated timing pass over the same blocks. */
typedef struct s_hp_acc
{
	uint64_t	raw;
	uint64_t	huffman;			/* pure codec output */
	uint64_t	huffman_adaptive;	/* RAW-fallback aware */
	uint64_t	ideal;				/* order-0 entropy ceiling bytes */
	uint64_t	header;				/* metadata of codec-used blocks */
	uint64_t	codec_blocks;
	uint64_t	blocks;
	double		enc_ns;
	double		dec_ns;
	uint64_t	timed_bytes;
}	hp_acc_t;

/*
 * Q8 quantization aggregate (Phase 3.1), split by tensor (0=K, 1=V), for
 * one (mode, group_elems) combo across every record. sum_sq_err/
 * sum_abs_err are element-weighted sums of per-record MSE*n / MAE*n --
 * combining them this way reproduces the exact combined MSE/MAE (not an
 * approximation), since MSE_i = sum_sq_i / n_i by definition. cosine/rel_l2
 * are only approximately combinable this way (element-weighted mean of
 * per-record values), which is documented at the point of use.
 */
typedef struct s_q8_acc
{
	uint64_t	raw[2];
	uint64_t	encoded[2];
	uint64_t	elements[2];
	uint64_t	saturated[2];
	uint64_t	nan_input[2];
	uint64_t	inf_input[2];
	double		sum_sq_err[2];
	double		sum_abs_err[2];
	double		max_abs_err[2];
	double		cosine_sum[2];
	double		rel_l2_sum[2];
	double		enc_ns;
	double		dec_ns;
	uint64_t	timed_bytes;
	int			decode_fail;
}	q8_acc_t;

/* Per-layer aggregate at the reference config only (K+V combined). */
typedef struct s_q8_layer
{
	uint64_t	raw;
	uint64_t	encoded;
	uint64_t	elements;
	double		sum_sq_err;
	int			seen;
}	q8_layer_t;

typedef struct s_kva_out
{
	FILE		*jsonl;
	FILE		*csv;
	FILE		*pred_csv;
	FILE		*highplane;
	FILE		*quant_csv;
	kva_totals_t	totals;
	kva_layer_t	layers[MAX_LAYERS];
	pred_acc_t	pred[NPRED];
	uint64_t	layer_pred_ideal[MAX_LAYERS][NPRED];
	uint64_t	layer_raw[MAX_LAYERS];
	int			layer_pred_seen[MAX_LAYERS];
	hp_acc_t	hp[N_BLOCK_SIZES];
	int			hp_integrity_ok;
	q8_acc_t	q8[N_Q8_MODES][N_GROUP_SIZES];
	q8_layer_t	q8_layers[MAX_LAYERS];
}	kva_out_t;

/* Order-0 entropy ceiling (bytes) for the two F16 byte planes, from the
 * mean plane entropies the metrics already measured. */
static uint64_t	huffman_ideal_bytes(const membrane_kv_metrics_t *m)
{
	double	plane;

	plane = (double)m->raw_bytes / 2.0;
	return ((uint64_t)ceil(plane * m->low_entropy / 8.0)
		+ (uint64_t)ceil(plane * m->high_entropy / 8.0));
}

static double	now_ns(void)
{
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((double)ts.tv_sec * 1e9 + (double)ts.tv_nsec);
}

static void	print_meta_json(FILE *f, const kva_opts_t *o)
{
	int	i;

	i = 0;
	while (i < o->meta_count)
	{
		const char	*eq = strchr(o->meta[i], '=');

		if (eq != NULL)
			fprintf(f, ",\"%.*s\":\"%s\"",
				(int)(eq - o->meta[i]), o->meta[i], eq + 1);
		i++;
	}
}

static void	cpu_model(char *out, size_t cap)
{
	FILE	*f;
	char	line[256];
	char	*colon;

	snprintf(out, cap, "unknown");
	f = fopen("/proc/cpuinfo", "r");
	if (f == NULL)
		return ;
	while (fgets(line, sizeof(line), f) != NULL)
	{
		if (strncmp(line, "model name", 10) == 0
			&& (colon = strchr(line, ':')) != NULL)
		{
			line[strcspn(line, "\n")] = '\0';
			snprintf(out, cap, "%s", colon + 2);
			break ;
		}
	}
	fclose(f);
}

static void	print_env(const kva_opts_t *o, kva_out_t *out)
{
	struct utsname	u;
	char			cpu[128];
	long			ram_mb;

	uname(&u);
	cpu_model(cpu, sizeof(cpu));
	ram_mb = sysconf(_SC_PHYS_PAGES) / 1024 * sysconf(_SC_PAGE_SIZE) / 1024;
	fprintf(out->jsonl,
		"{\"record\":\"env\",\"os\":\"%s %s\",\"cpu\":\"%s\","
		"\"ram_mb\":%ld,\"compiler\":\"%s\"", u.sysname, u.release, cpu,
		ram_mb, __VERSION__);
	print_meta_json(out->jsonl, o);
	fprintf(out->jsonl, "}\n");
	fprintf(stderr, "env: %s %s | %s | %ld MB RAM | gcc %s\n",
		u.sysname, u.release, cpu, ram_mb, __VERSION__);
}

static double	ratio_of(uint64_t raw, uint64_t stored)
{
	if (stored == 0)
		return (0.0);
	return ((double)raw / (double)stored);
}

static void	emit_jsonl_metrics(FILE *f, const membrane_kv_metrics_t *m)
{
	fprintf(f,
		"\"blocks\":%llu,\"raw_bytes\":%llu,"
		"\"rle_bytes\":%llu,\"rle_ratio\":%.4f,"
		"\"adaptive_bytes\":%llu,\"adaptive_ratio\":%.4f,"
		"\"adaptive_raw_blocks\":%llu,\"adaptive_rle_blocks\":%llu,"
		"\"zero_ratio\":%.6f,\"entropy_bits\":%.4f,"
		"\"total_runs\":%llu,\"max_run\":%llu,\"mean_run\":%.3f,"
		"\"integrity\":\"%s\"",
		(unsigned long long)m->blocks,
		(unsigned long long)m->raw_bytes,
		(unsigned long long)m->rle_bytes, ratio_of(m->raw_bytes, m->rle_bytes),
		(unsigned long long)m->adaptive_bytes,
		ratio_of(m->raw_bytes, m->adaptive_bytes),
		(unsigned long long)m->adaptive_raw_blocks,
		(unsigned long long)m->adaptive_rle_blocks,
		m->raw_bytes ? (double)m->zero_bytes / (double)m->raw_bytes : 0.0,
		m->entropy,
		(unsigned long long)m->total_runs, (unsigned long long)m->max_run,
		m->total_runs ? (double)m->raw_bytes / (double)m->total_runs : 0.0,
		m->integrity_ok ? "PASS" : "FAIL");
}

static void	emit_jsonl_byteplane(FILE *f, const membrane_kv_metrics_t *m)
{
	fprintf(f,
		",\"byteplane_bytes\":%llu,\"byteplane_ratio\":%.4f,"
		"\"byteplane_adaptive_bytes\":%llu,\"byteplane_adaptive_ratio\":%.4f,"
		"\"byteplane_raw_blocks\":%llu,\"byteplane_codec_blocks\":%llu,"
		"\"low_entropy_bits\":%.4f,\"high_entropy_bits\":%.4f,"
		"\"low_plane_ratio\":%.4f,\"high_plane_ratio\":%.4f,"
		"\"byteplane_applicable\":%d,\"byteplane_integrity\":\"%s\"",
		(unsigned long long)m->byteplane_bytes,
		ratio_of(m->raw_bytes, m->byteplane_bytes),
		(unsigned long long)m->byteplane_adaptive_bytes,
		ratio_of(m->raw_bytes, m->byteplane_adaptive_bytes),
		(unsigned long long)m->byteplane_raw_blocks,
		(unsigned long long)m->byteplane_codec_blocks,
		m->low_entropy, m->high_entropy,
		ratio_of(m->low_plane_bytes, m->low_plane_rle_bytes),
		ratio_of(m->high_plane_bytes, m->high_plane_rle_bytes),
		m->byteplane_applicable,
		m->byteplane_integrity_ok ? "PASS" : "FAIL");
}

static void	emit_jsonl_huffman(FILE *f, const membrane_kv_metrics_t *m)
{
	uint64_t	ideal;

	ideal = huffman_ideal_bytes(m);
	fprintf(f,
		",\"huffman_bytes\":%llu,\"huffman_ratio\":%.4f,"
		"\"huffman_adaptive_bytes\":%llu,\"huffman_adaptive_ratio\":%.4f,"
		"\"theoretical_ratio\":%.4f,\"efficiency\":%.4f,"
		"\"huffman_header_bytes\":%llu,\"huffman_codec_blocks\":%llu,"
		"\"huffman_integrity\":\"%s\"",
		(unsigned long long)m->huffman_bytes,
		ratio_of(m->raw_bytes, m->huffman_bytes),
		(unsigned long long)m->huffman_adaptive_bytes,
		ratio_of(m->raw_bytes, m->huffman_adaptive_bytes),
		ratio_of(m->raw_bytes, ideal),
		m->huffman_bytes ? (double)ideal / (double)m->huffman_bytes : 0.0,
		(unsigned long long)m->huffman_header_bytes,
		(unsigned long long)m->huffman_codec_blocks,
		m->huffman_integrity_ok ? "PASS" : "FAIL");
}

static void	emit_jsonl(kva_out_t *out, const kva_opts_t *o, const char *file,
				const membrane_kv_header_t *h, size_t bs,
				const membrane_kv_metrics_t *m)
{
	fprintf(out->jsonl,
		"{\"record\":\"metrics\",\"file\":\"%s\",\"model\":\"%s\","
		"\"layer\":%u,\"tensor\":\"%c\",\"token_start\":%u,\"token_end\":%u,"
		"\"dtype\":%u,\"block_size\":%zu,",
		file, h->model, h->layer, h->tensor_type ? 'V' : 'K',
		h->token_start, h->token_end, h->dtype, bs);
	emit_jsonl_metrics(out->jsonl, m);
	emit_jsonl_byteplane(out->jsonl, m);
	emit_jsonl_huffman(out->jsonl, m);
	print_meta_json(out->jsonl, o);
	fprintf(out->jsonl, "}\n");
}

static void	emit_csv(kva_out_t *out, const char *file,
				const membrane_kv_header_t *h, size_t bs,
				const membrane_kv_metrics_t *m)
{
	fprintf(out->csv,
		"%s,%s,%u,%c,%u,%u,%u,%zu,%llu,%llu,%llu,%.4f,%llu,%.4f,"
		"%.6f,%.4f,%llu,%s,"
		"%llu,%.4f,%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%s",
		file, h->model, h->layer, h->tensor_type ? 'V' : 'K',
		h->token_start, h->token_end, h->dtype, bs,
		(unsigned long long)m->blocks,
		(unsigned long long)m->raw_bytes,
		(unsigned long long)m->rle_bytes, ratio_of(m->raw_bytes, m->rle_bytes),
		(unsigned long long)m->adaptive_bytes,
		ratio_of(m->raw_bytes, m->adaptive_bytes),
		m->raw_bytes ? (double)m->zero_bytes / (double)m->raw_bytes : 0.0,
		m->entropy, (unsigned long long)m->max_run,
		m->integrity_ok ? "PASS" : "FAIL",
		(unsigned long long)m->byteplane_bytes,
		ratio_of(m->raw_bytes, m->byteplane_bytes),
		(unsigned long long)m->byteplane_adaptive_bytes,
		ratio_of(m->raw_bytes, m->byteplane_adaptive_bytes),
		m->low_entropy, m->high_entropy,
		ratio_of(m->low_plane_bytes, m->low_plane_rle_bytes),
		ratio_of(m->high_plane_bytes, m->high_plane_rle_bytes),
		m->byteplane_applicable,
		m->byteplane_integrity_ok ? "PASS" : "FAIL");
	fprintf(out->csv, ",%llu,%.4f,%llu,%.4f,%.4f,%.4f,%llu,%s",
		(unsigned long long)m->huffman_bytes,
		ratio_of(m->raw_bytes, m->huffman_bytes),
		(unsigned long long)m->huffman_adaptive_bytes,
		ratio_of(m->raw_bytes, m->huffman_adaptive_bytes),
		ratio_of(m->raw_bytes, huffman_ideal_bytes(m)),
		m->huffman_bytes
		? (double)huffman_ideal_bytes(m) / (double)m->huffman_bytes : 0.0,
		(unsigned long long)m->huffman_header_bytes,
		m->huffman_integrity_ok ? "PASS" : "FAIL");
	fprintf(out->csv, "\n");
}

static void	human_record(const membrane_kv_header_t *h,
				const membrane_kv_metrics_t *m)
{
	fprintf(stderr,
		"  layer %2u %c  tokens %u..%u  raw %7llu B  adaptive %.3fx  "
		"byteplane %.3fx (adaptive %.3fx)  Hlo %.3f Hhi %.3f  %s\n",
		h->layer, h->tensor_type ? 'V' : 'K', h->token_start, h->token_end,
		(unsigned long long)m->raw_bytes,
		ratio_of(m->raw_bytes, m->adaptive_bytes),
		ratio_of(m->raw_bytes, m->byteplane_bytes),
		ratio_of(m->raw_bytes, m->byteplane_adaptive_bytes),
		m->low_entropy, m->high_entropy,
		(m->integrity_ok && m->byteplane_integrity_ok) ? "PASS" : "FAIL");
}

static void	totals_add(kva_out_t *out, const membrane_kv_header_t *h,
				const membrane_kv_metrics_t *m)
{
	kva_totals_t	*t;
	int				idx;

	t = &out->totals;
	idx = (h->tensor_type != 0);
	t->raw[idx] += m->raw_bytes;
	t->rle[idx] += m->rle_bytes;
	t->adaptive[idx] += m->adaptive_bytes;
	t->byteplane[idx] += m->byteplane_bytes;
	t->byteplane_adaptive[idx] += m->byteplane_adaptive_bytes;
	if (!m->integrity_ok)
		t->integrity_ok = 0;
	if (!m->byteplane_integrity_ok || !m->byteplane_applicable)
		t->byteplane_integrity_ok = 0;
	if (h->layer < MAX_LAYERS)
	{
		out->layers[h->layer].raw += m->raw_bytes;
		out->layers[h->layer].byteplane_adaptive += m->byteplane_adaptive_bytes;
		out->layers[h->layer].seen = 1;
	}
}

static void	file_sum_add(kva_file_sum_t *fs, const membrane_kv_metrics_t *m)
{
	fs->raw += m->raw_bytes;
	fs->rle += m->rle_bytes;
	fs->adaptive += m->adaptive_bytes;
	fs->byteplane += m->byteplane_bytes;
	fs->byteplane_adaptive += m->byteplane_adaptive_bytes;
}

static void	emit_residual(kva_out_t *out, const kva_opts_t *o,
				const char *file, const membrane_kv_header_t *h,
				const membrane_residual_metrics_t *m, membrane_predictor_t p)
{
	fprintf(out->jsonl,
		"{\"record\":\"residual\",\"file\":\"%s\",\"model\":\"%s\","
		"\"layer\":%u,\"tensor\":\"%c\",\"predictor\":\"%s\","
		"\"stride_elems\":%llu,\"applicable\":%d,\"raw_bytes\":%llu,"
		"\"entropy_bits\":%.4f,\"low_entropy_bits\":%.4f,"
		"\"high_entropy_bits\":%.4f,\"zero_u16_ratio\":%.6f,"
		"\"zero_byte_ratio\":%.6f,\"longest_zero_run\":%llu,"
		"\"ideal_bytes\":%llu,\"theoretical_ratio\":%.4f,\"q0\":%.4f,"
		"\"q1\":%.4f,\"q2\":%.4f,\"q3\":%.4f",
		file, h->model, h->layer, h->tensor_type ? 'V' : 'K',
		membrane_predictor_name(p), (unsigned long long)m->stride_elems,
		m->applicable, (unsigned long long)m->raw_bytes, m->entropy,
		m->low_entropy, m->high_entropy,
		m->total_u16 ? (double)m->zero_u16 / (double)m->total_u16 : 0.0,
		m->raw_bytes ? (double)m->zero_bytes / (double)m->raw_bytes : 0.0,
		(unsigned long long)m->longest_zero_run,
		(unsigned long long)m->ideal_bytes,
		ratio_of(m->raw_bytes, m->ideal_bytes), m->quartile_entropy[0],
		m->quartile_entropy[1], m->quartile_entropy[2],
		m->quartile_entropy[3]);
	print_meta_json(out->jsonl, o);
	fprintf(out->jsonl, "}\n");
	if (out->pred_csv == NULL)
		return ;
	fprintf(out->pred_csv,
		"%s,%s,%u,%c,%s,%llu,%d,%llu,%.4f,%.4f,%.4f,%.6f,%.6f,%llu,%llu,"
		"%.4f,%.4f,%.4f,%.4f,%.4f\n",
		file, h->model, h->layer, h->tensor_type ? 'V' : 'K',
		membrane_predictor_name(p), (unsigned long long)m->stride_elems,
		m->applicable, (unsigned long long)m->raw_bytes, m->entropy,
		m->low_entropy, m->high_entropy,
		m->total_u16 ? (double)m->zero_u16 / (double)m->total_u16 : 0.0,
		m->raw_bytes ? (double)m->zero_bytes / (double)m->raw_bytes : 0.0,
		(unsigned long long)m->longest_zero_run,
		(unsigned long long)m->ideal_bytes,
		ratio_of(m->raw_bytes, m->ideal_bytes), m->quartile_entropy[0],
		m->quartile_entropy[1], m->quartile_entropy[2],
		m->quartile_entropy[3]);
}

static void	pred_accumulate(kva_out_t *out, const membrane_kv_header_t *h,
				int p, const membrane_residual_metrics_t *m)
{
	pred_acc_t	*a;
	int			idx;

	a = &out->pred[p];
	idx = (h->tensor_type != 0);
	a->raw[idx] += m->raw_bytes;
	a->ideal[idx] += m->ideal_bytes;
	a->bytes[idx] += m->raw_bytes;
	a->ent_bytes[idx] += m->entropy * (double)m->raw_bytes;
	a->q_first[idx] += m->quartile_entropy[0] * (double)m->raw_bytes;
	a->q_last[idx] += m->quartile_entropy[MEMBRANE_PRED_QUARTILES - 1]
		* (double)m->raw_bytes;
	if (h->layer < MAX_LAYERS)
	{
		out->layer_pred_ideal[h->layer][p] += m->ideal_bytes;
		if (p == 0)
			out->layer_raw[h->layer] += m->raw_bytes;
		out->layer_pred_seen[h->layer] = 1;
	}
}

/* Whole-payload predictive pass: every predictor mode on one tensor. */
static membrane_status_t	analyze_predictors(kva_out_t *out,
								const kva_opts_t *o, const char *file,
								const membrane_kv_header_t *h,
								const uint8_t *payload)
{
	membrane_residual_metrics_t	m;
	membrane_residual_cfg_t		cfg;
	membrane_status_t			st;
	int							p;

	cfg.row_elems = 0;
	if (h->n_dims >= 2 && h->dims[0] >= 2)
		cfg.row_elems = (size_t)(h->dims[0] / 2);
	cfg.n_rows = (h->n_dims >= 2) ? (size_t)h->dims[1] : 0;
	p = 0;
	while (p < NPRED)
	{
		cfg.predictor = (membrane_predictor_t)p;
		st = membrane_kv_residual_metrics(payload, h->payload_size, &cfg, &m);
		if (st != MEMBRANE_OK)
			return (st);
		emit_residual(out, o, file, h, &m, cfg.predictor);
		pred_accumulate(out, h, p, &m);
		p++;
	}
	return (MEMBRANE_OK);
}

static const char	*q8_mode_name(membrane_q8_mode_t mode)
{
	if (mode == MEMBRANE_Q8_AFFINE)
		return ("affine");
	return ("symmetric");
}

static void	emit_quant(kva_out_t *out, const kva_opts_t *o, const char *file,
				const membrane_kv_header_t *h, membrane_q8_mode_t mode,
				size_t group_elems, const membrane_kv_quant_metrics_t *m)
{
	fprintf(out->jsonl,
		"{\"record\":\"quant\",\"file\":\"%s\",\"model\":\"%s\","
		"\"layer\":%u,\"tensor\":\"%c\",\"mode\":\"%s\","
		"\"group_elems\":%zu,\"raw_bytes\":%llu,\"encoded_bytes\":%llu,"
		"\"ratio\":%.4f,\"elements\":%llu,\"saturation_ratio\":%.6f,"
		"\"nan_input\":%llu,\"inf_input\":%llu,\"mse\":%.8f,\"rmse\":%.6f,"
		"\"mae\":%.6f,\"max_abs_err\":%.6f,\"cosine_similarity\":%.6f,"
		"\"rel_l2_error\":%.6f,\"metadata_bytes\":%llu,\"decode_ok\":%d",
		file, h->model, h->layer, h->tensor_type ? 'V' : 'K',
		q8_mode_name(mode), group_elems,
		(unsigned long long)m->raw_bytes, (unsigned long long)m->encoded_bytes,
		ratio_of(m->raw_bytes, m->encoded_bytes),
		(unsigned long long)m->elements,
		m->elements ? (double)m->saturated / (double)m->elements : 0.0,
		(unsigned long long)m->nan_input, (unsigned long long)m->inf_input,
		m->mse, m->rmse, m->mae, m->max_abs_err, m->cosine_similarity,
		m->rel_l2_error, (unsigned long long)m->metadata_bytes, m->decode_ok);
	print_meta_json(out->jsonl, o);
	fprintf(out->jsonl, "}\n");
	if (out->quant_csv == NULL)
		return ;
	fprintf(out->quant_csv,
		"%s,%s,%u,%c,%s,%zu,%llu,%llu,%.4f,%llu,%.6f,%llu,%llu,%.8f,%.6f,"
		"%.6f,%.6f,%.6f,%.6f,%llu,%d\n",
		file, h->model, h->layer, h->tensor_type ? 'V' : 'K',
		q8_mode_name(mode), group_elems,
		(unsigned long long)m->raw_bytes, (unsigned long long)m->encoded_bytes,
		ratio_of(m->raw_bytes, m->encoded_bytes),
		(unsigned long long)m->elements,
		m->elements ? (double)m->saturated / (double)m->elements : 0.0,
		(unsigned long long)m->nan_input, (unsigned long long)m->inf_input,
		m->mse, m->rmse, m->mae, m->max_abs_err, m->cosine_similarity,
		m->rel_l2_error, (unsigned long long)m->metadata_bytes, m->decode_ok);
}

static void	q8_accumulate(kva_out_t *out, const membrane_kv_header_t *h,
				int mode_idx, int group_idx, const membrane_kv_quant_metrics_t *m)
{
	q8_acc_t	*a;
	int			idx;

	a = &out->q8[mode_idx][group_idx];
	idx = (h->tensor_type != 0);
	a->raw[idx] += m->raw_bytes;
	a->encoded[idx] += m->encoded_bytes;
	a->elements[idx] += m->elements;
	a->saturated[idx] += m->saturated;
	a->nan_input[idx] += m->nan_input;
	a->inf_input[idx] += m->inf_input;
	a->sum_sq_err[idx] += m->mse * (double)m->elements;
	a->sum_abs_err[idx] += m->mae * (double)m->elements;
	if (m->max_abs_err > a->max_abs_err[idx])
		a->max_abs_err[idx] = m->max_abs_err;
	a->cosine_sum[idx] += m->cosine_similarity * (double)m->elements;
	a->rel_l2_sum[idx] += m->rel_l2_error * (double)m->elements;
	if (!m->decode_ok)
		a->decode_fail = 1;
}

/* Times membrane_q8_encode/decode over the whole payload once (the sweep
 * loop already ran membrane_kv_quant_compute for correctness metrics; this
 * is a dedicated timing-only pass, mirroring time_highplane in Phase 2.4). */
static membrane_status_t	time_q8(const uint8_t *payload, size_t len,
								const membrane_q8_cfg_t *cfg, q8_acc_t *a)
{
	uint8_t				*enc;
	uint8_t				*dec;
	size_t				bound;
	size_t				el;
	size_t				dl;
	double				t0;
	double				t1;
	double				t2;
	membrane_status_t	st;

	if (len == 0)
		return (MEMBRANE_OK);
	bound = membrane_q8_bound(len, cfg);
	if (bound == SIZE_MAX)
		return (MEMBRANE_ERR_INVALID_ARG);
	enc = malloc(bound);
	dec = malloc(len);
	if (enc == NULL || dec == NULL)
		return (free(enc), free(dec), MEMBRANE_ERR_ALLOC_FAILED);
	t0 = now_ns();
	st = membrane_q8_encode(cfg, payload, len, enc, bound, &el, NULL);
	t1 = now_ns();
	if (st == MEMBRANE_OK)
		st = membrane_q8_decode(enc, el, dec, len, &dl);
	t2 = now_ns();
	if (st == MEMBRANE_OK)
	{
		a->enc_ns += t1 - t0;
		a->dec_ns += t2 - t1;
		a->timed_bytes += len;
	}
	return (free(enc), free(dec), st);
}

static void	q8_layer_accumulate(kva_out_t *out, const membrane_kv_header_t *h,
				const membrane_kv_quant_metrics_t *m)
{
	q8_layer_t	*l;

	if (h->layer >= MAX_LAYERS)
		return ;
	l = &out->q8_layers[h->layer];
	l->raw += m->raw_bytes;
	l->encoded += m->encoded_bytes;
	l->elements += m->elements;
	l->sum_sq_err += m->mse * (double)m->elements;
	l->seen = 1;
}

static void	q8_file_sum_add(kva_file_sum_t *fs,
				const membrane_kv_metrics_t *hm,
				const membrane_kv_quant_metrics_t *qm)
{
	fs->huffman += hm->huffman_bytes;
	fs->q8_raw += qm->raw_bytes;
	fs->q8_encoded += qm->encoded_bytes;
	fs->q8_elements += qm->elements;
	fs->q8_sum_sq_err += qm->mse * (double)qm->elements;
}

/* Whole-payload Q8 sweep: every (mode, group_elems) combo on one tensor.
 * `ref_hm` is the 64 KiB-block metrics already computed for this record
 * (carrying huffman_bytes), reused here only to feed the per-prompt
 * RAW/Huffman/Q8 comparison -- it is not recomputed. */
static membrane_status_t	analyze_quant(kva_out_t *out, const kva_opts_t *o,
								const char *file, const membrane_kv_header_t *h,
								const uint8_t *payload,
								const membrane_kv_metrics_t *ref_hm,
								kva_file_sum_t *fs)
{
	membrane_kv_quant_metrics_t	m;
	membrane_q8_cfg_t				cfg;
	membrane_status_t				st;
	int								mi;
	int								gi;

	mi = 0;
	while (mi < N_Q8_MODES)
	{
		cfg.mode = (membrane_q8_mode_t)mi;
		gi = 0;
		while (gi < N_GROUP_SIZES)
		{
			cfg.group_elems = g_group_sizes[gi];
			st = membrane_kv_quant_compute(payload, h->payload_size, &cfg, &m);
			if (st != MEMBRANE_OK)
				return (st);
			emit_quant(out, o, file, h, cfg.mode, cfg.group_elems, &m);
			q8_accumulate(out, h, mi, gi, &m);
			st = time_q8(payload, h->payload_size, &cfg,
					&out->q8[mi][gi]);
			if (st != MEMBRANE_OK)
				return (st);
			if (mi == Q8_REF_MODE && gi == Q8_REF_GROUP)
			{
				q8_layer_accumulate(out, h, &m);
				q8_file_sum_add(fs, ref_hm, &m);
			}
			gi++;
		}
		mi++;
	}
	return (MEMBRANE_OK);
}

static void	hp_accumulate(hp_acc_t *a, const membrane_kv_metrics_t *m)
{
	a->raw += m->raw_bytes;
	a->huffman += m->huffman_bytes;
	a->huffman_adaptive += m->huffman_adaptive_bytes;
	a->ideal += huffman_ideal_bytes(m);
	a->header += m->huffman_header_bytes;
	a->codec_blocks += m->huffman_codec_blocks;
	a->blocks += m->blocks;
}

/* Times the high-plane Huffman codec (encode then decode) over one
 * payload split into `bs`-byte blocks, accumulating wall-clock nanoseconds
 * and the raw bytes processed. Verifies bit-exact decode as it goes. */
static membrane_status_t	time_highplane(const uint8_t *payload, size_t len,
								size_t bs, hp_acc_t *a, int *integrity)
{
	const membrane_codec_vtable_t	*hp;
	uint8_t							*enc;
	uint8_t							*dec;
	size_t							off;
	size_t							n;
	size_t							el;
	size_t							dl;
	double							t0;
	double							t1;
	double							t2;
	membrane_status_t				st;

	hp = membrane_codec_get(MEMBRANE_CODEC_F16_HIGHPLANE_HUFFMAN);
	enc = malloc(hp->bound(bs) + 1);
	dec = malloc(bs + 1);
	if (enc == NULL || dec == NULL)
		return (free(enc), free(dec), MEMBRANE_ERR_ALLOC_FAILED);
	off = 0;
	st = MEMBRANE_OK;
	while (off < len && st == MEMBRANE_OK)
	{
		n = len - off;
		if (n > bs)
			n = bs;
		if (n % 2 == 0)
		{
			t0 = now_ns();
			st = hp->compress(payload + off, n, enc, hp->bound(bs), &el);
			t1 = now_ns();
			if (st == MEMBRANE_OK)
				st = hp->decompress(enc, el, dec, bs, &dl);
			t2 = now_ns();
			if (st == MEMBRANE_OK && (dl != n
					|| memcmp(dec, payload + off, n) != 0))
				*integrity = 0;
			a->enc_ns += t1 - t0;
			a->dec_ns += t2 - t1;
			a->timed_bytes += n;
		}
		off += n;
	}
	return (free(enc), free(dec), st);
}

/* Writes the record's high byte plane (odd bytes) to the offline dump so
 * an external coder (zstd/gzip/xz) can be compared against Huffman. */
static void	dump_highplane(FILE *f, const uint8_t *payload, size_t len)
{
	uint8_t	*high;
	size_t	i;

	high = malloc(len / 2 + 1);
	if (high == NULL)
		return ;
	i = 0;
	while (2 * i + 1 < len)
	{
		high[i] = payload[2 * i + 1];
		i++;
	}
	fwrite(high, 1, i, f);
	free(high);
}

static membrane_status_t	analyze_record(kva_out_t *out,
								const kva_opts_t *o, const char *file,
								const membrane_kv_header_t *h,
								const uint8_t *payload, kva_file_sum_t *fs)
{
	membrane_kv_metrics_t	m;
	membrane_kv_metrics_t	ref_hm;
	membrane_status_t		st;
	int						i;

	memset(&ref_hm, 0, sizeof(ref_hm));
	i = 0;
	while (i < N_BLOCK_SIZES)
	{
		st = membrane_kv_metrics_compute(payload, h->payload_size,
				g_block_sizes[i], &m);
		if (st != MEMBRANE_OK)
			return (st);
		emit_jsonl(out, o, file, h, g_block_sizes[i], &m);
		emit_csv(out, file, h, g_block_sizes[i], &m);
		hp_accumulate(&out->hp[i], &m);
		if (!m.huffman_integrity_ok)
			out->hp_integrity_ok = 0;
		st = time_highplane(payload, h->payload_size, g_block_sizes[i],
				&out->hp[i], &out->hp_integrity_ok);
		if (st != MEMBRANE_OK)
			return (st);
		if (g_block_sizes[i] == SUMMARY_BLOCK)
		{
			human_record(h, &m);
			totals_add(out, h, &m);
			file_sum_add(fs, &m);
			ref_hm = m;
		}
		i++;
	}
	if (out->highplane != NULL)
		dump_highplane(out->highplane, payload, h->payload_size);
	st = analyze_predictors(out, o, file, h, payload);
	if (st != MEMBRANE_OK)
		return (st);
	return (analyze_quant(out, o, file, h, payload, &ref_hm, fs));
}

static void	human_file_sum(const char *path, const kva_file_sum_t *fs)
{
	double	q8_rmse;

	q8_rmse = fs->q8_elements ? sqrt(fs->q8_sum_sq_err
			/ (double)fs->q8_elements) : 0.0;
	fprintf(stderr,
		"  prompt total: raw %llu B  adaptive %.3fx  byteplane %.3fx"
		" (adaptive %.3fx)\n",
		(unsigned long long)fs->raw, ratio_of(fs->raw, fs->adaptive),
		ratio_of(fs->raw, fs->byteplane),
		ratio_of(fs->raw, fs->byteplane_adaptive));
	fprintf(stderr,
		"  prompt RAW vs Huffman vs Q8(sym,32): huffman %.3fx"
		"  q8 %.3fx  q8 RMSE %.5f\n",
		ratio_of(fs->raw, fs->huffman), ratio_of(fs->q8_raw, fs->q8_encoded),
		q8_rmse);
	(void)path;
}

static membrane_status_t	analyze_file(kva_out_t *out, const kva_opts_t *o,
								const char *path)
{
	FILE					*f;
	membrane_kv_header_t	h;
	uint8_t					*payload;
	kva_file_sum_t			fs;
	membrane_status_t		st;

	f = fopen(path, "rb");
	if (f == NULL)
		return (fprintf(stderr, "cannot open %s\n", path), MEMBRANE_ERR_IO);
	fprintf(stderr, "%s:\n", path);
	memset(&fs, 0, sizeof(fs));
	st = membrane_kvdump_read_header(f, &h);
	while (st == MEMBRANE_OK)
	{
		st = membrane_kvdump_read_payload(f, &h, &payload);
		if (st != MEMBRANE_OK)
			break ;
		st = analyze_record(out, o, path, &h, payload, &fs);
		free(payload);
		if (st != MEMBRANE_OK)
			break ;
		st = membrane_kvdump_read_header(f, &h);
	}
	fclose(f);
	if (st == MEMBRANE_ERR_NOT_FOUND)
		return (human_file_sum(path, &fs), MEMBRANE_OK);
	return (fprintf(stderr, "error in %s (status %d)\n", path, st), st);
}

static void	human_kv_line(const kva_totals_t *t, int idx, char label)
{
	fprintf(stderr,
		"  %c: raw %llu B  rle %.3fx  adaptive %.3fx  byteplane %.3fx"
		" (adaptive %.3fx)\n",
		label, (unsigned long long)t->raw[idx],
		ratio_of(t->raw[idx], t->rle[idx]),
		ratio_of(t->raw[idx], t->adaptive[idx]),
		ratio_of(t->raw[idx], t->byteplane[idx]),
		ratio_of(t->raw[idx], t->byteplane_adaptive[idx]));
}

static void	human_layers(const kva_out_t *out)
{
	int	i;

	fprintf(stderr, "\nPer-layer byteplane adaptive (64 KiB blocks,"
		" K+V, all prompts):\n");
	i = 0;
	while (i < MAX_LAYERS)
	{
		if (out->layers[i].seen)
			fprintf(stderr, "  layer %2d: raw %llu B  byteplane %.3fx\n",
				i, (unsigned long long)out->layers[i].raw,
				ratio_of(out->layers[i].raw,
					out->layers[i].byteplane_adaptive));
		i++;
	}
}

static void	human_totals(const kva_out_t *out)
{
	const kva_totals_t	*t;

	t = &out->totals;
	fprintf(stderr, "\nK vs V (64 KiB blocks, all inputs):\n");
	human_kv_line(t, 0, 'K');
	human_kv_line(t, 1, 'V');
	human_layers(out);
	fprintf(stderr, "\n  integrity: adaptive %s  byteplane %s\n",
		t->integrity_ok ? "PASS" : "FAIL",
		t->byteplane_integrity_ok ? "PASS" : "FAIL");
}

/* Prints residual entropy and the entropy-ceiling ratio per predictor,
 * split K vs V, over whole tensor payloads. */
static void	human_pred_table(const kva_out_t *out)
{
	const pred_acc_t	*a;
	int					p;

	fprintf(stderr, "\nPredictive residual (whole tensors, all prompts):\n");
	fprintf(stderr, "  %-17s  K: H=bits ratio       V: H=bits ratio\n",
		"predictor");
	p = 0;
	while (p < NPRED)
	{
		a = &out->pred[p];
		fprintf(stderr,
			"  %-17s  K: %.3f  %.3fx      V: %.3f  %.3fx\n",
			membrane_predictor_name((membrane_predictor_t)p),
			a->bytes[0] ? a->ent_bytes[0] / (double)a->bytes[0] : 0.0,
			ratio_of(a->raw[0], a->ideal[0]),
			a->bytes[1] ? a->ent_bytes[1] / (double)a->bytes[1] : 0.0,
			ratio_of(a->raw[1], a->ideal[1]));
		p++;
	}
}

/* Best (lowest ideal size) predictor for one layer's K+V payloads. */
static int	layer_best_pred(const kva_out_t *out, int layer)
{
	int			best;
	int			p;
	uint64_t	lo;

	best = 0;
	lo = out->layer_pred_ideal[layer][0];
	p = 1;
	while (p < NPRED)
	{
		if (out->layer_pred_ideal[layer][p] < lo)
		{
			lo = out->layer_pred_ideal[layer][p];
			best = p;
		}
		p++;
	}
	return (best);
}

static void	human_pred_layers(const kva_out_t *out)
{
	int	i;
	int	best;

	fprintf(stderr, "\nBest predictor per layer (K+V, lowest entropy ceiling):\n");
	i = 0;
	while (i < MAX_LAYERS)
	{
		if (out->layer_pred_seen[i])
		{
			best = layer_best_pred(out, i);
			fprintf(stderr, "  layer %2d: %-17s  ceiling %.3fx\n", i,
				membrane_predictor_name((membrane_predictor_t)best),
				ratio_of(out->layer_raw[i], out->layer_pred_ideal[i][best]));
		}
		i++;
	}
}

/* Token-axis check: first vs last token quartile entropy for the token
 * predictor, per tensor. */
static void	human_pred_tokenaxis(const kva_out_t *out)
{
	const pred_acc_t	*a;

	a = &out->pred[MEMBRANE_PRED_XOR_PREV_TOKEN];
	fprintf(stderr, "\nToken-axis (xor_prev_token residual entropy, "
		"first vs last quartile):\n");
	fprintf(stderr, "  K: q0 %.3f -> q3 %.3f    V: q0 %.3f -> q3 %.3f\n",
		a->bytes[0] ? a->q_first[0] / (double)a->bytes[0] : 0.0,
		a->bytes[0] ? a->q_last[0] / (double)a->bytes[0] : 0.0,
		a->bytes[1] ? a->q_first[1] / (double)a->bytes[1] : 0.0,
		a->bytes[1] ? a->q_last[1] / (double)a->bytes[1] : 0.0);
}

static void	human_pred(const kva_out_t *out)
{
	human_pred_table(out);
	human_pred_layers(out);
	human_pred_tokenaxis(out);
}

/* The Phase 2.4 headline: actual high-plane Huffman ratio, entropy
 * ceiling, achieved efficiency, throughput, and metadata cost, per block
 * size, so the small-block overhead is explicit. */
static void	human_huffman(const kva_out_t *out)
{
	const hp_acc_t	*a;
	int				i;

	fprintf(stderr, "\nHigh-plane Huffman by block size (K+V, all prompts):\n");
	fprintf(stderr, "  block     actual  adaptive  ceiling  effic"
		"  enc GB/s  dec GB/s   meta%%\n");
	i = 0;
	while (i < N_BLOCK_SIZES)
	{
		a = &out->hp[i];
		fprintf(stderr,
			"  %6zu   %.3fx   %.3fx    %.3fx  %.3f   %7.3f   %7.3f  %6.2f\n",
			g_block_sizes[i], ratio_of(a->raw, a->huffman),
			ratio_of(a->raw, a->huffman_adaptive), ratio_of(a->raw, a->ideal),
			a->huffman ? (double)a->ideal / (double)a->huffman : 0.0,
			a->enc_ns > 0 ? (double)a->timed_bytes / a->enc_ns : 0.0,
			a->dec_ns > 0 ? (double)a->timed_bytes / a->dec_ns : 0.0,
			a->huffman ? 100.0 * (double)a->header / (double)a->huffman : 0.0);
		i++;
	}
	fprintf(stderr, "  integrity: %s\n", out->hp_integrity_ok ? "PASS" : "FAIL");
}

static double	q8_ratio(const q8_acc_t *a)
{
	return (ratio_of(a->raw[0] + a->raw[1], a->encoded[0] + a->encoded[1]));
}

static double	q8_rmse(const q8_acc_t *a)
{
	uint64_t	n;

	n = a->elements[0] + a->elements[1];
	if (n == 0)
		return (0.0);
	return (sqrt((a->sum_sq_err[0] + a->sum_sq_err[1]) / (double)n));
}

/* Element-weighted mean of per-record cosine similarity -- an approximate
 * combination (cosine does not combine linearly across records the way
 * MSE does), but a standard and adequate way to report an aggregate. */
static double	q8_cosine(const q8_acc_t *a)
{
	uint64_t	n;

	n = a->elements[0] + a->elements[1];
	if (n == 0)
		return (1.0);
	return ((a->cosine_sum[0] + a->cosine_sum[1]) / (double)n);
}

static double	q8_rel_l2(const q8_acc_t *a)
{
	uint64_t	n;

	n = a->elements[0] + a->elements[1];
	if (n == 0)
		return (0.0);
	return ((a->rel_l2_sum[0] + a->rel_l2_sum[1]) / (double)n);
}

static double	q8_saturation(const q8_acc_t *a)
{
	uint64_t	n;

	n = a->elements[0] + a->elements[1];
	if (n == 0)
		return (0.0);
	return ((double)(a->saturated[0] + a->saturated[1]) / (double)n);
}

/* The full sweep: every (mode, group_elems) combo, K+V combined, so the
 * effect of group granularity and quantization mode is directly visible. */
static void	human_q8_sweep(const kva_out_t *out)
{
	const q8_acc_t	*a;
	int				mi;
	int				gi;

	fprintf(stderr, "\nQ8 quantization sweep (K+V combined, all prompts):\n");
	fprintf(stderr, "  mode        group  ratio    RMSE      cosine    "
		"rel_L2   sat%%    enc GB/s  dec GB/s\n");
	mi = 0;
	while (mi < N_Q8_MODES)
	{
		gi = 0;
		while (gi < N_GROUP_SIZES)
		{
			a = &out->q8[mi][gi];
			fprintf(stderr,
				"  %-10s  %5zu  %.3fx  %.6f  %.6f  %.4f  %5.2f  %8.3f  %8.3f\n",
				q8_mode_name((membrane_q8_mode_t)mi), g_group_sizes[gi],
				q8_ratio(a), q8_rmse(a), q8_cosine(a), q8_rel_l2(a),
				100.0 * q8_saturation(a),
				a->enc_ns > 0 ? (double)a->timed_bytes / a->enc_ns : 0.0,
				a->dec_ns > 0 ? (double)a->timed_bytes / a->dec_ns : 0.0);
			gi++;
		}
		mi++;
	}
}

/* K vs V at the reference config (32 elements, symmetric). */
static void	human_q8_kv(const kva_out_t *out)
{
	const q8_acc_t	*a;

	a = &out->q8[Q8_REF_MODE][Q8_REF_GROUP];
	fprintf(stderr, "\nQ8 K vs V (group=%zu, %s, all prompts):\n",
		g_group_sizes[Q8_REF_GROUP], q8_mode_name(Q8_REF_MODE));
	fprintf(stderr, "  K: ratio %.3fx  RMSE %.6f  cosine %.6f  rel_L2 %.4f"
		"  sat%% %.2f\n",
		ratio_of(a->raw[0], a->encoded[0]),
		a->elements[0] ? sqrt(a->sum_sq_err[0] / (double)a->elements[0]) : 0.0,
		a->elements[0] ? a->cosine_sum[0] / (double)a->elements[0] : 1.0,
		a->elements[0] ? a->rel_l2_sum[0] / (double)a->elements[0] : 0.0,
		a->elements[0] ? 100.0 * (double)a->saturated[0]
			/ (double)a->elements[0] : 0.0);
	fprintf(stderr, "  V: ratio %.3fx  RMSE %.6f  cosine %.6f  rel_L2 %.4f"
		"  sat%% %.2f\n",
		ratio_of(a->raw[1], a->encoded[1]),
		a->elements[1] ? sqrt(a->sum_sq_err[1] / (double)a->elements[1]) : 0.0,
		a->elements[1] ? a->cosine_sum[1] / (double)a->elements[1] : 1.0,
		a->elements[1] ? a->rel_l2_sum[1] / (double)a->elements[1] : 0.0,
		a->elements[1] ? 100.0 * (double)a->saturated[1]
			/ (double)a->elements[1] : 0.0);
}

static void	human_q8_layers(const kva_out_t *out)
{
	const q8_layer_t	*l;
	int					i;

	fprintf(stderr, "\nQ8 per-layer (group=%zu, %s, K+V, all prompts):\n",
		g_group_sizes[Q8_REF_GROUP], q8_mode_name(Q8_REF_MODE));
	i = 0;
	while (i < MAX_LAYERS)
	{
		l = &out->q8_layers[i];
		if (l->seen)
			fprintf(stderr, "  layer %2d: ratio %.3fx  RMSE %.6f\n", i,
				ratio_of(l->raw, l->encoded),
				l->elements ? sqrt(l->sum_sq_err / (double)l->elements) : 0.0);
		i++;
	}
}

/* Phase 3.1 success criteria (item 7 of the task): actual ratio >= 1.8x,
 * cosine similarity >= 0.99, relative L2 error reported explicitly, and
 * saturation kept low -- checked against every swept config so it is
 * visible which granularities and modes actually meet the bar. */
static void	human_q8_criteria(const kva_out_t *out)
{
	const q8_acc_t	*a;
	int				mi;
	int				gi;
	int				pass;

	fprintf(stderr, "\nQ8 success criteria (ratio>=1.8x, cosine>=0.99):\n");
	mi = 0;
	while (mi < N_Q8_MODES)
	{
		gi = 0;
		while (gi < N_GROUP_SIZES)
		{
			a = &out->q8[mi][gi];
			pass = (q8_ratio(a) >= 1.8 && q8_cosine(a) >= 0.99
					&& !a->decode_fail);
			fprintf(stderr,
				"  %-10s group=%-4zu ratio %.3fx cosine %.6f rel_L2 %.4f"
				" sat%% %.2f  %s\n",
				q8_mode_name((membrane_q8_mode_t)mi), g_group_sizes[gi],
				q8_ratio(a), q8_cosine(a), q8_rel_l2(a),
				100.0 * q8_saturation(a), pass ? "MEETS BAR" : "below bar");
			gi++;
		}
		mi++;
	}
}

static void	human_q8(const kva_out_t *out)
{
	human_q8_sweep(out);
	human_q8_kv(out);
	human_q8_layers(out);
	human_q8_criteria(out);
}

static int	opt_apply(kva_opts_t *o, int c)
{
	if (c == 'j')
		o->jsonl_path = optarg;
	else if (c == 'v')
		o->csv_path = optarg;
	else if (c == 'x' && o->meta_count < MAX_META)
		o->meta[o->meta_count++] = optarg;
	else if (c == 'p')
		o->pred_csv_path = optarg;
	else if (c == 'H')
		o->highplane_path = optarg;
	else if (c == 'q')
		o->quant_csv_path = optarg;
	else if (c != 'x')
		return (-1);
	return (0);
}

static int	parse_opts(int argc, char **argv, kva_opts_t *o)
{
	static struct option	lo[] = {
	{"jsonl", required_argument, 0, 'j'},
	{"csv", required_argument, 0, 'v'},
	{"pred-csv", required_argument, 0, 'p'},
	{"dump-highplane", required_argument, 0, 'H'},
	{"quant-csv", required_argument, 0, 'q'},
	{"meta", required_argument, 0, 'x'},
	{0, 0, 0, 0}};
	int						c;

	memset(o, 0, sizeof(*o));
	c = getopt_long(argc, argv, "j:v:p:H:q:x:", lo, NULL);
	while (c != -1)
	{
		if (opt_apply(o, c) != 0)
			return (-1);
		c = getopt_long(argc, argv, "j:v:p:H:q:x:", lo, NULL);
	}
	o->inputs = argv + optind;
	o->input_count = argc - optind;
	if (o->input_count < 1 || o->jsonl_path == NULL || o->csv_path == NULL)
	{
		fprintf(stderr, "Usage: membrane-kv-analyze --jsonl OUT --csv OUT "
			"[--pred-csv OUT] [--dump-highplane OUT] [--quant-csv OUT] "
			"[--meta k=v]... DUMP...\n");
		return (-1);
	}
	return (0);
}

static int	open_outputs(kva_out_t *out, const kva_opts_t *o)
{
	out->jsonl = fopen(o->jsonl_path, "w");
	out->csv = fopen(o->csv_path, "w");
	if (out->jsonl == NULL || out->csv == NULL)
		return (fprintf(stderr, "cannot open outputs\n"), -1);
	fprintf(out->csv, "file,model,layer,tensor,token_start,token_end,dtype,"
		"block_size,blocks,raw_bytes,rle_bytes,rle_ratio,adaptive_bytes,"
		"adaptive_ratio,zero_ratio,entropy_bits,max_run,integrity,"
		"byteplane_bytes,byteplane_ratio,byteplane_adaptive_bytes,"
		"byteplane_adaptive_ratio,low_entropy_bits,high_entropy_bits,"
		"low_plane_ratio,high_plane_ratio,byteplane_applicable,"
		"byteplane_integrity,"
		"huffman_bytes,huffman_ratio,huffman_adaptive_bytes,"
		"huffman_adaptive_ratio,theoretical_ratio,efficiency,"
		"huffman_header_bytes,huffman_integrity\n");
	out->totals.integrity_ok = 1;
	out->totals.byteplane_integrity_ok = 1;
	out->hp_integrity_ok = 1;
	if (o->pred_csv_path != NULL)
	{
		out->pred_csv = fopen(o->pred_csv_path, "w");
		if (out->pred_csv == NULL)
			return (fprintf(stderr, "cannot open pred-csv\n"), -1);
		fprintf(out->pred_csv, "file,model,layer,tensor,predictor,"
			"stride_elems,applicable,raw_bytes,entropy_bits,low_entropy_bits,"
			"high_entropy_bits,zero_u16_ratio,zero_byte_ratio,longest_zero_run,"
			"ideal_bytes,theoretical_ratio,q0,q1,q2,q3\n");
	}
	if (o->highplane_path != NULL)
	{
		out->highplane = fopen(o->highplane_path, "wb");
		if (out->highplane == NULL)
			return (fprintf(stderr, "cannot open highplane dump\n"), -1);
	}
	if (o->quant_csv_path != NULL)
	{
		out->quant_csv = fopen(o->quant_csv_path, "w");
		if (out->quant_csv == NULL)
			return (fprintf(stderr, "cannot open quant-csv\n"), -1);
		fprintf(out->quant_csv, "file,model,layer,tensor,mode,group_elems,"
			"raw_bytes,encoded_bytes,ratio,elements,saturation_ratio,"
			"nan_input,inf_input,mse,rmse,mae,max_abs_err,cosine_similarity,"
			"rel_l2_error,metadata_bytes,decode_ok\n");
	}
	return (0);
}

int	main(int argc, char **argv)
{
	kva_opts_t	o;
	kva_out_t	out;
	int			i;
	int			rc;

	if (parse_opts(argc, argv, &o) != 0)
		return (2);
	memset(&out, 0, sizeof(out));
	if (open_outputs(&out, &o) != 0)
		return (2);
	print_env(&o, &out);
	rc = 0;
	i = 0;
	while (i < o.input_count)
	{
		if (analyze_file(&out, &o, o.inputs[i]) != MEMBRANE_OK)
			rc = 1;
		i++;
	}
	human_totals(&out);
	human_pred(&out);
	human_huffman(&out);
	human_q8(&out);
	fclose(out.jsonl);
	fclose(out.csv);
	if (out.pred_csv != NULL)
		fclose(out.pred_csv);
	if (out.highplane != NULL)
		fclose(out.highplane);
	if (out.quant_csv != NULL)
		fclose(out.quant_csv);
	i = 0;
	while (i < N_Q8_MODES * N_GROUP_SIZES)
	{
		if (out.q8[i / N_GROUP_SIZES][i % N_GROUP_SIZES].decode_fail)
			rc = 1;
		i++;
	}
	if (rc == 0 && (!out.totals.integrity_ok
			|| !out.totals.byteplane_integrity_ok || !out.hp_integrity_ok))
		rc = 1;
	return (rc);
}
