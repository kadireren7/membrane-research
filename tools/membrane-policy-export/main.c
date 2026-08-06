#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "membrane/hash.h"
#include "membrane/llama_commit.h"
#include "membrane/policy.h"

# define MAX_LAYERS 256

typedef struct s_export_opts
{
	const char	*model_path;
	uint32_t	layer_count;
	const char	*kbits_csv;
	const char	*vbits_csv;
	const char	*model_name;
	const char	*tier_name;
	double		cosine_min;
	double		top1_min;
	double		top5_min;
	double		cosine_margin;
	double		top1_margin;
	double		top5_margin;
	uint32_t	search_budget;
	uint32_t	evals_used;
	const char	*out_path;
}	export_opts_t;

static int	die(const char *msg)
{
	fprintf(stderr, "membrane-policy-export: %s\n", msg);
	return (1);
}

static int	parse_csv_precisions(const char *csv, membrane_precision_t *out,
				uint32_t expect_n)
{
	const char	*p;
	char		*end;
	long		v;
	uint32_t	n;

	p = csv;
	n = 0;
	while (*p != '\0' && n < expect_n)
	{
		v = strtol(p, &end, 10);
		if (end == p)
			return (0);
		out[n] = (membrane_precision_t)v;
		n++;
		p = end;
		if (*p == ',')
			p++;
		else if (*p != '\0')
			return (0);
	}
	return (n == expect_n && *p == '\0');
}

static int	parse_args(int argc, char **argv, export_opts_t *o)
{
	int	i;

	memset(o, 0, sizeof(*o));
	o->model_name = "model";
	o->tier_name = "balanced";
	i = 1;
	while (i < argc)
	{
		if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
			o->model_path = argv[++i];
		else if (strcmp(argv[i], "--layer-count") == 0 && i + 1 < argc)
			o->layer_count = (uint32_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--kbits") == 0 && i + 1 < argc)
			o->kbits_csv = argv[++i];
		else if (strcmp(argv[i], "--vbits") == 0 && i + 1 < argc)
			o->vbits_csv = argv[++i];
		else if (strcmp(argv[i], "--model-name") == 0 && i + 1 < argc)
			o->model_name = argv[++i];
		else if (strcmp(argv[i], "--tier") == 0 && i + 1 < argc)
			o->tier_name = argv[++i];
		else if (strcmp(argv[i], "--cosine-min") == 0 && i + 1 < argc)
			o->cosine_min = atof(argv[++i]);
		else if (strcmp(argv[i], "--top1-min") == 0 && i + 1 < argc)
			o->top1_min = atof(argv[++i]);
		else if (strcmp(argv[i], "--top5-min") == 0 && i + 1 < argc)
			o->top5_min = atof(argv[++i]);
		else if (strcmp(argv[i], "--cosine-margin") == 0 && i + 1 < argc)
			o->cosine_margin = atof(argv[++i]);
		else if (strcmp(argv[i], "--top1-margin") == 0 && i + 1 < argc)
			o->top1_margin = atof(argv[++i]);
		else if (strcmp(argv[i], "--top5-margin") == 0 && i + 1 < argc)
			o->top5_margin = atof(argv[++i]);
		else if (strcmp(argv[i], "--search-budget") == 0 && i + 1 < argc)
			o->search_budget = (uint32_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--evals-used") == 0 && i + 1 < argc)
			o->evals_used = (uint32_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
			o->out_path = argv[++i];
		else
			return (die("unknown or malformed option"));
		i++;
	}
	if (o->model_path == NULL || o->layer_count == 0
			|| o->layer_count > MAX_LAYERS || o->kbits_csv == NULL
			|| o->vbits_csv == NULL || o->out_path == NULL)
		return (die("usage: --model PATH --layer-count N --kbits CSV "
				"--vbits CSV --out POLICY [--model-name NAME] "
				"[--tier NAME] [--cosine-min F] [--top1-min F] "
				"[--top5-min F] [--cosine-margin F] [--top1-margin F] "
				"[--top5-margin F] [--search-budget N] [--evals-used N]"));
	return (0);
}

int	main(int argc, char **argv)
{
	export_opts_t			o;
	membrane_precision_t	kbits[MAX_LAYERS];
	membrane_precision_t	vbits[MAX_LAYERS];
	membrane_policy_build_t	b;
	uint8_t					digest[MEMBRANE_SHA256_DIGEST_BYTES];
	char					hex[MEMBRANE_SHA256_HEX_LEN + 1];

	if (parse_args(argc, argv, &o) != 0)
		return (1);
	if (!parse_csv_precisions(o.kbits_csv, kbits, o.layer_count))
		return (die("--kbits: expected exactly layer-count comma-"
				"separated precisions (16/8/4)"));
	if (!parse_csv_precisions(o.vbits_csv, vbits, o.layer_count))
		return (die("--vbits: expected exactly layer-count comma-"
				"separated precisions (16/8/4)"));
	if (membrane_sha256_file(o.model_path, hex) != MEMBRANE_OK)
		return (die("could not read --model to compute its SHA-256"));
	memset(&b, 0, sizeof(b));
	{
		size_t	i;

		i = 0;
		while (i < MEMBRANE_SHA256_DIGEST_BYTES)
		{
			unsigned int	byte;

			sscanf(hex + i * 2, "%2x", &byte);
			digest[i] = (uint8_t)byte;
			i++;
		}
	}
	memcpy(b.model_sha256, digest, MEMBRANE_SHA256_DIGEST_BYTES);
	b.llama_cpp_commit = MEMBRANE_LLAMA_CPP_COMMIT;
	b.layer_count = o.layer_count;
	b.k_prec = kbits;
	b.v_prec = vbits;
	b.model_name = o.model_name;
	b.tier_name = o.tier_name;
	b.cosine_min = o.cosine_min;
	b.top1_min = o.top1_min;
	b.top5_min = o.top5_min;
	b.cosine_margin = o.cosine_margin;
	b.top1_margin = o.top1_margin;
	b.top5_margin = o.top5_margin;
	b.search_budget = o.search_budget;
	b.evals_used = o.evals_used;
	b.created_unix_time = (uint64_t)time(NULL);
	if (membrane_policy_save(o.out_path, &b) != MEMBRANE_OK)
		return (die("membrane_policy_save failed"));
	fprintf(stderr, "wrote %s: model=%s tier=%s layers=%u hash=%s "
		"llama.cpp=%s\n", o.out_path, o.model_name, o.tier_name,
		o.layer_count, hex, MEMBRANE_LLAMA_CPP_COMMIT);
	return (0);
}
