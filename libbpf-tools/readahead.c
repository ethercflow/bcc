// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
// Copyright (c) 2020 Wenbo Zhang
//
// Based on readahead(8) from from BPF-Perf-Tools-Book by Brendan Gregg.
// 8-Jun-2020   Wenbo Zhang   Created this.
#include <argp.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "readahead.h"
#include "readahead.skel.h"
#include "trace_helpers.h"

static struct env {
	int duration;
	bool verbose;
} env = {
	.duration = -1
};

static volatile bool exiting;

const char *argp_program_version = "readahead 0.1";
const char *argp_program_bug_address = "<bpf@vger.kernel.org>";
const char argp_program_doc[] =
"Show fs automatic read-ahead usage.\n"
"\n"
"USAGE: readahead [--help] [-d DURATION]\n"
"\n"
"EXAMPLES:\n"
"    readahead              # summarize on-CPU time as a histogram"
"    readahead -d 10        # trace for 10 seconds only\n";

static const struct argp_option opts[] = {
	{ "duration", 'd', "DURATION", 0, "Duration to trace"},
	{ "verbose", 'v', NULL, 0, "Verbose debug output" },
	{},
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'v':
		env.verbose = true;
		break;
	case 'd':
		errno = 0;
		env.duration = strtol(arg, NULL, 10);
		if (errno || env.duration <= 0) {
			fprintf(stderr, "Invalid duration: %s\n", arg);
			argp_usage(state);
		}
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

int libbpf_print_fn(enum libbpf_print_level level,
		    const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sig_handler(int sig)
{
	exiting = true;
}

int main(int argc, char **argv)
{
	static const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};
	struct ksyms *ksyms = NULL;
	struct readahead_bpf *obj;
	struct hist *histp;
	int err;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;

	libbpf_set_print(libbpf_print_fn);

	err = bump_memlock_rlimit();
	if (err) {
		fprintf(stderr, "failed to increase rlimit: %d\n", err);
		return 1;
	}

	obj = readahead_bpf__open_and_load();
	if (!obj) {
		fprintf(stderr, "failed to open and/or load BPF object\n");
		return 1;
	}

	ksyms = ksyms__load();
	if (!ksyms) {
		fprintf(stderr, "failed to load kallsyms\n");
		goto cleanup;
	}

	/*
	 * From v5.10-rc1 (8238287), rename __do_page_cache_readahead() to
	 * do_page_cache_ra()
	 */
	if (ksyms__get_symbol(ksyms, "do_page_cache_ra")) {
		obj->links.do_page_cache_ra =
			bpf_program__attach(obj->progs.do_page_cache_ra);
		err = libbpf_get_error(obj->links.do_page_cache_ra);
		if (err) {
			fprintf(stderr, "failed to attach "
				"do_page_cache_ra: %s\n",
				strerror(err));
			goto cleanup;
		}
		obj->links.do_page_cache_ra_ret =
			bpf_program__attach(obj->progs.do_page_cache_ra_ret);
		err = libbpf_get_error(obj->links.do_page_cache_ra_ret);
		if (err) {
			fprintf(stderr, "failed to attach "
				"do_page_cache_ra_ret: %s\n",
				strerror(err));
			goto cleanup;
		}
	} else if (ksyms__get_symbol(ksyms, "__do_page_cache_readahead")) {
		obj->links.do_page_cache_readahead =
			bpf_program__attach(obj->progs.do_page_cache_readahead);
		err = libbpf_get_error(obj->links.do_page_cache_readahead);
		if (err) {
			fprintf(stderr, "failed to attach "
				"do_page_cache_readahead: %s\n",
				strerror(err));
			goto cleanup;
		}
		obj->links.do_page_cache_readahead_ret =
			bpf_program__attach(obj->progs.do_page_cache_readahead_ret);
		err = libbpf_get_error(obj->links.do_page_cache_readahead_ret);
		if (err) {
			fprintf(stderr, "failed to attach "
				"do_page_cache_readahead_ret: %s\n",
				strerror(err));
			goto cleanup;
		}
	} else {
		fprintf(stderr, "failed to find symbol: do_page_cache_ra/"
				"__do_page_cache_readahead, "
				"unsupport kernel version\n");
		goto cleanup;
	}

	obj->links.page_cache_alloc_ret =
		bpf_program__attach(obj->progs.page_cache_alloc_ret);
	err = libbpf_get_error(obj->links.page_cache_alloc_ret);
	if (err) {
		fprintf(stderr, "failed to attach "
			"page_cache_alloc_ret: %s\n",
			strerror(err));
		goto cleanup;
	}

	obj->links.mark_page_accessed =
		bpf_program__attach(obj->progs.mark_page_accessed);
	err = libbpf_get_error(obj->links.mark_page_accessed);
	if (err) {
		fprintf(stderr, "failed to attach "
			"mark_page_accessed: %s\n",
			strerror(err));
		goto cleanup;
	}

	signal(SIGINT, sig_handler);

	printf("Tracing fs read-ahead ... Hit Ctrl-C to end.\n");

	sleep(env.duration);
	printf("\n");

	histp = &obj->bss->hist;

	printf("Readahead unused/total pages: %d/%d\n",
		histp->unused, histp->total);
	print_log2_hist(histp->slots, MAX_SLOTS, "msecs");

cleanup:
	readahead_bpf__destroy(obj);
	ksyms__free(ksyms);
	return err != 0;
}
