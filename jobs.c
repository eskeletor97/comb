/* comb - view management: layout, cursor anchoring, batched
 filter/search scans, edit debouncing */

#define _GNU_SOURCE
#include "comb.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* jobs-private bookkeeping: the pending-pattern details nobody else reads */
static int js_extend;		/* search extends a literal prefix */
static int jf_clearing, jf_was_filtered;	/* commit-time cursor anchoring */
static size_t jf_was, jf_was_row;
static char jf_q[MAX_QUERY], js_q[MAX_QUERY];
static char search_prev[MAX_QUERY];	/* last committed search text */

/* layout: log pane, then status bar (source/position/keys), then the
 * command line -- everything worth looking at sits at the bottom, near
 * the eye's resting point. Tiny ttys drop the command line first, then
 * the status bar */
int have_status_bar(void)
{
	return rows >= 2;
}

int have_input_bar(void)
{
	return rows >= 3;
}

size_t pane_rows(void)
{
	int n = rows - have_status_bar() - have_input_bar();
	return (size_t)(n > 0 ? n : 1);
}


void ensure_visible(void)
{
	size_t vis = pane_rows();
	if (cur >= nv)
		cur = nv ? nv - 1 : 0;
	if (cur < top)
		top = cur;
	else if (wrap) {
		/* lowest window that fits cur: walk up from cur while the
		 * accumulated rows fit the pane */
		size_t acc = 0, i = cur;
		while (i > top) {
			size_t hr = line_rows(&lines[view[i]]);
			if (acc + hr > vis) {
				if (i < cur)
					i++;   /* exclude the line that broke the budget */
				break; /* else: line is taller than the pane, pin it */
			}
			acc += hr;
			i--;
		}
		top = i;
	} else if (cur >= top + vis) {
		top = cur - vis + 1;
	}
	/* the list shrank (filter/reload/rotation): don't leave the
	 * viewport stranded past the last line -- end-anchor it instead */
	if (!wrap) {
		if (top + vis > nv)
			top = nv > vis ? nv - vis : 0;
		return;
	}
	/* wrap: same anchor, in rows. Early-out when the tail fills the pane. */
	size_t acc = 0, i = top;
	while (i < nv && acc < vis)
		acc += line_rows(&lines[view[i++]]);
	if (acc >= vis)
		return;
	acc = 0;
	i = nv;
	while (i > 0) {
		size_t hr = line_rows(&lines[view[i - 1]]);
		if (acc + hr > vis)
			break;
		i--;
		acc += hr;
	}
	top = i;
}

/* first view slot holding a line index >= line; view[] is sorted */
static size_t view_floor(size_t line)
{
	size_t lo = 0, hi = nv;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (view[mid] < line)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}



/* --- long-scan jobs -------------------------------------------------
 * On a 200M-line log a mistyped regex costs minutes, so filter and
 * search rescans don't run as one blocking pass: each main-loop lap
 * steps one batch through the thread pool, then control returns so keys
 * are read (Esc bails between batches) and a spinner is painted.
 *
 * The visible state -- view[], srchit flags, committed patterns -- is
 * untouched until commit: results accumulate in scratch buffers and the
 * new pattern waits in the pending specs above. Cancelling therefore
 * means just freeing scratch; small files still finish inside their
 * first lap, indistinguishable from the old blocking pass. */
enum { K_VIEW_REPLACE, K_VIEW_NARROW, K_VIEW_EXTEND, K_SEARCH };
static int job_kind;
static int job_inv;	/* captured at start: immune to mid-job inv flips */
static size_t job_lo, job_pos, job_end;	/* progress over [lo,end) */
static unsigned job_spin;	/* spinner frame counter */
static size_t *job_arr;		/* view collector, kept across batches */
static size_t job_n, job_cap;
static unsigned char *job_hits;	/* search sweep scratch */


/* put the prompt text back to the last committed query: a cancelled
 * attempt must not leave its rejected input in the buffer */
static void job_restore_edit(void)
{
	if (!editing)
		return;
	snprintf(edit, sizeof edit, "%s", editing_search ? search : query);
	ecur = strlen(edit);
	pending_update = 0;
}

/* drop an in-flight scan without touching anything visible */
void job_discard(void)
{
	if (!job_active)
		return;
	if (jf_on && jf_re)
		regfree(&jfre);
	if (js_on && js_re)
		regfree(&jsre);
	jf_on = js_on = 0;
	job_active = 0;
	free(job_arr);
	job_arr = NULL;
	job_n = job_cap = 0;
	free(job_hits);
	job_hits = NULL;
}

/* per-line half of the highlight-search sweep: results land in hits[]
 * and reach lines[].srchit only at commit. Extending a literal can only
 * turn hits off, so lines already marked false skip the match test. */
typedef struct {
	unsigned char *hits;
	int on, extend;
} sjob_ctx;

static void search_job_work(size_t lo, size_t hi, int slot, void *ctx)
{
	sjob_ctx *c = ctx;
	(void)slot;
	regmatch_t sm;
	for (size_t i = lo; i < hi; i++) {
		unsigned char v = 0;
		if (c->on && !(c->extend && !lines[i].srchit))
			v = (unsigned char)!!search_match(&lines[i], &sm);
		c->hits[i] = v;
	}
}

/* apply a finished scan (commit) or drop it (cancel). Everything swaps
 * in atomically here, which is what makes mid-job cancel cheap. */
void job_finish(int commit)
{
	int kind = job_kind;
	size_t i;
	job_active = 0;
	jf_on = js_on = 0;	/* matchers fall back to committed patterns */
	if (kind == K_SEARCH) {
		if (commit) {
			if (js_re) {
				regex_t old;
				if (searched_re) {
					old = sre;
					sre = jsre;
					regfree(&old);
				} else {
					sre = jsre;
				}
				searched_re = 1;
				searched_alt = 0;
			} else if (js_alt) {
				if (searched_re)
					regfree(&sre);
				salt = jsalt;
				searched_alt = 1;
				searched_re = 0;
			} else {
				if (searched_re)
					regfree(&sre);
				slit = jslit;
				searched_re = searched_alt = 0;
			}
			searched = js_active;
			snprintf(search, sizeof search, "%s", js_q);
			snprintf(search_prev, sizeof search_prev, "%s", js_q);
			for (i = 0; i < job_end && i < nlines; i++)
				lines[i].srchit = job_hits[i];
		} else {
			/* lines pushed while the job ran were marked with the
			 * pending pattern; re-mark them under the restored one */
			for (i = job_end; i < nlines; i++) {
				regmatch_t sm;
				lines[i].srchit = (unsigned char)
					(searched && search_match(&lines[i], &sm));
			}
			job_restore_edit();
		}
	} else if (kind == K_VIEW_EXTEND) {
		if (commit) {
			if (nv + job_n > vcap) {
				size_t nc = vcap ? vcap : 1024;
				while (nc < nv + job_n)
					nc *= 2;
				vcap = nc;
				view = xrealloc(view, vcap * sizeof(*view));
			}
            if (job_n)   /* zero matches leaves both buffers NULL: skip */
			memcpy(view + nv, job_arr, job_n * sizeof(*view));
			nv += job_n;
		} else {
			job_restore_edit();
		}
	} else {	/* K_VIEW_REPLACE / K_VIEW_NARROW */
		if (commit) {
			if (kind == K_VIEW_REPLACE) {
				if (jf_all) {	/* clearing: back to unfiltered */
					if (filtered && filtered_re)
						regfree(&re);
					filtered_re = 0;
					filtered_alt = 0;
					filtered = 0;
					filter_inv = 0;
					query[0] = 0;
					lit.len = 0;
				} else {
					if (jf_re) {
						regex_t old;
						if (filtered && filtered_re) {
							old = re;
							re = jfre;
							regfree(&old);
						} else {
							re = jfre;
						}
						filtered_re = 1;
						filtered_alt = 0;
					} else if (jf_alt) {
						if (filtered && filtered_re)
							regfree(&re);
						alt = jfalt;
						filtered_alt = 1;
						filtered_re = 0;
					} else {
						if (filtered && filtered_re)
							regfree(&re);
						lit = jflit;
						filtered_re = 0;
						filtered_alt = 0;
					}
					filtered = 1;
					snprintf(query, sizeof query, "%s", jf_q);
				}
			}
			if (vcap < job_n) {
				size_t nc = vcap ? vcap : 1024;
				while (nc < job_n)
					nc *= 2;
				vcap = nc;
				view = xrealloc(view, vcap * sizeof(*view));
			}
			if (job_n)   /* zero matches leaves both buffers NULL: skip */
			memcpy(view, job_arr, job_n * sizeof(*view));
			nv = job_n;
			/* cursor anchoring, identical to the blocking path:
			 * clear returns to the pre-filter selection, edits
			 * re-anchor to the nearest line in file order */
			if (jf_clearing && jf_was_filtered &&
			    filter_anchor < nlines) {
				size_t lo = view_floor(filter_anchor);
				if (lo < nv && view[lo] == filter_anchor) {
					cur = lo;
					size_t vis = pane_rows();
					size_t max_top = nv > vis ? nv - vis : 0;
					top = lo > filter_row ? lo - filter_row : 0;
					if (top > max_top)
						top = max_top;
				}
			} else if (!jf_clearing) {
				cur = view_floor(jf_was);
				size_t vis = pane_rows();
				size_t max_top = nv > vis ? nv - vis : 0;
				top = cur > jf_was_row ? cur - jf_was_row : 0;
				if (top > max_top)
					top = max_top;
			}
			ensure_visible();
		} else {
			job_restore_edit();
		}
	}
	free(job_arr);
	job_arr = NULL;
	job_n = job_cap = 0;
	free(job_hits);
	job_hits = NULL;
	if (pend_ext) {	/* tail appended during the scan: pick it up now */
		pend_ext = 0;
		if (pend_ext_from < nlines)
			extend_view(pend_ext_from);
	}
	dirty = 1;
}

/* run one batch through the thread pool; paints the spinner and commits
 * when the range ends */
void step_job(void)
{
	size_t hi = job_end - job_pos > JOB_BATCH
			    ? job_pos + JOB_BATCH : job_end;
	if (job_kind == K_SEARCH) {
		sjob_ctx c = { job_hits, js_on && js_active, js_extend };
		par_run(job_pos, hi, par_threads(hi - job_pos),
			search_job_work, &c);
	} else {
		scan_collect(job_pos, hi,
			     job_kind == K_VIEW_NARROW ? pos_view : pos_ident,
			     query_match, job_inv, &job_arr, &job_n, &job_cap);
	}
	job_pos = hi;
	if (rows >= 2) {
		static const char frames[] = "|/-\\";
		const char *what = job_kind == K_SEARCH ? "searching" : "filtering";
		int pct = (int)((job_pos - job_lo) * 100 /
				(job_end - job_lo ? job_end - job_lo : 1));
		char buf[80];
		snprintf(buf, sizeof buf, "%c %s %d%%  esc bails",
			 frames[job_spin++ & 3], what, pct);
		printf("\x1b[%d;1H\x1b[K\x1b[1;7m %s \x1b[0m", rows, buf);
		fflush(stdout);
	}
	if (job_pos >= job_end)
		job_finish(1);
}

/* synchronous completion for callers outside the event loop (init,
 * tests): run batches back-to-back until the job lands */
void job_flush(void)
{
	while (job_active)
		step_job();
}

/* follow pushed new lines while filtered: collect their membership as a
 * job too, so a huge tail can't stall the loop */
void start_extend_view(size_t from)
{
	if (from >= nlines)
		return;
	job_discard();
	jf_on = 0;		/* workers test the committed pattern */
	job_kind = K_VIEW_EXTEND;
	job_inv = filter_inv;
	job_lo = from;
	job_pos = from;
	job_end = nlines;
	job_n = 0;
	job_active = 1;
}

void update_filter(const char *q)
{
	int was_filtered = filtered, clearing = !*q;
	int was_literal = filtered && !filtered_re && !filtered_alt;
	char prev[sizeof query];
	snprintf(prev, sizeof prev, "%s", query);
	size_t was = nv ? view[cur] : 0;
	size_t was_row = cur - top;
	msg[0] = 0;

	job_discard();	/* any in-flight scan just went obsolete */

	jf_on = jf_all = jf_re = jf_alt = 0;
	if (clearing) {
		jf_on = jf_all = 1;	/* pending: every line matches again */
	} else {
		int icase = smart_case(q);
		int is_alt = re_mode && alt_parse(q, icase, &jfalt);
		if (re_mode && !is_alt) {
			if (regcomp(&jfre, q, REG_EXTENDED | (icase ? REG_ICASE : 0))) {
				snprintf(msg, sizeof msg, "bad regex: %.100s", q);
				return;
			}
			jf_re = 1;
		} else if (is_alt) {
			jf_alt = 1;
		} else {
			lit_parse(q, icase, &jflit);
		}
		jf_on = 1;
	}

	/* query grew by appended chars: old matches are a superset, so
	 * re-testing just view[] suffices -- but only while including.
	 * Inverted, shrinking matches make outside lines eligible. */
	size_t prevlen = strlen(prev);
	int narrow = was_literal && !re_mode && !clearing && !filter_inv && prevlen &&
	    strlen(q) > prevlen && !memcmp(q, prev, prevlen);

	/* commit-time cursor anchoring data (see job_finish) */
	jf_clearing = clearing;
	jf_was_filtered = was_filtered;
	jf_was = was;
	jf_was_row = was_row;
	snprintf(jf_q, sizeof jf_q, "%s", q);

	job_kind = narrow ? K_VIEW_NARROW : K_VIEW_REPLACE;
	job_inv = filter_inv;
	job_lo = 0;
	job_pos = 0;
	job_end = narrow ? nv : nlines;
	job_n = 0;
	job_active = 1;
}

void extend_view(size_t from)
{
	scan_collect(from, nlines, pos_ident, query_match, filter_inv,
		     &view, &nv, &vcap);
}

void rebuild_view(void)
{
	job_discard();	/* a reload/rotation invalidates scratch refs */
	nv = 0;
	extend_view(0);
	ensure_visible();
}

/* does the highlight-search pattern hit this line? fills m with the span.
 * While a search job is in flight the pending pattern answers: workers
 * must test the new query while n/N and the scrollbar keep serving the
 * previous results until commit. */
int search_match(const Line *L, regmatch_t *m)
{
	int on, is_re, is_alt;
	const regex_t *rep;
	const LitSpec *lp;
	const AltSpec *ap;
	if (js_on) {
		on = js_active;
		is_re = js_re;
		is_alt = js_alt;
		rep = &jsre;
		lp = &jslit;
		ap = &jsalt;
	} else {
		on = searched;
		is_re = searched_re;
		is_alt = searched_alt;
		rep = &sre;
		lp = &slit;
		ap = &salt;
	}
	if (!on)
		return 0;
	/* a zero-width regex match is not a hit: n/N and the scrollbar need
	 * a real span to land on */
	return pattern_match(L, is_re, is_alt, rep, lp, ap, m) &&
	       (!is_re || m->rm_eo > m->rm_so);
}

/* commit a highlight-search pattern: validate, then scan as a batched
 * job (see step_job). Extending a literal pattern can only turn hits
 * off, so lines already marked false are skipped -- typing stays cheap
 * on huge files. */
void update_search(const char *q)
{
	int icase = smart_case(q);
	size_t prevlen = strlen(search_prev);
	int extend = searched && !searched_re && !re_mode && prevlen &&
		     strlen(q) > prevlen && !memcmp(q, search_prev, prevlen);
	msg[0] = 0;

	job_discard();	/* any in-flight sweep just went obsolete */

	js_on = 1;
	js_active = !!*q;
	js_extend = extend;
	js_re = js_alt = 0;
	if (*q && re_mode) {
		if (alt_parse(q, icase, &jsalt)) {
			js_alt = 1;	/* compile before swapping: a bad regex
					 must keep the old search */
		} else if (regcomp(&jsre, q,
				   REG_EXTENDED | (icase ? REG_ICASE : 0))) {
			snprintf(msg, sizeof msg, "bad regex: %.100s", q);
			js_on = 0;
			return;	/* keep the old search */
		} else {
			js_re = 1;
		}
	} else if (*q) {
		lit_parse(q, icase, &jslit);
	}
	snprintf(js_q, sizeof js_q, "%s", q);

	free(job_hits);
	job_hits = xrealloc(NULL, nlines ? nlines : 1);
	job_kind = K_SEARCH;
	job_lo = 0;
	job_pos = 0;
	job_end = nlines;
	job_active = 1;
}

/* schedule the recompute for when typing idles; keeps the live buffered
 * query but defers the heavy per-line pass so a keystroke burst costs one */
void defer_update(void)
{
	edit[MAX_QUERY - 1] = 0;
	job_discard();	/* keystrokes obsolete any in-flight scan */
	pending_update = 1;
	debounce_due = now_ms() + SEARCH_DEBOUNCE_MS;
}

void apply_edit(void)
{
	edit[MAX_QUERY - 1] = 0;
	pending_update = 0;
	if (editing_search)
		update_search(edit);
	else
		update_filter(edit);
	dirty = 1;	/* update_filter doesn't self-mark; a commit must repaint */
}

int debounce_elapsed(void)
{
	return now_ms() >= debounce_due;
}

int debounce_ms_left(void)
{
	uint64_t left = debounce_due - now_ms();
	return left > 0 ? (int)left : 0;
}
