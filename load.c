/* comb - line ingestion: progress display, sanitizing,
 * mmap/window loading, tail follow.
 *
 * The per-line workspace is deliberately tiny: a compact lidx[] index
 * records only where each line's raw bytes live and how long they are.
 * Everything the viewer decorates lines with -- display width, service
 * tag span, palette slot, severity, search hit -- is computed lazily by
 * lt_fill() only for lines the user actually sees. Filter/search scans
 * operate on (ptr,len) straight out of the index without materialising.
 * The one eagerly-full thing is the service-color slot order, which must
 * follow the file's first-appearance order to stay stable. */

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
static int line_needs_sanitize(const char *s, size_t n);
static void commit_line_slot(size_t i);
static int tag_span(const char *s, size_t n, int *so, int *eo);

/* Line text lives in bump-allocated ~1 MiB chunks; chunks are never
 * moved or freed individually, so lidx[].raw and svc_seen pointers stay
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

/* A staged dirty line that has not yet become a complete line is held
 * here (raw bytes), see feed()/flush_pending(). */
static char *pend;
static size_t plen;
static int flushed_partial;	/* last pushed line had no trailing newline */
static int skip_to_nl;		/* a line exceeded MAX_LINE_LEN: drop up to the next '\n' */
/* transient per-line tag spans, filled by the parallel fill so the
 * progress bar reflects the color pass too; freed after assign_slots. */
static int *load_span;
static size_t load_span_n;


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
/* read (count) and index (fill) byte progress for the two-bar parallel load */
static size_t prog_read, prog_idx;
static int prog_shown;
static int prog_spin;
static uint64_t prog_due;	/* stdin redraw deadline */

/* compact byte count: 37.3GiB / 743MiB / 9.5KiB. Callers pass room for the
 * widest rendering (a size_t that big is "17179869184.0GiB" plus NUL). */
void human_bytes(char *o, size_t v)
{
	if (v >> 30)
		snprintf(o, HUMAN_BYTES_BUF, "%.1fGiB", v / 1073741824.0);
	else if (v >> 20)
		snprintf(o, HUMAN_BYTES_BUF, "%.1fMiB", v / 1048576.0);
	else if (v >= 10 << 10)
		snprintf(o, HUMAN_BYTES_BUF, "%.0fKiB", v / 1024.0);
	else if (v >> 10)
		snprintf(o, HUMAN_BYTES_BUF, "%.1fKiB", v / 1024.0);
	else
		snprintf(o, HUMAN_BYTES_BUF, "%zuB", v);
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

static void prog_paint_row(int row, const char *text)
{
	char c[16];
	snprintf(c, sizeof c, "\x1b[%d;1H\x1b[2K", row);
	fputs(c, stdout);
	fputs(text, stdout);
	fflush(stdout);
	prog_shown = 1;
}

static void prog_paint(const char *text) { prog_paint_row(1, text); }

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
	char cur[HUMAN_BYTES_BUF], tot[HUMAN_BYTES_BUF], rt[HUMAN_BYTES_BUF];
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

/* build a "[###-----]" bar of width bw for val/tot; returns 0 if too narrow
 * (o left empty), else fills o and returns 1. */
static int prog_bar(char *o, int bw, uint64_t val, uint64_t tot)
{
	if (bw <= 0) {
		o[0] = 0;
		return 0;
	}
	int fill = (int)(val * bw / (tot ? tot : 1));
	for (int i = 0; i < bw; i++)
		o[i + 1] = i < fill ? '#' : '-';
	o[0] = '[';
	o[bw + 1] = ']';
	o[bw + 2] = 0;
	return 1;
}

/* two-phase parallel load: row 1 is the file read, row 2 the index build, so
 * a big cold read doesn't stall at a single ambiguous percentage. */
static void prog_paint_both(void)
{
	char cur[HUMAN_BYTES_BUF], tot[HUMAN_BYTES_BUF], rt[HUMAN_BYTES_BUF];
	human_bytes(cur, prog_read);
	human_bytes(tot, fmap_len);
	human_bytes(rt, (size_t)(prog_read / prog_secs()));
	int bw = cols >= 50 ? 22 : cols >= 36 ? 10 : 0;
	char bar[26];
	int has = prog_bar(bar, bw, prog_read, fmap_len);
	const char *name = use_stdin ? "(stdin)"
		: strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	char buf[192];
	size_t room = cols > 66 ? cols - 66 : 0;
	if (room >= 4)
		snprintf(buf, sizeof buf,
			 "reading %.*s%s%s %d%% %s/%s %s/s",
			 (int)(room > 40 ? 40 : room), name,
			 has ? " " : "", bar, (int)(prog_read * 100 / fmap_len),
			 cur, tot, rt);
	else
		snprintf(buf, sizeof buf, "reading %d%% %s/%s %s/s",
			 (int)(prog_read * 100 / fmap_len), cur, tot, rt);
	prog_paint_row(1, buf);

	char ib[26];
	prog_bar(ib, bw, prog_idx, fmap_len);
	snprintf(buf, sizeof buf, "indexing%s%s %d%%",
		 has ? " " : "", ib, (int)(prog_idx * 100 / fmap_len));
	if (rows >= 2)
		prog_paint_row(2, buf);
}

static void prog_stdin(void)
{
	uint64_t now = now_ms();
	if (now < prog_due || prog_fed < PROG_MIN_FEED)
		return;
	prog_due = now + 100;
	char ln[32], hb[HUMAN_BYTES_BUF], rt[HUMAN_BYTES_BUF];
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
	fputs("\x1b[1;1H\x1b[2K\x1b[2;1H\x1b[2K", stdout);
	fflush(stdout);
	prog_shown = 0;
}


/* Does the span hold any byte sanitize must rewrite -- tab, CR, ESC, or
 * another C0/DEL control a terminal would act on (bell, backspace, form-feed,
 * charset shift...)? Only ASCII-range controls matter; a high-bit byte is a
 * UTF-8 sequence and must be left alone. One SIMD pass sees the whole block,
 * which keeps an all-clean file cheap. Falls back to a scalar scan. */
#if USE_SSE2
static int line_needs_sanitize(const char *s, size_t n)
{
	const __m128i space = _mm_set1_epi8(0x20);
	const __m128i del   = _mm_set1_epi8(0x7f);
	const __m128i high  = _mm_set1_epi8(0x80);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
		__m128i ascii = _mm_cmpeq_epi8(_mm_and_si128(v, high),
					     _mm_setzero_si128());
		__m128i ctrl = _mm_or_si128(_mm_cmplt_epi8(v, space),
					    _mm_cmpeq_epi8(v, del));
		ctrl = _mm_and_si128(ctrl, ascii);
		if (_mm_movemask_epi8(ctrl) != 0)
			return 1;
	}
	for (; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20 || c == 0x7f)
			return 1;
	}
	return 0;
}
#else
static int line_needs_sanitize(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x20 || c == 0x7f)
			return 1;
	}
	return 0;
}
#endif

/* What lt_text() must do before a line can be drawn; stored in lidx[].dirty.
 * A plain CRLF line is the common case and needs no copy at all -- the
 * display form is the same bytes with the CR counted out of the length. */
enum {
	L_CLEAN = 0,	/* raw bytes already are the display form */
	L_TRIMCR,	/* only a trailing CR: trim it, still zero-copy */
	L_SANITIZE	/* tab/ESC inside: sanitise once on first touch */
};

/* One pass decides all three states; the CR test runs first so a CRLF file
 * costs a single SIMD scan per line instead of a probe plus a copy. */
static unsigned char classify_line(const char *s, size_t n)
{
	if (n > 0 && (unsigned char)s[n - 1] == '\r' && !line_needs_sanitize(s, n - 1))
		return L_TRIMCR;
	return line_needs_sanitize(s, n) ? L_SANITIZE : L_CLEAN;
}

/* strip ANSI sequences and CRs, expand tabs, and rewrite surviving C0/DEL
 * controls as ^X (cat -v style) so nothing a terminal would act on is ever
 * emitted; returns an arena-allocated string. */
static char *sanitize(const char *s, size_t n, size_t *outlen)
{
	/* fast path: with no control byte there is nothing to expand or strip,
	 * so just copy the run (the SIMD probe beats a scalar byte pass) */
	if (!line_needs_sanitize(s, n)) {
		char *o = arena_alloc(n + 1);
		memcpy(o, s, n);
		o[n] = 0;
		*outlen = n;
		return o;
	}
	size_t extra = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\t')
			extra += 3;		/* 1 byte -> 4 spaces */
		else if (c < 0x20 || c == 0x7f)
			extra += 1;		/* ctrl -> two-cell ^X; CR is dropped */
	}
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
		if (c < 0x20) {	/* C0 control (not tab/CR/ESC): render as ^X */
			o[j++] = '^';
			o[j++] = (char)(c ^ 0x40);	/* 0x00->@ ... 0x1f->_ */
			continue;
		}
		if (c == 0x7f) {	/* DEL */
			o[j++] = '^';
			o[j++] = '?';
			continue;
		}
		o[j++] = s[i];
	}
	o[j] = 0;
	*outlen = j;
	return o;
}


/* --- the compact per-line index ------------------------------------- */

/* Grow lidx[] and the mark/hit bitmaps to hold at least `need` lines.
 * New bitmap bytes are zeroed so a freshly appended line starts clean. */
static void line_cap_grow(size_t need)
{
	if (need <= lcap)
		return;
	size_t nc = lcap ? lcap : 1024;
	while (nc < need)
		nc *= 2;
	size_t oldbytes = lcap ? lcap / 8 + 1 : 0;
	lidx = xrealloc(lidx, nc * sizeof(*lidx));
	mark_bit = xrealloc(mark_bit, nc / 8 + 1);
	hit_bit = xrealloc(hit_bit, nc / 8 + 1);
	memset(mark_bit + oldbytes, 0, (nc / 8 + 1) - oldbytes);
	memset(hit_bit + oldbytes, 0, (nc / 8 + 1) - oldbytes);
	lcap = nc;
}

/* Append one line to the index: where its raw bytes live and how long.
 * Nothing else is computed here -- width/tag/severity are lazy. A line
 * longer than MAX_LINE_LEN is cut there and flagged, so one pathological
 * line can't pin unbounded memory or CPU. */
static void push_raw(const char *raw, size_t len, int trunc)
{
	if (!trunc && len > MAX_LINE_LEN)	/* backstop: callers normally pre-cap */
		{ len = MAX_LINE_LEN; trunc = 1; }
	line_cap_grow(nlines + 1);
	LineIdx *X = &lidx[nlines];
	X->raw = raw;
	X->len = (uint32_t)len;
	X->dirty = classify_line(raw, len);
	X->slot = 0xFF;	/* assigned by assign_slots() in file order */
	X->trunc = trunc ? 1 : 0;
	if (len > wc_max)
		wc_max = len;	/* conservative display-width bound */
	nlines++;
}

/* bitset helpers over mark_bit / hit_bit */
static int bit_get(const unsigned char *b, size_t i)
{
	return (b[i >> 3] >> (i & 7)) & 1;
}

int lt_is_marked(size_t i)	{ return bit_get(mark_bit, i); }
void lt_mark(size_t i, int on)
{
	unsigned char m = (unsigned char)(1 << (i & 7));
	if (on)
		mark_bit[i >> 3] |= m;
	else
		mark_bit[i >> 3] &= (unsigned char)~m;
}
int hit_at(size_t i)		{ return bit_get(hit_bit, i); }
void set_hit(size_t i, int on)
{
	unsigned char m = (unsigned char)(1 << (i & 7));
	if (on)
		hit_bit[i >> 3] |= m;
	else
		hit_bit[i >> 3] &= (unsigned char)~m;
}


/* --- lazy per-line access ------------------------------------------- */

/* Fetch the displayed (sanitised) text of line i. Clean lines return the
 * raw bytes (zero-copy).
 *
 * Materialising a dirty line is write-once: the sanitised copy replaces the
 * raw bytes in the index and the line turns clean, so a redraw or the next
 * scan pass reuses it. Sanitising on every call instead used to allocate a
 * fresh arena copy per line per pass -- a full-file scan then a repaint grew
 * the arena by another copy of the file each time, without bound. The arena
 * copy stays valid until reset_lines() rewinds, so the pointer is stable. */
const char *lt_text(size_t i, size_t *len)
{
	LineIdx *X = &lidx[i];
	if (X->dirty == L_TRIMCR) {
		X->len--;
		X->dirty = L_CLEAN;
	} else if (X->dirty == L_SANITIZE) {
		size_t cl;
		char *clean = sanitize(X->raw, X->len, &cl);
		X->raw = clean;
		X->len = (uint32_t)cl;
		X->dirty = L_CLEAN;
	}
	*len = X->len;
	return X->raw;
}

/* Materialise line i into *L (caller-owned): sanitised text, display
 * width, service-tag span, palette slot, and the mark/hit bits. Called
 * only for lines being drawn, navigated, or copied. */
void lt_fill(Line *L, size_t i)
{
	size_t len;
	const char *s = lt_text(i, &len);
	L->s = s;
	L->len = len;
	L->marked = (unsigned char)lt_is_marked(i);
	L->srchit = (unsigned char)hit_at(i);
	L->trunc = lidx[i].trunc;
	L->sev = NULL;
	L->wcols = str_cols(s, len);
	if (L->trunc) {
		size_t mc = str_cols(LINE_TRUNC_TAIL, sizeof(LINE_TRUNC_TAIL) - 1);
		if (L->wcols + mc > wc_max)
			wc_max = L->wcols + mc;
	}
	if (L->wcols > wc_max)
		wc_max = L->wcols;
	detect_tag(L);
	L->slot = lidx[i].slot == 0xFF ? -1 : (int)lidx[i].slot;
}


/* --- service-color slot (file-order first appearance) --------------- */

#define SVC_TAB_BITS 8
#define SVC_TAB_SIZE (1u << SVC_TAB_BITS)
#define SVC_MAX_SEEN 128

static struct {
	const char *name;	/* into a line's raw bytes; NULL = empty slot */
	size_t len;
	int slot;
} svc_seen[SVC_TAB_SIZE];
static size_t nsvc_seen;

static size_t svc_hash(const char *name, size_t len)
{
	uint32_t h = 2166136261u;
	for (size_t k = 0; k < len; k++)
		h = (h ^ (unsigned char)name[k]) * 16777619u;
	return h;
}

/* return (and if new, record) the palette slot for a service-tag string.
 * Order follows first appearance, so adjacent services get distinct hues;
 * the name is stored by pointer (raw bytes are stable until reset_lines). */
static int slot_for_tag(const char *name, size_t len)
{
	size_t h = svc_hash(name, len) & (SVC_TAB_SIZE - 1);
	size_t empty = (size_t)-1;
	for (size_t probe = 0; probe < SVC_TAB_SIZE; probe++) {
		size_t s = (h + probe) & (SVC_TAB_SIZE - 1);
		const char *seen = svc_seen[s].name;
		if (!seen) {
			empty = s;
			break;
		}
		if (svc_seen[s].len == len && !memcmp(seen, name, len))
			return svc_seen[s].slot;
	}
	/* rare tag beyond the first-appearance cap, or a full table: hashed hue */
	if (empty == (size_t)-1 || nsvc_seen >= SVC_MAX_SEEN)
		return (int)(svc_hash(name, len) % NSVC_COLORS);
	svc_seen[empty].name = name;
	svc_seen[empty].len = len;
	svc_seen[empty].slot = (int)(nsvc_seen % NSVC_COLORS);
	nsvc_seen++;
	return svc_seen[empty].slot;
}

/* Assign the palette slot for line i from a precomputed (so,eo) tag span,
 * or by detecting the span on the fly when span is NULL. */
static void commit_slot_from(size_t i, const int *span)
{
	int so, eo;
	if (span) {
		so = span[2 * i];
		eo = span[2 * i + 1];
		if (so < 0) {
			lidx[i].slot = 0xFF;
			return;
		}
	} else {
		if (lidx[i].len > 2147483647 ||
		    !tag_span(lidx[i].raw, lidx[i].len, &so, &eo)) {
			lidx[i].slot = 0xFF;
			return;
		}
	}
	lidx[i].slot = (unsigned char)slot_for_tag(lidx[i].raw + so,
						   (size_t)(eo - so));
}

static void commit_line_slot(size_t i)
{
	commit_slot_from(i, NULL);
}

/* parallel detect-tag pass: order-independent, so it splits across cores
 * and writes each line's (so,eo) into span[]. */
typedef struct { int *span; } span_ctx;
static void span_work(size_t lo, size_t hi, int slot, void *ctx)
{
	span_ctx *c = ctx;
	(void)slot;
	for (size_t i = lo; i < hi; i++) {
		int so, eo;
		size_t len = lidx[i].len;
		if (len > 2147483647 || !tag_span(lidx[i].raw, len, &so, &eo))
			so = eo = -1;
		c->span[2 * i] = so;
		c->span[2 * i + 1] = eo;
	}
}

/* Rebuild svc_seen and slot every indexed line in file order. On a big
 * file the tag detection (the expensive part) is parallelised into a
 * transient span array; slot assignment stays sequential so the palette
 * rotation follows first appearance. Called once after an initial load
 * and after a rotation rebuild. */
static void assign_slots(void)
{
	nsvc_seen = 0;
	memset(svc_seen, 0, sizeof svc_seen);
	if (load_span) {
		/* spans already computed in the parallel fill; lines beyond the
		 * indexed range (appended while loading) detect on the fly */
		for (size_t i = 0; i < nlines; i++)
			commit_slot_from(i, i < load_span_n ? load_span : NULL);
		free(load_span);
		load_span = NULL;
		load_span_n = 0;
	} else if (par_threads(nlines) > 1) {
		int *span = xrealloc(NULL, 2 * nlines * sizeof(int));
		span_ctx sc = { span };
		par_run(0, nlines, par_threads(nlines), span_work, &sc);
		for (size_t i = 0; i < nlines; i++)
			commit_slot_from(i, span);
		free(span);
	} else {
		for (size_t i = 0; i < nlines; i++)
			commit_line_slot(i);
	}
}


/* --- ingestion ------------------------------------------------------ */

/* Buffer raw line bytes and index each complete line. Raw bytes are
 * copied to the arena so lazy materialisation can re-sanitise them. */
static void feed(const char *data, size_t n)
{
	pend = xrealloc(pend, plen + n);
	memcpy(pend + plen, data, n);
	plen += n;
	prog_fed += n;

	/* A line passed MAX_LINE_LEN and is being discarded: drop bytes until the
	 * newline that closes it, so an endless run (e.g. /dev/zero) can't hold a
	 * growing partial line in memory or rescan it quadratically. */
	if (skip_to_nl) {
		const char *nl = memchr(pend, '\n', plen);
		if (!nl) {
			plen = 0;
			return;
		}
		size_t k = (size_t)(nl - pend);
		memmove(pend, pend + k + 1, plen - (k + 1));
		plen -= k + 1;
		skip_to_nl = 0;
	}

	size_t start = 0;
	/* If a final partial line was flushed as a line, a leading '\n' merely
	 * terminates it; don't emit a spurious empty line for it. */
	if (flushed_partial && plen > 0 && pend[0] == '\n')
		start = 1;
	flushed_partial = 0;
	for (;;) {
		const char *nl = memchr(pend + start, '\n', plen - start);
		if (!nl)
			break;
		size_t k = (size_t)(nl - pend);
		size_t len = k - start;
		int trunc = len > MAX_LINE_LEN;
		size_t save = trunc ? MAX_LINE_LEN : len;
		char *raw = arena_alloc(save + 1);
		memcpy(raw, pend + start, save);
		raw[save] = 0;
		push_raw(raw, save, trunc);
		commit_line_slot(nlines - 1);
		start = k + 1;
	}
	/* A trailing partial line with no '\n' yet that already exceeds the cap:
	 * index its prefix now and drop the rest as it arrives (see skip_to_nl). */
	if (!skip_to_nl && plen - start > MAX_LINE_LEN) {
		char *raw = arena_alloc(MAX_LINE_LEN + 1);
		memcpy(raw, pend + start, MAX_LINE_LEN);
		raw[MAX_LINE_LEN] = 0;
		push_raw(raw, MAX_LINE_LEN, 1);
		commit_line_slot(nlines - 1);
		skip_to_nl = 1;
		start = plen;
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
	if (skip_to_nl) {
		/* the over-long line already emitted its truncated prefix; the bytes
		 * still buffered are overflow to be dropped, not a real line */
		plen = 0;
		skip_to_nl = 0;
		return;
	}
	int trunc = plen > MAX_LINE_LEN;
	size_t save = trunc ? MAX_LINE_LEN : plen;
	char *raw = arena_alloc(save + 1);
	memcpy(raw, pend, save);
	raw[save] = 0;
	push_raw(raw, save, trunc);
	commit_line_slot(nlines - 1);
	plen = 0;
	flushed_partial = 1;
}


/* --- parallel mmap loading -------------------------------------------
 * The per-line work still needs the whole file's line boundaries, so the
 * index is built by splitting the mapping into line-aligned chunks and
 * counting then filling in place. Unlike the old eager loader there is no
 * width/tag/search pass here -- each chunk only records raw pointers and
 * the dirty probe, so the fill is nearly as cheap as the count. The
 * order-dependent bits (service slot, wc_max) are folded in after the
 * joins. */

/* Parallel loading only pays off once the file is big enough to amortize the
 * thread spawns and the 2-pass (count then fill) scan; on a warm 12-core box
 * a ~7 MiB log is ~4x slower parallel than single-threaded. It wins when the
 * file is already paged or on fast storage; on a slow disk a single thread
 * saturates the read anyway, so the extra pass only adds cost. Stay sequential
 * below here and let -t 1 force a single thread when you know storage is slow. */
#define LOAD_MIN_BYTES ((size_t)1 << 29)	/* 512 MiB */
#define LOAD_MAX_THREADS MAX_THREADS

static size_t load_done;	/* chunks finished in the current pass */
static size_t load_nt;		/* chunk count of the running load */
static size_t load_bnds[LOAD_MAX_THREADS + 1];	/* per-pass chunk boundaries */
/* each worker's current scan position (absolute file offset) */
static size_t load_chunk_prog[LOAD_MAX_THREADS];

/* Work done so far: the sum of bytes scanned across every slice. */
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
	if ((size_t)n > bytes / ((size_t)256 << 10))
		n = bytes / ((size_t)256 << 10);
	if (n < 1)
		n = 1;
	return (size_t)n;
}

typedef struct {
	const char *lo, *hi;	/* byte range: whole lines */
	size_t idx;		/* slice number: indexes load_chunk_prog */
	size_t base;		/* lidx[base .. base+count) */
	size_t count;		/* lines in this slice */
	size_t peak;		/* slice-local max raw length */
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

enum { LPH_NONE = 0, LPH_READ, LPH_INDEX };

static void load_spawn(load_ctx *ctx, int prog_phase)
{
	size_t nt = ctx->nt;
	pthread_t th[LOAD_MAX_THREADS];
	load_arg arg[LOAD_MAX_THREADS];
	int made[LOAD_MAX_THREADS];
	for (size_t k = 0; k < nt; k++) {
		arg[k] = (load_arg){ ctx, (int)k };
		made[k] = pthread_create(&th[k], NULL, load_tramp, &arg[k]) == 0;
	}
	if (prog_phase != LPH_NONE) {
		size_t last = 0;
		while (__sync_fetch_and_add(&load_done, 0) < nt) {
			size_t work;
			if (prog_phase == LPH_READ) {
				prog_read = load_work_done();
				work = prog_read;
			} else {
				prog_read = fmap_len;	/* read finished */
				prog_idx = load_work_done();
				work = prog_idx;
			}
			if (work >= last && work - last >= PROG_STEP_BYTES) {
				last = work;
				prog_paint_both();
			}
			struct timespec ts = { 0, 2 * 1000000 };
			nanosleep(&ts, NULL);
		}
		/* One conclusive frame: the loop polls a step behind the last chunk's
		 * final position, so re-read once the workers have all signalled done. */
		if (prog_phase == LPH_INDEX) {
			prog_idx = load_work_done();
			prog_paint_both();
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

/* Fill a slice of the index: record each line's raw pointer and length and
 * whether it needs sanitising. No width/tag/severity work here. */
static void fill_chunk(load_chunk *c)
{
	const char *p = c->lo, *e = c->hi;
	LineIdx *L = lidx + c->base;
	size_t i = 0, maxc = 0;
	size_t next = (size_t)(p - fmap) + PROG_STEP_BYTES;
	for (;;) {
		const char *nl = memchr(p, '\n', (size_t)(e - p));
		if (!nl)
			break;
		size_t n = (size_t)(nl - p);
		int trunc = n > MAX_LINE_LEN;
		size_t save = trunc ? MAX_LINE_LEN : n;
		L[i].raw = p;
		L[i].len = (uint32_t)save;
		L[i].dirty = classify_line(p, save);
		L[i].slot = 0xFF;
		L[i].trunc = trunc ? 1 : 0;
		if (save > maxc)
			maxc = save;
		if (load_span) {
			int so, eo;
			if (n > 2147483647 || !tag_span(p, n, &so, &eo))
				so = eo = -1;
			load_span[2 * (c->base + i)] = so;
			load_span[2 * (c->base + i) + 1] = eo;
		}
		i++;
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
	prog_read = 0;
	prog_idx = 0;
	load_spawn(&cc, LPH_READ);	/* count pass: this is where cold pages fault in */

	size_t total_lines = 0;
	for (size_t k = 0; k < nt; k++) {
		chunks[k].base = total_lines;
		total_lines += chunks[k].count;
	}
	line_cap_grow(total_lines);
	nlines = total_lines;

	/* colour detection runs here (parallel), ahead of the 100% bar, so the
	 * only post-fill work is the fast slot ordering. */
	load_span = xrealloc(NULL, 2 * total_lines * sizeof(int));
	load_span_n = total_lines;

	load_ctx fc = { chunks, nt, total, fill_chunk };
	for (size_t k = 0; k < nt; k++)
		load_chunk_prog[k] = bnds[k];
	load_done = 0;
	load_spawn(&fc, LPH_INDEX);	/* fill pass: page-cached, index bar advances here */

	wc_max = 0;
	for (size_t k = 0; k < nt; k++)
		if (chunks[k].peak > wc_max)
			wc_max = chunks[k].peak;

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
}

/* scan newly visible mapping range into the line index; a trailing partial
 * line goes back through pend so streaming/follow continue seamlessly */
static void drain_map(void)
{
	/* a big file is the common choke point: split across cores */
	if (fmap_len - fmap_pos >= LOAD_MIN_BYTES &&
	    load_threads(fmap_len - fmap_pos) > 1) {
		drain_map_par();
		return;
	}
	size_t start = 0;
	for (;;) {
		const char *nl = memchr(fmap + fmap_pos + start, '\n',
					fmap_len - fmap_pos - start);
		if (!nl)
			break;
		const char *s = fmap + fmap_pos + start;
		size_t len = (size_t)(nl - s);
		int trunc = len > MAX_LINE_LEN;
		push_raw(s, trunc ? MAX_LINE_LEN : len, trunc);
		start = (size_t)(nl - (fmap + fmap_pos)) + 1;
		prog_file(fmap_pos + start);
	}
	fmap_pos += start;
	if (fmap_pos < fmap_len)
		feed(fmap + fmap_pos, fmap_len - fmap_pos);
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
			continue;
		if (fe == fs || s[fe - 1] != ':' || fe - fs > 64 || s[fs] == '<')
			continue;
		size_t b = fs, e = fe - 1;
		if (e == b) {	/* lone ':': reach back one word */
			if (pf == (size_t)-1)
				continue;
			b = pf;
			e = fs;
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
 * palette slot is left -1; lt_fill reads it from lidx[].slot instead. */
static void detect_tag(Line *L)
{
	int fs, e;
	L->slot = -1;
	L->tag_so = -1;
	L->tag_eo = -1;
	if (L->len > 2147483647 || !tag_span(L->s, L->len, &fs, &e))
		return;
	L->tag_so = fs;
	L->tag_eo = e;
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
	lcap = 0;
	free(lidx);
	lidx = NULL;
	free(mark_bit);
	mark_bit = NULL;
	free(hit_bit);
	hit_bit = NULL;
	free(load_span);
	load_span = NULL;
	load_span_n = 0;
	plen = 0;
	flushed_partial = 0;
	nmarked = 0;
	nv = 0;	/* view entries referenced the freed buffers */
	nsvc_seen = 0;	/* tag pointers referenced the freed buffers */
	memset(svc_seen, 0, sizeof svc_seen);
	wc_max = 0;
	prog_mark = 0;
	skip_to_nl = 0;
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
	madvise(p, (size_t)st.st_size, MADV_SEQUENTIAL);
	fmap = p;
	fmap_len = (size_t)st.st_size;
	drain_map();
}

void load_all(void)
{
	if (use_stdin) {
		append_stdin();
		flush_pending();
		assign_slots();
		return;
	}
	if (fd < 0) {
		fd = open(path, O_RDONLY);
		if (fd < 0)
			die_sys("cannot open %s", path);
		struct stat st;
		/* opens fine, reads EISDIR, and the pane would just say "(empty)" */
		if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
			die("%s is a directory", path);
		/* A char device (e.g. /dev/urandom), FIFO or socket never reaches EOF, so
		 * the eager one-shot read below would block forever and buffer the whole
		 * stream. comb's filter/search model holds every line, so an unbounded
		 * source can't be shown; refuse it (as less does) instead of hanging.
		 * Pipe it in via stdin instead -- that path streams and follows live. */
		if (fstat(fd, &st) == 0 && !S_ISREG(st.st_mode))
			die("%s is not a regular file; pipe it in instead (e.g. cat %s | comb)",
			    path, path);
	}
	try_map();
	lseek(fd, (off_t)fmap_len, SEEK_SET);
	read_available(fd);
	flush_pending();
	fsize = lseek(fd, 0, SEEK_CUR);
	assign_slots();
}

/* returns 0 = no new data, 1 = new lines appended, 2 = file rotated */
static int append_new(void)
{
	struct stat st, fst;
	int rotated = 0;
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
		lseek(fd, (off_t)fmap_len, SEEK_SET);
	}
	off_t before = lseek(fd, 0, SEEK_CUR);
	read_available(fd);
	fsize = lseek(fd, 0, SEEK_CUR);
	if (rotated) {
		flush_pending();
		assign_slots();
	}
	return rotated ? 2 : (fsize != before);
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
		fd = open(path, O_RDONLY);
		if (fd < 0)
			return 0;
		fsize = 0;
	}
	return append_new();
}
