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
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_QUERY 256
#define MARK_BG "\x1b[48;5;238m"
#define VAL_COLOR "\x1b[38;5;223m"	/* quoted values */
#define PAREN_COLOR "\x1b[38;5;115m"	/* (...) context */
#define DIM "\x1b[2m"

typedef struct {
	char *s;
	size_t len;
	int tag_so, tag_eo;	/* byte span of the service tag, -1 if none */
	int slot;		/* svc_palette index, -1 if no tag */
	unsigned char marked;
} Line;

static Line *lines;
static size_t nlines, lcap;

static size_t *view;
static size_t nv, vcap;

static char path[4096];
static int use_stdin;
static int fd = -1;      /* log file */
static int kfd = 0;      /* keyboard: stdin, or /dev/tty when stdin is data */
static off_t fsize;

static regex_t re;
static int filtered;
static char query[MAX_QUERY];
static char edit[MAX_QUERY];
static int editing;

static int follow = 1;
static int running = 1;
static int dirty = 1;

static size_t cur, top;
static int hscroll;
static int rows = 24, cols = 80;

static char msg[160];
static struct termios saved_tio;
static int tio_saved;
static volatile sig_atomic_t got_winch;
static size_t nmarked;
static int mdir = -1;	/* space/x sweep direction: -1 up, 1 down */

static void assign_service(Line *L);

enum {
	K_NONE = 0x100, K_EOF, K_ESC, K_UP, K_DOWN, K_LEFT, K_RIGHT,
	K_PGUP, K_PGDN, K_HOME, K_END, K_DEL
};

/*
 * Key bindings -- edit to taste. Each row maps one key to one action;
 * many keys may share an action. Keys are whatever read_key() returns:
 * plain characters, control codes via CTL(), or the K_* specials.
 * The filter prompt deliberately keeps fixed editing keys
 * (type / Backspace / Ctrl-u / Enter / Esc), vim-prompt style.
 */
#define CTL(x)	((x) & 0x1f)
enum {
	A_NONE, A_QUIT, A_REPAINT,
	A_DOWN, A_UP, A_PGDOWN, A_PGUP, A_TOP, A_BOT,
	A_LEFT, A_RIGHT, A_HSTART, A_HEND,
	A_FILTER, A_CLEAR_FILTER, A_CANCEL,
	A_MARK, A_UNMARK, A_COPY,
	A_FOLLOW, A_RELOAD,
};

static const struct { int key; int act; } keymap[] = {
	{ 'q',	      A_QUIT },
	{ CTL('c'),   A_QUIT },

	/* vertical movement; sets the space/x sweep direction */
	{ 'j',	      A_DOWN },
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
	{ '$',	      A_HEND },

	/* filter / marks / clipboard */
	{ '/',	      A_FILTER },
	{ '?',	      A_CLEAR_FILTER },
	{ 0x1b,	     A_CANCEL },	/* clears marks first, else the filter */
	{ ' ',	      A_MARK },
	{ 'x',	      A_UNMARK },
	{ 'c',	      A_COPY },
	{ 'y',	      A_COPY },
	{ '\r',	     A_COPY },
	{ '\n',	     A_COPY },

	/* toggles */
	{ 'f',	      A_FOLLOW },
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
	if (errno)
		fprintf(stderr, ": %s", strerror(errno));
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
		die("not a terminal");
	struct termios t = saved_tio;
	t.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT);
	t.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG | IEXTEN);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(kfd, TCSANOW, &t) < 0)
		die("tcsetattr");
	tio_saved = 1;
}

/* undo raw mode + alternate screen; safe from die() at any point */
static void restore_terminal(void)
{
	if (!tio_saved)
		return;
	fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
	fflush(stdout);
	tcsetattr(kfd, TCSANOW, &saved_tio);
}

static void get_winsize(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
		cols = ws.ws_col;
		rows = ws.ws_row;
	}
}

/* --- input feeding / line storage --- */

static char *pend;
static size_t plen;

static void push_line(char *clean, size_t len)
{
	if (nlines == lcap) {
		lcap = lcap ? lcap * 2 : 1024;
		lines = xrealloc(lines, lcap * sizeof(*lines));
	}
	lines[nlines].s = clean;
	lines[nlines].len = len;
	lines[nlines].marked = 0;
	assign_service(&lines[nlines]);
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
	for (size_t i = 0; i < plen; i++) {
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

/* per-service color: pastel foreground assigned by rotation in order of
 * first appearance so adjacent services never share a color. */
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
 * if no tag. The tag is the first field ending in ':' after the first
 * field (timestamp), with a trailing "[pid]" stripped:
 *
 *   2026-08-22T23:53:27.7 localhost NetworkManager[508]: <info> ...
 *   \---- field 1 ----/   ^field 2 ^field 3
 *                                    \*so ... *eo/
 *
 * Field 1 is always skipped (++field < 2): it is the timestamp, whether
 * ISO 8601 or syslog's three-word "Aug 22 23:53:27" (the day/time words
 * are then candidates but never end in ':'). Guards: empty field,
 * no ':' at the end, longer than 64 bytes, or starting with '<' (that's
 * a verbosity token like <info>, not a tag).
 *
 * "chronyd[553]:" -> "chronyd"; bare "foo:" also matches. */
static int tag_span(const char *s, size_t n, int *so, int *eo)
{
	size_t i = 0;
	int field = 0;
	while (i < n) {
		while (i < n && s[i] == ' ')
			i++;
		if (i >= n)
			break;
		size_t fs = i;
		while (i < n && s[i] != ' ')
			i++;
		size_t fe = i;
		if (++field < 2)
			continue;
		if (fe == fs || s[fe - 1] != ':' || fe - fs > 64 || s[fs] == '<')
			continue;
		size_t e = fe - 1;
		if (e > fs && s[e - 1] == ']') {
			size_t k = e - 1;
			while (k > fs && s[k - 1] != '[')
				k--;
			if (k > fs)
				e = k - 1;
		}
		if (e == fs)
			continue;
		*so = (int)fs;
		*eo = (int)e;
		return 1;
	}
	return 0;
}

/* Assign the palette slot for this line's service tag. Runs at push time
 * so slots depend only on order of appearance in the file; assigning at
 * render time instead made reloads reshuffle colors (draw order differs
 * from file order). */
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
	nmarked = 0;
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
		return;
	}
	if (fd < 0) {
		fd = open(path, O_RDONLY);
		if (fd < 0)
			die("cannot open %s", path);
	}
	fsize = 0;
	read_available(fd);
	fsize = lseek(fd, 0, SEEK_CUR);
}

/* returns 1 if new data arrived (or the file was rotated) */
static int append_new(void)
{
	struct stat st, fst;
	/* rotation by rename+recreate keeps our fd on the old inode whose
	 * size never changes, so also compare the path's inode; the size
	 * check alone covers copytruncate-style truncation */
	if (fstat(fd, &fst) == 0 && stat(path, &st) == 0 &&
	    (st.st_ino != fst.st_ino || st.st_dev != fst.st_dev ||
	     (off_t)st.st_size < fsize)) {
		close(fd);
		reset_lines();
		fd = open(path, O_RDONLY);
		if (fd < 0)
			return 0;
		fsize = 0;
	}
	off_t before = lseek(fd, 0, SEEK_CUR);
	read_available(fd);
	off_t now = lseek(fd, 0, SEEK_CUR);
	fsize = now;
	return now != before;
}

/* --- filtering --- */

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
	size_t vis = (size_t)(rows > 1 ? rows - 1 : 1);
	if (cur >= nv)
		cur = nv ? nv - 1 : 0;
	if (cur < top)
		top = cur;
	if (cur >= top + vis)
		top = cur - vis + 1;
}

static void rebuild_view(void);

static void update_filter(const char *q)
{
	msg[0] = 0;
	if (!*q) {
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
}

static void rebuild_view(void)
{
	nv = 0;
	for (size_t i = 0; i < nlines; i++) {
		if (!filtered || regexec(&re, lines[i].s, 0, NULL, 0) == 0)
			push_view(i);
	}
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
	/* a keyword counts only when its left neighbor isn't part of a word,
	 * flag or path: "--debug", "/var/debug" and "terrain" (for "err")
	 * must not dim the line; "level=debug", "<warn>", "errors" must */
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
	char buf[16];
	if (n == 0 || n >= sizeof buf)
		return NULL;
	memcpy(buf, s, n);
	buf[n] = 0;
	for (size_t i = 0; i < sizeof tok / sizeof tok[0]; i++)
		if (!strcasecmp(buf, tok[i].w))
			return tok[i].a;
	return NULL;
}

/* structural tinting for whatever shape the log has: dim the preamble
 * (timestamp/host) and epoch stamps, hue the verbosity token, accent
 * "quoted values". Purely cosmetic guesses; no format is required. */
static int collect_spans(const Line *L, Span *sp)
{
	int n = 0;
	const char *s = L->s;

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
	/* parenthesis groups, nesting included; unbalanced ones stay plain */
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

static void draw_line(size_t idx, int iscur)
{
	Line *L = &lines[idx];
	const char *col = severity(L->s);
	regmatch_t m;
	int ms = -1, me = -1;
	if (filtered && regexec(&re, L->s, 1, &m, 0) == 0) {
		ms = (int)m.rm_so;
		me = (int)m.rm_eo;
	}

	Span sp[LINE_SPANS];
	size_t nsp = (size_t)collect_spans(L, sp);

	const Span *act = NULL;
	size_t si = 0;
	size_t b = 0, dc = 0, emitted = 0;

	/* base style = cursor/mark bg + severity fg (+match inverse if mid-match) */
#define BASE() do { \
		fputs("\x1b[0m", stdout); \
		if (iscur) \
			fputs("\x1b[100m", stdout); \
		else if (L->marked) \
			fputs(MARK_BG, stdout); \
		if (*col) \
			fputs(col, stdout); \
		if (ms >= 0 && b >= (size_t)ms && b < (size_t)me) \
			fputs("\x1b[7m", stdout); \
	} while (0)

	BASE();

	while (b < L->len) {
		size_t cl = u8len((unsigned char)L->s[b]);
		if (dc >= (size_t)hscroll + (size_t)cols)
			break;
		if (act && (size_t)act->se <= b) {
			act = NULL;
			BASE();
		}
		if (!act && si < nsp && (size_t)sp[si].so == b) {
			act = &sp[si++];
			fputs(act->attr, stdout);
		}
		if (ms >= 0 && b == (size_t)ms)
			fputs("\x1b[7m", stdout);
		if (dc >= (size_t)hscroll) {
			fwrite(L->s + b, 1, cl, stdout);
			emitted++;
		}
		if (me >= 0 && b + cl == (size_t)me)
			fputs("\x1b[27m", stdout);
		b += cl;
		dc++;
	}
	BASE();
#undef BASE
	/* pad to viewport width so shorter lines erase longer predecessors */
	for (size_t k = emitted; k < (size_t)cols; k++)
		fputc(' ', stdout);
	fputs("\x1b[0m", stdout);
}

static void draw_status(void)
{
	printf("\x1b[%d;1H\x1b[K", rows);
	const char *src = use_stdin ? "(stdin)" : path;
	char left[512];

	if (editing) {
		snprintf(left, sizeof left, "/%s", edit);
		fputs("\x1b[1;7m ", stdout);
		fputs(left, stdout);
		fputs(" \x1b[0m", stdout);
		int cc = 0;	/* columns = codepoints here, not bytes */
		for (size_t i = 0; i < strlen(left); ) {
			i += u8len((unsigned char)left[i]);
			cc++;
		}
		printf("\x1b[%d;%dH\x1b[?25h", rows, cc + 3);
		return;
	}

	char where[64];
	snprintf(where, sizeof where, "%zu/%zu", nv ? cur + 1 : 0, nv);

	char hint[128];
	if (msg[0])
		snprintf(hint, sizeof hint, "%.120s ", msg);
	else if (cols >= 68)
		snprintf(hint, sizeof hint,
			 "/ filter  ? clear  spc/x mark  c copy  f follow  r reload  q quit");
	else if (cols >= 38)
		snprintf(hint, sizeof hint, "/ filter  spc mark  c copy  q quit");
	else
		hint[0] = 0;
	int hw = (int)strlen(hint);
	if (hw > cols - 2) {
		hw = cols - 2;
		hint[hw] = 0;
	}

	/* Fit left of the hint: shrink the path first, then the query,
	 * then drop decorations. A wrapped status line would scroll the
	 * pane up and swallow a content row. */
	const char *flw = follow && !use_stdin ? "  follow" : "";
	int budget = cols - hw - 2;
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
	int pad = cols - lw - hw;
	if (pad < 1)
		pad = 1;

	fputs("\x1b[1;7m", stdout);
	fputs(left, stdout);
	fputs("\x1b[0m", stdout);
	for (int i = 0; i < pad; i++)
		fputc(' ', stdout);
	fputs(msg[0] ? "\x1b[1;7m" : "\x1b[2m", stdout);
	fputs(hint, stdout);
	fputs("\x1b[0m", stdout);
	fputs("\x1b[?25l", stdout);
}

static void render(void)
{
	size_t vis = (size_t)(rows - 1);
	fputs("\x1b[H", stdout);
	for (size_t r = 0; r < vis; r++) {
		printf("\x1b[%zu;1H", r + 1);
		if (top + r < nv) {
			draw_line(view[top + r], top + r == cur);
		} else {
			if (nv == 0 && r == 0 && !filtered)
				fputs("(empty)", stdout);
			fputs("\x1b[J", stdout);
			break;
		}
	}
	draw_status();
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
	struct { const char *name; const char *argv[5]; } cands[] = {
		{ "xclip",  { "xclip", "-selection", "clipboard", "-in", NULL } },
		{ "wl-copy", { "wl-copy", NULL } },
		{ "pbcopy", { "pbcopy", NULL } },
		{ "termux-clipboard-set", { "termux-clipboard-set", NULL } },
	};
	for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
		if (!has_prog(cands[i].name))
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
			dup2(pp[0], STDIN_FILENO);
			close(pp[0]); close(pp[1]);
			execvp(cands[i].name, (char *const *)cands[i].argv);
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
	/* OSC 52: works in most terminals, including over ssh */
	printf("\x1b]52;c;%s\a", b);
	fflush(stdout);
	free(b);
	clip_helper(s, len);
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
		for (size_t i = 0; i < nlines; i++)
			lines[i].marked = 0;
		nmarked = 0;
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

static int read_byte(void)
{
	unsigned char c;
	if (read(kfd, &c, 1) != 1)
		return K_EOF;
	return c;
}

static int wait_byte(void)
{
	struct pollfd p = { .fd = kfd, .events = POLLIN };
	if (poll(&p, 1, 50) <= 0)
		return K_ESC;
	return read_byte();
}

static int read_key(void)
{
	int c = read_byte();
	if (c == 0x1b) {
		if (wait_byte() != '[')
			return K_ESC;
		int c2 = wait_byte();
		if (c2 == K_ESC)
			return K_ESC;
		switch (c2) {
		case 'A': return K_UP;
		case 'B': return K_DOWN;
		case 'C': return K_RIGHT;
		case 'D': return K_LEFT;
		case 'H': return K_HOME;
		case 'F': return K_END;
		case '5': return (wait_byte() == '~') ? K_PGUP : K_NONE;
		case '6': return (wait_byte() == '~') ? K_PGDN : K_NONE;
		case '1': case '7': return (wait_byte() == '~') ? K_HOME : K_NONE;
		case '4': case '8': return (wait_byte() == '~') ? K_END : K_NONE;
		case '3': return (wait_byte() == '~') ? K_DEL : K_NONE;
		default: return K_NONE;
		}
	}
	return c;
}

static void apply_edit(void)
{
	edit[MAX_QUERY - 1] = 0;
	update_filter(edit);
}

/* --- main --- */

static void usage(FILE *out)
{
	fputs(
"usage: comb [-e REGEX] FILE | -\n"
"\n"
"keys:\n"
"  j/k, arrows      scroll          g/G, Home/End   top/bottom\n"
"  Ctrl-d/u, PgDn/U page            h/l, Left/Right scroll sideways\n"
"  /                filter (regex, incremental; smart case)\n"
"  ?                clear filter\n"
"  Enter            accept filter   Esc             cancel; clear marks or filter\n"
"  Space            mark line, sweep        x       unmark line, sweep\n"
"                   (sweep direction follows the last j/k or arrow)\n"
"  c or y           copy marked lines (clears marks), else current line\n"
"  f                toggle live follow      r       reload\n"
"  q, Ctrl-c        quit\n", out);
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
		} else if (!strcmp(argv[i], "-")) {
			use_stdin = 1;
		} else if (argv[i][0] == '-') {
			usage(stderr);
			return 1;
		} else {
			file = argv[i];
		}
	}
	if (!file && !use_stdin) {
		usage(stderr);
		return 1;
	}
	if (file) {
		if (snprintf(path, sizeof path, "%s", file) >= (int)sizeof path)
			die("path too long");
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);	/* auto-reap; wl-copy must outlive us */
	signal(SIGWINCH, on_winch);
	if (use_stdin) {
		kfd = open("/dev/tty", O_RDONLY);
		if (kfd < 0)
			die("stdin is not interactive and /dev/tty unavailable");
	}
	raw_on();
	get_winsize();
	fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);
	fflush(stdout);

	load_all();
	if (init_re)
		update_filter(init_re);	/* rebuilds the view itself */
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
		if (follow && fd >= 0) {
			int stick = nv > 0 && cur >= nv - 1;
			if (append_new()) {
				rebuild_view();
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
			if (poll(&p, 1, 200) <= 0)
				continue;
		} else if (poll(&p, 1, 0) <= 0) {
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
				if (key >= 32 && key < 127 &&
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

		switch (key_action(key)) {
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
		case A_RIGHT:
			hscroll += 8;
			break;
		case A_HSTART:
			hscroll = 0;
			break;
		case A_HEND:
			if (nv) {
				Line *L = &lines[view[cur]];
				int len = (int)L->len;
				hscroll = len > cols ? len - cols : 0;
			}
			break;
		case A_FILTER:
			editing = 1;
			snprintf(edit, sizeof edit, "%s", query);
			break;
		case A_MARK:
			if (nv) {
				Line *L = &lines[view[cur]];
				if (!L->marked) {
					L->marked = 1;
					nmarked++;
				}
				if (mdir < 0 ? cur > 0 : cur + 1 < nv)
					cur += mdir;	/* sweep in last-moved direction */
			}
			break;
		case A_UNMARK:
			if (nv) {
				Line *L = &lines[view[cur]];
				if (L->marked) {
					L->marked = 0;
					nmarked--;
				}
				if (mdir < 0 ? cur > 0 : cur + 1 < nv)
					cur += mdir;
			}
			break;
		case A_CLEAR_FILTER:
			if (filtered) {
				edit[0] = 0;
				update_filter("");
			}
			break;
		case A_CANCEL:
			if (nmarked) {
				for (size_t i = 0; i < nlines; i++)
					lines[i].marked = 0;
				nmarked = 0;
				snprintf(msg, sizeof msg, "marks cleared");
			} else if (filtered) {
				edit[0] = 0;
				update_filter("");
			}
			break;
		case A_COPY:
			copy_current();
			break;
		case A_FOLLOW:
			follow = !follow;
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
