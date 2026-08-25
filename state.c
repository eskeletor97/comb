/* comb - shared viewer state.
 * The one place that defines the cross-module globals declared in
 * comb.h; each module mutates the slices it owns. */
#define _GNU_SOURCE
#include "comb.h"

#include <unistd.h>

Line *lines;
size_t nlines, lcap;

size_t *view;
size_t nv, vcap;

char path[4096];
int use_stdin;
int fd = -1;	/* log file */
int kfd = 0;	/* keyboard: stdin, or /dev/tty when stdin isn't a tty */
off_t fsize;
int stdin_eof;	/* pipe closed: no more input will ever come */

/* Zero-copy window onto a regular file: clean lines point into the
 * mapping instead of owning an arena copy. */
char *fmap;
size_t fmap_len, fmap_pos;

/* running max of line widths; maintained at push time, see push_line.
 * reset_lines rewinds it alongside the lines themselves. */
size_t wc_max;

Pat filter_pat;
Pat search_pat;

/* candidate for in-flight scans; the UI keeps serving the last committed
 * view/pattern until the job lands (see step_job), so Esc can drop a
 * mistyped query wholesale. Workers test the pending spec; n/N,
 * push_line marking and the status bar stay on the committed ones. */
Pat pending_pat;

int re_mode;		/* UI toggle for the next filter: literal vs ERE */
int filter_inv;		/* live filter excludes matching lines */
char edit[MAX_QUERY];
int editing;
size_t ecur;	/* insertion point: byte offset into edit[] */
int editing_search;	/* prompt currently edits search, not filter */

int follow = 1;
int wrap;
int nocolor;
const char *mark_bg = MARK_BG;
int running = 1;
int dirty = 1;

size_t cur, top;
size_t filter_anchor;	/* line selected when filtering began */
size_t filter_row;	/* its screen row, restored on clear */
int hscroll;
int rows = 24, cols = 80;

char msg[160];
struct termios saved_tio;
int tio_saved;
volatile sig_atomic_t got_winch;
size_t nmarked;

int pending_update;	/* a query change awaits the debounce */
uint64_t debounce_due;	/* deadline for the deferred recompute */
int force_render;	/* commit a queued-keys coalescing render */

int max_threads;	/* runtime -t/--threads; 0 = auto */
uint64_t view_epoch;	/* bumped on view[]/srchit changes; see comb.h */

/* Worker count: the -t override, else nproc-2 (leaving a couple of cores
 * for the UI/main loop), clamped to [1, MAX_THREADS]. */
int effective_threads(void)
{
	if (max_threads > 0)
		return max_threads;
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	if (n < 1)
		n = 1;
	n = n > 2 ? n - 2 : 1;
	if (n > MAX_THREADS)
		n = MAX_THREADS;
	return (int)n;
}

uint64_t prog_t0;	/* set in main, right before load_all() */
size_t prog_fed;	/* bytes handed to feed() */

/* long-scan jobs (engine lives in jobs.c, driven from the main loop) */
int job_active;
int pend_ext;		/* follow append deferred past the job */
size_t pend_ext_from;
