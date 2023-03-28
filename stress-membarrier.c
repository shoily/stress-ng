/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-pthread.h"

#if defined(HAVE_LINUX_MEMBARRIER_H)
#include <linux/membarrier.h>
#endif

#if defined(__NR_membarrier)
#define HAVE_MEMBARRIER
#endif

static const stress_help_t help[] = {
	{ NULL,	"membarrier N",		"start N workers performing membarrier system calls" },
	{ NULL,	"membarrier-ops N",	"stop after N membarrier bogo operations" },
	{ NULL,	NULL,			NULL }
};

#if defined(HAVE_LIB_PTHREAD) && \
    defined(HAVE_MEMBARRIER)

typedef struct {
	pthread_t pthread;	/* thread handle */
	int pthread_ret;	/* thread create return */
	double duration;	/* membarrier duration */
	double count;		/* membarrier call count */
} membarrier_info_t;

#define MAX_MEMBARRIER_THREADS	(4)

static volatile bool keep_running;
static sigset_t set;

#if !defined(HAVE_LINUX_MEMBARRIER_H)
enum membarrier_cmd {
	MEMBARRIER_CMD_QUERY					= (0 << 0),
	MEMBARRIER_CMD_GLOBAL					= (1 << 0),
	MEMBARRIER_CMD_SHARED = MEMBARRIER_CMD_GLOBAL,
	MEMBARRIER_CMD_GLOBAL_EXPEDITED				= (1 << 1),
	MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED		= (1 << 2),
	MEMBARRIER_CMD_PRIVATE_EXPEDITED			= (1 << 3),
	MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED		= (1 << 4),
	MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE		= (1 << 5),
	MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE	= (1 << 6),
	MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ			= (1 << 7),
	MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ		= (1 << 8),
};

enum membarrier_cmd_flag {
        MEMBARRIER_CMD_FLAG_CPU					= (1 << 0),
};
#endif

static int stress_membarrier_exercise(const stress_args_t *args, membarrier_info_t *info)
{
	int ret;
	unsigned int i, mask;
	double t;

	ret = shim_membarrier(MEMBARRIER_CMD_QUERY, 0, 0);
	if (ret < 0) {
		pr_fail("%s: membarrier CMD QUERY failed, errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return -1;
	}
	mask = (unsigned int)ret;

	t = stress_time_now();
	for (i = 1; i; i <<= 1) {
		if (i & mask) {

			VOID_RET(int, shim_membarrier((int)i, 0, 0));
			info->count += 1.0;

#if defined(MEMBARRIER_CMD_FLAG_CPU)
			/* Exercise MEMBARRIER_CMD_FLAG_CPU flag */
			VOID_RET(int, shim_membarrier((int)i, MEMBARRIER_CMD_FLAG_CPU, 0));
			info->count += 1.0;
#endif
		}
	}
	info->duration += stress_time_now() - t;

	for (i = 1; i; i <<= 1) {
		if (i & mask) {
			/* Exercise illegal flags */
			VOID_RET(int, shim_membarrier((int)i, ~0, 0));

			/* Exercise illegal cpu_id */
			VOID_RET(int, shim_membarrier((int)i, 0, INT_MAX));
		}
	}

	/* Exercise illegal command */
	for (i = 1; i; i <<= 1) {
		if (!(i & mask)) {
			VOID_RET(int, shim_membarrier((int)i, 0, 0));
			break;
		}
	}
	return 0;
}

static void *stress_membarrier_thread(void *arg)
{
	static void *nowt = NULL;
	const stress_pthread_args_t *pargs = (stress_pthread_args_t *)arg;
	const stress_args_t *args = pargs->args;
	membarrier_info_t *info = (membarrier_info_t *)pargs->data;

	/*
	 *  Block all signals, let controlling thread
	 *  handle these
	 */
	(void)sigprocmask(SIG_BLOCK, &set, NULL);

	while (keep_running && keep_stressing_flag()) {
		if (stress_membarrier_exercise(args, info) < 0)
			break;
	}

	return &nowt;
}

/*
 *  stress on membarrier()
 *	stress system by IO sync calls
 */
static int stress_membarrier(const stress_args_t *args)
{
	int ret;
	/* We have MAX_MEMBARRIER_THREADS plus the stressor process */
	membarrier_info_t info[MAX_MEMBARRIER_THREADS + 1];
	size_t i;
	stress_pthread_args_t pargs = { args, NULL, 0 };
	double duration = 0.0, count = 0.0, rate;

	ret = shim_membarrier(MEMBARRIER_CMD_QUERY, 0, 0);
	if (ret < 0) {
		if (errno == ENOSYS) {
			if (args->instance == 0)
				pr_inf_skip("%s: stressor will be skipped, "
					"membarrier not supported\n",
					args->name);
			return EXIT_NOT_IMPLEMENTED;
		}
		pr_fail("%s: membarrier failed: errno=%d: (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_FAILURE;
	}
	if (!(ret & MEMBARRIER_CMD_SHARED)) {
		pr_inf("%s: membarrier MEMBARRIER_CMD_SHARED "
			"not supported\n", args->name);
		return EXIT_NOT_IMPLEMENTED;
	}

	(void)sigfillset(&set);
	for (i = 0; i < MAX_MEMBARRIER_THREADS + 1; i++) {
		info[i].pthread_ret = -1;
		(void)memset(&info[i].pthread, 0, sizeof(info[i].pthread));
		info[i].duration = 0.0;
		info[i].count = 0.0;
	}
	keep_running = true;

	for (i = 0; i < MAX_MEMBARRIER_THREADS; i++) {
		pargs.data = &info[i];
		info[i].pthread_ret =
			pthread_create(&info[i].pthread, NULL,
				stress_membarrier_thread, (void *)&pargs);
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		if (stress_membarrier_exercise(args, &info[MAX_MEMBARRIER_THREADS]) < 0) {
			pr_fail("%s: membarrier failed: errno=%d: (%s)\n",
				args->name, errno, strerror(errno));
		}
		inc_counter(args);
	} while (keep_stressing(args));

	keep_running = false;

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	for (i = 0; i < MAX_MEMBARRIER_THREADS + 1; i++) {
		duration += info[i].duration;
		count += info[i].count;
	}
	rate = (duration > 0) ? count / duration : 0.0;
	stress_metrics_set(args, 0, "membarrier calls per sec", rate);

	for (i = 0; i < MAX_MEMBARRIER_THREADS; i++) {
		if (info[i].pthread_ret == 0)
			(void)pthread_join(info[i].pthread, NULL);
	}
	return EXIT_SUCCESS;
}

stressor_info_t stress_membarrier_info = {
	.stressor = stress_membarrier,
	.class = CLASS_CPU_CACHE | CLASS_MEMORY,
	.help = help
};
#else
stressor_info_t stress_membarrier_info = {
	.stressor = stress_unimplemented,
	.class = CLASS_CPU_CACHE | CLASS_MEMORY,
	.help = help,
	.unimplemented_reason = "built without pthread support or membarrier() system call"
};
#endif
