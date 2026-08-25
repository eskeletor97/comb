/* comb - a small terminal log viewer: highlight, filter, follow, copy.
 * build: see Makefile */

#define _GNU_SOURCE
#include "comb.h"

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* parse a thread count into *out, clamping to [1, MAX_THREADS]. Rejects
 * anything that isn't a clean integer (so -t bogus errors, -t 0 clamps to 1). */
static int parse_threads(const char *s, int *out)
{
	char *end;
	long v = strtol(s, &end, 10);
	if (end == s || *end)
		return 0;
	if (v < 1)
		v = 1;
	*out = v > MAX_THREADS ? MAX_THREADS : (int)v;
	return 1;
}

int main(int argc, char **argv)
{
	const char *init_re = NULL;
	const char *file = NULL;

	int endopts = 0;
	for (int i = 1; i < argc; i++) {
		if (!endopts && !strcmp(argv[i], "--")) {
			endopts = 1;
		} else if (!endopts && !strcmp(argv[i], "-e") && i + 1 < argc) {
			init_re = argv[++i];
		} else if (!endopts &&
			   (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))) {
			usage(stdout);
			return 0;
		} else if (!endopts &&
			   (!strcmp(argv[i], "--no-color") || !strcmp(argv[i], "-C"))) {
			nocolor = 1;
		} else if (!endopts &&
			   (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--threads"))) {
			if (i + 1 >= argc || !parse_threads(argv[++i], &max_threads)) {
				usage(stderr);
				return 1;
			}
		} else if (!endopts && !strncmp(argv[i], "--threads=", 10)) {
			if (!parse_threads(argv[i] + 10, &max_threads)) {
				usage(stderr);
				return 1;
			}
		} else if (!endopts && !strcmp(argv[i], "-")) {
			use_stdin = 1;
		} else if (argv[i][0] == '-') {
			usage(stderr);
			return 1;
		} else {
			file = argv[i];
		}
	}
	if (!file && !use_stdin && !isatty(STDIN_FILENO))
		use_stdin = 1;	/* piped or redirected: no need for an explicit - */
	if (!file && !use_stdin) {
		usage(stderr);
		return 1;
	}
	if (file) {
		if (snprintf(path, sizeof path, "%s", file) >= (int)sizeof path)
			die("path too long");
	}

	if (!isatty(STDIN_FILENO)) {
		/* keys must not come from piped/redirected data */
		int t = open("/dev/tty", O_RDONLY);
		if (t >= 0) {
			kfd = t;
		} else if (use_stdin) {
			die("stdin is not interactive and /dev/tty unavailable");
		}
	}

	if (getenv("NO_COLOR"))
		nocolor = 1;
	mark_bg = nocolor ? "\x1b[7m" : MARK_BG;

	if (use_stdin)
		fcntl(STDIN_FILENO, F_SETFL,
		      fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);	/* auto-reap; wl-copy must outlive us */
	signal(SIGWINCH, on_winch);
	signal(SIGTERM, on_sigexit);
	signal(SIGHUP, on_sigexit);
	signal(SIGINT, on_sigexit);
	/* A privileged feeder (doas/sudo dmesg -w | comb) prompts for its
	 * password on this very tty; stay in cooked mode until the pipe
	 * produces its first byte or closes, so the prompt works. A tty on
	 * stdin has no feeder prompt to wait for. */
	if (use_stdin && !isatty(STDIN_FILENO)) {
		struct pollfd pw = { .fd = STDIN_FILENO, .events = POLLIN };
		poll(&pw, 1, -1);
	}
	tty_enter();
	prog_t0 = now_ms();
	load_all();
	prog_hide();
	{
		/* flex a little: how much landed and how fast */
		char g[32], hb[16];
		group_digits(g, nlines);
		human_bytes(hb, fmap_len + prog_fed);
		double secs = (now_ms() - prog_t0) / 1000.0;
		if (secs >= 1.0) {
			char ps[16];
			human_bytes(ps, (size_t)((fmap_len + prog_fed) / secs));
			snprintf(msg, sizeof msg,
				 "loaded %s lines (%s) in %.1fs (%s/s)",
				 g, hb, secs, ps);
		} else
			snprintf(msg, sizeof msg,
				 "loaded %s lines (%s) in %.0fms",
				 g, hb, secs * 1000);
	}

	if (init_re) {
		re_mode = 1;	/* -e promises a REGEX */
		update_filter(init_re);
		job_flush();
	} else
		rebuild_view();
	cur = nv ? nv - 1 : 0;
	ensure_visible();

	int key;
	while (running) {
		if (got_winch) {
			got_winch = 0;
			get_winsize();
			ensure_visible();
			dirty = 1;
		}
		if (follow && !(use_stdin && stdin_eof)) {
			int stick = nv > 0 && cur >= nv - 1;
			size_t old = nlines;
			int got = pump_follow();
			if (got == 2) {
				job_discard(); /* rotation: lines[] were rebuilt from scratch */
				if (filter_pat.active)
					update_filter(filter_pat.text);	/* rescan as a job */
				else
					rebuild_view();
			} else if (got == 1) {
				if (job_active) {
					if (!pend_ext || old < pend_ext_from)
						pend_ext_from = old;
					pend_ext = 1;
				} else if (filter_pat.active && nlines - old > JOB_BATCH) {
					start_extend_view(old);	/* big tail: batch it */
				} else {
					extend_view(old);
				}
			}
			if (got) {
				if (stick)
					cur = nv ? nv - 1 : 0;
				ensure_visible();
				dirty = 1;
			}
		}

		/* a deferred filter/search recompute fires once editing idles */
		if (editing && pending_update && !key_pending() && debounce_elapsed()) {
			apply_edit();
		}

		/* Long scans run in batches so typing and the bail key stay live.
		 * A batch is only a few ms of threaded work, so pausing a full poll
		 * after every batch idles the machine ~90% of the time and caps the
		 * parallel scan's speedup at ~1.5x (on a 12-core box it pegged at
		 * roughly one core's worth of CPU). Keep feeding batches while no
		 * key is ready, polling the keyboard non-blockingly between batches
		 * so Esc still bails out promptly -- but always run at least one
		 * batch per lap, so a small scan finishes before a pending key is
		 * served (read_key's Esc lookahead may buffer the next key). */
		if (job_active) {
			struct pollfd kp = { .fd = kfd, .events = POLLIN };
			do {
				step_job();
				if (!job_active || key_pending() || got_winch)
					break;
			} while (poll(&kp, 1, 0) == 0);
		}

		/* paint pending changes before waiting; skipped while
		 * keystrokes are queued so key repeats coalesce, unless a
		 * commit just landed -- that must be shown before the queue */
		if (dirty && (!key_pending() || force_render)) {
			render();
			dirty = 0;
			force_render = 0;
		}

		if (!key_pending()) {
			struct pollfd pp[2] = {
				{ .fd = kfd,	.events = POLLIN },
				{ .fd = STDIN_FILENO,	.events = POLLIN },
			};
			int np = follow && use_stdin && !stdin_eof ? 2 : 1;
			int wait = job_active ? 100 : 200;
			if (editing && pending_update) {
				int left = debounce_ms_left();
				if (left < wait)
					wait = left;
			}
			poll(pp, np, wait);
			/* woke for data (or timed out): lap around; woke for
			 * a key: fall through and read it */
			if (!(pp[0].revents & POLLIN))
				continue;
		}

		key = read_key();
		if (key == K_EOF)
			break;
		msg[0] = 0;

		/* Esc bails out of an in-flight scan first: on huge files it is
		 * the only way back to the last good view */
		if (job_active && key == 0x1b) {
			job_finish(0);
			snprintf(msg, sizeof msg, "cancelled");
			editing = 0;
			editing_search = 0;
			force_render = 1;
			continue;
		}

		if (editing)
			edit_key(key);
		else
			view_action(key_action(key));
		ensure_visible();
		dirty = 1;
	}

	restore_terminal();
	/* a live feeder would outlive us and keep the pipeline (and the
	 * shell waiting on it) alive; take down the job's group like
	 * Ctrl-C would. Best effort: root-owned feeders ignore it. */
	if (use_stdin && !stdin_eof) {
		tio_saved = 0;	/* on_sigexit must not repaint the leave sequence */
		kill(0, SIGINT);
	}
	return 0;
}
