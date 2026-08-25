/* comb - UTF-8 display widths, syntax spans, screen painting */

#define _GNU_SOURCE
#include "comb.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#if defined(__SSE2__) && defined(__GNUC__)
#define USE_SSE2 1
#include <emmintrin.h>
#else
#define USE_SSE2 0
#endif

static size_t u8len(unsigned char c)
{
	if (c < 0x80)
		return 1;
	if ((c & 0xE0) == 0xC0)
		return 2;
	if ((c & 0xF0) == 0xE0)
		return 3;
	if ((c & 0xF8) == 0xF0)
		return 4;
	return 1;
}

/* display width of a codepoint in terminal cells: East Asian
 * Wide/Fullwidth = 2, common combining marks = 0, else 1. A compact,
 * locale-independent wcwidth() stand-in covering what shows up in
 * logs (CJK, Hangul, Kana, fullwidth, emoji). */
static int glyph_width(unsigned cp)
{
	static const struct { unsigned lo, hi; } wide[] = {
		{0x1100, 0x115F}, {0x2E80, 0x303E}, {0x3041, 0x33FF},
		{0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
		{0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
		{0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6},
		{0x1F300, 0x1FAFF}, {0x20000, 0x3FFFD},
	};
	static const struct { unsigned lo, hi; } zero[] = {
		{0x0300, 0x036F}, {0x0483, 0x0489}, {0x1AB0, 0x1AFF},
		{0x200B, 0x200F}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F},
	};
	for (size_t i = 0; i < sizeof wide / sizeof wide[0]; i++)
		if (cp >= wide[i].lo && cp <= wide[i].hi)
			return 2;
	for (size_t i = 0; i < sizeof zero / sizeof zero[0]; i++)
		if (cp >= zero[i].lo && cp <= zero[i].hi)
			return 0;
	return 1;
}

/* decode one UTF-8 codepoint; malformed input falls back to the raw
 * lead byte so nothing is ever skipped or lost */
static unsigned u8_decode(const char *s, size_t rem, size_t *cl)
{
	unsigned char c = (unsigned char)s[0];
	size_t n = u8len(c);
	unsigned cp;
	if (n > rem) {
		*cl = 1;
		return c;
	}
	switch (n) {
	case 2: cp = c & 0x1f; break;
	case 3: cp = c & 0x0f; break;
	case 4: cp = c & 0x07; break;
	default: *cl = 1; return c;
	}
	for (size_t i = 1; i < n; i++) {
		unsigned char cc = (unsigned char)s[i];
		if ((cc & 0xC0) != 0x80) {
			*cl = 1;
			return c;
		}
		cp = cp << 6 | (cc & 0x3f);
	}
	*cl = n;
	return cp;
}

/* index of the first byte with the high bit set at or after i, else n;
 * movemask on the raw bytes extracts all eight high bits at once */
#if USE_SSE2
static size_t first_wide_byte(const char *s, size_t n, size_t i)
{
	for (; i + 16 <= n; i += 16) {
		unsigned m = (unsigned)_mm_movemask_epi8(
			_mm_loadu_si128((const __m128i *)(const void *)(s + i)));
		if (m)
			return i + (size_t)__builtin_ctz(m);
	}
	while (i < n && !((unsigned char)s[i] & 0x80))
		i++;
	return i;
}
#endif

size_t str_cols(const char *s, size_t n)
{
	size_t w = 0, i = 0;
	while (i < n) {
#if USE_SSE2
		size_t j = first_wide_byte(s, n, i);
		w += j - i;	/* pure-ASCII run: one cell per byte */
		i = j;
#else
		if ((unsigned char)s[i] < 0x80) {
			size_t j = i;
			while (j < n && (unsigned char)s[j] < 0x80)
				j++;
			w += j - i;
			i = j;
			continue;
		}
#endif
		if (i < n) {
			size_t cl;
			w += (size_t)glyph_width(u8_decode(s + i, n - i, &cl));
			i += cl;
		}
	}
	return w;
}

/* widths are computed at push time and immutable thereafter; widest_col
 * just reads the running max kept alongside them */
size_t widest_col(void)
{
	return wc_max;
}

size_t line_cols(Line *L)
{
	return L->wcols;
}

size_t line_rows(Line *L)
{
	if (!wrap)
		return 1;
	size_t n = (line_cols(L) + (size_t)cols - 1) / (size_t)cols;
	return n ? n : 1;
}


/* like memmem/strstr but works on non-NUL-terminated spans */
static const char *case_find(const char *s, size_t n, const char *needle)
{
	size_t m = strlen(needle);
	if (m == 0 || n < m)
		return NULL;
	for (size_t i = 0; i + m <= n; i++)
		if (!strncasecmp(s + i, needle, m))
			return s + i;
	return NULL;
}

static const char *severity(const char *s, size_t n)
{
	/* a keyword counts only when the preceding byte isn't alnum, '-'
	 * or '/': "--debug", "/var/debug", "terrain" stay plain;
	 * "level=debug", "<warn>", "errors" still match */
	if (nocolor)
		return "";
	for (size_t i = 0; i < sizeof sev_palette / sizeof sev_palette[0]; i++) {
		const char *p = s;
		while ((p = case_find(p, (size_t)(s + n - p), sev_palette[i].w)) != NULL) {
			if (p == s || (!isalnum((unsigned char)p[-1]) &&
					      p[-1] != '-' && p[-1] != '/'))
				return sev_palette[i].a;
			p += strlen(sev_palette[i].w);
		}
	}
	return "";
}

typedef struct {
	int so, se;
	const char *attr;
} Span;

#define LINE_SPANS 24

/* insert keeping spans sorted by start; overlapping earlier spans win */
static void add_span(Span *sp, int *n, int so, int se, const char *attr)
{
	if (so < 0 || se <= so || *n >= LINE_SPANS)
		return;
	int i = 0;
	while (i < *n && sp[i].so <= so)
		i++;
	if (i > 0 && so < sp[i - 1].se)
		return;
	if (i < *n && se > sp[i].so)
		return;
	memmove(sp + i + 1, sp + i, (size_t)(*n - i) * sizeof *sp);
	sp[i].so = so;
	sp[i].se = se;
	sp[i].attr = attr;
	(*n)++;
}

/* <info>-style verbosity tokens get their own hue; warn/error/etc. are
 * already covered by the whole-line severity pass */
static const char *token_attr(const char *s, size_t n)
{
	if (nocolor || n == 0)
		return NULL;
	for (size_t i = 0; i < sizeof token_palette / sizeof token_palette[0]; i++)
		if (strlen(token_palette[i].w) == n &&
		    !strncasecmp(s, token_palette[i].w, n))
			return token_palette[i].a;
	return NULL;
}

/* structural tinting for whatever shape the log has; format-less*/
static int collect_spans(const Line *L, Span *sp)
{
	int n = 0;
	const char *s = L->s;

	if (nocolor)
		return 0;
	if (L->tag_so > 0)
		add_span(sp, &n, 0, L->tag_so, DIM);
	if (L->slot >= 0)
		add_span(sp, &n, L->tag_so, L->tag_eo,
			 svc_palette[L->slot]);

	size_t i = L->tag_eo >= 0 ? (size_t)L->tag_eo : 0;
	/* <level> verbosity token somewhere shortly after the tag */
	size_t win = i + 64 <= L->len ? i + 64 : L->len;
	for (size_t k = i; k < win; k++) {
		if (s[k] != '<')
			continue;
		size_t j = k + 1;
		while (j < L->len && s[j] != '>' && j - k < 32)
			j++;
		if (j < L->len && s[j] == '>') {
			const char *a = token_attr(s + k + 1, j - k - 1);
			if (a)
				add_span(sp, &n, (int)k, (int)j + 1, a);
		}
		break;
	}
	/* epoch stamps: [1234567890.1234]; short [pid]-style groups stay
	 * untouched, so require several digits */
	int ep = 0;
	for (size_t k = i; k < L->len && ep < 2; k++) {
		if (s[k] != '[')
			continue;
		size_t j = k + 1;
		while (j < L->len && s[j] == ' ')
			j++;	/* dmesg pads: [    0.403686] */
		while (j < L->len && ((s[j] >= '0' && s[j] <= '9') ||
				      s[j] == '.'))
			j++;
		if (j < L->len && s[j] == ']' && j - k >= 7) {
			add_span(sp, &n, (int)k, (int)j + 1, DIM);
			ep++;
			k = j;
		}
	}

	/* double and single quotes */
	int q = 0;
	for (size_t k = 0; k < L->len && q < 12; k++) {
		char qc = s[k];
		if (qc != '"' && qc != '\'')
			continue;
		if (qc == '\'' && k > 0 &&
		    isalnum((unsigned char)s[k - 1]))
			continue;
		size_t j = k + 1;
		while (j < L->len && s[j] != qc)
			j++;
		if (j >= L->len)
			continue;
		if (qc == '\'' && j + 1 < L->len &&
		    isalnum((unsigned char)s[j + 1]))
			continue;
		add_span(sp, &n, (int)k, (int)j + 1, QUOTE_COLOR);
		q++;
		k = j;
	}
	/* parenthesis groups, nesting included; unbalanced ones stay plain.
	 * Shares the quotes' budget of 12 spans per line */
	for (size_t k = 0; k < L->len && q < 12; k++) {
		if (s[k] != '(')
			continue;
		int depth = 0;
		size_t j = k;
		for (; j < L->len; j++) {
			if (s[j] == '(')
				depth++;
			else if (s[j] == ')' && --depth == 0)
				break;
		}
		if (j >= L->len)
			continue;
		add_span(sp, &n, (int)k, (int)(j + 1), PAREN_COLOR);
		q++;
		k = j;
	}
	return n;
}

/* draw one logical line into the pane starting at row r0 (0-based),
 * using at most vmax rows; returns rows consumed. Wrap mode continues
 * on the next row; otherwise it stops at the right edge (hscroll). */
static size_t draw_line(size_t idx, int iscur, size_t r0, size_t vmax)
{
	Line *L = &lines[idx];
	if (!L->sev)
		L->sev = severity(L->s, L->len);	/* scan once, lines are immutable */
	const char *col = L->sev;
	regmatch_t m;
	int ms = -1, me = -1;
	int ss = -1, se = -1;	/* search-match overlay, like the filter's */
	m.rm_so = 0;
	m.rm_eo = (regoff_t)L->len;	/* REG_STARTEND: no NUL needed */
	if (!nocolor && L->srchit) {
		regmatch_t sm;
		if (search_match(L, &sm)) {
			ss = (int)sm.rm_so;
			se = (int)sm.rm_eo;
		}
	}
	if (query_match(L, &m)) {
		ms = (int)m.rm_so;
		me = (int)m.rm_eo;
	}

	Span sp[LINE_SPANS];
	size_t nsp = (size_t)collect_spans(L, sp);

	const Span *act = NULL;
	int inv = 0;	/* filter-match inverse state, synced across BASE() */
	int sinv = 0;	/* search-highlight overlay state */
	size_t si = 0;
	size_t b = 0, dc = 0, sc = 0, row = r0;

	/* base style = cursor/mark bg + severity fg + both match overlays */
#define BASE() do { \
		fputs("\x1b[0m", stdout); \
		if (iscur) \
			fputs(nocolor ? "\x1b[7m" : "\x1b[100m", stdout); \
		else if (L->marked) \
			fputs(mark_bg, stdout); \
		if (*col) \
			fputs(col, stdout); \
		inv = ms >= 0 && b >= (size_t)ms && b < (size_t)me; \
		sinv = ss >= 0 && b >= (size_t)ss && b < (size_t)se; \
		if (inv || sinv) \
			fputs("\x1b[7m", stdout); \
	} while (0)

	printf("\x1b[%zu;1H", row + 1);
	BASE();

	while (b < L->len) {
		size_t cl;
		int gw = glyph_width(u8_decode(L->s + b, L->len - b, &cl));
		if ((wrap ? sc + (size_t)gw > (size_t)cols
			   : dc + (size_t)gw > (size_t)hscroll + (size_t)cols)) {
			if (!wrap)
				break;
			if (sc == 0) {	/* lone glyph wider than a whole row */
				fputc('?', stdout);
				b += cl;
				dc += (size_t)gw;
				sc = 1;
				continue;
			}
			if (row + 1 >= vmax)
				break;
			for (size_t k = sc; k < (size_t)cols; k++)
				fputc(' ', stdout);
			row++;
			printf("\x1b[%zu;1H", row + 1);
			BASE();
			if (act && !sinv)	/* span styling doesn't survive reset */
				fputs(act->attr, stdout);
			sc = 0;
			continue;
		}
		if (!wrap && dc < (size_t)hscroll) {
			b += cl;
			dc += (size_t)gw;
			continue;
		}
		if (act && (size_t)act->se <= b) {
			act = NULL;
			BASE();
		}
		while (si < nsp && (size_t)sp[si].se <= b)
			si++;	/* spans that ended off-screen */
		if (!act && si < nsp && (size_t)sp[si].so <= b) {
			act = &sp[si++];
			if (!sinv)	/* search overlay outranks spans */
				fputs(act->attr, stdout);
		}
		int want_inv = ms >= 0 && b >= (size_t)ms && b < (size_t)me;
		int want_sinv = ss >= 0 && b >= (size_t)ss && b < (size_t)se;
		if (want_inv != inv || want_sinv != sinv) {
			BASE();	/* repaint the whole stack on any overlay edge */
			if (act && !sinv)	/* span yields to the search overlay */
				fputs(act->attr, stdout);
		}
		fwrite(L->s + b, 1, cl, stdout);
		b += cl;
		dc += (size_t)gw;
		sc += (size_t)gw;
	}
	BASE();
#undef BASE
	/* pad to viewport width so shorter lines erase longer predecessors */
	for (size_t k = sc; k < (size_t)cols; k++)
		fputc(' ', stdout);
	fputs("\x1b[0m", stdout);
	return row - r0 + 1;
}

/* top bar: one inverse strip -- source, position and state on the left,
 * key reference right-aligned. A hint that can't fit is hidden whole,
 * never shrunk into noise. */
static void draw_status_bar(void)
{
	if (!have_status_bar())
		return;
	static const char *const hints[] = {
		"/ filter  ? clear  spc/x mark  c copy  f follow  w wrap  r reload  q quit",
		"/ filter  ? clear  spc/x mark  c copy  f follow  r reload  q quit",
		"/ filter  spc mark  c copy  q quit",
	};
	const char *src = use_stdin ? "(stdin)" : path;
	char left[512];

	char where[64];
	snprintf(where, sizeof where, "%zu/%zu", nv ? cur + 1 : 0, nv);

	/* fit within the row: shrink the path, then the query, then drop
	 * decorations; a wrapped bar would scroll the pane and swallow
	 * a content row */
	int budget = cols - 2;
	const char *flw = follow && !use_stdin ? "  follow" : "";
	const char *rmk = re_mode ? "  (R)" : "";
	const char *imk = filter_inv ? "  (!)" : "";
	char mk[32];
	mk[0] = 0;
	if (nmarked)
		snprintf(mk, sizeof mk,
			 budget >= 16 ? "  %zu marked" : " *%zu", nmarked);
	size_t sl = strlen(src), ql = filter_pat.active ? strlen(filter_pat.text) : 0;
	for (;;) {
		snprintf(left, sizeof left, " %.*s  %s%s%s%s%s%s%.*s%s",
			 (int)sl, src, where, mk, flw, rmk, imk,
			 filter_pat.active ? "  /" : "", (int)ql, filter_pat.text,
			 (nv == 0 && filter_pat.active) ? "  (no matches)" : "");
		if ((int)strlen(left) <= budget)
			break;
		if (sl) {	/* path yields first; position and marks stay */
			sl /= 2;
			while (sl && ((unsigned char)src[sl] & 0xC0) == 0x80)
				sl--;
		} else if (ql > 4) {
			ql -= ql / 4 + 1;
			while (ql && ((unsigned char)filter_pat.text[ql] & 0xC0) == 0x80)
				ql--;
		} else if (*flw) {
			flw = "";
		} else if (*rmk) {
			rmk = "";
		} else if (*imk) {
			imk = "";
		} else {
			break;	/* only position (+marks) left: clamp below */
		}
	}
	int lw = (int)strlen(left);
	if (lw > budget && budget >= 0) {
		while (budget > 0 &&
		       ((unsigned char)left[budget] & 0xC0) == 0x80)
			budget--;
		left[budget] = 0;
		lw = budget;
	}
	/* longest reference that fits whole; none if the row is too tight */
	const char *hint = NULL;
	int hw = 0;
	for (size_t k = 0; k < sizeof hints / sizeof hints[0]; k++) {
		int len = (int)strlen(hints[k]);
		if (lw + len + 2 <= cols) {
			hint = hints[k];
			hw = len;	/* only set when actually shown */
			break;
		}
	}

	printf("\x1b[%d;1H\x1b[1;7m",
	       have_input_bar() ? rows - 1 : rows);
	fputs(left, stdout);
	for (int i = 0; i < cols - lw - hw - 1; i++)
		fputc(' ', stdout);
	if (hint)
		fputs(hint, stdout);
	fputc(' ', stdout);
	fputs("\x1b[0m", stdout);
}

/* input bar: the vim-style filter prompt while editing, transient notices
 * (bad regex, mode flips, copy confirmations) otherwise. The notice is
 * right-aligned on this row so it never hides behind prompt content: pad
 * from column curcol, then inverse video ending at the right edge. */
static void notice(int curcol)
{
	if (!*msg)
		return;
	int nw = (int)strlen(msg);
	int room = cols - curcol + 1;
	if (nw > room)
		return;
	for (int i = 0; i < room - nw; i++)
		fputc(' ', stdout);
	fputs("\x1b[1;7m", stdout);
	fwrite(msg, 1, (size_t)nw, stdout);
	fputs("\x1b[0m", stdout);
}

static void draw_input_bar(void)
{
	if (!editing && !have_input_bar()) {
		fputs("\x1b[?25l", stdout);
		return;
	}
	printf("\x1b[%d;1H\x1b[K", rows);

	if (editing) {
		/* window [off,eend) of the edit buffer: must hold the cursor
		 * and fit maxw cells, clipping both sides. The cursor pins
		 * toward the right edge while scrolling left. */
		size_t elen = strlen(edit);
		int pfx = 2 + (re_mode ? 1 : 0) +
			  (!editing_search && filter_inv ? 1 : 0);
		int maxw = cols - (pfx + 2) > 1 ? cols - (pfx + 2) : 1;
		size_t off = 0;
		while (off < elen &&
		       str_cols(edit + off, ecur - off) > (size_t)(maxw - 1)) {
			size_t cl;
			u8_decode(edit + off, elen - off, &cl);
			off += cl;
		}
		size_t eend = off, acc = 0;
		while (eend < elen) {
			size_t cl;
			int gw = glyph_width(u8_decode(edit + eend,
						       elen - eend, &cl));
			if (acc + (size_t)gw > (size_t)maxw)
				break;
			acc += gw;
			eend += cl;
		}
		int cw = (int)str_cols(edit + off, ecur - off);
		fputs("\x1b[1;7m ", stdout);
		fputc(editing_search ? '\\' : '/', stdout);
		if (re_mode)
			fputc('r', stdout);
		if (!editing_search && filter_inv)
			fputc('!', stdout);
		fwrite(edit + off, 1, eend - off, stdout);
		fputs(" \x1b[0m", stdout);
		/* cursor sits on the char right of it, or on our trailing
		 * space when it is at the end of the window */
		int pw = pfx + cw + 1;
		notice(pw + 1);
		printf("\x1b[%d;%dH\x1b[?25h", rows, pw);
		return;
	}

	notice(1);
	fputs("\x1b[?25l", stdout);
}

void render(void)
{
	size_t vis = pane_rows();
	fputs("\x1b[H", stdout);
	size_t r = 0, i = top;
	for (; i < nv && r < vis; i++)
		r += draw_line(view[i], i == cur, r, vis);
	if (r < vis) {
		printf("\x1b[%zu;1H", r + 1);
		if (nv == 0 && !filter_pat.active)
			fputs("(empty)", stdout);
		fputs("\x1b[J", stdout);
	}
	/* scrollbar: right-edge rail over the full line count; '|' is the
	 * window thumb (always visible), '-' dashes the track, '#' marks
	 * track rows holding search hits outside the window */
	if (cols > 1 && (search_pat.active || nv > vis) && nv > 0) {
		size_t drew = i - top;
		size_t tlo = nv > vis ? top * vis / nv : 0;
		size_t thi = nv > vis ? (top + drew) * vis / nv : vis;
		if (thi <= tlo)
			thi = tlo + 1;
		fputs("\x1b[0m", stdout);
		for (size_t sr = 0; sr < vis; sr++) {
			int hit = 0;
			if (search_pat.active) {
				size_t lo = sr * nv / vis;
				size_t hi2 = (sr + 1) * nv / vis;
				if (hi2 <= lo)
					hi2 = lo + 1;
				for (size_t k = lo; k < hi2 && k < nv; k++)
					if (lines[view[k]].srchit) {
						hit = 1;
						break;
					}
			}
			char c = sr >= tlo && sr < thi ? '|' : '-';
			printf("\x1b[%zu;%zuH", sr + 1, (size_t)cols);
			if (hit)
				fputs("\x1b[7m", stdout);
			fputc(c, stdout);
			fputs("\x1b[0m", stdout);
		}
	}
	draw_status_bar();
	draw_input_bar();
	fflush(stdout);
}
