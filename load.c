/* comb - line ingestion: progress display, sanitizing,
 mmap/window loading, tail follow */

#define _GNU_SOURCE
#include "comb.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__SSE2__) && defined(__GNUC__)
#define USE_SSE2 1
#include <emmintrin.h>
#else
#define USE_SSE2 0
#endif

static void detect_tag(Line *L);
static void commit_slot(Line *L);

/* Line text lives in bump-allocated ~1 MiB chunks; chunks are never
 * moved or freed individually, so Line.s and svc_seen pointers stay
 * valid until reset_lines rewinds the arena. */
#define ARENA_CHUNK ((size_t)1 << 20)
static char **achunk;
static size_t nachunk, acap, apos;
static pthread_mutex_t arena_mu = PTHREAD_MUTEX_INITIALIZER;

/* The parallel loader calls sanitize from many threads at once; the
 * bump is serialized but the actual sanitize copy happens outside the
 * lock, and clean files (the common case) never copy at all. */
static void *arena_alloc(size_t n)
{
	pthread_mutex_lock(&arena_mu);
	if (nachunk == acap) {
		acap = acap ? acap * 2 : 16;
		achunk = xrealloc(achunk, acap * sizeof(*achunk));
	}
	if (nachunk == 0 || apos + n > ARENA_CHUNK) {
		size_t cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;
		achunk[nachunk++] = xrealloc(NULL, cap);
		apos = 0;
	}
	void *p = achunk[nachunk - 1] + apos;
	apos += n;
	pthread_mutex_unlock(&arena_mu);
	return p;
}

static void arena_reset(void)
{
	for (size_t i = 0; i < nachunk; i++)
		free(achunk[i]);
	nachunk = 0;
}

/* Zero-copy window onto a regular file: clean lines point into the
 * mapping instead of owning an arena copy. */

static char *pend;
static size_t plen;
static int flushed_partial;	/* last pushed line had no trailing newline */


/* --- load progress: the only UI painted before the first render.
 * Big files take tens of seconds to scan; without this comb looks
 * hung. Regular files know their size, so they draw a percent bar and
 * tick per PROG_STEP_BYTES scanned; stdin has no total and spins on a
 * timer instead. Both are silent on small loads, so ordinary files
 * never flicker, and prog_hide erases everything before the first
 * render paints the pane. */
#define PROG_STEP_BYTES ((size_t)32 << 20)
#define PROG_MIN_FEED  ((size_t)1 << 20)
static size_t prog_mark;	/* file offset of the last bar draw */
static int prog_shown;
static int prog_spin;
static uint64_t prog_due;	/* stdin redraw deadline */

/* compact byte count: 37.3GiB / 743MiB / 9.5KiB */
void human_bytes(char *o, size_t v)
{
	if (v >> 30)
		snprintf(o, 16, "%.1fGiB", v / 1073741824.0);
	else if (v >> 20)
		snprintf(o, 16, "%.1fMiB", v / 1048576.0);
	else if (v >= 10 << 10)
		snprintf(o, 16, "%.0fKiB", v / 1024.0);
	else if (v >> 10)
		snprintf(o, 16, "%.1fKiB", v / 1024.0);
	else
		snprintf(o, 16, "%zuB", v);
}

/* decimal with , groups: 272298969 -> 272,298,969 (no locale: comb
 * never calls setlocale, so %'d is unavailable) */
void group_digits(char *o, size_t v)
{
	char d[32];
	int k = 0;
	do {
		d[k++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (k > 0) {
		*o++ = d[--k];
		if (k && k % 3 == 0)
			*o++ = ',';
	}
	*o = 0;
}

static void prog_paint(const char *text)
{
	fputs("\x1b[1;1H\x1b[2K", stdout);
	fputs(text, stdout);
	fflush(stdout);
	prog_shown = 1;
}

static double prog_secs(void)
{
	double s = (now_ms() - prog_t0) / 1000.0;
	return s < 0.001 ? 0.001 : s;
}

static void prog_file(size_t done)
{
	if (done < prog_mark || done - prog_mark < PROG_STEP_BYTES)
		return;
	prog_mark = done;
	char cur[16], tot[16], rt[16];
	human_bytes(cur, done);
	human_bytes(tot, fmap_len);
	human_bytes(rt, (size_t)(done / prog_secs()));
	char buf[192], bar[26];
	const char *bars = "";
	int bw = cols >= 50 ? 22 : cols >= 36 ? 10 : 0;
	if (bw) {
		int fill = (int)((uint64_t)done * bw / fmap_len);
		for (int i = 0; i < bw; i++)
			bar[i + 1] = i < fill ? '#' : '-';
		bar[0] = '[';
		bar[bw + 1] = ']';
		bar[bw + 2] = 0;
	}
	if (!bw)
		bar[0] = 0;
	else
		bars = " ";
	const char *name = use_stdin ? "(stdin)"
		: strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	size_t room = cols > 66 ? cols - 66 : 0;
	if (room >= 4)
		snprintf(buf, sizeof buf,
			 "loading %.*s%s%s %d%% %s/%s %s/s",
			 (int)(room > 40 ? 40 : room), name,
			 bars, bar, (int)(done * 100 / fmap_len),
			 cur, tot, rt);
	else
		snprintf(buf, sizeof buf, "loading %d%% %s/%s %s/s",
			 (int)(done * 100 / fmap_len), cur, tot, rt);
	prog_paint(buf);
}

static void prog_stdin(void)
{
	uint64_t now = now_ms();
	if (now < prog_due || prog_fed < PROG_MIN_FEED)
		return;
	prog_due = now + 100;
	char ln[32], hb[16], rt[16];
	group_digits(ln, nlines);
	human_bytes(hb, prog_fed);
	human_bytes(rt, (size_t)(prog_fed / prog_secs()));
	char buf[160];
	snprintf(buf, sizeof buf, "reading stdin %c %s lines %s %s/s",
		 "|/-\\"[prog_spin++ & 3], ln, hb, rt);
	prog_paint(buf);
}

void prog_hide(void)
{
	if (!prog_shown)
		return;
	fputs("\x1b[1;1H\x1b[2K", stdout);
	fflush(stdout);
	prog_shown = 0;
}


/* Per-line construction shared by the sequential (push_line) and parallel
 * (load workers) loaders: everything that only reads this line's bytes and
 * this Line slot. The service color slot is left to a later commit_slot
 * pass because it must follow first-appearance order over the whole file. */
static void fill_line_fields(Line *L, const char *s, size_t len)
{
	regmatch_t sm;
	L->s = s;
	L->len = len;
	L->marked = 0;
	L->srchit = (unsigned char)search_match(L, &sm);
	L->sev = NULL;
	/* measure now: the page holding this line passes through the cache
	 * exactly once, and deferring the width pass made logs bigger than
	 * the page cache re-read the whole file on the first hscroll */
	L->wcols = str_cols(L->s, L->len);
	detect_tag(L);
}

static void push_line(const char *clean, size_t len)
{
	if (nlines == lcap) {
		lcap = lcap ? lcap * 2 : 1024;
		lines = xrealloc(lines, lcap * sizeof(*lines));
	}
	Line *L = &lines[nlines];
	fill_line_fields(L, clean, len);
	if (L->wcols > wc_max)
		wc_max = L->wcols;
	commit_slot(L);
	nlines++;
}

/* Does the span hold a tab, CR, or ESC byte (anything sanitize must
 * rewrite)? Three separate memchr calls scan the line three times; a
 * single SIMD pass compares all three at once, which matters for the
 * common case where the file is entirely clean. Falls back to memchr
 * on non-SSE2 targets. */
#if USE_SSE2
static int line_has_crlfesc(const char *s, size_t n)
{
	const __m128i tab = _mm_set1_epi8('\t');
	const __m128i cr = _mm_set1_epi8('\r');
	const __m128i esc = _mm_set1_epi8(0x1b);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
		__m128i hit = _mm_or_si128(_mm_cmpeq_epi8(v, tab),
				    _mm_or_si128(_mm_cmpeq_epi8(v, cr),
						 _mm_cmpeq_epi8(v, esc)));
		if (_mm_movemask_epi8(hit) != 0)
			return 1;
	}
	for (; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\t' || c == '\r' || c == 0x1b)
			return 1;
	}
	return 0;
}
#else
static int line_has_crlfesc(const char *s, size_t n)
{
	return memchr(s, '\t', n) || memchr(s, '\r', n) || memchr(s, 0x1b, n);
}
#endif

/* strip ANSI sequences and CRs, expand tabs; returns arena-allocated
 * string. Worst case is tab expansion (+3 bytes each). */
static char *sanitize(const char *s, size_t n, size_t *outlen)
{
	/* fast path: with no tab/CR/ESC there is nothing to expand or strip,
	 * so just copy the run (the SIMD probe beats a scalar byte pass) */
	if (!line_has_crlfesc(s, n)) {
		char *o = arena_alloc(n + 1);
		memcpy(o, s, n);
		o[n] = 0;
		*outlen = n;
		return o;
	}
	size_t extra = 0;
	for (size_t i = 0; i < n; i++)
		if (s[i] == '\t')
			extra += 3;
	char *o = arena_alloc(n + extra + 1);
	size_t j = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\r')
			continue;
		if (c == '\t') {
			o[j++] = ' '; o[j++] = ' '; o[j++] = ' '; o[j++] = ' ';
			continue;
		}
		if (c == 0x1b && i + 1 < n && s[i + 1] == '[') {
			i += 2;
			while (i < n && !((unsigned char)s[i] >= 0x40 &&
					  (unsigned char)s[i] <= 0x7e))
				i++;
			continue;
		}
		if (c == 0x1b && i + 1 < n && s[i + 1] == ']') {
			i += 2;
			while (i < n && s[i] != 0x07) {
				if (s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\') {
					i++;
					break;
				}
				i++;
			}
			continue;
		}
		if (c == 0x1b) { /* lone escape: eat the final byte too */
			if (i + 1 < n && (unsigned char)s[i + 1] >= 0x40 &&
			    (unsigned char)s[i + 1] <= 0x5f)
				i++;
			continue;
		}
		o[j++] = s[i];
	}
	o[j] = 0;
	*outlen = j;
	return o;
}

static void feed(const char *data, size_t n)
{
	pend = xrealloc(pend, plen + n);
	memcpy(pend + plen, data, n);
	plen += n;
	prog_fed += n;
	size_t start = 0;
	/* If a final partial line was flushed as a line, a leading '\n' merely
	 * terminates it; don't emit a spurious empty line for it. */
	if (flushed_partial && plen > 0 && pend[0] == '\n')
		start = 1;
	flushed_partial = 0;
	/* memchr skips straight between newlines instead of byte-wise */
	for (;;) {
		const char *nl = memchr(pend + start, '\n', plen - start);
		if (!nl)
			break;
		size_t k = (size_t)(nl - pend);
		size_t len;
		char *clean = sanitize(pend + start, k - start, &len);
		push_line(clean, len);
		start = k + 1;
	}
	memmove(pend, pend + start, plen - start);
	plen -= start;
}

/* push a trailing partial line (no '\n') as a real line, so a last
 * record lacking a newline still shows */
void flush_pending(void)
{
	if (plen == 0)
		return;
	size_t len;
	char *clean = sanitize(pend, plen, &len);
	push_line(clean, len);
	plen = 0;
	flushed_partial = 1;
}

/* --- parallel mmap loading -------------------------------------------
 * The per-line work (sanitize probe, UTF-8 width, search hit, tag span) is
 * embarrassingly parallel: each piece reads only this line's bytes and this
 * Line slot. Only the line index, wc_max and the service-color
 * first-appearance order are file-global, and those are cheap to reduce or
 * defer. So a large mapping is split into line-aligned chunks, the chunk
 * counts are summed to lay out lines[] once, then threads fill their slice
 * in place. The order-dependent bits (service slot, wc_max) are folded in
 * after the joins. */

/* Parallel loading only pays off once the file is big enough to amortize the
 * thread spawns and the 2-pass (count then fill) scan. On a warm 12-core box
 * a ~7 MiB log is ~4x slower parallel than single-threaded; the crossover is
 * well past a few dozen MiB, so stay sequential below here. */
#define LOAD_MIN_BYTES ((size_t)1 << 26)	/* 64 MiB */
#define LOAD_MAX_THREADS MAX_THREADS

static size_t load_done;	/* chunks finished in the current pass */
static size_t load_nt;		/* chunk count of the running load */
static size_t load_bnds[LOAD_MAX_THREADS + 1];	/* per-pass chunk boundaries */
/* each worker's current scan position (absolute file offset). A worker writes
 * only its own slot and the main thread reads the whole array, so the bar can
 * advance even while earlier slices are still chewing. */
static size_t load_chunk_prog[LOAD_MAX_THREADS];

/* Work done so far: the sum of bytes scanned across every slice. Unlike a
 * contiguous prefix (which stalls on the slowest leading slice and then jumps
 * when the equal-sized slices finish together), this climbs smoothly the whole
 * time every worker is chewing. */
static size_t load_work_done(void)
{
	size_t sum = 0;
	for (size_t k = 0; k < load_nt; k++) {
		size_t cp = __atomic_load_n(&load_chunk_prog[k], __ATOMIC_RELAXED);
		size_t start = load_bnds[k];
		if (cp > start)
			sum += cp - start;
	}
	return sum;
}

static size_t load_threads(size_t bytes)
{
	long n = effective_threads();
	/* each thread should chew at least ~256 KiB, else spawn cost wins */
	if ((size_t)n > bytes / ((size_t)256 << 10))
		n = bytes / ((size_t)256 << 10);
	if (n < 1)
		n = 1;
	return (size_t)n;
}

typedef struct {
	const char *lo, *hi;	/* byte range: whole lines */
	size_t idx;		/* slice number: indexes load_chunk_prog */
	size_t base;		/* lines[base .. base+count) */
	size_t count;		/* lines in this slice */
	size_t peak;		/* slice-local max display width */
} load_chunk;

typedef struct {
	load_chunk *chunks;
	size_t nt;
	size_t base;	/* bytes of work already banked by earlier passes */
	void (*work)(load_chunk *);
} load_ctx;

typedef struct {
	load_ctx *ctx;
	int slot;
} load_arg;

static void *load_tramp(void *p)
{
	load_arg *a = p;
	a->ctx->work(&a->ctx->chunks[a->slot]);
	return NULL;
}

static void load_spawn(load_ctx *ctx, int with_prog)
{
	size_t nt = ctx->nt;
	pthread_t th[LOAD_MAX_THREADS];
	load_arg arg[LOAD_MAX_THREADS];
	int made[LOAD_MAX_THREADS];
	for (size_t k = 0; k < nt; k++) {
		arg[k] = (load_arg){ ctx, (int)k };
		made[k] = pthread_create(&th[k], NULL, load_tramp, &arg[k]) == 0;
	}
	if (with_prog) {
		/* The main thread idles here while workers report their scan position.
		 * The count and fill passes together bank 2x the file's bytes, so the
		 * bar spans one continuous 0..100% sweep across both: count fills the
		 * first half, fill the second. Halving the running total keeps it on
		 * prog_file's fmap_len denominator. */
		while (__sync_fetch_and_add(&load_done, 0) < nt) {
			prog_file((ctx->base + load_work_done()) / 2);
			/* Tiny poll: this wait becomes the load's latency for files that
			 * finish in one tick, so keep it far below a human's blur. */
			struct timespec ts = { 0, 2 * 1000000 };
			nanosleep(&ts, NULL);
		}
	}
	for (size_t k = 0; k < nt; k++) {
		if (made[k])
			pthread_join(th[k], NULL);
		else
			ctx->work(&ctx->chunks[k]);	/* failed create: run inline */
	}
}

static void count_chunk(load_chunk *c)
{
	const char *p = c->lo, *e = c->hi;
	size_t n = 0;
	size_t next = (size_t)(p - fmap) + PROG_STEP_BYTES;
	for (;;) {
		const char *nl = memchr(p, '\n', (size_t)(e - p));
		if (!nl)
			break;
		n++;
		p = nl + 1;
		if ((size_t)(p - fmap) >= next) {
			__atomic_store_n(&load_chunk_prog[c->idx],
					 (size_t)(p - fmap), __ATOMIC_RELAXED);
			next += PROG_STEP_BYTES;
		}
	}
	c->count = n;
	__atomic_store_n(&load_chunk_prog[c->idx], (size_t)(c->hi - fmap),
			 __ATOMIC_RELAXED);
	__sync_fetch_and_add(&load_done, 1);
}

static void fill_chunk(load_chunk *c)
{
	const char *p = c->lo, *e = c->hi;
	Line *L = lines + c->base;
	size_t i = 0, maxc = 0;
	size_t next = (size_t)(p - fmap) + PROG_STEP_BYTES;
	for (;;) {
		const char *nl = memchr(p, '\n', (size_t)(e - p));
		if (!nl)
			break;
		size_t n = (size_t)(nl - p);
		if (line_has_crlfesc(p, n)) {
			size_t cl;
			char *clean = sanitize(p, n, &cl);
			fill_line_fields(&L[i++], clean, cl);
		} else {
			fill_line_fields(&L[i++], p, n);
		}
		p = nl + 1;
		if ((size_t)(p - fmap) >= next) {
			__atomic_store_n(&load_chunk_prog[c->idx],
					 (size_t)(p - fmap), __ATOMIC_RELAXED);
			next += PROG_STEP_BYTES;
		}
	}
	/* any trailing bytes after the last '\n' are left for the feed path
	 * below, so a file that grows across the mmap/read seam merges the
	 * partial line instead of splitting it */
	for (size_t j = 0; j < i; j++)
		if (L[j].wcols > maxc)
			maxc = L[j].wcols;
	c->peak = maxc;
	__atomic_store_n(&load_chunk_prog[c->idx], (size_t)(c->hi - fmap),
			 __ATOMIC_RELAXED);
	__sync_fetch_and_add(&load_done, 1);
}

static void drain_map_par(void)
{
	size_t total = fmap_len - fmap_pos;
	size_t nt = load_threads(total);
	load_chunk chunks[LOAD_MAX_THREADS];
	size_t bnds[LOAD_MAX_THREADS + 1];

	bnds[0] = 0;
	for (size_t k = 1; k < nt; k++) {
		size_t nominal = total * k / nt;
		size_t prev = bnds[k - 1];
		if (nominal <= prev) {
			bnds[k] = prev;	/* previous boundary already past nominal */
			continue;
		}
		/* align the boundary to the next line start so no line splits */
		const char *nl = memchr(fmap + fmap_pos + nominal, '\n',
					total - nominal);
		bnds[k] = nl ? (size_t)(nl - (fmap + fmap_pos)) + 1 : total;
	}
	bnds[nt] = total;
	load_nt = nt;
	for (size_t k = 0; k <= nt; k++)
		load_bnds[k] = bnds[k];
	for (size_t k = 0; k < nt; k++) {
		chunks[k].lo = fmap + fmap_pos + bnds[k];
		chunks[k].hi = fmap + fmap_pos + bnds[k + 1];
		chunks[k].idx = k;
		chunks[k].base = chunks[k].count = chunks[k].peak = 0;
	}

	load_ctx cc = { chunks, nt, 0, count_chunk };
	for (size_t k = 0; k < nt; k++)
		load_chunk_prog[k] = bnds[k];
	load_done = 0;
	load_spawn(&cc, 1);	/* count pass: this is where cold pages fault in */

	size_t total_lines = 0;
	for (size_t k = 0; k < nt; k++) {
		chunks[k].base = total_lines;
		total_lines += chunks[k].count;
	}
	if (total_lines > lcap) {
		lines = xrealloc(lines, total_lines * sizeof(*lines));
		lcap = total_lines;
	}
	nlines = total_lines;

	load_ctx fc = { chunks, nt, total, fill_chunk };
	for (size_t k = 0; k < nt; k++)
		load_chunk_prog[k] = bnds[k];
	load_done = 0;
	load_spawn(&fc, 1);	/* fill pass: the heavy per-line work */

	wc_max = 0;
	for (size_t k = 0; k < nt; k++)
		if (chunks[k].peak > wc_max)
			wc_max = chunks[k].peak;
	/* service color follows first-appearance order: one sequential pass */
	for (size_t i = 0; i < nlines; i++)
		commit_slot(&lines[i]);

	/* buffer a final partial line (no trailing '\n') the way the sequential
	 * drain does, so a growing file's first extra read merges it correctly */
	fmap_pos = fmap_len;
	size_t tail = fmap_len;
	while (tail > 0 && fmap[tail - 1] != '\n')
		tail--;
	if (tail < fmap_len) {
		fmap_pos = tail;
		feed(fmap + tail, fmap_len - tail);
	}
	prog_file(fmap_len);
}

/* scan newly visible mapping range into lines; a trailing partial line
 * goes back through pend so streaming/follow continue seamlessly */
static void drain_map(void)
{
	/* a big file is the common choke point: split across cores */
	if (fmap_len - fmap_pos >= LOAD_MIN_BYTES &&
	    load_threads(fmap_len - fmap_pos) > 1) {
		drain_map_par();
		return;
	}
	size_t start = fmap_pos;
	for (;;) {
		const char *nl = memchr(fmap + start, '\n', fmap_len - start);
		if (!nl)
			break;
		size_t k = (size_t)(nl - fmap);
		const char *s = fmap + start;
		size_t n = k - start;
		/* SIMD probe for a tab/CR/ESC: if absent the line is clean */
		if (line_has_crlfesc(s, n)) {
			size_t len;
			char *clean = sanitize(s, n, &len);
			push_line(clean, len);
		} else {
			push_line(s, n);
		}
		start = k + 1;
		prog_file(start);
	}
	fmap_pos = start;
	if (start < fmap_len)
		feed(fmap + start, fmap_len - start);
}

/* non-blocking drain of piped stdin; latches EOF so closed pipes
 * settle into ordinary one-shot input. Returns 1 if lines were added. */
static int append_stdin(void)
{
	size_t old = nlines;
	for (;;) {
		char buf[65536];
		ssize_t g = read(STDIN_FILENO, buf, sizeof buf);
		if (g > 0) {
			feed(buf, (size_t)g);
			prog_stdin();
			continue;
		}
		if (g < 0 && errno == EINTR)
			continue;
		stdin_eof = g == 0 || errno != EAGAIN;
		break;
	}
	if (stdin_eof)
		flush_pending();
	return nlines != old;
}

static struct {
	const char *name;	/* points into a Line's buffer */
	size_t len;
	int slot;
} svc_seen[128];
static size_t nsvc_seen;

/* Byte span of the syslog service tag; returns 1 and fills *so / *eo, or 0
 * if none. Heuristic: the first field ending in ':' after a timestamp-ish
 * preamble, with a trailing "[pid]" stripped:
 *
 *   2026-08-22T23:53:27.7 localhost NetworkManager[508]: <info> ...
 *   \--- field 1 ---/   ^field 2  ^field 3
 *                                  \*so .. *eo/
 *
 * Field 1 is the timestamp, ISO 8601 or syslog's "Aug 22 23:53:27"
 * (the time words never end in ':'). Guards: empty field, no trailing
 * ':', longer than 64 bytes, or a leading '<' -- a verbosity token
 * like <info>, not a tag. "chronyd[553]:" and bare "foo:" match.
 * A lone ':' also matches, absorbing the preceding word, so sloppy
 * spacing like the kernel's "Spectre V2 : Mitigation: ..." yields
 * one stable tag per prefix instead of random prose words. */
static int tag_span(const char *s, size_t n, int *so, int *eo)
{
	/* A tag is the *last* byte of a space-delimited field, so its ':'
	 * is always followed by a space or end-of-line. Skim for such a
	 * field-boundary colon and reject lines without one (the common
	 * case) with a single memchr walk instead of the field walk below. */
	const char *colon = s, *end = s + n;
	while ((colon = memchr(colon, ':', (size_t)(end - colon))) != NULL) {
		if (colon + 1 == end || colon[1] == ' ')
			break;
		colon++;
	}
	if (colon == NULL)
		return 0;

	size_t i = 0;
	int field = 0, saw_digit = 0;
	size_t pfs = (size_t)-1;	/* start of the previous field */
	while (i < n) {
		while (i < n && s[i] == ' ')
			i++;
		if (i >= n)
			break;
		size_t fs = i;
		int digit = 0;
		/* one pass per field: find its end and, until a digit has been
		 * seen in some field, whether it holds one */
		while (i < n && s[i] != ' ') {
			if (!saw_digit && !digit &&
			    (unsigned char)s[i] >= '0' && (unsigned char)s[i] <= '9')
				digit = 1;
			i++;
		}
		size_t fe = i;
		size_t pf = pfs;
		pfs = fs;
		saw_digit = saw_digit || digit;
		if (++field < 2 || !saw_digit)
			continue;	/* tag needs a timestamp-ish preamble,
					 * else prose reads as "tag: text" */
		if (fe == fs || s[fe - 1] != ':' || fe - fs > 64 || s[fs] == '<')
			continue;
		size_t b = fs, e = fe - 1;
		if (e == b) {	/* lone ':': reach back one word */
			if (pf == (size_t)-1)
				continue;
			b = pf;
			e = fs;	/* stop before the ':' */
		}
		if (e > b && s[e - 1] == ']') {
			size_t k = e - 1;
			while (k > b && s[k - 1] != '[')
				k--;
			if (k > b)
				e = k - 1;
		}
		while (e > b && s[e - 1] == ' ')
			e--;
		if (e == b)
			continue;
		*so = (int)b;
		*eo = (int)e;
		return 1;
	}
	return 0;
}

/* Detect this line's service-tag byte span. Order-independent: only reads
 * the line's own bytes, so it is safe to run in the parallel loader. The
 * palette slot is left -1 until commit_slot runs in file order. */
static void detect_tag(Line *L)
{
	int fs, e;
	L->slot = -1;
	L->tag_so = -1;
	L->tag_eo = -1;
	/* tag spans are ints: a single line longer than 2GiB has none */
	if (L->len > 2147483647 || !tag_span(L->s, L->len, &fs, &e))
		return;
	L->tag_so = fs;
	L->tag_eo = e;
}

/* Assign the palette slot for an already-detected tag. Order-dependent:
 * slots follow order of first appearance in the file, so a parallel load
 * calls this once per line in file order after the heavy work is done.
 * Run at push time for the sequential path: slots follow order of
 * appearance in the file, and render-time assignment reshuffles colors
 * on reload (draw order differs from file order). */
static void commit_slot(Line *L)
{
	int fs = L->tag_so, e = L->tag_eo;
	if (fs < 0)
		return;
	for (size_t j = 0; j < nsvc_seen; j++)
		if (svc_seen[j].len == (size_t)(e - fs) &&
		    !memcmp(svc_seen[j].name, L->s + fs, (size_t)(e - fs))) {
			L->slot = svc_seen[j].slot;
			return;
		}
	if (nsvc_seen == 128) {
		/* table full: hash into the palette instead */
		unsigned h = 2166136261u;
		for (int k = fs; k < e; k++)
			 h = (h ^ (unsigned char)L->s[k]) * 16777619u;
		L->slot = (int)(h % NSVC_COLORS);
		return;
	}
	svc_seen[nsvc_seen].name = L->s + fs;
	svc_seen[nsvc_seen].len = (size_t)(e - fs);
	svc_seen[nsvc_seen].slot = (int)(nsvc_seen % NSVC_COLORS);
	L->slot = svc_seen[nsvc_seen].slot;
	nsvc_seen++;
}

void reset_lines(void)
{
	arena_reset();
	if (fmap) {
		munmap(fmap, fmap_len);
		fmap = NULL;
		fmap_len = fmap_pos = 0;
	}
	nlines = 0;
	plen = 0;
	flushed_partial = 0;
	nmarked = 0;
	nv = 0;	/* view entries referenced the freed buffers */
	nsvc_seen = 0;	/* tag pointers referenced the freed buffers */
	wc_max = 0;	/* folded widths belonged to the old lines */
	prog_mark = 0;
}

static void read_available(int src)
{
	char buf[65536];
	ssize_t g;
	while ((g = read(src, buf, sizeof buf)) > 0)
		feed(buf, (size_t)g);
}

/* Prefer a zero-copy window onto the file when it is regular and
 * non-empty; anything else (pipes, weird files, mmap failure) falls
 * back to reading through pend. */
static void try_map(void)
{
	struct stat st;
	if (fmap || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
	    st.st_size <= 0)
		return;
	void *p = mmap(NULL, (size_t)st.st_size, PROT_READ,
		       MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED)
		return;
	fmap = p;
	fmap_len = (size_t)st.st_size;
	drain_map();
}

void load_all(void)
{
	if (use_stdin) {
		append_stdin();
		flush_pending();
		return;
	}
	if (fd < 0) {
		fd = open(path, O_RDONLY);
		if (fd < 0)
			die_sys("cannot open %s", path);
	}
	try_map();
	lseek(fd, (off_t)fmap_len, SEEK_SET);
	read_available(fd);
	flush_pending();
	fsize = lseek(fd, 0, SEEK_CUR);
}

/* returns 0 = no new data, 1 = new lines appended, 2 = file rotated */
static int append_new(void)
{
	struct stat st, fst;
	int rotated = 0;
	/* rename+recreate rotation keeps our fd on the old inode, whose
	 * size never changes -- compare the path's inode too; the size
	 * check alone covers copytruncate truncation */
	if (fstat(fd, &fst) == 0 && stat(path, &st) == 0 &&
	    (st.st_ino != fst.st_ino || st.st_dev != fst.st_dev ||
	     (off_t)st.st_size < fsize)) {
		close(fd);
		reset_lines();
		fd = open(path, O_RDONLY);
		if (fd < 0)
			return 0;
		fsize = 0;
		rotated = 1;
		try_map();
		/* skip what mmap already covered; fmap_len is 0 when unmapped,
		 * so an mmap failure still reads the whole file */
		lseek(fd, (off_t)fmap_len, SEEK_SET);
	}
	off_t before = lseek(fd, 0, SEEK_CUR);
	read_available(fd);
	off_t now = lseek(fd, 0, SEEK_CUR);
	fsize = now;
	return rotated ? 2 : (now != before);
}

/* one round of new data from the followed source: 0 = none,
 * 1 = lines appended, 2 = rotation rebuilt all lines */
int pump_follow(void)
{
	if (use_stdin) {
		struct pollfd ps = { .fd = STDIN_FILENO, .events = POLLIN };
		return poll(&ps, 1, 0) > 0 ? append_stdin() : 0;
	}
	if (fd < 0) {
		/* rotation race lost the file between stat and open;
		 * retry until it reappears */
		fd = open(path, O_RDONLY);
		if (fd < 0)
			return 0;
		fsize = 0;
	}
	return append_new();
}
