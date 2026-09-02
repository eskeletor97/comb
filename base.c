/* comb - errors, allocation, clock, terminal setup/teardown */

#define _GNU_SOURCE
#include "comb.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

void on_winch(int sig)
{
	(void)sig;
	got_winch = 1;
}


void die(const char *fmt, ...)
{
	va_list ap;
	restore_terminal();
	va_start(ap, fmt);
	fprintf(stderr, "comb: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

/* syscall failures append strerror(errno); plain die() must not -- errno
 * is too often stale from benign calls that ignored their own failure */
void die_sys(const char *fmt, ...)
{
	int e = errno;
	va_list ap;
	restore_terminal();
	va_start(ap, fmt);
	fprintf(stderr, "comb: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, ": %s", strerror(e));
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		die("out of memory");
	return q;
}

uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void raw_on(void)
{
	if (tcgetattr(kfd, &saved_tio))
		die_sys("not a terminal");
	struct termios t = saved_tio;
	t.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT);
	t.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG | IEXTEN);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(kfd, TCSANOW, &t) < 0)
		die_sys("tcsetattr");
	tio_saved = 1;
}

void restore_terminal(void)
{
	if (!tio_saved)
		return;
	fputs(TTY_LEAVE, stdout);
	fflush(stdout);
	tcsetattr(kfd, TCSANOW, &saved_tio);
}

/* emergency terminal restore on SIGTERM/SIGHUP/SIGINT: only
 * async-signal-safe calls, unlike restore_terminal()'s stdio */
void on_sigexit(int sig)
{
	if (tio_saved) {
		tcsetattr(kfd, TCSANOW, &saved_tio);
		write(STDOUT_FILENO, TTY_LEAVE, sizeof TTY_LEAVE - 1);
	}
	_exit(128 + sig);
}

void get_winsize(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
		cols = ws.ws_col;
		rows = ws.ws_row;
	}
}

/* (re)acquire the terminal: raw mode + fresh alternate screen */
void tty_enter(void)
{
	raw_on();
	get_winsize();
	fputs(TTY_ENTER, stdout);
	fflush(stdout);
}
