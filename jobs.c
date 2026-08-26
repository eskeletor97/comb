/* comb - view management: layout, cursor anchoring, batched
 filter/search scans, edit debouncing */

#define _GNU_SOURCE
#include "comb.h"

#include <stdlib.h>
#include <string.h>

/* jobs-private bookkeeping: the pending-pattern details nobody else reads */
static int js_extend;		/* search extends a literal prefix */
static int jf_clearing, jf_was_filtered;	/* commit-time cursor anchoring */
static size_t jf_was, jf_was_row;
static char search_prev[MAX_QUERY];	/* last committed search text */

/* swap a finished candidate into its committed slot. Ownership of src->re
 * moves to dst; clearing src->is_re marks that so no later regfree of the
 * pending spec can ever touch the same regex twice. */
static void install_pattern(Pat *dst, Pat *src)
{
	if (dst->is_re)
		regfree(&dst->re);
	dst->is_re = src->is_re;
	dst->is_alt = src->is_alt;
	if (src->is_re)
		dst->re = src->re;
	else if (src->is_alt)
		dst->alt = src->alt;
	else
		dst->lit = src->lit;
	src->is_re = src->is_alt = 0;
	snprintf(dst->text, sizeof dst->text, "%s", src->text);
}

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
			size_t hr = line_rows(view_at(i));
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
		acc += line_rows(view_at(i++));
	if (acc >= vis)
		return;
	acc = 0;
	i = nv;
	while (i > 0) {
		size_t hr = line_rows(view_at(i - 1));
		if (acc + hr > vis)
			break;
		i--;
		acc += hr;
	}
	top = i;
}

/* first view slot holding a line index >= line; view[] is sorted (and
 * the identity view is trivially sorted, so it is just line clamped). */
static size_t view_floor(size_t line)
{
	if (!view)
		return line > nv ? nv : line;
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
 * The visible state -- view[], the hit bits, committed patterns -- is
 * untouched until commit: results accumulate in scratch buffers and the
 * new pattern waits in the pending specs above. Cancelling therefore
 * means just freeing scratch; small files still finish inside their
 * first lap, indistinguishable from the old blocking pass. */
enum { K_VIEW_REPLACE, K_VIEW_NARROW, K_VIEW_EXTEND, K_SEARCH };
static int job_kind;
static int job_inv;	/* captured at start: immune to mid-job inv flips */
static size_t job_lo, job_pos, job_end;	/* progress over [lo,end) */
static uint64_t job_t0;	/* job start, for the progress grace period */
static unsigned job_spin;	/* spinner frame counter */
static uint64_t job_spin_ms;	/* last spinner draw, throttled by JOB_SPIN_MS */
static size_t *job_arr;		/* view collector, kept across batches */
static size_t job_n, job_cap;
static unsigned char *job_hits;	/* search sweep scratch */


/* put the prompt text back to the last committed query: a cancelled
 * attempt must not leave its rejected input in the buffer */
static void job_restore_edit(void)
{
	if (!editing)
		return;
	snprintf(edit, sizeof edit, "%s",
		 editing_search ? search_pat.text : filter_pat.text);
	ecur = strlen(edit);
	pending_update = 0;
}

/* drop an in-flight scan without touching anything visible */
void job_discard(void)
{
	if (!job_active)
		return;
	job_spin_ms = 0;	/* a fresh job may redraw its first frame now */
	if (pending_pat.active && pending_pat.is_re)
		regfree(&pending_pat.re);
	/* clear ownership alongside active: job_finish(0) regfrees on is_re
	 * alone, so a stale flag here would double-free the same regex_t */
	pending_pat.active = 0;
	pending_pat.is_re = 0;
	pending_pat.is_alt = 0;
	job_active = 0;
	free(job_arr);
	job_arr = NULL;
	job_n = job_cap = 0;
	free(job_hits);
	job_hits = NULL;
}

/* per-line half of the highlight-search sweep: results land in hits[]
 * and reach hit_bit only at commit. Extending a literal can only turn
 * hits off, so lines already unmarked in the committed bits skip it. */
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
		if (c->on && !(c->extend && !hit_at(i))) {
			size_t len;
			const char *s = lt_text(i, &len);
			v = (unsigned char)!!search_match(s, len, &sm);
		}
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
	pending_pat.active = 0;		/* matchers fall back to committed specs */
	if (!commit && pending_pat.is_re) {	/* cancelled candidate: unowned now */
		regfree(&pending_pat.re);
		pending_pat.is_re = 0;
	}
	if (kind == K_SEARCH) {
		if (commit) {
			install_pattern(&search_pat, &pending_pat);
			search_pat.active = pending_pat.enable_on_commit;
			snprintf(search_prev, sizeof search_prev, "%s",
				 pending_pat.text);
			for (i = 0; i < job_end && i < nlines; i++)
				set_hit(i, job_hits[i]);
		} else {
			/* lines pushed while the job ran were marked with the
			 * pending pattern; re-mark them under the restored one */
			for (i = job_end; i < nlines; i++) {
				regmatch_t sm;
				size_t len;
				const char *s = lt_text(i, &len);
				set_hit(i, search_match(s, len, &sm));
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
			if (job_n) { /* zero matches leaves both buffers NULL: skip */
				memcpy(view + nv, job_arr, job_n * sizeof(*view));
			}
			nv += job_n;
		} else {
			job_restore_edit();
		}
	} else {	/* K_VIEW_REPLACE / K_VIEW_NARROW */
		if (commit) {
			if (!*pending_pat.text) {	/* clearing: unfiltered identity */
				if (filter_pat.is_re)
					regfree(&filter_pat.re);
				memset(&filter_pat, 0,
				       sizeof filter_pat);
				filter_inv = 0;
				view = NULL;	/* huge unfiltered logs use the identity view */
				vcap = 0;
				nv = nlines;
			} else {
				/* K_VIEW_NARROW must install the pattern too:
				 * its literal extends the committed one, and leaving
				 * filter_pat at the earlier full-scan commit would
				 * desync the status query, highlight spans and
				 * follow-extend against the narrowed view. */
				install_pattern(&filter_pat,
						&pending_pat);
				filter_pat.active = 1;
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
			}
			/* cursor anchoring, identical to the blocking path:
			 * clear returns to the pre-filter selection, edits
			 * re-anchor to the nearest line in file order */
			if (jf_clearing && jf_was_filtered &&
			    filter_anchor < nlines) {
				size_t lo = view_floor(filter_anchor);
				if (lo < nv && view_at(lo) == filter_anchor) {
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
	view_epoch++;	/* view membership / hit bits just changed */
}

/* spinner chip text for render.c's chip zone; NULL inside the grace
 * period (fast scans must not flash it) or when no job is running.
 * Advances the frame per call: called once per rendered frame. */
const char *job_spin_text(void)
{
	static char buf[80];
	if (!job_active || now_ms() - job_t0 < JOB_PROG_MS)
		return NULL;
	static const char frames[] = "|/-\\";
	snprintf(buf, sizeof buf, "%c %s %d%%  esc bails",
		 frames[job_spin++ & 3],
		 job_kind == K_SEARCH ? "searching" : "filtering",
		 (int)((job_pos - job_lo) * 100 /
		       (job_end - job_lo ? job_end - job_lo : 1)));
	return buf;
}

/* run one batch through the thread pool; requests a repaint for the
 * spinner chip and commits when the range ends */
void step_job(void)
{
	size_t hi = job_end - job_pos > JOB_BATCH
			    ? job_pos + JOB_BATCH : job_end;
	if (job_kind == K_SEARCH) {
		sjob_ctx c = { job_hits, pending_pat.active &&
				       pending_pat.enable_on_commit, js_extend };
		par_run(job_pos, hi, par_threads(hi - job_pos),
			search_job_work, &c);
	} else {
		scan_collect(job_pos, hi,
			     job_kind == K_VIEW_NARROW ? pos_view : pos_ident,
			     query_match, job_inv, &job_arr, &job_n, &job_cap);
	}
	job_pos = hi;
	uint64_t now = now_ms();
	if (now - job_t0 >= JOB_PROG_MS && now - job_spin_ms >= JOB_SPIN_MS) {
		job_spin_ms = now;
		paint_chips();	/* main loop is busy feeding batches: no render() */
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
	pending_pat.active = 0;		/* workers test the committed pattern */
	job_kind = K_VIEW_EXTEND;
	job_inv = filter_inv;
	job_lo = from;
	job_pos = from;
	job_end = nlines;
	job_n = 0;
	job_t0 = now_ms();
	job_active = 1;
}

void update_filter(const char *q)
{
	int was_filtered = filter_pat.active, clearing = !*q;
	int was_literal = filter_pat.active && !filter_pat.is_re &&
			  !filter_pat.is_alt;
	char prev[sizeof filter_pat.text];
	snprintf(prev, sizeof prev, "%s", filter_pat.text);
	size_t was = nv ? view_at(cur) : 0;
	size_t was_row = cur - top;
	msg[0] = 0;

	job_discard();	/* any in-flight scan just went obsolete */

	pending_pat.active = pending_pat.enable_on_commit = 0;
	pending_pat.is_re = pending_pat.is_alt = 0;
	if (!clearing) {
		int icase = smart_case(q);
		int is_alt = re_mode && alt_parse(q, icase, &pending_pat.alt);
		if (re_mode && !is_alt) {
			if (regcomp(&pending_pat.re, q,
					    REG_EXTENDED | (icase ? REG_ICASE : 0))) {
				snprintf(msg, sizeof msg, "bad regex: %.100s", q);
				return;
			}
			pending_pat.is_re = 1;
		} else if (is_alt) {
			pending_pat.is_alt = 1;
		} else {
			lit_parse(q, icase, &pending_pat.lit);
		}
	}
	/* a clearing job parses the empty query as an empty literal: every
	 * line matches again with an empty highlight span, exactly like the
	 * committed no-filter state answers */
	else
		lit_parse("", 1, &pending_pat.lit);
	pending_pat.active = 1;

	/* query grew by appended chars: old matches are a superset, so
	 * re-testing just view[] suffices -- but only while including.
	 * Inverted, shrinking matches make outside lines eligible.
	 * A trailing '$' breaks the superset rule: "err$" anchors to EOL,
	 * appending flips that '$' to a literal, so lines that never matched
	 * the old view can now match. Re-scan everything in that case. */
	size_t prevlen = strlen(prev);
	int narrow = was_literal && !re_mode && !clearing && !filter_inv &&
	    !filter_pat.lit.eol && prevlen &&
	    strlen(q) > prevlen && !memcmp(q, prev, prevlen);

	/* commit-time cursor anchoring data (see job_finish) */
	jf_clearing = clearing;
	jf_was_filtered = was_filtered;
	jf_was = was;
	jf_was_row = was_row;
	snprintf(pending_pat.text, sizeof pending_pat.text, "%s", q);

	job_kind = narrow ? K_VIEW_NARROW : K_VIEW_REPLACE;
	/* a clearing job must collect every line: inversion belonged to the
	 * filter being removed, and match-all against inv=1 would keep nothing.
	 * But the result is just the identity view, so skip the scan entirely. */
	job_inv = clearing ? 0 : filter_inv;
	job_lo = 0;
	job_pos = 0;
	job_end = clearing ? 0 : (narrow ? nv : nlines);
	job_n = 0;
	job_t0 = now_ms();
	job_active = 1;
}

void extend_view(size_t from)
{
	/* no filter: the view is the identity range over every line, so it
	 * is O(1) -- no scan, no array. */
	if (!filter_pat.active) {
		nv = nlines;
		view = NULL;
		vcap = 0;
		view_epoch++;
		return;
	}
	scan_collect(from, nlines, pos_ident, query_match, filter_inv,
		     &view, &nv, &vcap);
	view_epoch++;
}

void rebuild_view(void)
{
	job_discard();	/* a reload/rotation invalidates scratch refs */
	nv = 0;
	extend_view(0);
	ensure_visible();
}

/* commit a highlight-search pattern: validate, then scan as a batched
 * job (see step_job). Extending a literal pattern can only turn hits
 * off, so lines already marked false are skipped -- typing stays cheap
 * on huge files. */
void update_search(const char *q)
{
	int icase = smart_case(q);
	size_t prevlen = strlen(search_prev);
	/* same superset caveat as update_filter: a trailing '$' in the prior
	 * literal would turn literal on append and add new hits, which the
	 * skip-already-false shortcut would wrongly drop. */
	int extend = search_pat.active && !search_pat.is_re && !re_mode &&
		     !search_pat.lit.eol && prevlen &&
		     strlen(q) > prevlen && !memcmp(q, search_prev, prevlen);
	msg[0] = 0;

	job_discard();	/* any in-flight sweep just went obsolete */

	pending_pat.active = pending_pat.enable_on_commit = 0;
	pending_pat.is_re = pending_pat.is_alt = 0;
	if (*q && re_mode) {
		if (alt_parse(q, icase, &pending_pat.alt)) {
			pending_pat.is_alt = 1;	/* compile before swapping: a bad regex
						 * must keep the old search */
		} else if (regcomp(&pending_pat.re, q,
				   REG_EXTENDED | (icase ? REG_ICASE : 0))) {
			snprintf(msg, sizeof msg, "bad regex: %.100s", q);
			return;	/* keep the old search */
		} else {
			pending_pat.is_re = 1;
		}
	} else if (*q) {
		lit_parse(q, icase, &pending_pat.lit);
	}
	pending_pat.active = 1;		/* workers route here while flying */
	pending_pat.enable_on_commit = !!*q;
	js_extend = extend;
	snprintf(pending_pat.text, sizeof pending_pat.text, "%s", q);

	free(job_hits);
	job_hits = xrealloc(NULL, nlines ? nlines : 1);
	job_kind = K_SEARCH;
	job_lo = 0;
	job_pos = 0;
	job_end = nlines;
	job_t0 = now_ms();
	job_active = 1;
}

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
