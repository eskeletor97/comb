/* comb - a small terminal log viewer: highlight, filter, follow, copy.
 * build: cc -O2 -Wall -Wextra -o comb comb.c
 */
#define _GNU_SOURCE
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
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define MAX_QUERY 256
#define MARK_BG "\x1b[48;5;238m"
#define TTY_ENTER "\x1b[?1049h\x1b[?25l\x1b[2J"
#define TTY_LEAVE "\x1b[0m\x1b[?25h\x1b[?1049l"
#define VAL_COLOR "\x1b[38;5;223m"	/* quoted values */
#define PAREN_COLOR "\x1b[38;5;115m"	/* (...) context */
#define DIM "\x1b[2m"

typedef struct {
	char *s;
	size_t len;
	int tag_so, tag_eo;	/* byte span of the service tag, -1 if none */
	int slot;		/* svc_palette index, -1 if no tag */
	unsigned char marked;
	size_t wcols;		/* display width cache, 0 = uncomputed */
	const char *sev;	/* severity SGR cache, NULL = unscanned */
} Line;

static Line *lines;
static size_t nlines, lcap;

static size_t *view;
static size_t nv, vcap;

static char path[4096];
static int use_stdin;
static int fd = -1;      /* log file */
static int kfd = 0;      /* keyboard: stdin, or /dev/tty when stdin isn't a tty */
static off_t fsize;

static regex_t re;
static int filtered;
static char query[MAX_QUERY];
static char edit[MAX_QUERY];
static int editing;

static int follow = 1;
static int wrap;
static int nocolor;
static const char *mark_bg = MARK_BG;
static int running = 1;
static int dirty = 1;

static size_t widest;	/* display cols of the widest line seen */
static size_t cur, top;
static size_t filter_anchor;	/* line selected when filtering began */
static size_t filter_row;	/* its screen row, restored on clear */
static int hscroll;
static int rows = 24, cols = 80;

static char msg[160];
static struct termios saved_tio;
static int tio_saved;
static volatile sig_atomic_t got_winch;
static size_t nmarked;
static int mdir = -1;	/* space/x sweep direction: -1 up, 1 down */

static void assign_service(Line *L);
static size_t str_cols(const char *s, size_t n);

enum {
	K_NONE = 0x100, K_EOF, K_UP, K_DOWN, K_LEFT, K_RIGHT,
	K_PGUP, K_PGDN, K_HOME, K_END
};

/*
 * Key bindings -- edit to taste; usage()'s keys section is generated
 * from this table. Keys are whatever read_key() returns: plain
 * characters, CTL() control codes, or the K_* specials. The filter
 * prompt keeps its own fixed editing keys (type, Backspace, Ctrl-u,
 * Enter, Esc).
 */
#define CTL(x)	((x) & 0x1f)
enum {
	A_NONE, A_QUIT, A_REPAINT,
	A_DOWN, A_UP, A_PGDOWN, A_PGUP, A_TOP, A_BOT,
	A_LEFT, A_RIGHT, A_HSTART, A_HEND,
	A_FILTER, A_CLEAR_FILTER, A_CANCEL,
	A_MARK, A_UNMARK, A_COPY,
	A_FOLLOW, A_RELOAD, A_WRAP, A_STOP,
};

static const struct { int key; int act; } keymap[] = {
	{ 'q',	      A_QUIT },
	{ CTL('c'),   A_QUIT },

	/* vertical movement; sets the space/x sweep direction */
	{ 'j',	      A_DOWN },
	{ 'e',	      A_DOWN },
	{ '\r',	      A_DOWN },
	{ '\n',	      A_DOWN },
	{ K_DOWN,	     A_DOWN },
	{ 'k',	      A_UP },
	{ K_UP,	   A_UP },
	{ 'd',	      A_PGDOWN },
	{ CTL('d'),   A_PGDOWN },
	{ K_PGDN,	     A_PGDOWN },
	{ CTL('f'),   A_PGDOWN },
	{ 'u',	      A_PGUP },
	{ CTL('u'),   A_PGUP },
	{ K_PGUP,	     A_PGUP },
	{ 'b',	      A_PGUP },
	{ CTL('b'),   A_PGUP },
	{ 'g',	      A_TOP },
	{ K_HOME,	     A_TOP },
	{ 'G',	      A_BOT },
	{ K_END,	   A_BOT },

	/* horizontal movement */
	{ 'h',	      A_LEFT },
	{ K_LEFT,	     A_LEFT },
	{ 'l',	      A_RIGHT },
	{ K_RIGHT,	    A_RIGHT },
	{ '0',	      A_HSTART },
	{ '^',	      A_HSTART },
	{ '$',	      A_HEND },

	/* filter / marks / clipboard */
	{ '/',	      A_FILTER },
	{ '?',	      A_CLEAR_FILTER },
	{ 0x1b,	     A_CANCEL },	/* clears marks first, else the filter */
	{ ' ',	      A_MARK },
	{ 'x',	      A_UNMARK },
	{ 'c',	      A_COPY },
	{ 'y',	      A_COPY },

	{ CTL('z'),   A_STOP },

	/* toggles */
	{ 'f',	      A_FOLLOW },
	{ 'w',	      A_WRAP },
	{ 'r',	      A_RELOAD },
	{ CTL('l'),   A_REPAINT },
};

static int key_action(int key)
{
	for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
		if (keymap[i].key == key)
			return keymap[i].act;
	return A_NONE;
}

static void on_winch(int sig)
{
	(void)sig;
	got_winch = 1;
}

static void restore_terminal(void);

static void die(const char *fmt, ...)
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
static void die_sys(const char *fmt, ...)
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

static void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		die("out of memory");
	return q;
}

/* --- terminal --- */

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

/* undo raw mode + alternate screen; safe from die() at any point */
static void restore_terminal(void)
{
	if (!tio_saved)
		return;
	fputs(TTY_LEAVE, stdout);
	fflush(stdout);
	tcsetattr(kfd, TCSANOW, &saved_tio);
}

/* emergency terminal restore on SIGTERM/SIGHUP/SIGINT: only
 * async-signal-safe calls, unlike restore_terminal()'s stdio */
static void on_sigexit(int sig)
{
	if (tio_saved) {
		tcsetattr(kfd, TCSANOW, &saved_tio);
		write(STDOUT_FILENO, TTY_LEAVE, sizeof TTY_LEAVE - 1);
	}
	_exit(128 + sig);
}

static void get_winsize(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
		cols = ws.ws_col;
		rows = ws.ws_row;
	}
}

/* (re)acquire the terminal: raw mode + fresh alternate screen */
static void tty_enter(void)
{
	raw_on();
	get_winsize();
	fputs(TTY_ENTER, stdout);
	fflush(stdout);
}

/* --- input feeding / line storage --- */

static char *pend;
static size_t plen;
static int flushed_partial;	/* last pushed line had no trailing newline */

static void push_line(char *clean, size_t len)
{
	if (nlines == lcap) {
		lcap = lcap ? lcap * 2 : 1024;
		lines = xrealloc(lines, lcap * sizeof(*lines));
	}
	lines[nlines].s = clean;
	lines[nlines].len = len;
	lines[nlines].marked = 0;
	lines[nlines].wcols = 0;
	lines[nlines].sev = NULL;
	assign_service(&lines[nlines]);
	{
		Line *L = &lines[nlines];
		size_t w = str_cols(L->s, L->len);
		L->wcols = w;	/* cache; draw time would recompute anyway */
		if (w > widest)
			widest = w;
	}
	nlines++;
}

/* strip ANSI sequences and CRs, expand tabs; returns malloc'd string.
 * Worst case is tab expansion (+3 bytes each); everything else shrinks. */
static char *sanitize(const char *s, size_t n, size_t *outlen)
{
	size_t extra = 0;
	for (size_t i = 0; i < n; i++)
		if (s[i] == '\t')
			extra += 3;
	char *o = xrealloc(NULL, n + extra + 1);
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
	size_t start = 0;
	/* If a final partial line was flushed as a line, a leading '\n' merely
	 * terminates it; don't emit a spurious empty line for it. */
	if (flushed_partial && plen > 0 && pend[0] == '\n')
		start = 1;
	flushed_partial = 0;
	for (size_t i = start; i < plen; i++) {
		if (pend[i] == '\n') {
			size_t len;
			char *clean = sanitize(pend + start, i - start, &len);
			push_line(clean, len);
			start = i + 1;
		}
	}
	memmove(pend, pend + start, plen - start);
	plen -= start;
}

/* push a trailing partial line (no '\n') as a real line, so a last
 * record lacking a newline still shows */
static void flush_pending(void)
{
	if (plen == 0)
		return;
	size_t len;
	char *clean = sanitize(pend, plen, &len);
	push_line(clean, len);
	plen = 0;
	flushed_partial = 1;
}

/* per-service pastels, assigned by rotation in order of first appearance
 * so neighboring services never share a color */
static const char *const svc_palette[] = {
	"\x1b[38;5;215m",	/* peach */
	"\x1b[38;5;79m",	/* seafoam */
	"\x1b[38;5;110m",	/* cornflower */
	"\x1b[38;5;146m",	/* lilac */
	"\x1b[38;5;151m",	/* sage */
	"\x1b[38;5;179m",	/* honey */
	"\x1b[38;5;173m",	/* coral */
	"\x1b[38;5;80m",	/* sky */
	"\x1b[38;5;182m",	/* mauve */
	"\x1b[38;5;144m",	/* sand */
	"\x1b[38;5;115m",	/* celadon */
	"\x1b[38;5;250m",	/* silver */
};
#define NSVC_COLORS (sizeof svc_palette / sizeof svc_palette[0])

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
	size_t i = 0;
	int field = 0, saw_digit = 0;
	size_t pfs = (size_t)-1;	/* start of the previous field */
	while (i < n) {
		while (i < n && s[i] == ' ')
			i++;
		if (i >= n)
			break;
		size_t fs = i;
		while (i < n && s[i] != ' ')
			i++;
		size_t fe = i;
		size_t pf = pfs;
		pfs = fs;
		int digit = 0;
		for (size_t k = fs; k < fe; k++)
			if (s[k] >= '0' && s[k] <= '9') {
				digit = 1;
				break;
			}
		saw_digit |= digit;
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

/* Assign the palette slot for this line's tag. Runs at push time: slots
 * follow order of appearance in the file, and render-time assignment
 * reshuffles colors on reload (draw order differs from file order). */
static void assign_service(Line *L)
{
	int fs, e;
	L->slot = -1;
	L->tag_so = -1;
	L->tag_eo = -1;
	if (!tag_span(L->s, L->len, &fs, &e))
		return;
	L->tag_so = fs;
	L->tag_eo = e;
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

static void reset_lines(void)
{
	for (size_t i = 0; i < nlines; i++)
		free(lines[i].s);
	nlines = 0;
	plen = 0;
	flushed_partial = 0;
	nmarked = 0;
	widest = 0;
	nv = 0;	/* view entries referenced the freed buffers */
	nsvc_seen = 0;	/* tag pointers referenced the freed buffers */
}

static void read_available(int src)
{
	char buf[65536];
	ssize_t g;
	while ((g = read(src, buf, sizeof buf)) > 0)
		feed(buf, (size_t)g);
}

static void load_all(void)
{
	if (use_stdin) {
		read_available(STDIN_FILENO);
		flush_pending();
		return;
	}
	if (fd < 0) {
		fd = open(path, O_RDONLY);
		if (fd < 0)
			die_sys("cannot open %s", path);
	}
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
	}
	off_t before = lseek(fd, 0, SEEK_CUR);
	read_available(fd);
	off_t now = lseek(fd, 0, SEEK_CUR);
	fsize = now;
	return rotated ? 2 : (now != before);
}

/* --- filtering --- */

/* layout: top bar (source/position/keys), log pane, input bar (prompt
 * or notices); tiny ttys drop the input bar first, then the top bar */
static int have_top_bar(void)
{
	return rows >= 2;
}

static int have_input_bar(void)
{
	return rows >= 3;
}

static size_t pane_rows(void)
{
	int n = rows - have_top_bar() - have_input_bar();
	return (size_t)(n > 0 ? n : 1);
}

static size_t line_rows(Line *L);

static void push_view(size_t i)
{
	if (nv == vcap) {
		vcap = vcap ? vcap * 2 : 1024;
		view = xrealloc(view, vcap * sizeof(*view));
	}
	view[nv++] = i;
}

static void ensure_visible(void)
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

static void rebuild_view(void);

static void update_filter(const char *q)
{
	int was_filtered = filtered, clearing = !*q;
	msg[0] = 0;
	if (clearing) {
		if (filtered)
			regfree(&re);
		filtered = 0;
		query[0] = 0;
	} else {
		int icase = 1;
		for (const char *p = q; *p; p++)
			if (isupper((unsigned char)*p)) {
				icase = 0;
				break;
			}
		regex_t nr;
		int flags = REG_EXTENDED | (icase ? REG_ICASE : 0);
		if (regcomp(&nr, q, flags) != 0) {
			snprintf(msg, sizeof msg, "bad regex: %s", q);
			return;
		}
		if (filtered)
			regfree(&re);
		re = nr;
		filtered = 1;
		snprintf(query, sizeof query, "%s", q);
	}
	rebuild_view();
	if (clearing && was_filtered && filter_anchor < nlines) {
		/* return to the line selected before filtering began; view[]
		 * is sorted, so the line is a binary search away. If it left
		 * with a rotation/reload, keep the clamped position instead. */
		size_t lo = 0, hi = nv;
		while (lo < hi) {
			size_t mid = lo + (hi - lo) / 2;
			if (view[mid] < filter_anchor)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo < nv && view[lo] == filter_anchor) {
			cur = lo;
			/* re-window so the line lands on the row it occupied
			 * before filtering; ensure_visible() keeps this as-is */
			size_t vis = pane_rows();
			size_t max_top = nv > vis ? nv - vis : 0;
			top = lo > filter_row ? lo - filter_row : 0;
			if (top > max_top)
				top = max_top;
		}
	}
	ensure_visible();
}

static void extend_view(size_t from)
{
	for (size_t i = from; i < nlines; i++) {
		if (!filtered || regexec(&re, lines[i].s, 0, NULL, 0) == 0)
			push_view(i);
	}
}

static void rebuild_view(void)
{
	nv = 0;
	extend_view(0);
	ensure_visible();
}

/* --- rendering --- */

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

/* display width in terminal cells */
static size_t str_cols(const char *s, size_t n)
{
	size_t w = 0;
	for (size_t i = 0; i < n; ) {
		size_t cl;
		w += (size_t)glyph_width(u8_decode(s + i, n - i, &cl));
		i += cl;
	}
	return w;
}

/* cached str_cols(); widths never change once a line is stored */
static size_t line_cols(Line *L)
{
	if (!L->wcols)
		L->wcols = str_cols(L->s, L->len);
	return L->wcols;
}

/* pane rows the logical line occupies */
static size_t line_rows(Line *L)
{
	if (!wrap)
		return 1;
	size_t n = (line_cols(L) + (size_t)cols - 1) / (size_t)cols;
	return n ? n : 1;
}


static const char *severity(const char *s)
{
	static const char *sev[][2] = {
		{ "fatal", "\x1b[1;95m" }, { "panic", "\x1b[1;95m" },
		{ "emerg", "\x1b[1;95m" }, { "alert", "\x1b[1;95m" },
		{ "crit",  "\x1b[1;95m" }, { "error", "\x1b[1;91m" },
		{ "err",   "\x1b[1;91m" }, { "warn",  "\x1b[1;93m" },
		{ "debug", "\x1b[2m" },    { "trace", "\x1b[2m" },
		{ NULL, NULL }
	};
	/* a keyword counts only when the preceding byte isn't alnum, '-'
	 * or '/': "--debug", "/var/debug", "terrain" stay plain;
	 * "level=debug", "<warn>", "errors" still match */
	if (nocolor)
		return "";
	for (int i = 0; sev[i][0]; i++) {
		const char *p = s;
		while ((p = strcasestr(p, sev[i][0])) != NULL) {
			if (p == s || (!isalnum((unsigned char)p[-1]) &&
				      p[-1] != '-' && p[-1] != '/'))
				return sev[i][1];
			p += strlen(sev[i][0]);
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
	static const struct { const char *w; const char *a; } tok[] = {
		{ "info",   "\x1b[38;5;110m" },	/* cornflower */
		{ "notice", "\x1b[38;5;79m" },	/* seafoam */
		{ "audit",  "\x1b[38;5;146m" },	/* lilac */
	};
	if (nocolor || n == 0)
		return NULL;
	for (size_t i = 0; i < sizeof tok / sizeof tok[0]; i++)
		if (strlen(tok[i].w) == n && !strncasecmp(s, tok[i].w, n))
			return tok[i].a;
	return NULL;
}

/* structural tinting for whatever shape the log has: dim the preamble
 * (timestamp/host) and epoch stamps, hue the verbosity token, accent
 * "quoted values" and (parenthesized) context. All guesses; no format
 * is required. */
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

	/* quoted values: "double" and 'single'. A single-quote may not hug
	 * letters, so apostrophes (don't, cats') stay plain */
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
		add_span(sp, &n, (int)k, (int)j + 1, VAL_COLOR);
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
		L->sev = severity(L->s);	/* scan once, lines are immutable */
	const char *col = L->sev;
	regmatch_t m;
	int ms = -1, me = -1;
	if (filtered && regexec(&re, L->s, 1, &m, 0) == 0) {
		ms = (int)m.rm_so;
		me = (int)m.rm_eo;
	}

	Span sp[LINE_SPANS];
	size_t nsp = (size_t)collect_spans(L, sp);

	const Span *act = NULL;
	int inv = 0;	/* match-highlight state, kept in sync across BASE() */
	size_t si = 0;
	size_t b = 0, dc = 0, sc = 0, row = r0;

	/* base style = cursor/mark bg + severity fg (+match inverse if mid-match) */
#define BASE() do { \
		fputs("\x1b[0m", stdout); \
		if (iscur) \
			fputs(nocolor ? "\x1b[7m" : "\x1b[100m", stdout); \
		else if (L->marked) \
			fputs(mark_bg, stdout); \
		if (*col) \
			fputs(col, stdout); \
		inv = ms >= 0 && b >= (size_t)ms && b < (size_t)me; \
		if (inv) \
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
			if (act)	/* span styling doesn't survive SGR reset */
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
			fputs(act->attr, stdout);
		}
		int want_inv = ms >= 0 && b >= (size_t)ms && b < (size_t)me;
		if (want_inv && !inv) {
			fputs("\x1b[7m", stdout);
			inv = 1;
		}
		fwrite(L->s + b, 1, cl, stdout);
		if (!want_inv && inv) {
			fputs("\x1b[27m", stdout);
			inv = 0;
		}
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
static void draw_top_bar(void)
{
	if (!have_top_bar())
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
	char mk[32];
	mk[0] = 0;
	if (nmarked)
		snprintf(mk, sizeof mk,
			 budget >= 16 ? "  %zu marked" : " *%zu", nmarked);
	size_t sl = strlen(src), ql = filtered ? strlen(query) : 0;
	for (;;) {
		snprintf(left, sizeof left, " %.*s  %s%s%s%s%.*s%s",
			 (int)sl, src, where, mk, flw,
			 filtered ? "  /" : "", (int)ql, query,
			 (nv == 0 && filtered) ? "  (no matches)" : "");
		if ((int)strlen(left) <= budget)
			break;
		if (sl) {	/* path yields first; position and marks stay */
			sl /= 2;
			while (sl && ((unsigned char)src[sl] & 0xC0) == 0x80)
				sl--;
		} else if (ql > 4) {
			ql -= ql / 4 + 1;
			while (ql && ((unsigned char)query[ql] & 0xC0) == 0x80)
				ql--;
		} else if (*flw) {
			flw = "";
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

	printf("\x1b[1;1H\x1b[1;7m");
	fputs(left, stdout);
	for (int i = 0; i < cols - lw - hw - 1; i++)
		fputc(' ', stdout);
	if (hint)
		fputs(hint, stdout);
	fputc(' ', stdout);
	fputs("\x1b[0m", stdout);
}

/* command line, vim-style: the filter prompt while editing, transient
 * notices otherwise; reserved for future commands (:...) */
static void draw_input_bar(void)
{
	if (!editing && !have_input_bar()) {
		fputs("\x1b[?25l", stdout);
		return;
	}
	printf("\x1b[%d;1H\x1b[K", rows);

	if (editing) {
		/* show the tail: a prompt wider than the pane would autowrap
		 * and scroll the screen on every keystroke */
		size_t elen = strlen(edit);
		int ew = (int)str_cols(edit, elen);
		int maxw = cols - 4 > 1 ? cols - 4 : 1;	/* " /" + cursor + " " */
		int tw = ew;
		size_t off = 0, p = 0;
		while (tw > maxw && p < elen) {
			size_t cl;
			tw -= glyph_width(u8_decode(edit + p, elen - p, &cl));
			p += cl;
			off = p;
		}
		fputs("\x1b[1;7m /", stdout);
		fputs(edit + off, stdout);
		fputs(" \x1b[0m", stdout);
		int pw = (tw > 0 ? tw : 0) + 3;
		/* show update_filter() notices (e.g. "bad regex") instead of
		 * hiding them behind the prompt */
		int nw = (int)strlen(msg);
		int room = cols - (pw - 1);
		if (*msg && nw < room) {
			for (int i = 0; i < room - nw; i++)
				fputc(' ', stdout);
			fputs("\x1b[1;7m", stdout);
			fwrite(msg, 1, (size_t)nw, stdout);
			fputs("\x1b[0m", stdout);
		}
		printf("\x1b[%d;%dH\x1b[?25h", rows, pw);
		return;
	}

	if (*msg) {
		int nw = (int)strlen(msg);
		if (nw > cols - 2)
			nw = cols - 2;
		fputs("\x1b[1;7m ", stdout);
		fwrite(msg, 1, (size_t)nw, stdout);
		fputs(" \x1b[0m", stdout);
	}
	fputs("\x1b[?25l", stdout);
}

static void render(void)
{
	size_t vis = pane_rows();
	fputs("\x1b[H", stdout);
	size_t r0 = have_top_bar(), r = r0, i = top;
	for (; i < nv && r - r0 < vis; i++)
		r += draw_line(view[i], i == cur, r, vis);
	if (r - r0 < vis) {
		printf("\x1b[%zu;1H", r + 1);
		if (nv == 0 && !filtered)
			fputs("(empty)", stdout);
		fputs("\x1b[J", stdout);
	}
	draw_top_bar();
	draw_input_bar();
	fflush(stdout);
}

/* --- clipboard --- */

static size_t b64enc(const char *d, size_t n, char *o)
{
	static const char t[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t j = 0;
	for (size_t i = 0; i < n; i += 3) {
		unsigned v = (unsigned char)d[i] << 16;
		if (i + 1 < n)
			v |= (unsigned char)d[i + 1] << 8;
		if (i + 2 < n)
			v |= (unsigned char)d[i + 2];
		o[j++] = t[(v >> 18) & 63];
		o[j++] = t[(v >> 12) & 63];
		o[j++] = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
		o[j++] = (i + 2 < n) ? t[v & 63] : '=';
	}
	o[j] = 0;
	return j;
}

static int has_prog(const char *prog)
{
	const char *p = getenv("PATH");
	if (!p)
		return 0;
	char full[4096];
	while (*p) {
		const char *e = strchr(p, ':');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n + strlen(prog) + 2 < sizeof full) {
			memcpy(full, p, n);
			full[n] = 0;
			if (n)
				strcat(full, "/");
			strcat(full, prog);
			if (access(full, X_OK) == 0)
				return 1;
		}
		if (!e)
			break;
		p = e + 1;
	}
	return 0;
}

static void clip_helper(const char *s, size_t len)
{
	static const char *const cands[][5] = {
		{ "xclip", "-selection", "clipboard", "-in", NULL },
		{ "wl-copy", NULL },
		{ "pbcopy", NULL },
		{ "termux-clipboard-set", NULL },
	};
	for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
		if (!has_prog(cands[i][0]))
			continue;
		int pp[2];
		if (pipe(pp) < 0)
			return;
		pid_t pid = fork();
		if (pid < 0) {
			close(pp[0]); close(pp[1]);
			return;
		}
		if (pid == 0) {
			int dn = open("/dev/null", O_WRONLY);
			if (dn >= 0) {	/* a failing xclip must not garble the TUI */
				dup2(dn, STDOUT_FILENO);
				dup2(dn, STDERR_FILENO);
				close(dn);
			}
			dup2(pp[0], STDIN_FILENO);
			close(pp[0]); close(pp[1]);
			execvp(cands[i][0], (char *const *)cands[i]);
			_exit(127);
		}
		close(pp[0]);
		size_t off = 0;
		while (off < len) {
			ssize_t w = write(pp[1], s + off, len - off);
			if (w <= 0)
				break;
			off += (size_t)w;
		}
		close(pp[1]);
		/* no waitpid: wl-copy stays alive to own the clipboard */
		return;
	}
}

static void copy_text(const char *s, size_t len)
{
	size_t need = 4 * ((len + 2) / 3) + 1;
	char *b = xrealloc(NULL, need);
	b64enc(s, len, b);
	/* OSC 52, so copy works over ssh too */
	printf("\x1b]52;c;%s\a", b);
	fflush(stdout);
	free(b);
	clip_helper(s, len);
}

static void clear_marks(void)
{
	for (size_t i = 0; i < nlines; i++)
		lines[i].marked = 0;
	nmarked = 0;
}

static void copy_current(void)
{
	if (nmarked > 0) {
		size_t total = 0;
		for (size_t i = 0; i < nlines; i++)
			if (lines[i].marked)
				total += lines[i].len + 1;
		char *buf = xrealloc(NULL, total + 1);
		size_t off = 0;
		int cnt = 0;
		for (size_t i = 0; i < nlines; i++) {
			if (!lines[i].marked)
				continue;
			memcpy(buf + off, lines[i].s, lines[i].len);
			off += lines[i].len;
			buf[off++] = '\n';
			cnt++;
		}
		copy_text(buf, off);
		free(buf);
		clear_marks();
		snprintf(msg, sizeof msg, "copied %d marked lines (%zu bytes)",
			 cnt, off);
		return;
	}
	if (nv == 0)
		return;
	Line *L = &lines[view[cur]];
	copy_text(L->s, L->len);
	snprintf(msg, sizeof msg, "copied line %zu (%zu bytes)", cur + 1, L->len);
}

/* --- input --- */

static int peeked = -1;	/* unget buffer for read_key()'s Esc lookahead */

static int read_byte(void)
{
	if (peeked >= 0) {
		int c = peeked;
		peeked = -1;
		return c;
	}
	unsigned char c;
	if (read(kfd, &c, 1) != 1)
		return K_EOF;
	return c;
}

/* peeked byte awaits: main loop must not poll-wait on the empty fd */
static int key_pending(void)
{
	return peeked >= 0;
}

static int wait_byte(void)
{
	struct pollfd p = { .fd = kfd, .events = POLLIN };
	if (poll(&p, 1, 50) <= 0)
		return K_NONE;
	return read_byte();
}

/* map a fully-drained CSI/SS3 sequence; seq[n-1] is the final byte */
static int decode_csi(const char *seq, size_t n)
{
	switch (seq[n - 1]) {
	case 'A': return K_UP;
	case 'B': return K_DOWN;
	case 'C': return K_RIGHT;
	case 'D': return K_LEFT;
	case 'H': return K_HOME;
	case 'F': return K_END;
	case '~':	/* single-digit param picks the key; 15..24~ are F-keys */
		if (seq[0] >= '1' && seq[0] <= '8' &&
		    (seq[1] == '~' || seq[1] == ';')) {
			switch (seq[0]) {
			case '1': case '7': return K_HOME;
			case '4': case '8': return K_END;
			case '5': return K_PGUP;
			case '6': return K_PGDN;
			}
		}
		return K_NONE;	/* 2 ins, 15+ F-keys, 200/201 paste ... */
	default:
		return K_NONE;	/* modified keys, mouse, unknown: drained */
	}
}

static int read_key(void)
{
	int c = read_byte();
	if (c != 0x1b)
		return c;
	int c2 = wait_byte();
	if (c2 != '[' && c2 != 'O') {
		/* Alt-chord / fast Esc+key: don't swallow the byte */
		if (c2 < K_NONE)
			peeked = c2;
		return c;	/* lone Escape */
	}
	/* swallow the whole sequence up to its final byte (@..~) so no
	 * leftover bytes ever replay as keystrokes */
	char seq[16];
	size_t n = 0;
	int f;
	do {
		f = wait_byte();
		if (f == K_EOF || f == K_NONE)
			return c;	/* aborted sequence: treat as Esc */
		if (n < sizeof seq - 1)
			seq[n++] = (char)f;
	} while (f < 0x40 || f > 0x7e);
	return decode_csi(seq, n);
}

static void apply_edit(void)
{
	edit[MAX_QUERY - 1] = 0;
	update_filter(edit);
}

/* --- main --- */

static const char *key_name(int key, char *buf, size_t n)
{
	switch (key) {
	case 0x1b:		return "Esc";
	case '\r': case '\n':	return "Enter";
	case ' ':		return "Space";
	}
	switch (key) {
	case K_UP:	return "Up";
	case K_DOWN:	return "Down";
	case K_LEFT:	return "Left";
	case K_RIGHT:	return "Right";
	case K_PGUP:	return "PgUp";
	case K_PGDN:	return "PgDn";
	case K_HOME:	return "Home";
	case K_END:	return "End";
	}
	if (key < 32) {			/* CTL(x): x & 0x1f */
		snprintf(buf, n, "Ctrl-%c", key + '`');
		return buf;
	}
	snprintf(buf, n, "%c", key);
	return buf;
}

/* keys section of --help, generated from keymap[] so the two never drift */
static void print_keys(FILE *out)
{
	static const struct { int act; const char *desc; } acts[] = {
		{ A_DOWN,	"next line" },
		{ A_UP,		"previous line" },
		{ A_PGDOWN,	"page down" },
		{ A_PGUP,	"page up" },
		{ A_TOP,	"go to top" },
		{ A_BOT,	"go to bottom" },
		{ A_LEFT,	"scroll left" },
		{ A_RIGHT,	"scroll right" },
		{ A_HSTART,	"scroll to left edge" },
		{ A_HEND,	"scroll to right edge" },
		{ A_FILTER,	"filter (incremental regex, smart case)" },
		{ A_CLEAR_FILTER, "clear filter" },
		{ A_CANCEL,	"cancel editing; clear marks, else filter" },
		{ A_MARK,	"mark line and sweep" },
		{ A_UNMARK,	"unmark line and sweep" },
		{ A_COPY,	"copy marked lines (clears marks), else current" },
		{ A_FOLLOW,	"toggle follow" },
		{ A_RELOAD,	"reload file" },
		{ A_WRAP,	"toggle line wrap" },
		{ A_STOP,	"suspend comb (fg to resume)" },
		{ A_QUIT,	"quit" },
	};
	for (size_t i = 0; i < sizeof acts / sizeof acts[0]; i++) {
		char keys[64], nm[16], prev[16];
		size_t used = 0;
		keys[0] = prev[0] = 0;
		for (size_t k = 0; k < sizeof keymap / sizeof keymap[0]; k++) {
			if (keymap[k].act != acts[i].act)
				continue;
			const char *nm2 = key_name(keymap[k].key, nm, sizeof nm);
			if (!strcmp(nm2, prev))
				continue;	/* \r and \n are both Enter */
			snprintf(prev, sizeof prev, "%s", nm2);
			used += (size_t)snprintf(keys + used,
						 sizeof keys - used,
						 used ? "/%s" : "%s", nm2);
		}
		fprintf(out, "  %-23s%s\n", keys, acts[i].desc);
	}
	fputs("\n  Sweep direction follows the last up/down move.\n", out);
}

static void usage(FILE *out)
{
	fputs(
"usage: comb [-e REGEX] [--no-color] [FILE]\n"
"\n"
"View, filter and copy log files. Reads FILE, or stdin when piped.\n"
"\n"
"options:\n"
"  -e REGEX               start with REGEX as the filter\n"
"  --no-color, -C         disable syntax coloring (also honors NO_COLOR)\n"
"  -                      read from stdin (implied when stdin is not a tty)\n"
"  -h, --help             show this help\n"
"\n"
"keys:\n", out);
	print_keys(out);
}

int main(int argc, char **argv)
{
	const char *init_re = NULL;
	const char *file = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-e") && i + 1 < argc) {
			init_re = argv[++i];
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(argv[i], "--no-color") || !strcmp(argv[i], "-C")) {
			nocolor = 1;
		} else if (!strcmp(argv[i], "-")) {
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

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);	/* auto-reap; wl-copy must outlive us */
	signal(SIGWINCH, on_winch);
	signal(SIGTERM, on_sigexit);
	signal(SIGHUP, on_sigexit);
	signal(SIGINT, on_sigexit);
	tty_enter();

	load_all();
	if (init_re)
		update_filter(init_re);
	else
		rebuild_view();
	cur = nv ? nv - 1 : 0;
	ensure_visible();

	if (use_stdin)
		follow = 0;

	int key;
	while (running) {
		if (got_winch) {
			got_winch = 0;
			get_winsize();
			ensure_visible();
			dirty = 1;
		}
		if (follow && !use_stdin) {
			int stick = nv > 0 && cur >= nv - 1;
			size_t old = nlines;
			if (fd < 0) {
				/* rotation race lost the file between stat and
				 * open; retry until it reappears */
				fd = open(path, O_RDONLY);
				if (fd >= 0)
					fsize = 0;
			}
			if (fd < 0)
				continue;
			int got = append_new();
			if (got == 2)
				rebuild_view();	/* rotation: lines[] were rebuilt from scratch */
			else if (got == 1)
				extend_view(old);
			if (got) {
				if (stick)
					cur = nv ? nv - 1 : 0;
				ensure_visible();
				dirty = 1;
			}
		}

		struct pollfd p = { .fd = kfd, .events = POLLIN };
		/* Paint when no keystroke is already waiting; if input is
		 * queued (key repeat), keep draining and coalesce the repaint. */
		if (!dirty) {
			if (!key_pending() && poll(&p, 1, 200) <= 0)
				continue;
		} else if (!key_pending() && poll(&p, 1, 0) <= 0) {
			render();
			dirty = 0;
			continue;
		}

		key = read_key();
		if (key == K_EOF)
			break;
		msg[0] = 0;

		if (editing) {
			switch (key) {
			case 0x1b:
				editing = 0;
				break;
			case '\r': case '\n':
				editing = 0;
				break;
			case 127: case 8:
				if (edit[0]) {
					size_t l = strlen(edit);
					while (l > 0 && (edit[--l] & 0xC0) == 0x80)
						;
					edit[l] = 0;
					apply_edit();
				}
				break;
			case 21: /* ctrl-u */
				edit[0] = 0;
				apply_edit();
				break;
			default:
				if (key >= 32 && key < 256 && key != 127 &&
				    strlen(edit) < MAX_QUERY - 1) {
					size_t l = strlen(edit);
					edit[l] = (char)key;
					edit[l + 1] = 0;
					apply_edit();
				}
				break;
			}
			dirty = 1;
			continue;
		}

		int act = key_action(key);
		switch (act) {
		case A_QUIT:
			running = 0;
			break;
		case A_DOWN:
			mdir = 1;
			if (cur + 1 < nv)
				cur++;
			break;
		case A_UP:
			mdir = -1;
			if (cur > 0)
				cur--;
			break;
		case A_PGDOWN:
			cur += (size_t)(rows > 2 ? rows - 2 : 1);
			break;
		case A_PGUP:
			cur -= (size_t)(rows > 2 ? rows - 2 : 1);
			if (cur > nv)
				cur = 0;
			break;
		case A_TOP:
			cur = 0;
			break;
		case A_BOT:
			cur = nv ? nv - 1 : 0;
			break;
		case A_LEFT:
			hscroll -= 8;
			if (hscroll < 0)
				hscroll = 0;
			break;
		case A_RIGHT: {
			/* stop where content stops: past the widest line is
			 * blankness that only looks like more log */
			size_t max = widest > (size_t)cols
					   ? widest - (size_t)cols : 0;
			if ((size_t)hscroll < max)
				hscroll = (size_t)hscroll + 8 > max
						  ? (int)max : hscroll + 8;
			break;
		}
		case A_HSTART:
			hscroll = 0;
			break;
		case A_HEND:
			if (nv) {
				Line *L = &lines[view[cur]];
				size_t cw = line_cols(L);
				hscroll = cw > (size_t)cols
						  ? (int)(cw - (size_t)cols)
						  : 0;
			}
			break;
		case A_FILTER:
			editing = 1;
			filter_anchor = nv ? view[cur] : 0;
			filter_row = cur - top;
			snprintf(edit, sizeof edit, "%s", query);
			break;
		case A_MARK:
		case A_UNMARK: {
			int want = (act == A_MARK);
			if (nv) {
				Line *L = &lines[view[cur]];
				if ((int)L->marked != want) {
					L->marked = (unsigned char)want;
					nmarked += want ? 1 : -1;
				}
				if (mdir < 0) {
					if (cur > 0)
						cur--;
				} else if (cur + 1 < nv) {
					cur++;
				}
			}
			break;
		}
		case A_CLEAR_FILTER:
			if (filtered) {
				edit[0] = 0;
				update_filter("");
			}
			break;
		case A_CANCEL:
			if (nmarked) {
				clear_marks();
				snprintf(msg, sizeof msg, "marks cleared");
			} else if (filtered) {
				edit[0] = 0;
				update_filter("");
			}
			break;
		case A_WRAP:
			wrap = !wrap;
			snprintf(msg, sizeof msg, "wrap %s",
				 wrap ? "on" : "off");
			break;
		case A_COPY:
			copy_current();
			break;
		case A_FOLLOW:
			follow = !follow;
			if (!follow) {
				flush_pending();	/* settle a final partial line */
				rebuild_view();
			}
			snprintf(msg, sizeof msg, "follow %s",
				 follow ? "on" : "off");
			break;
		case A_RELOAD:
			if (!use_stdin) {
				reset_lines();
				if (fd >= 0)
					close(fd);
				fd = -1;
				load_all();
				rebuild_view();
				cur = nv ? nv - 1 : 0;
				ensure_visible();
				snprintf(msg, sizeof msg, "reloaded");
			}
			break;
		case A_STOP:
			/* hand the tty back for the shell's job control; on
			 * continue, pick up exactly where we left off */
			restore_terminal();
			signal(SIGTSTP, SIG_DFL);	/* may be inherited as SIG_IGN */
			raise(SIGTSTP);
			tty_enter();
			dirty = 1;
			break;
		case A_REPAINT:
		default:
			break;
		}
		ensure_visible();
		dirty = 1;
	}

	restore_terminal();
	return 0;
}
