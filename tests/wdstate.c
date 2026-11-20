/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wdstate - read-only probe for the FreeBSD 15 watchdog core on the 520B.
 *
 * Why this exists: the sysctls cannot answer whether the dog is being serviced.
 *
 *   hw.watchdog.wd_last_msecs  always 0. In sys/dev/watchdog/watchdog.c the
 *                              backing variable wd_last_sbt_sysctl is
 *                              declared and sysctl-bound but never assigned.
 *   hw.watchdog.wd_last_u[_secs]  the last requested PERIOD, not a timestamp.
 *
 * The only real evidence is wd_lastpat / wd_lastpat_valid (watchdog.c:90-91),
 * set inside wdog_kern_pat_sbt() only when a hardware watchdog_list handler
 * returns error == 0. Those are not exported as sysctls. They are reachable
 * through WDIOC_GETTIMELEFT, which is what this program calls.
 *
 * Reading WDIOC_GETTIMELEFT:
 *   ENOENT        wd_lastpat_valid == 0 -> no hardware handler has EVER
 *                 accepted a pat since boot. The dog is not being serviced.
 *   >= 0          seconds SINCE the last successful pat. The name is a lie:
 *                 wd_get_time_left() returns ts.tv_sec - wd_lastpat, i.e.
 *                 elapsed, not remaining. Compare against the period.
 *
 * WDIOC_GETTIMEOUT returns wd_last_sbt, the last period handed down.
 * WDIOC_GETPRETIMEOUT returns the core's pre-timeout callout setting.
 *
 * Nothing here writes. No pat, no set, no disarm. Safe to run any time.
 *
 * Build on any FreeBSD 15.1 host (static so it runs on the box regardless of
 * the libc it has):
 *   cc -static -O1 -o wdstate tests/wdstate.c
 * Run on the 520B as root:
 *   ./wdstate
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/watchdog.h>

/*
 * Include order matters. sys/watchdog.h pulls only sys/ioccom.h and
 * sys/_types.h, which give __sbintime_t but NOT sbintime_t - so the
 * WDIOC_* _IOR/_IOW macros expand an undeclared name and the compile
 * dies. sbintime_t is defined by sys/types.h:193 (unconditionally, not
 * kernel-only). watchdogd.c does the same thing: sys/types.h before
 * sys/watchdog.h. Using any other width here silently mismatches the
 * ioctl number and the kernel answers ENOTTY, which looks like "the
 * driver does not support this" when it is really our type error.
 */

/*
 * sbintime_t is 32.32 FIXED POINT, not nanoseconds. The integer half is
 * seconds and the fraction is 2^-32 seconds; SBT_1S == 1LL << 32.
 * Dividing by 1e9 (the nanosecond mistake) turns a 120 s timeout into
 * "515.4 s" and a 2 s gap into "8.6 s" - plausible-looking numbers that are
 * both wrong, which is the dangerous kind of bug. Everything here divides
 * by SBT_1S.
 */
#ifndef	SBT_1S
#define	SBT_1S	(1LL << 32)
#endif

#define	SBT_S(x)	((double)(x) / (double)SBT_1S)

static void
sample(void)
{
	sbintime_t v;
	int fd;

	/* _PATH_WATCHDOG is "fido" on FreeBSD 15 (sys/sys/watchdog.h:37), so
	 * this opens /dev/fido. Use the macro, never a literal path. */
	fd = open("/dev/" _PATH_WATCHDOG, O_RDONLY);
	if (fd < 0)
		err(1, "open /dev/" _PATH_WATCHDOG);

	v = 0;
	if (ioctl(fd, WDIOC_GETTIMEOUT, &v) != 0)
		warn("WDIOC_GETTIMEOUT");
	else
		printf("  GETTIMEOUT    : %14lld sbt = %8.3f s "
		    "(last period handed to the core)\n", (long long)v,
		    SBT_S(v));

	v = 0;
	if (ioctl(fd, WDIOC_GETPRETIMEOUT, &v) != 0)
		warn("WDIOC_GETPRETIMEOUT");
	else
		printf("  GETPRETIMEOUT : %14lld sbt = %8.3f s\n",
		    (long long)v, SBT_S(v));

	/* The decisive one. */
	v = 0;
	if (ioctl(fd, WDIOC_GETTIMELEFT, &v) != 0) {
		if (errno == ENOENT) {
			printf("  GETTIMELEFT   : ENOENT -> wd_lastpat_valid "
			    "== 0\n");
			printf("                  NO hardware watchdog handler "
			    "has accepted a pat since boot.\n");
			printf("                  The dog is NOT being "
			    "serviced.\n");
			close(fd);
			exit(1);
		}
		err(1, "WDIOC_GETTIMELEFT");
	}
	printf("  GETTIMELEFT   : %14lld sbt = %8.3f s SINCE the last "
	    "successful pat (not remaining)\n", (long long)v, SBT_S(v));

	close(fd);
}

int
main(int argc, char **argv)
{
	int n, i, iv;

	/*
	 * wd_lastpat is a whole-second CLOCK_MONOTONIC_FAST stamp, so the
	 * reported gap has 1 s resolution. Sampling across one nap interval
	 * shows the sawtooth and proves the cadence; a single sample does not.
	 */
	n = (argc > 1) ? atoi(argv[1]) : 1;
	iv = (argc > 2) ? atoi(argv[2]) : 10;

	for (i = 0; i < n; i++) {
		printf("sample %d / %d\n", i + 1, n);
		sample();
		if (i + 1 < n)
			sleep(iv);
	}
	return (0);
}
