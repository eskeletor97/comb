/* comb - pattern matching: literals, literal alternations,
 ERE dispatch, parallel scan framework */

#define _GNU_SOURCE
#include "comb.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

#if defined(__SSE2__) && defined(__GNUC__)
#define USE_SSE2 1
#include <emmintrin.h>
#else
#define USE_SSE2 0
#endif

/* earliest offset of a byte equal to lo or hi, or -1; the icase search
 * probes both case-variants of the pattern's first byte. SSE2 compares
 * both in one pass; the fallback is two memchr calls. */
#if USE_SSE2
static ptrdiff_t find_icase_byte(const char *s, size_t n, unsigned char lo,
				 unsigned char hi)
{
	const __m128i vlo = _mm_set1_epi8((char)lo);
	const __m128i vhi = _mm_set1_epi8((char)hi);
	size_t i = 0;
	for (; i + 16 <= n; i += 16) {
		__m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
		__m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, vlo),
					 _mm_cmpeq_epi8(v, vhi));
		unsigned mask = (unsigned)_mm_movemask_epi8(m);
		if (mask)
			return (ptrdiff_t)(i + __builtin_ctz(mask));
	}
	for (; i < n; i++)
		if ((unsigned char)s[i] == lo || (unsigned char)s[i] == hi)
			return (ptrdiff_t)i;
	return -1;
}
#else
static ptrdiff_t find_icase_byte(const char *s, size_t n, unsigned char lo,
				 unsigned char hi)
{
	const char *end = s + n;
	const char *a = memchr(s, lo, n);
	const char *b = lo == hi ? NULL : memchr(s, hi, n);
	const char *hit = !a ? b : !b ? a : (a < b ? a : b);
	return hit ? (ptrdiff_t)(hit - s) : -1;
}
#endif


int smart_case(const char *q)
{
	for (const char *p = q; *p; p++)
		if (isupper((unsigned char)*p))
			return 0;
	return 1;
}

/* shared parse for the filter's and the highlight-search literal machines */
void lit_parse(const char *q, int icase, LitSpec *ls)
{
	ls->len = 0;
	ls->bol = (*q == '^');
	const char *p = q + ls->bol;
	size_t n = strlen(p);
	ls->eol = n > 0 && p[n - 1] == '$';
	if (ls->eol)
		n--;
	memcpy(ls->buf, p, n);
	ls->len = n;
	ls->icase = icase;
}

/* case-insensitive byte compare, folding ASCII A-Z to lower. comb never
 * calls setlocale(), so the process is in the "C" locale where strncasecmp
 * folds exactly the ASCII letters -- this is equivalent but avoids
 * strncasecmp_l's per-call locale-table lookups, which dominate the hot
 * icase search. */
static int icase_cmp(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char ca = (unsigned char)a[i];
		unsigned char cb = (unsigned char)b[i];
		if (ca >= 'A' && ca <= 'Z')
			ca = (unsigned char)(ca + 32);
		if (cb >= 'A' && cb <= 'Z')
			cb = (unsigned char)(cb + 32);
		if (ca != cb)
			return (int)ca - (int)cb;
	}
	return 0;
}

/* byte offset of the first hit of pat[0..patlen) in s[0..len), or -1;
 * shared by the filter's and the search's literal matchers */
static ptrdiff_t pat_find(const char *pat, size_t patlen, int bol, int eol,
			  int icase, const char *s, size_t len)
{
#define PAT_EQ(p) \
	!(icase ? icase_cmp((p), pat, patlen) : memcmp((p), pat, patlen))
	if (patlen == 0)
		return bol && eol ? (len == 0 ? 0 : -1) : 0;
	if (len < patlen)
		return -1;
	size_t tail = len - patlen;
	if (bol && eol)
		return len == patlen && PAT_EQ(s) ? 0 : -1;
	if (bol)
		return PAT_EQ(s) ? 0 : -1;
	if (eol)
		return PAT_EQ(s + tail) ? (ptrdiff_t)tail : -1;
	if (!icase) {
		const char *h = memmem(s, len, pat, patlen);
		return h ? (ptrdiff_t)(h - s) : -1;
	}
	/* icase: scan for either case of byte 0 in one pass, verify folded.
	 * Fold by hand: comb stays in the C locale, so glibc's tolower/
	 * toupper would only add a table-pointer fetch per call -- and this
	 * runs once per line during scans. */
	unsigned char c0 = (unsigned char)pat[0];
	unsigned char lo = c0 >= 'A' && c0 <= 'Z' ? (unsigned char)(c0 | 32) : c0;
	unsigned char hi = c0 >= 'a' && c0 <= 'z' ? (unsigned char)(c0 & ~32u) : c0;
	const char *p = s, *end = s + len;
	while (p < end) {
		ptrdiff_t off = find_icase_byte(p, (size_t)(end - p), lo, hi);
		if (off < 0)
			return -1;
		const char *hit = p + off;
		if ((size_t)(end - hit) >= patlen && PAT_EQ(hit))
			return hit - s;
		p = hit + 1;
	}
#undef PAT_EQ
	return -1;
}

/* Try to read q as flat alternation of literals, the common log regex:
 * one or more `^?literal$?` pieces joined by `|`. When it parses we skip
 * glibc regexec entirely -- pat_find's SIMD/memmem path is far faster per
 * line and needs no per-call regex state. Returns 1 to use the alternate
 * matcher; 0 means fall back to the compiled regex. Any regex-feature
 * (classes, groups, quantifiers, .*, interior anchors) rejects it. */
/* is the byte at pos escaped (an odd run of backslashes before it)?  So
 * `\$` is a literal dollar, not an anchor, and `\|` is a literal pipe,
 * not a separator. `s` is the branch start; backslashes can't be escaped
 * across a `|` boundary. */
static int is_escaped(const char *s, const char *pos)
{
	int n = 0;
	while (pos > s && pos[-1] == '\\') {
		pos--;
		n++;
	}
	return n & 1;
}

int alt_parse(const char *q, int icase, AltSpec *as)
{
	as->n = 0;
	as->icase = (unsigned char)icase;
	size_t bp = 0;
	const char *p = q;
	if (!*p)
		return 0;
	while (*p) {
		if (as->n >= ALT_MAX)
			return 0;
		int bol = 0, eol = 0;
		if (*p == '^')
			{ bol = 1; p++; }
		/* find the branch end, honoring escapes so `\|` isn't a split */
		const char *bend = p;
		while (*bend && *bend != '|') {
			if (*bend == '\\' && bend[1])
				bend++;	/* skip the escaped byte */
			bend++;
		}
		const char *sep = bend;	/* separator (or end) before any anchor strip */
		if (bend > p && bend[-1] == '$' && !is_escaped(p, bend - 1)) {
			eol = 1;
			bend--;
		}
		size_t so = bp;
		int empty = 1;
		while (p < bend) {
			unsigned char c = (unsigned char)*p++;
			if (c == '\\') {
				if (p >= bend)
					return 0;	/* dangling backslash */
				unsigned char e = (unsigned char)*p++;
				if (!strchr("\\.*+?[]()|^${}", e))
					return 0;	/* perl-class / unknown escape */
				c = e;
			} else if (c == '.' || c == '[' || c == ']' ||
				   c == '(' || c == ')' || c == '*' ||
				   c == '+' || c == '?' || c == '{' ||
				   c == '}' || c == '^' || c == '$') {
				return 0;	/* unsupported regex syntax */
			}
			if (bp >= MAX_QUERY)
				return 0;
			as->buf[bp++] = (char)c;
			empty = 0;
		}
		if (empty)
			return 0;	/* empty alternative */
		as->off[as->n] = so;
		as->len[as->n] = (size_t)(bp - so);
		as->bol[as->n] = (unsigned char)bol;
		as->eol[as->n] = (unsigned char)eol;
		as->n++;
		if (*sep == '|') {
			p = sep + 1;
			if (!*p || *p == '|')
				return 0;	/* empty alternative */
		} else {
			break;
		}
	}
	return 1;
}

/* best literal match across the alternation. Each piece finds its own
 * leftmost hit; the winner is the leftmost, breaking ties by longest
 * (approximating POSIX leftmost-longest for disjoint literal pieces). */
static int alt_match(const AltSpec *as, const char *s, size_t len,
		      regmatch_t *m)
{
	ptrdiff_t best = -1, best_len = -1;
	for (int i = 0; i < as->n; i++) {
		ptrdiff_t off = pat_find(as->buf + as->off[i], as->len[i],
					as->bol[i], as->eol[i], as->icase,
					s, len);
		if (off < 0)
			continue;
		if (best == -1 || off < best ||
		    (off == best && (ptrdiff_t)as->len[i] > best_len)) {
			best = off;
			best_len = (ptrdiff_t)as->len[i];
		}
	}
	if (best < 0)
		return 0;
	m->rm_so = (regoff_t)best;
	m->rm_eo = (regoff_t)(best + best_len);
	return 1;
}

/* shared matcher for one Pat spec: dispatch its literal, literal
 * alternation or compiled regex against one line, filling m with the
 * match (highlight) span */
static int pattern_match(const char *s, size_t len, const Pat *p,
			 regmatch_t *m)
{
	if (p->is_alt)
		return alt_match(&p->alt, s, len, m);
	if (p->is_re) {
		m->rm_so = 0;
		m->rm_eo = (regoff_t)len;	/* REG_STARTEND: no NUL needed */
		return regexec(&p->re, s, 1, m, REG_STARTEND) == 0;
	}
	ptrdiff_t off = pat_find(p->lit.buf, p->lit.len, p->lit.bol, p->lit.eol,
				 p->lit.icase, s, len);
	if (off < 0)
		return 0;
	m->rm_so = (regoff_t)off;
	m->rm_eo = (regoff_t)(off + (ptrdiff_t)p->lit.len);
	return 1;
}

/* active-filter test; on match fills m with the highlight span */
int query_match(const char *s, size_t len, regmatch_t *m)
{
	if (pending_pat.active) {	/* a scan is testing a candidate pattern */
		/* an empty candidate text is a pending clear: the empty literal
		 * matches every line again, with an empty highlight span */
		return pattern_match(s, len, &pending_pat, m);
	}
	if (!filter_pat.active) {
		m->rm_so = m->rm_eo = 0;	/* empty span: nothing to highlight */
		return 1;
	}
	/* no zero-width guard here: the return decides view membership, and
	 * a pattern like a* matching empty must keep lines visible */
	return pattern_match(s, len, &filter_pat, m);
}

/* does the highlight-search pattern hit this line? fills m with the span.
 * While a search job is in flight the pending pattern answers: workers
 * must test the new query while n/N and the scrollbar keep serving the
 * previous results until commit. An empty candidate query must answer
 * "no hits" rather than falling back to the committed search -- lines
 * pushed mid-sweep are never rewritten at commit (see job_finish). */
int search_match(const char *s, size_t len, regmatch_t *m)
{
	const Pat *p = pending_pat.active ? &pending_pat : &search_pat;
	int on = p->active && (p != &pending_pat || p->enable_on_commit);
	regmatch_t mm;
	if (!on)
		return 0;
	if (!pattern_match(s, len, p, &mm))
		return 0;
	/* a zero-width regex match is not a hit: n/N and the scrollbar need
	 * a real span to land on */
	if (p->is_re && mm.rm_eo == mm.rm_so)
		return 0;
	*m = mm;
	return 1;
}

/* --- parallel scan --------------------------------------------------
 * Filtering and the highlight-search sweep walk every line. Each line's
 * match test is independent and reads only the immutable pattern plus the
 * line text, so the range splits cleanly across a few pthreads and the
 * ordered results reassemble in slot order. Engaged only above
 * PAR_MIN_LINES: below that, spawn/join would cost more than the scan.
 * The main thread is blocked in par_run until every worker joins, so the
 * query and line state it reads cannot change mid-flight. */
#define PAR_MAX_THREADS MAX_THREADS
#define PAR_MIN_LINES ((size_t)1 << 16)
#define PAR_MIN_SPAN ((size_t)1 << 13)

int par_threads(size_t len)
{
	if (len < PAR_MIN_LINES)
		return 1;
	long n = effective_threads();
	if ((size_t)n > len / PAR_MIN_SPAN)
		n = len / PAR_MIN_SPAN;
	if (n < 1)
		n = 1;
	return (int)n;
}

typedef void (*par_work)(size_t lo, size_t hi, int slot, void *ctx);
typedef struct {
	par_work work;
	void *ctx;
	size_t lo, hi;
	int slot;
} par_arg;

static void *par_spawn(void *p)
{
	par_arg *a = p;
	a->work(a->lo, a->hi, a->slot, a->ctx);
	return NULL;
}

/* run work() over [lo,hi) split into nthreads contiguous slices. A failed
 * pthread_create runs that slice inline; par_run always joins everything
 * it created before returning. */
void par_run(size_t lo, size_t hi, int nthreads, par_work work, void *ctx)
{
	size_t len = hi - lo;
	if ((size_t)nthreads > len)
		nthreads = (int)len;
	if (nthreads <= 1) {
		work(lo, hi, 0, ctx);
		return;
	}
	par_arg arg[PAR_MAX_THREADS];
	pthread_t th[PAR_MAX_THREADS];
	int created[PAR_MAX_THREADS];
	size_t span = len / (size_t)nthreads;
	size_t start = lo;
	for (int k = 0; k < nthreads; k++) {
		size_t end = (k == nthreads - 1) ? hi : start + span;
		arg[k] = (par_arg){ work, ctx, start, end, k };
		created[k] = pthread_create(&th[k], NULL, par_spawn, &arg[k]) == 0;
		start = end;
	}
	for (int k = 0; k < nthreads; k++) {
		if (created[k])
			pthread_join(th[k], NULL);
		else
			arg[k].work(arg[k].lo, arg[k].hi, arg[k].slot, ctx);
	}
}

typedef struct {
	size_t *a;
	size_t n, cap;
} par_list;

typedef struct {
	size_t (*pos_line)(size_t p);	/* position -> line index */
	int (*match)(const char *s, size_t len, regmatch_t *m);
	int inv;
	par_list *lst;			/* per-slot match collectors */
} col_ctx;

static void col_work(size_t lo, size_t hi, int slot, void *ctx)
{
	col_ctx *c = ctx;
	par_list *b = &c->lst[slot];
	regmatch_t m;
	for (size_t p = lo; p < hi; p++) {
		size_t li = c->pos_line(p);
		size_t len;
		const char *s = lt_text(li, &len);
		if (c->match(s, len, &m) != c->inv) {
			if (b->n == b->cap) {
				b->cap = b->cap ? b->cap * 2 : 512;
				b->a = xrealloc(b->a, b->cap * sizeof(*b->a));
			}
			b->a[b->n++] = li;
		}
	}
}

/* Append to *arr every position p in [lo,hi) whose line
 * match(line_text(pos_line(p))) != inv, preserving position order. Each
 * pthread builds one slice; the slices concatenate in slot order. */
void scan_collect(size_t lo, size_t hi, size_t (*pos_line)(size_t),
			 int (*match)(const char *s, size_t len, regmatch_t *m), int inv,
			 size_t **arr, size_t *n, size_t *cap)
{
	int nthreads = par_threads(hi - lo);
	par_list lst[PAR_MAX_THREADS] = {0};
	col_ctx c = { pos_line, match, inv, lst };
	par_run(lo, hi, nthreads, col_work, &c);
	size_t tot = 0;
	for (int k = 0; k < nthreads; k++)
		tot += lst[k].n;
	if (*n + tot > *cap) {
		size_t want = *n + tot;
		size_t nc = *cap ? *cap : 1024;
		while (nc < want)
			nc *= 2;
		*cap = nc;
		*arr = xrealloc(*arr, *cap * sizeof(**arr));
	}
	size_t out = *n;
	for (int k = 0; k < nthreads; k++) {
		if (lst[k].n)
			memcpy(*arr + out, lst[k].a, lst[k].n * sizeof(**arr));
		out += lst[k].n;
		free(lst[k].a);
	}
	*n = out;
}

size_t pos_ident(size_t p) { return p; }
size_t pos_view(size_t p) { return view_at(p); }
