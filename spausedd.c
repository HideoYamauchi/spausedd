/*
 * Copyright (c) 2018-2021, Red Hat, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND RED HAT, INC. DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL RED HAT, INC. BE LIABLE
 * FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Author: Jan Friesse <jfriesse@redhat.com>
 */

#include <sys/types.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_VMGUESTLIB
#include <vmGuestLib.h>
#endif

#define PROGRAM_NAME			"spausedd"

#define DEFAULT_TIMEOUT			200

/*
 * Maximum allowed timeout is one hour
 */
#define MAX_TIMEOUT			(1000 * 60 * 60)

#define DEFAULT_MAX_STEAL_THRESHOLD	10  /* vSphere環境以外の閾値 */
#define DEFAULT_MAX_STEAL_THRESHOLD_GL	100 /* vSphere環境の閾値 */

#define NO_NS_IN_SEC			1000000000ULL
#define NO_NS_IN_MSEC			1000000ULL
#define NO_MSEC_IN_SEC			1000ULL

#ifndef LOG_TRACE
#define LOG_TRACE			(LOG_DEBUG + 1)
#endif

enum move_to_root_cgroup_mode {
	MOVE_TO_ROOT_CGROUP_MODE_OFF = 0,
	MOVE_TO_ROOT_CGROUP_MODE_ON = 1,
	MOVE_TO_ROOT_CGROUP_MODE_AUTO = 2,
};

/*
 * Globals
 */
static int log_debug = 0;
static int log_to_syslog = 0;
static int log_to_stderr = 0;

static uint64_t times_not_scheduled = 0;

/*
 * If current steal percent is larger than max_steal_threshold warning is shown.
 * Default is DEFAULT_MAX_STEAL_THRESHOLD (or DEFAULT_MAX_STEAL_THRESHOLD_GL if
 * HAVE_VMGUESTLIB and VMGuestLib init success)
 */
static double max_steal_threshold = DEFAULT_MAX_STEAL_THRESHOLD;
static int max_steal_threshold_user_set = 0;

static volatile sig_atomic_t stop_main_loop = 0;

static volatile sig_atomic_t display_statistics = 0;

#ifdef HAVE_VMGUESTLIB
static int use_vmguestlib_stealtime = 0;
static VMGuestLibHandle guestlib_handle;
#endif

/*
 * Definitions (for attributes)
 */
static void	log_printf(int priority, const char *format, ...)
    __attribute__((__format__(__printf__, 2, 3)));

static void	log_vprintf(int priority, const char *format, va_list ap)
    __attribute__((__format__(__printf__, 2, 0)));

/*
 * Logging functions
 */
static const char log_month_str[][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void
log_vprintf(int priority, const char *format, va_list ap)
{
	va_list ap_copy;
	int final_priority;
	time_t current_time;
	struct tm tm_res;

	if ((priority < LOG_DEBUG) || (priority == LOG_DEBUG && log_debug >= 1)
	    || (priority == LOG_TRACE && log_debug >= 2)) {
		if (log_to_stderr) {
			current_time = time(NULL);
			localtime_r(&current_time, &tm_res);
			fprintf(stderr, "%s %02d %02d:%02d:%02d ",
				log_month_str[tm_res.tm_mon], tm_res.tm_mday, tm_res.tm_hour,
				tm_res.tm_min, tm_res.tm_sec);

			fprintf(stderr, "%s: ", PROGRAM_NAME);
			va_copy(ap_copy, ap);
			vfprintf(stderr, format, ap_copy);
			va_end(ap_copy);
			fprintf(stderr, "\n");
		}

		if (log_to_syslog) {
			final_priority = priority;
			if (priority > LOG_INFO) {
				final_priority = LOG_INFO;
			}

			va_copy(ap_copy, ap);
			vsyslog(final_priority, format, ap);
			va_end(ap_copy);
		}
	}
}

static void
log_printf(int priority, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	log_vprintf(priority, format, ap);

	va_end(ap);
}

static void
log_perror(int priority, const char *s)
{
	int stored_errno;

	stored_errno = errno;

	log_printf(priority, "%s (%u): %s", s, stored_errno, strerror(stored_errno));
}

static int
util_strtonum(const char *str, long long int min_val, long long int max_val, long long int *res)
{
	long long int tmp_ll;
	char *ep;

	if (min_val > max_val) {
		return (-1);
	}

	errno = 0;

	tmp_ll = strtoll(str, &ep, 10);
	if (ep == str || *ep != '\0' || errno != 0) {
		return (-1);
	}

	if (tmp_ll < min_val || tmp_ll > max_val) {
		return (-1);
	}

	*res = tmp_ll;

	return (0);
}

/*
 * Utils
 */
static void
utils_mlockall(void)
{
	int res;
	struct rlimit rlimit;

	rlimit.rlim_cur = RLIM_INFINITY;
	rlimit.rlim_max = RLIM_INFINITY;

	res = setrlimit(RLIMIT_MEMLOCK, &rlimit);
	if (res == -1) {
		log_printf(LOG_WARNING, "Could not increase RLIMIT_MEMLOCK, not locking memory");

		return;
	}

	res = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (res == -1) {
		log_printf(LOG_WARNING, "Could not mlockall");
	}
}

static void
utils_tty_detach(void)
{
	int devnull;

	switch (fork()) {
		case -1:
			err(1, "Can't create child process");
			break;
		case 0:
			break;
		default:
			exit(0);
			break;
	}

	/* Create new session */
	(void)setsid();

	/*
	 * Map stdin/out/err to /dev/null.
	 */
	devnull = open("/dev/null", O_RDWR);
	if (devnull == -1) {
		err(1, "Can't open /dev/null");
	}

	if (dup2(devnull, 0) < 0 || dup2(devnull, 1) < 0
	    || dup2(devnull, 2) < 0) {
		close(devnull);
		err(1, "Can't dup2 stdin/out/err to /dev/null");
	}
	close(devnull);
}

static int
utils_set_rr_scheduler(int silent)
{
#ifdef _POSIX_PRIORITY_SCHEDULING
	int max_prio;
	struct sched_param param;
	int res;

	max_prio = sched_get_priority_max(SCHED_RR);
	if (max_prio == -1) {
		if (!silent) {
			log_perror(LOG_WARNING, "Can't get maximum SCHED_RR priority");
		}

		return (-1);
	}

	param.sched_priority = max_prio;
	res = sched_setscheduler(0, SCHED_RR, &param);
	if (res == -1) {
		if (!silent) {
			log_perror(LOG_WARNING, "Can't set SCHED_RR");
		}

		return (-1);
	}
#else
	log_printf(LOG_WARNING, "Platform without sched_get_priority_min");
#endif

	return (0);
}

static void
utils_move_to_root_cgroup(void)
{
	FILE *f;
	const char *cgroup_task_fname = NULL;

	/*
	 * /sys/fs/cgroup is hardcoded, because most of Linux distributions are now
	 * using systemd and systemd uses hardcoded path of cgroup mount point.
	 *
	 * This feature is expected to be removed as soon as systemd gets support
	 * for managing RT configuration.
	 */
	f = fopen("/sys/fs/cgroup/cpu/cpu.rt_runtime_us", "rt");
	if (f == NULL) {
		/*
		 * Try cgroup v2
		 */
		f = fopen("/sys/fs/cgroup/cgroup.procs", "rt");
		if (f == NULL) {
			log_printf(LOG_DEBUG, "cpu.rt_runtime_us or cgroup.procs doesn't exist -> "
			    "system without cgroup or with disabled CONFIG_RT_GROUP_SCHED");

			return ;
		} else {
			log_printf(LOG_DEBUG, "Moving main pid to cgroup v2 root cgroup");

			cgroup_task_fname = "/sys/fs/cgroup/cgroup.procs";
		}
	} else {
		log_printf(LOG_DEBUG, "Moving main pid to cgroup v1 root cgroup");

		cgroup_task_fname = "/sys/fs/cgroup/cpu/tasks";
	}
	(void)fclose(f);

	f = fopen(cgroup_task_fname, "w");
	if (f == NULL) {
		log_printf(LOG_WARNING, "Can't open cgroups tasks file for writing");
		return ;
	}

	if (fprintf(f, "%jd\n", (intmax_t)getpid()) <= 0) {
		log_printf(LOG_WARNING, "Can't write spausedd pid into cgroups tasks file");
	}

	if (fclose(f) != 0) {
		log_printf(LOG_WARNING, "Can't close cgroups tasks file");
		return ;
	}
}

/*
 * Signal handlers
 */
static void
signal_int_handler(int sig)
{

	stop_main_loop = 1;
}

static void
signal_usr1_handler(int sig)
{

	display_statistics = 1;
}

static void
signal_handlers_register(void)
{
	struct sigaction act;

	act.sa_handler = signal_int_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;

	sigaction(SIGINT, &act, NULL);

	act.sa_handler = signal_int_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;

	sigaction(SIGTERM, &act, NULL);

	act.sa_handler = signal_usr1_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;

	sigaction(SIGUSR1, &act, NULL);
}

/*
 * Function to get current CLOCK_MONOTONIC in nanoseconds
 */
static uint64_t
nano_current_get(void)
{
	uint64_t res;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	res = (uint64_t)(ts.tv_sec * NO_NS_IN_SEC) + (uint64_t)ts.tv_nsec;
	return (res);
}

/*
 * Get steal time provided by kernel
 */
static uint64_t
nano_stealtime_kernel_get(void)
{
	FILE *f;
	char buf[4096];
	uint64_t s_user, s_nice, s_system, s_idle, s_iowait, s_irq, s_softirq, s_steal;
	uint64_t res_steal;
	long int clock_tick;
	uint64_t factor;

	res_steal = 0;

	f = fopen("/proc/stat", "rt");
	if (f == NULL) {
		return (res_steal);
	}

	while (fgets(buf, sizeof(buf), f) != NULL) {
		s_user = s_nice = s_system = s_idle = s_iowait = s_irq = s_softirq = s_steal = 0;
		if (sscanf(buf, "cpu %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64
		    " %"PRIu64" %"PRIu64,
		    &s_user, &s_nice, &s_system, &s_idle, &s_iowait, &s_irq, &s_softirq,
		    &s_steal) > 4) {
			/*
			 * Got valid line
			 */
			clock_tick = sysconf(_SC_CLK_TCK);
			if (clock_tick == -1) {
				log_printf(LOG_TRACE, "Can't get _SC_CLK_TCK, using 100");
				clock_tick = 100;
			}

			factor = NO_NS_IN_SEC / clock_tick;
			res_steal = s_steal * factor;

			log_printf(LOG_TRACE, "nano_stealtime_get kernel stats: "
			    "user = %"PRIu64", nice = %"PRIu64"s, system = %"PRIu64
			    ", idle = %"PRIu64", iowait = %"PRIu64", irq = %"PRIu64
			    ", softirq = %"PRIu64", steal = %"PRIu64", factor = %"PRIu64
			    ", result steal = %"PRIu64,
			    s_user, s_nice, s_system, s_idle, s_iowait, s_irq, s_softirq, s_steal,
			    factor, res_steal);

			break;
		}
	}

	fclose(f);

	return (res_steal);
}

/*
 * Get steal time provided by vmguestlib
 */
#ifdef HAVE_VMGUESTLIB
/* vSphere SDKの場合 */
static uint64_t
nano_stealtime_vmguestlib_get(void)
{
	VMGuestLibError gl_err;
	uint64_t stolen_ms;
	uint64_t res_steal;
	uint64_t used_ms, elapsed_ms;
	static uint64_t prev_stolen_ms, prev_used_ms, prev_elapsed_ms;

	/*
          仮想マシンに関する情報を更新します。この情報はVMGuestLibHandleに関連付けられています。
          VMGuestLib_UpdateInfoは、システムコールと同様のCPUリソースを必要とするため、パフォーマンスに影響を与える可能性があります。パフォーマンスが心配な場合は、VMGuestLib_UpdateInfoへの呼び出しの数を最小限に抑えてください。

          プログラムが複数のスレッドを使用する場合、各スレッドは異なるハンドルを使用する必要があります。それ以外の場合は、更新呼び出しの周りにロックスキームを実装する必要があります。vSphere Guest APIは、ハンドルを使用したアクセスに関する内部ロックを実装していません。
	*/
	gl_err = VMGuestLib_UpdateInfo(guestlib_handle);
	if (gl_err != VMGUESTLIB_ERROR_SUCCESS) {
		log_printf(LOG_WARNING, "Can't update stolen time from guestlib: %s",
		    VMGuestLib_GetErrorText(gl_err));

		return (0);
	}
        /* 仮想マシンが準備完了状態（実行状態に移行可能）であったが、実行がスケジュールされていなかったミリ秒数を取得 */
	gl_err = VMGuestLib_GetCpuStolenMs(guestlib_handle, &stolen_ms);
	if (gl_err != VMGUESTLIB_ERROR_SUCCESS) {
		log_printf(LOG_WARNING, "Can't get stolen time from guestlib: %s",
		    VMGuestLib_GetErrorText(gl_err));

		return (0);
	}

	/*
	 * For debug purpose, returned errors ignored
	 */
	used_ms = elapsed_ms = 0;
        /*
        仮想マシンがCPUを使用したミリ秒数を取得します。この値には、ゲストオペレーティングシステムによって使用された時間と、この仮想マシンのタスクの仮想化コードによって使用された時間が含まれます。この値を経過時間（VMGuestLib_GetElapsedMs）と組み合わせて、仮想マシンの実効CPU速度を見積もることができます。この値はelapsedMsのサブセットです。 
        */
	(void)VMGuestLib_GetCpuUsedMs(guestlib_handle, &used_ms);
        /* 
        仮想マシンがサーバー上で最後に実行を開始してから経過したミリ秒数を取得します。経過時間のカウントは、仮想マシンの電源がオンになるか、再開されるか、VMotionを使用して移行されるたびに再開されます。この値は、仮想マシンがその時間中に処理能力を使用しているかどうかに関係なく、ミリ秒をカウントします。この値を仮想マシンが使用するCPU時間（VMGuestLib_GetCpuUsedMs）と組み合わせて、仮想マシンの実効CPU速度を見積もることができます。cpuUsedMSは、この値のサブセットです
        */
	(void)VMGuestLib_GetElapsedMs(guestlib_handle, &elapsed_ms);

	log_printf(LOG_TRACE, "nano_stealtime_vmguestlib_get stats: "
	    "stolen = %"PRIu64" (%"PRIu64"), used = %"PRIu64" (%"PRIu64"), "
	    "elapsed = %"PRIu64" (%"PRIu64")",
	    stolen_ms, stolen_ms - prev_stolen_ms, used_ms, used_ms - prev_used_ms,
	    elapsed_ms, elapsed_ms - prev_elapsed_ms);

	prev_stolen_ms = stolen_ms;
	prev_used_ms = used_ms;
	prev_elapsed_ms = elapsed_ms;

	res_steal = NO_NS_IN_MSEC * stolen_ms;

	return (res_steal);
}
#endif

/*
 * Get steal time
 */
static uint64_t
nano_stealtime_get(void)
{
	uint64_t res;

#ifdef HAVE_VMGUESTLIB
	if (use_vmguestlib_stealtime) {
		res = nano_stealtime_vmguestlib_get();
	} else {
		res = nano_stealtime_kernel_get();
	}
#else
	res = nano_stealtime_kernel_get();
#endif

	return (res);
}


/*
 * VMGuestlib
 */
static void
guestlib_init(void)
{
#ifdef HAVE_VMGUESTLIB
/* vSphere SDKの場合 */
	VMGuestLibError gl_err;

	/*
他のvSphereGuestAPI関数で使用するためのハンドルを取得します。ゲストライブラリハンドルは、仮想マシンに関する情報にアクセスするためのコンテキストを提供します。仮想マシンの統計と状態データは特定のゲストライブラリハンドルに関連付けられているため、1つのハンドルを使用しても、別のハンドルに関連付けられているデータには影響しません。
	*/
	gl_err = VMGuestLib_OpenHandle(&guestlib_handle);
	if (gl_err != VMGUESTLIB_ERROR_SUCCESS) {
		log_printf(LOG_DEBUG, "Can't open guestlib handle: %s", VMGuestLib_GetErrorText(gl_err));
		return ;
	}

	log_printf(LOG_INFO, "Using VMGuestLib");

	use_vmguestlib_stealtime = 1;

	if (!max_steal_threshold_user_set) {
//オプションなし時のデフォルト閾値:100
		max_steal_threshold = DEFAULT_MAX_STEAL_THRESHOLD_GL;
	}
#endif
}

static void
guestlib_fini(void)
{
#ifdef HAVE_VMGUESTLIB
	VMGuestLibError gl_err;

	if (use_vmguestlib_stealtime) {
		/*
			VMGuestLib_OpenHandleで取得したハンドルを解放します。
		*/
		gl_err = VMGuestLib_CloseHandle(guestlib_handle);

		if (gl_err != VMGUESTLIB_ERROR_SUCCESS) {
			log_printf(LOG_DEBUG, "Can't close guestlib handle: %s", VMGuestLib_GetErrorText(gl_err));
		}
	}
#endif
}

/*
 * MAIN FUNCTIONALITY
 */
static void
print_statistics(uint64_t tv_start)
{
	uint64_t tv_diff;
	uint64_t tv_now;

	tv_now = nano_current_get();
	tv_diff = tv_now - tv_start;
	log_printf(LOG_INFO, "During %0.4fs runtime %s was %"PRIu64"x not scheduled on time",
	    (double)tv_diff / NO_NS_IN_SEC, PROGRAM_NAME, times_not_scheduled);
}

static void
poll_run(uint64_t timeout)
{
	uint64_t tv_now;
	uint64_t tv_prev;	// Time before poll syscall
	uint64_t tv_diff;
	uint64_t tv_max_allowed_diff;
	uint64_t tv_start;
	uint64_t steal_now;
	uint64_t steal_prev;
	uint64_t steal_diff;
	int poll_res;
	int poll_timeout;
	double steal_perc;

        /* チェック差分、pollタイマー時間、開始nano時間の取得 */
	tv_max_allowed_diff = timeout * NO_NS_IN_MSEC;
	poll_timeout = timeout / 3;
	tv_start = nano_current_get();

	log_printf(LOG_INFO, "Running main poll loop with maximum timeout %"PRIu64
	    " and steal threshold %0.0f%%", timeout, max_steal_threshold);

	while (!stop_main_loop) {
		/*
		 * Fetching stealtime can block so get it before monotonic time
		 */
                /* 開始時のsteal,nano時間の取得 */
		steal_prev = steal_now = nano_stealtime_get();
		tv_prev = tv_now = nano_current_get();

		if (display_statistics) {
			print_statistics(tv_start);

			display_statistics = 0;
		}

		log_printf(LOG_DEBUG, "now = %0.4fs, max_diff = %0.4fs, poll_timeout = %0.4fs, "
		    "steal_time = %0.4fs",
		    (double)tv_now / NO_NS_IN_SEC, (double)tv_max_allowed_diff / NO_NS_IN_SEC,
		    (double)poll_timeout / NO_MSEC_IN_SEC, (double)steal_now / NO_NS_IN_SEC);

		if (poll_timeout < 0) {
			poll_timeout = 0;
		}
		/* デフォルト200ms/3=66msのタイマーの実行 */
		poll_res = poll(NULL, 0, poll_timeout);
		if (poll_res == -1) {
			if (errno != EINTR) {
				log_perror(LOG_ERR, "Poll error");
				exit(2);
			}
		}

		/*
		 * Fetching stealtime can block so first get monotonic and then steal time
		 */
                /* タイマー完了nano時間の取得と、差分の計算 */
		tv_now = nano_current_get();
		tv_diff = tv_now - tv_prev;
		/* タイマー完了stealの取得　*/
		steal_now = nano_stealtime_get();
		steal_diff = steal_now - steal_prev;
                /* steal差分/nano差分 */
		steal_perc = ((double)steal_diff / tv_diff) * (double)100;

//log_printf(LOG_INFO, "max_steal_threshold : %0.1f%%", max_steal_threshold);
		if (tv_diff > tv_max_allowed_diff) {
			/* タイマーの経過時間が200msを超えた場合 */
			log_printf(LOG_ERR, "Not scheduled for %0.4fs (threshold is %0.4fs), "
			    "steal time is %0.4fs (%0.2f%%)",
			    (double)tv_diff / NO_NS_IN_SEC,
			    (double)tv_max_allowed_diff / NO_NS_IN_SEC,
			    (double)steal_diff / NO_NS_IN_SEC,
			    steal_perc);

			if (steal_perc > max_steal_threshold) {
                                /* nano単位でのsteal差分が閾値を超えた場合は、steal差分も出力 */
				log_printf(LOG_WARNING, "Steal time is > %0.1f%%, this is usually because "
				    "of overloaded host machine", max_steal_threshold);
			}
			times_not_scheduled++;
		}
	}

	log_printf(LOG_INFO, "Main poll loop stopped");
	print_statistics(tv_start);
}

/*
 * CLI
 */
static void
usage(void)
{
	printf("usage: %s [-dDfhp] [-m steal_th] [-P mode] [-t timeout]\n", PROGRAM_NAME);
	printf("\n");
	printf("  -d            Display debug messages\n");
	printf("  -D            Run on background - daemonize\n");
	printf("  -f            Run foreground - do not daemonize (default)\n");
	printf("  -h            Show help\n");
	printf("  -p            Do not set RR scheduler\n");
	printf("  -m steal_th   Steal percent threshold\n");
	printf("  -P mode       Move process to root cgroup only when needed (auto), always (on) or never (off)\n");
	printf("  -t timeout    Set timeout value (default: %u)\n", DEFAULT_TIMEOUT);
}

int
main(int argc, char **argv)
{
	int ch;
	int foreground;
	long long int tmpll;
	uint64_t timeout;
	int set_prio;
	enum move_to_root_cgroup_mode move_to_root_cgroup;
	int silent;

	foreground = 1;
	timeout = DEFAULT_TIMEOUT;
	set_prio = 1;
	move_to_root_cgroup = MOVE_TO_ROOT_CGROUP_MODE_AUTO;
	max_steal_threshold = DEFAULT_MAX_STEAL_THRESHOLD;
	max_steal_threshold_user_set = 0;

	while ((ch = getopt(argc, argv, "dDfhpm:P:t:")) != -1) {
		switch (ch) {
		case 'D':
			foreground = 0;
			break;
		case 'd':
			log_debug++;
			break;
		case 'f':
			foreground = 1;
			break;
		case 'm':
			if (util_strtonum(optarg, 1, UINT32_MAX, &tmpll) != 0) {
				errx(1, "Steal percent threshold %s is invalid", optarg);
			}
			max_steal_threshold_user_set = 1;
			max_steal_threshold = tmpll;
			break;
		case 't':
			if (util_strtonum(optarg, 1, MAX_TIMEOUT, &tmpll) != 0) {
				errx(1, "Timeout %s is invalid", optarg);
			}

			timeout = (uint64_t)tmpll;
			break;
		case 'h':
		case '?':
			usage();
			exit(1);
			break;
		case 'P':
			if (strcasecmp(optarg, "on") == 0) {
				move_to_root_cgroup = MOVE_TO_ROOT_CGROUP_MODE_ON;
			} else if (strcasecmp(optarg, "off") == 0) {
				move_to_root_cgroup = MOVE_TO_ROOT_CGROUP_MODE_OFF;
			} else if (strcasecmp(optarg, "auto") == 0) {
				move_to_root_cgroup = MOVE_TO_ROOT_CGROUP_MODE_AUTO;
			} else {
				errx(1, "Move to root cgroup mode %s is invalid", optarg);
			}
			break;
		case 'p':
			set_prio = 0;
			break;
		default:
			errx(1, "Unhandled option %c", ch);
		}
	}

	if (foreground) {
		log_to_stderr = 1;
	} else {
		log_to_syslog = 1;
		utils_tty_detach();
		openlog(PROGRAM_NAME, LOG_PID, LOG_DAEMON);
	}

	utils_mlockall();

	if (move_to_root_cgroup == MOVE_TO_ROOT_CGROUP_MODE_ON) {
		utils_move_to_root_cgroup();
	}

	if (set_prio) {
		silent = (move_to_root_cgroup == MOVE_TO_ROOT_CGROUP_MODE_AUTO);

		if (utils_set_rr_scheduler(silent) == -1 &&
		    move_to_root_cgroup == MOVE_TO_ROOT_CGROUP_MODE_AUTO) {
			/*
			 * Try to move process to root cgroup and try set priority again
			 */
			utils_move_to_root_cgroup();

			(void)utils_set_rr_scheduler(0);
		}
	}

	signal_handlers_register();

	guestlib_init();
	/* タイマー実行ループ */
	poll_run(timeout);

	guestlib_fini();

	if (!foreground) {
		closelog();
	}

	return (0);
}
