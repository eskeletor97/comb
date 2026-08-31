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

/* widest_col() is a conservative bound: the max raw line length, refined
 * with measured widths as lines are materialised. Kept so the horizontal-
 * scroll cap never stops short of real content. */
size_t widest_col(void)
{
	return wc_max;
}

size_t line_cols(size_t i)
{
	Line lz;
	lt_fill(&lz, i);
	if (lz.trunc)
		return lz.wcols + str_cols(LINE_TRUNC_TAIL, sizeof(LINE_TRUNC_TAIL) - 1);
	return lz.wcols;
}

size_t line_rows(size_t i)
{
	if (!wrap)
		return 1;
	size_t n = (line_cols(i) + (size_t)cols - 1) / (size_t)cols;
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
/* total bytes the quote/paren scans may chase per line. Unbalanced brackets
 * would otherwise rescan to end-of-line from every position, turning one
 * tall line into a quadratic frame (a 16k-char `((((` line was ~35ms/redraw). */
#define SPAN_SCAN_BUDGET 4096

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
	int q = 0, budget = SPAN_SCAN_BUDGET;
	for (size_t k = 0; k < L->len && q < 12 && budget > 0; k++) {
		char qc = s[k];
		if (qc != '"' && qc != '\'')
			continue;
		if (qc == '\'' && k > 0 &&
		    isalnum((unsigned char)s[k - 1]))
			continue;
		size_t j = k + 1, lim = k + (size_t)budget;
		if (lim > L->len)
			lim = L->len;
		while (j < lim && s[j] != qc)
			j++;
		budget -= (int)(j - k);
		if (j >= L->len || s[j] != qc)
			continue;
		if (qc == '\'' && j + 1 < L->len &&
		    isalnum((unsigned char)s[j + 1]))
			continue;
		add_span(sp, &n, (int)k, (int)j + 1, QUOTE_COLOR);
		q++;
		k = j;
	}
	/* parenthesis groups, nesting included; unbalanced ones stay plain.
	 * Shares the quotes' budget of 12 spans per line, and the scan budget
	 * above so a line of stray '(' costs a bounded amount of work. */
	for (size_t k = 0; k < L->len && q < 12 && budget > 0; k++) {
		if (s[k] != '(')
			continue;
		int depth = 0;
		size_t j = k, lim = k + (size_t)budget;
		if (lim > L->len)
			lim = L->len;
		for (; j < lim; j++) {
			if (s[j] == '(')
				depth++;
			else if (s[j] == ')' && --depth == 0)
				break;
		}
		budget -= (int)(j - k);
		if (depth != 0 || j >= L->len)
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
	Line lz;
	Line *L = &lz;
	lt_fill(L, idx);
	if (!L->sev)
		L->sev = severity(L->s, L->len);	/* L is fresh each call */
	const char *col = L->sev;
	regmatch_t m;
	int ms = -1, me = -1;
	int ss = -1, se = -1;	/* search-match overlay, like the filter's */
	m.rm_so = 0;
	m.rm_eo = (regoff_t)L->len;	/* REG_STARTEND: no NUL needed */
	if (!nocolor && L->srchit) {
		regmatch_t sm;
		if (search_match(L->s, L->len, &sm)) {
			ss = (int)sm.rm_so;
			se = (int)sm.rm_eo;
		}
	}
	if (query_match(L->s, L->len, &m)) {
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
		unsigned cp = u8_decode(L->s + b, L->len - b, &cl);
		int gw = glyph_width(cp);
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
		/* Last line of defense: C0 is caret-escaped in sanitize, so reaching
		 * here with a control means a standalone C1 byte (0x80-0x9f), which an
		 * 8-bit terminal would treat as an ESC-based control sequence. Emit an
		 * inert one-cell glyph instead; a valid UTF-8 char (cl > 1) is untouched. */
		if (cp < 0x20 || cp == 0x7f || (cl == 1 && cp >= 0x80 && cp <= 0x9f)) {
			fputs("\xef\xbf\xbd", stdout);	/* U+FFFD */
			b += cl;
			dc += (size_t)gw;
			sc += (size_t)gw;
			continue;
		}
		fwrite(L->s + b, 1, cl, stdout);
		b += cl;
		dc += (size_t)gw;
		sc += (size_t)gw;
	}
	/* The truncation marker is a display tail, not log content: it is not
	 * matched, searched, colored or copied, but it is drawn through the same
	 * wrap/scroll accounting above, so the rows here agree with line_rows(). */
	if (L->trunc) {
		const char *t = LINE_TRUNC_TAIL;
		size_t tn = sizeof(LINE_TRUNC_TAIL) - 1, q = 0;
		while (q < tn) {
			size_t cl;
			int gw = glyph_width(u8_decode(t + q, tn - q, &cl));
			if ((wrap ? sc + (size_t)gw > (size_t)cols
				 : dc + (size_t)gw > (size_t)hscroll + (size_t)cols)) {
				if (!wrap)
					break;
				if (row + 1 >= vmax)
					break;
				for (size_t k = sc; k < (size_t)cols; k++)
					fputc(' ', stdout);
				row++;
				printf("\x1b[%zu;1H", row + 1);
				BASE();
				sc = 0;
				continue;
			}
			if (!wrap && dc < (size_t)hscroll) {
				q += cl;
				dc += (size_t)gw;
				continue;
			}
			fputs("\x1b[2m", stdout);	/* dim the tail so it reads as decoration */
			fwrite(t + q, 1, cl, stdout);
			q += cl;
			dc += (size_t)gw;
			sc += (size_t)gw;
		}
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
		/* fit by terminal cells, not bytes: a UTF-8 path/query overflows
		 * the row even when strlen() looks short */
		if ((int)str_cols(left, strlen(left)) <= budget)
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
	int lw = (int)str_cols(left, strlen(left));
	if (lw > budget && budget >= 0) {
		/* drop whole glyphs until the painted width fits, instead of
		 * cutting a byte count that says more than the cells it paints */
		size_t blen = 0, w = 0, total = strlen(left);
		while (blen < total) {
			size_t cl;
			int gw = glyph_width(u8_decode(left + blen, total - blen, &cl));
			if ((int)(w + (size_t)gw) > budget)
				break;
			w += (size_t)gw;
			blen += cl;
		}
		left[blen] = 0;
		lw = (int)w;
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
 * (bad regex, mode flips, copy confirmations) otherwise. Chips are
 * right-aligned on this row so they never hide behind prompt content:
 * the spinner chip sits flush right (stable column while it animates),
 * the latest message to its left; a cluster that can't fit between the
 * cursor and the edge drops the message first, then skips entirely. */
/* latest message plus the live spinner, one inverse-video cluster flush
 * right on the input bar row (stable column while the spinner animates).
 * All chip text is ASCII: bytes == cells. Like prompt/status bar, the
 * cluster carries a single painted space on each edge. A cluster that
 * can't fit between curcol and the edge drops the message first;
 * returns the cell width drawn, 0 if nothing fits. */
static int chips(int curcol)
{
	const char *spin = job_spin_text();
	int sw = spin ? (int)strlen(spin) : 0;
	int mw = *msg ? (int)str_cols(msg, strlen(msg)) : 0;
	int gap = mw && sw ? 2 : 0;	/* spacer lives between chips only */
	if (mw && mw + gap + sw + 2 > cols - curcol)
		mw = 0;		/* tight fit: message yields to live feedback */
	if ((!mw && !sw) || sw + 2 > cols - curcol)
		return 0;
	gap = mw && sw ? 2 : 0;	/* msg may have just been dropped */
	int w = mw + gap + sw + 2;
	int start = cols - w + 1;
	printf("\x1b[%d;%dH\x1b[1;7m ", rows, start);
	if (mw)
		printf("%s", msg);
	if (gap)
		fputs("  ", stdout);
	if (sw)
		fputs(spin, stdout);
	fputc(' ', stdout);
	fputs("\x1b[0m", stdout);
	return w;
}

/* visible edit-buffer window [off,eend), prefix width and cursor column:
 * must hold the cursor and fit maxw cells, clipping both sides. The
 * cursor pins toward the right edge while scrolling left. */
struct edwin {
	size_t off, eend;
	int pfx, cw;
};

static void edit_window(struct edwin *w)
{
	size_t elen = strlen(edit);
	w->pfx = 2 + (re_mode ? 1 : 0) +
		 (!editing_search && filter_inv ? 1 : 0);
	int maxw = cols - (w->pfx + 2) > 1 ? cols - (w->pfx + 2) : 1;
	w->off = 0;
	while (w->off < elen &&
	       str_cols(edit + w->off, ecur - w->off) > (size_t)(maxw - 1)) {
		size_t cl;
		u8_decode(edit + w->off, elen - w->off, &cl);
		w->off += cl;
	}
	w->eend = w->off;
	int acc = 0;
	while (w->eend < elen) {
		size_t cl;
		int gw = glyph_width(u8_decode(edit + w->eend,
					       elen - w->eend, &cl));
		if (acc + gw > maxw)
			break;
		acc += gw;
		w->eend += cl;
	}
	w->cw = (int)str_cols(edit + w->off, ecur - w->off);
}

/* partial chip-zone repaint from the job-feeding fast path: with no keys
 * arriving, the main loop steps batches without ever reaching render(),
 * so the spinner tick paints here directly. Never touches prompt cells.
 * Chips can only shrink via a keypress or job end, both of which force
 * a full render that clears the row -- no stale-cell handling needed. */
void paint_chips(void)
{
	if (!have_input_bar())
		return;
	int curcol = 1;
	if (editing) {
		struct edwin w;
		edit_window(&w);
		curcol = w.pfx + w.cw + 1;
	}
	if (!chips(curcol))
		return;
	if (editing)
		printf("\x1b[%d;%dH\x1b[?25h", rows, curcol);
	else
		fputs("\x1b[?25l", stdout);
	fflush(stdout);
}

static void draw_input_bar(void)
{
	if (!editing && !have_input_bar()) {
		fputs("\x1b[?25l", stdout);
		return;
	}
	printf("\x1b[%d;1H\x1b[K", rows);

	if (editing) {
		struct edwin w;
		edit_window(&w);
		fputs("\x1b[1;7m ", stdout);
		fputc(editing_search ? '\\' : '/', stdout);
		if (re_mode)
			fputc('r', stdout);
		if (!editing_search && filter_inv)
			fputc('!', stdout);
		fwrite(edit + w.off, 1, w.eend - w.off, stdout);
		fputs(" \x1b[0m", stdout);
		/* cursor sits on the char right of it, or on our trailing
		 * space when it is at the end of the window */
		int pw = w.pfx + w.cw + 1;
		chips(pw + 1);
		printf("\x1b[%d;%dH\x1b[?25h", rows, pw);
		return;
	}

	chips(1);
	fputs("\x1b[?25l", stdout);
}

void render(void)
{
	size_t vis = pane_rows();
	fputs("\x1b[H", stdout);
	size_t r = 0, i = top;
	for (; i < nv && r < vis; i++)
		r += draw_line(view_at(i), i == cur, r, vis);
	if (r < vis) {
		printf("\x1b[%zu;1H", r + 1);
		if (nv == 0 && !filter_pat.active)
			fputs("(empty)", stdout);
		fputs("\x1b[J", stdout);
	}
	/* scrollbar: right-edge rail over the full line count; '|' is the
	 * window thumb (always visible), '-' dashes the track, '#' marks
	 * track rows holding search hits outside the window.
	 *
	 * The hit marks depend only on view[]/nv/vis, never on top, so they
	 * are cached per view_epoch: rebuilding per frame would rescan every
	 * view entry on each keystroke of a huge filtered view. */
	static unsigned char *rail;
	static size_t rail_cap, rail_nv;
	static int rail_vis;
	static uint64_t rail_gen;
	if (cols > 1 && (search_pat.active || nv > vis) && nv > 0) {
		size_t drew = i - top;
		size_t tlo = nv > vis ? top * vis / nv : 0;
		size_t thi = nv > vis ? (top + drew) * vis / nv : vis;
		if (thi <= tlo)
			thi = tlo + 1;
		if (search_pat.active &&
		    (rail_gen != view_epoch || rail_nv != nv ||
		     rail_vis != (int)vis)) {
			if (rail_cap < vis) {
				rail_cap = vis;
				rail = xrealloc(rail, rail_cap);
			}
			for (size_t sr = 0; sr < vis; sr++) {
				size_t lo = sr * nv / vis;
				size_t hi2 = (sr + 1) * nv / vis;
				if (hi2 <= lo)
					hi2 = lo + 1;
				unsigned char hit = 0;
				for (size_t k = lo; k < hi2 && k < nv; k++)
					if (hit_at(view_at(k))) {
						hit = 1;
						break;
					}
				rail[sr] = hit;
			}
			rail_gen = view_epoch;
			rail_nv = nv;
			rail_vis = (int)vis;
		}
		fputs("\x1b[0m", stdout);
		for (size_t sr = 0; sr < vis; sr++) {
			char c = sr >= tlo && sr < thi ? '|' : '-';
			printf("\x1b[%zu;%zuH", sr + 1, (size_t)cols);
			if (search_pat.active && rail[sr])
				fputs("\x1b[7m", stdout);
			fputc(c, stdout);
			fputs("\x1b[0m", stdout);
		}
	}
	draw_status_bar();
	draw_input_bar();
	fflush(stdout);
}
