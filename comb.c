/* comb - a small terminal log viewer: highlight, filter, follow, copy.
 * build: cc -O2 -Wall -Wextra -o comb comb.c
 */
#define _GNU_SOURCE
#include "config.h"
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
#include <stddef.h>
#include <unistd.h>

typedef struct {
	const char *s;
	size_t len;
	int tag_so, tag_eo;	/* byte span of the service tag, -1 if none */
	int slot;		/* svc_palette index, -1 if no tag */
	unsigned char marked;
	unsigned char srchit;	/* highlight-search hit, for n/N + scrollbar */
	size_t wcols;		/* display width cache, 0 = uncomputed */
	const char *sev;	/* severity SGR cache, NULL = unscanned */
} Line;

/* parsed literal pattern, shared shape for the filter's and search's
 * matchers: buffer + ^/$ anchors + smart-case flag */
typedef struct {
	char buf[MAX_QUERY];
	size_t len;		/* 0: degenerate pattern, matches everywhere */
	int bol, eol, icase;
} LitSpec;

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
static int re_mode;		/* UI toggle for the next filter: literal vs ERE */
static int filtered_re;		/* the live filter is a compiled regex */
static int filter_inv;		/* live filter excludes matching lines */
static char query[MAX_QUERY];
static char edit[MAX_QUERY];
static int editing;
static size_t ecur;	/* insertion point: byte offset into edit[] */

/* highlight-only search: marks lines but never narrows view[] */
static char search[MAX_QUERY];
static int searched;
static int editing_search;	/* prompt currently edits search, not filter */
static regex_t sre;
static int searched_re;
static LitSpec slit;

static int follow = 1;
static int wrap;
static int nocolor;
static const char *mark_bg = MARK_BG;
static int running = 1;
static int dirty = 1;

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
static int search_match(const Line *L, regmatch_t *m);

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

/* Line text lives in bump-allocated ~1 MiB chunks; chunks are never
 * moved or freed individually, so Line.s and svc_seen pointers stay
 * valid until reset_lines rewinds the arena. */
#define ARENA_CHUNK ((size_t)1 << 20)
static char **achunk;
static size_t nachunk, acap, apos;

static void *arena_alloc(size_t n)
{
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
static char *fmap;
static size_t fmap_len, fmap_pos;

static char *pend;
static size_t plen;
static int flushed_partial;	/* last pushed line had no trailing newline */
static int stdin_eof;	/* pipe closed: no more input will ever come */

static void push_line(const char *clean, size_t len)
{
	if (nlines == lcap) {
		lcap = lcap ? lcap * 2 : 1024;
		lines = xrealloc(lines, lcap * sizeof(*lines));
	}
	Line *L = &lines[nlines];
	L->s = clean;
	L->len = len;
	regmatch_t sm;
	L->marked = 0;
	L->srchit = (unsigned char)(searched && search_match(L, &sm));
	L->sev = NULL;
	assign_service(L);
	nlines++;
}

/* strip ANSI sequences and CRs, expand tabs; returns arena-allocated
 * string. Worst case is tab expansion (+3 bytes each). */
static char *sanitize(const char *s, size_t n, size_t *outlen)
{
	/* fast path: with no tab/CR/ESC there is nothing to expand or strip,
	 * so just copy the run (memchr is SIMD, beating a scalar byte pass) */
	if (!memchr(s, '\t', n) && !memchr(s, '\r', n) && !memchr(s, 0x1b, n)) {
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

/* scan newly visible mapping range into lines; a trailing partial line
 * goes back through pend so streaming/follow continue seamlessly */
static void drain_map(void)
{
	size_t start = fmap_pos;
	for (;;) {
		const char *nl = memchr(fmap + start, '\n', fmap_len - start);
		if (!nl)
			break;
		size_t k = (size_t)(nl - fmap);
		const char *s = fmap + start;
		size_t n = k - start;
		/* SIMD probe for a tab/CR/ESC: if absent the line is clean */
		if (memchr(s, '\t', n) || memchr(s, '\r', n) || memchr(s, 0x1b, n)) {
			size_t len;
			char *clean = sanitize(s, n, &len);
			push_line(clean, len);
		} else {
			push_line(s, n);
		}
		start = k + 1;
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

static void load_all(void)
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
static int pump_follow(void)
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

/* layout: log pane, then status bar (source/position/keys), then the
 * command line -- everything worth looking at sits at the bottom, near
 * the eye's resting point. Tiny ttys drop the command line first, then
 * the status bar */
static int have_status_bar(void)
{
	return rows >= 2;
}

static int have_input_bar(void)
{
	return rows >= 3;
}

static size_t pane_rows(void)
{
	int n = rows - have_status_bar() - have_input_bar();
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

static void rebuild_view(void);

/* Literal filter (the default): memmem/memchr beats glibc regexec by
 * orders of magnitude over millions of lines. The pattern is a plain
 * substring; leading ^ / trailing $ anchor to line start/end. Ctrl-R
 * in the prompt flips to POSIX ERE mode instead. */
static LitSpec lit;

/* lowercase query matches case-insensitively; any uppercase flips exact */
static int smart_case(const char *q)
{
	for (const char *p = q; *p; p++)
		if (isupper((unsigned char)*p))
			return 0;
	return 1;
}

/* shared parse for the filter's and the highlight-search literal machines */
static void lit_parse(const char *q, int icase, LitSpec *ls)
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

/* byte offset of the first hit of pat[0..patlen) in s[0..len), or -1;
 * shared by the filter's and the search's literal matchers */
static ptrdiff_t pat_find(const char *pat, size_t patlen, int bol, int eol,
			  int icase, const char *s, size_t len)
{
#define PAT_EQ(p) \
	!(icase ? strncasecmp((p), pat, patlen) : memcmp((p), pat, patlen))
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
	/* icase: memchr either case of byte 0 (SIMD), verify folded */
	char lo = (char)tolower((unsigned char)pat[0]);
	char hi = (char)toupper((unsigned char)pat[0]);
	const char *p = s, *end = s + len;
	while (p < end) {
		size_t rem = (size_t)(end - p);
		const char *a = memchr(p, lo, rem);
		const char *b = lo == hi ? NULL : memchr(p, hi, rem);
		const char *hit = !a ? b : !b ? a : (a < b ? a : b);
		if (!hit)
			return -1;
		if ((size_t)(end - hit) >= patlen && PAT_EQ(hit))
			return hit - s;
		p = hit + 1;
	}
#undef PAT_EQ
	return -1;
}

/* shared matcher for the filter and the highlight search: dispatch a
 * literal LitSpec or a compiled regex against one line, filling m with
 * the match (highlight) span */
static int pattern_match(const Line *L, int is_re, const regex_t *re,
			 const LitSpec *ls, regmatch_t *m)
{
	if (is_re) {
		m->rm_so = 0;
		m->rm_eo = (regoff_t)L->len;	/* REG_STARTEND: no NUL needed */
		return regexec(re, L->s, 1, m, REG_STARTEND) == 0;
	}
	ptrdiff_t off = pat_find(ls->buf, ls->len, ls->bol, ls->eol,
				 ls->icase, L->s, L->len);
	if (off < 0)
		return 0;
	m->rm_so = (regoff_t)off;
	m->rm_eo = (regoff_t)(off + (ptrdiff_t)ls->len);
	return 1;
}

/* active-filter test; on match fills m with the highlight span */
static int query_match(const Line *L, regmatch_t *m)
{
	if (!filtered) {
		m->rm_so = m->rm_eo = 0;	/* empty span: nothing to highlight */
		return 1;
	}
	/* no zero-width guard here: the return decides view membership, and
	 * a pattern like a* matching empty must keep lines visible */
	return pattern_match(L, filtered_re, &re, &lit, m);
}

/* Narrow an extended query in place: appending chars can only shrink
 * the match set, so survivors must already be in view[]. */
static void refilter_narrow(void)
{
	regmatch_t m;
	size_t k = 0;
	for (size_t i = 0; i < nv; i++)
		if (query_match(&lines[view[i]], &m) != filter_inv)
			view[k++] = view[i];
	nv = k;
}

static void update_filter(const char *q)
{
	int was_filtered = filtered, clearing = !*q;
	char prev[sizeof query];
	snprintf(prev, sizeof prev, "%s", query);
	size_t was = nv ? view[cur] : 0;
	size_t was_row = cur - top;
	msg[0] = 0;
	if (clearing) {
		if (filtered && filtered_re)
			regfree(&re);
		filtered_re = 0;
		filtered = 0;
		filter_inv = 0;
		query[0] = 0;
		lit.len = 0;
	} else {
		int icase = smart_case(q);
		regex_t nr;
		if (re_mode) {
			int flags = REG_EXTENDED | (icase ? REG_ICASE : 0);
			if (regcomp(&nr, q, flags) != 0) {
				snprintf(msg, sizeof msg, "bad regex: %.100s", q);
				return;
			}
		}
		/* compile before freeing: a bad regex must keep the old view */
		if (filtered && filtered_re)
			regfree(&re);
		filtered_re = 0;
		if (re_mode) {
			re = nr;
			filtered_re = 1;
		} else {
			lit_parse(q, icase, &lit);
		}
		filtered = 1;
		snprintf(query, sizeof query, "%s", q);
	}
	/* query grew by appended chars: old matches are a superset, so
	 * re-testing just view[] suffices -- but only while including.
	 * Inverted, shrinking matches make outside lines eligible. */
	size_t prevlen = strlen(prev);
	if (was_filtered && !clearing && !filter_inv && prevlen &&
	    strlen(q) > prevlen && !memcmp(q, prev, prevlen))
		refilter_narrow();
	else
		rebuild_view();
	if (clearing && was_filtered && filter_anchor < nlines) {
		/* return to the line selected before filtering began; it is a
		 * binary search away since view[] is sorted. If it left with
		 * a rotation/reload, keep the clamped position instead. */
		size_t lo = view_floor(filter_anchor);
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
	} else if (!clearing) {
		/* membership reshuffled (mode flip, exclude-mode edits): a
		 * numeric cur would point at an arbitrary line, so re-anchor
		 * to the nearest line in file order instead */
		cur = view_floor(was);
		/* window the landed line back onto its old screen row,
		 * clamped like the clear-path restore below */
		size_t vis = pane_rows();
		size_t max_top = nv > vis ? nv - vis : 0;
		top = cur > was_row ? cur - was_row : 0;
		if (top > max_top)
			top = max_top;
	}
	ensure_visible();
}

static void extend_view(size_t from)
{
	regmatch_t m;
	for (size_t i = from; i < nlines; i++)
		if (query_match(&lines[i], &m) != filter_inv)
			push_view(i);
}

static void rebuild_view(void)
{
	nv = 0;
	extend_view(0);
	ensure_visible();
}

/* does the highlight-search pattern hit this line? fills m with the span */
static int search_match(const Line *L, regmatch_t *m)
{
	if (!searched)
		return 0;
	/* a zero-width regex match is not a hit: n/N and the scrollbar need
	 * a real span to land on */
	return pattern_match(L, searched_re, &sre, &slit, m) &&
	       (!searched_re || m->rm_eo > m->rm_so);
}

/* commit a highlight-search pattern: validate, then mark every line.
 * Extending a literal pattern can only turn hits off, so lines already
 * marked false are skipped -- typing stays cheap on huge files. */
static void update_search(const char *q)
{
	static char prev[MAX_QUERY];
	int icase = smart_case(q);
	size_t prevlen = strlen(prev);
	int extend = searched && !searched_re && !re_mode && prevlen &&
		     strlen(q) > prevlen && !memcmp(q, prev, prevlen);
	if (*q && re_mode) {
		regex_t nr;
		if (regcomp(&nr, q, REG_EXTENDED | (icase ? REG_ICASE : 0))) {
			snprintf(msg, sizeof msg, "bad regex: %.100s", q);
			return;	/* keep the old search */
		}
		if (searched_re)
			regfree(&sre);
		sre = nr;
		searched_re = 1;
	} else if (*q) {
		lit_parse(q, icase, &slit);
		searched_re = 0;
	}
	snprintf(search, sizeof search, "%s", q);
	searched = !!*q;
	for (size_t i = 0; i < nlines; i++) {
		if (!searched) {
			lines[i].srchit = 0;
			continue;
		}
		if (extend && !lines[i].srchit)
			continue;
		regmatch_t sm;
		lines[i].srchit = (unsigned char)search_match(&lines[i], &sm);
	}
	snprintf(prev, sizeof prev, "%s", q);
	dirty = 1;
}

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

static size_t str_cols(const char *s, size_t n)
{
	size_t w = 0, i = 0;
	while (i < n) {
		if ((unsigned char)s[i] < 0x80) {
			/* run of plain ASCII: one cell per byte; this is
			 * ~all of most logs, so skip UTF-8 decoding */
			size_t j = i;
			while (j < n && (unsigned char)s[j] < 0x80)
				j++;
			w += j - i;
			i = j;
		} else {
			size_t cl;
			w += (size_t)glyph_width(u8_decode(s + i, n - i, &cl));
			i += cl;
		}
	}
	return w;
}

/* widths never change once a line is stored */
static size_t line_cols(Line *L)
{
	if (!L->wcols)
		L->wcols = str_cols(L->s, L->len);
	return L->wcols;
}

/* widest line in the file; measures uncached lines on demand, so the
 * first hscroll-right pays what load no longer does up front */
static size_t widest_col(void)
{
	size_t w = 0;
	for (size_t i = 0; i < nlines; i++) {
		size_t cw = line_cols(&lines[i]);
		if (cw > w)
			w = cw;
	}
	return w;
}

static size_t line_rows(Line *L)
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
	size_t sl = strlen(src), ql = filtered ? strlen(query) : 0;
	for (;;) {
		snprintf(left, sizeof left, " %.*s  %s%s%s%s%s%s%.*s%s",
			 (int)sl, src, where, mk, flw, rmk, imk,
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

static void render(void)
{
	size_t vis = pane_rows();
	fputs("\x1b[H", stdout);
	size_t r = 0, i = top;
	for (; i < nv && r < vis; i++)
		r += draw_line(view[i], i == cur, r, vis);
	if (r < vis) {
		printf("\x1b[%zu;1H", r + 1);
		if (nv == 0 && !filtered)
			fputs("(empty)", stdout);
		fputs("\x1b[J", stdout);
	}
	/* scrollbar: right-edge rail over the full line count; '|' is the
	 * window thumb (always visible), '-' dashes the track, '#' marks
	 * track rows holding search hits outside the window */
	if (cols > 1 && (searched || nv > vis) && nv > 0) {
		size_t drew = i - top;
		size_t tlo = nv > vis ? top * vis / nv : 0;
		size_t thi = nv > vis ? (top + drew) * vis / nv : vis;
		if (thi <= tlo)
			thi = tlo + 1;
		fputs("\x1b[0m", stdout);
		for (size_t sr = 0; sr < vis; sr++) {
			int hit = 0;
			if (searched) {
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
	static const struct {
		const char *argv[5];
		int wl, x11;	/* required session: WAYLAND_DISPLAY / DISPLAY */
	} cands[] = {
		{ { "wl-copy", NULL },					1, 0 },
		{ { "xclip", "-selection", "clipboard", "-in", NULL },	0, 1 },
		{ { "pbcopy", NULL },					0, 0 },
		{ { "termux-clipboard-set", NULL },			0, 0 },
	};
	int wl = getenv("WAYLAND_DISPLAY") != NULL;
	int x11 = getenv("DISPLAY") != NULL;
	for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
		if ((cands[i].wl && !wl) || (cands[i].x11 && !x11))
			continue;
		if (!has_prog(cands[i].argv[0]))
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
			execvp(cands[i].argv[0], (char *const *)cands[i].argv);
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
		for (size_t i = 0; i < nlines; i++) {
			if (!lines[i].marked)
				continue;
			memcpy(buf + off, lines[i].s, lines[i].len);
			off += lines[i].len;
			buf[off++] = '\n';
		}
		copy_text(buf, off);
		free(buf);
		snprintf(msg, sizeof msg, "copied %zu marked lines (%zu bytes)",
			 nmarked, off);
		clear_marks();
		return;
	}
	if (nv == 0)
		return;
	Line *L = &lines[view[cur]];
	copy_text(L->s, L->len);
	snprintf(msg, sizeof msg, "copied line %zu (%zu bytes)", cur + 1, L->len);
}

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
			case '3':	   return K_DEL;
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

/* previous/next UTF-8 boundary around i; i sits on a boundary */
static size_t u8_prev(const char *s, size_t i)
{
	do i--;
	while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80);
	return i;
}

static size_t u8_next(const char *s, size_t i, size_t n)
{
	i++;
	while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80)
		i++;
	return i;
}

static void move_up(void)
{
	mdir = -1;
	if (cur > 0)
		cur--;
}

static void move_down(void)
{
	mdir = 1;
	if (cur + 1 < nv)
		cur++;
}

static void page_down(void)
{
	cur += pane_rows();
}

static void page_up(void)
{
	cur = cur > pane_rows() ? cur - pane_rows() : 0;
}

static void apply_edit(void)
{
	edit[MAX_QUERY - 1] = 0;
	if (editing_search)
		update_search(edit);
	else
		update_filter(edit);
}

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
		{ A_FILTER,	"filter (incremental, smart case literal)" },
		{ A_SEARCH,	"highlight-only search (no filtering)" },
		{ A_SNEXT,	"next search match" },
		{ A_SPREV,	"previous search match" },
		{ A_CLEAR_SEARCH, "clear the highlight search" },
		{ A_CLEAR_FILTER, "clear filter" },
		{ A_CANCEL,	"cancel editing; clear marks, else filter" },
		{ A_MARK,	"mark line and sweep" },
		{ A_UNMARK,	"unmark line and sweep" },
		{ A_COPY,	"copy marked lines (clears marks), else current" },
		{ A_FOLLOW,	"toggle follow" },
		{ A_RELOAD,	"reload file" },
		{ A_WRAP,	"toggle line wrap" },
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
	fputs("\n  Sweep direction follows the last up/down move.\n"
	      "  In a prompt, Ctrl-R toggles literal/regex mode; Ctrl-V (filter\n"
	      "  only) excludes matching lines. (R)/(!) show in the status bar.\n"
	      "  The right-edge scrollbar marks rows containing search hits.\n", out);
}

static void usage(FILE *out)
{
	fputs(
"usage: comb [-e REGEX] [--no-color] [FILE]\n"
"\n"
"View, filter and copy log files. Reads FILE, or stdin when piped.\n"
"Filters match literal text by default (smart case).\n"
"\n"
"options:\n"
"  -e REGEX               start with REGEX as the filter\n"
"  --no-color, -C         disable syntax coloring (also honors NO_COLOR)\n"
"  -h, --help             show this help\n"
"\n"
"keys:\n", out);
	print_keys(out);
}

/* filter prompt editing; Up/Down/PgUp/PgDn stay live to scroll the
 * results -- terminal wheel scroll arrives as these keys too */
static void edit_key(int key)
{
	size_t elen = strlen(edit);

	switch (key) {
	case 0x1b:	case '\r': case '\n':
			editing = 0;
			editing_search = 0;
			break;
		case K_UP:
			move_up();
			break;
		case K_DOWN:
			move_down();
			break;
		case K_PGDN:
			page_down();
			break;
		case K_PGUP:
			page_up();
			break;
		case K_LEFT: case CTL('b'):
			if (ecur > 0)
				ecur = u8_prev(edit, ecur);
			break;
		case K_RIGHT: case CTL('f'):
			if (ecur < elen)
				ecur = u8_next(edit, ecur, elen);
			break;
		case K_HOME: case CTL('a'):
			ecur = 0;
			break;
		case K_END: case CTL('e'):
			ecur = elen;
			break;
		case K_DEL:
			if (ecur < elen) {
				size_t n = u8_next(edit, ecur, elen);
				memmove(edit + ecur, edit + n, elen - n + 1);
				apply_edit();
			}
			break;
		case 0x7f: case CTL('h'):	/* backspace */
			if (ecur > 0) {
				size_t p = u8_prev(edit, ecur);
				memmove(edit + p, edit + ecur, elen - ecur + 1);
				ecur = p;
				apply_edit();
			}
			break;
		case CTL('u'):
			edit[0] = 0;
			ecur = 0;
			apply_edit();
			break;
		case CTL('r'):	/* toggle literal/regex filter */
			re_mode = !re_mode;
			apply_edit();
			if (!*msg)	/* bad-regex notice wins over the mode notice */
				snprintf(msg, sizeof msg, re_mode ? "regex mode"
							  : "literal mode");
			break;
		case CTL('v'):	/* exclude instead of include matches */
			if (editing_search)
				break;	/* filter-only toggle */
			filter_inv = !filter_inv;
			apply_edit();
			if (!*msg)
				snprintf(msg, sizeof msg, filter_inv
							  ? "excluding matches"
							  : "including matches");
			break;
		default:
			if (key >= 32 && key < 256 && elen < MAX_QUERY - 1) {
				/* multibyte chars arrive as separate bytes:
				 * in-order insertion keeps them contiguous
				 * behind the lead byte */
				memmove(edit + ecur + 1, edit + ecur,
					elen - ecur + 1);
				edit[ecur++] = (char)key;
				apply_edit();
			}
			break;
	}
}

static void view_action(int act)
{
	switch (act) {
	case A_QUIT:
		running = 0;
		break;
	case A_DOWN:
		move_down();
		break;
	case A_UP:
		move_up();
		break;
	case A_PGDOWN:
		page_down();
		break;
	case A_PGUP:
		page_up();
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
		size_t maxc = widest_col();
		size_t max = maxc > (size_t)cols
				   ? maxc - (size_t)cols : 0;
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
		editing_search = 0;
		filter_anchor = nv ? view[cur] : 0;
		filter_row = cur - top;
		snprintf(edit, sizeof edit, "%s", query);
		ecur = strlen(edit);
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
	case A_SEARCH:
		editing = 1;
		editing_search = 1;
		snprintf(edit, sizeof edit, "%s", search);
		ecur = strlen(edit);
		break;
	case A_SNEXT:
	case A_SPREV: {
		if (!searched || !nlines || !nv)
			break;
		int dir = act == A_SNEXT ? 1 : -1;
		/* walk the visible view[], not the file: srchit is set on every
		 * line, so scanning lines[] would land on a hit hidden by the
		 * filter and snap cur to a nearby visible, non-match line. */
		size_t li = cur;
		size_t i = li;
		do
			i = dir > 0 ? (i + 1 < nv ? i + 1 : 0)
				    : (i > 0 ? i - 1 : nv - 1);
		while (!lines[view[i]].srchit && i != li);
		if (lines[view[i]].srchit) {
			cur = i;
			/* the hit may sit beyond the right edge: bring its span
			 * into view, keeping a margin clear of the scrollbar */
			regmatch_t sm;
			if (search_match(&lines[view[i]], &sm)) {
				size_t so = (size_t)sm.rm_so, se = (size_t)sm.rm_eo;
				size_t maxc = widest_col();
				size_t max = maxc + 2 > (size_t)cols
						 ? maxc + 2 - (size_t)cols : 0;
				size_t hs = (size_t)hscroll;
				if (se >= hs + (size_t)cols - 1)
					hs = se + 2 > (size_t)cols
						     ? se + 2 - (size_t)cols : 0;
				if (so < hs)
					hs = so > 2 ? so - 2 : 0;
				if (hs > max)
					hs = max;
				hscroll = (int)hs;
			}
		} else
			snprintf(msg, sizeof msg, "no matches");
		break;
	}
	case A_CLEAR_SEARCH:
		if (searched) {
			update_search("");
			snprintf(msg, sizeof msg, "search cleared");
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
		restore_terminal();
		signal(SIGTSTP, SIG_DFL);	/* may be inherited as SIG_IGN */
		/* stop the whole foreground group, like kernel ISIG would:
		 * under doas/sudo our parent shares the pgrp, and until it
		 * stops too the waiting shell never regains the prompt */
		kill(0, SIGTSTP);
		tty_enter();
		tcflush(kfd, TCIFLUSH);	/* keys typed while suspended */
		dirty = 1;
		break;
	case A_REPAINT:
	default:
		break;
	}
}

#ifndef COMB_TEST
/* tests/selftest.c includes this file and provides its own main() */
int main(int argc, char **argv)
{
	const char *init_re = NULL;
	const char *file = NULL;

	int endopts = 0;
	for (int i = 1; i < argc; i++) {
		if (!endopts && !strcmp(argv[i], "--")) {
			endopts = 1;
		} else if (!endopts && !strcmp(argv[i], "-e") && i + 1 < argc) {
			init_re = argv[++i];
		} else if (!endopts &&
			   (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))) {
			usage(stdout);
			return 0;
		} else if (!endopts &&
			   (!strcmp(argv[i], "--no-color") || !strcmp(argv[i], "-C"))) {
			nocolor = 1;
		} else if (!endopts && !strcmp(argv[i], "-")) {
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

	if (use_stdin)
		fcntl(STDIN_FILENO, F_SETFL,
		      fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);	/* auto-reap; wl-copy must outlive us */
	signal(SIGWINCH, on_winch);
	signal(SIGTERM, on_sigexit);
	signal(SIGHUP, on_sigexit);
	signal(SIGINT, on_sigexit);
	/* A privileged feeder (doas/sudo dmesg -w | comb) prompts for its
	 * password on this very tty; stay in cooked mode until the pipe
	 * produces its first byte or closes, so the prompt works. */
	if (use_stdin) {
		struct pollfd pw = { .fd = STDIN_FILENO, .events = POLLIN };
		poll(&pw, 1, -1);
	}
	tty_enter();

	load_all();
	if (init_re) {
		re_mode = 1;	/* -e promises a REGEX */
		update_filter(init_re);
	} else
		rebuild_view();
	cur = nv ? nv - 1 : 0;
	ensure_visible();

	int key;
	while (running) {
		if (got_winch) {
			got_winch = 0;
			get_winsize();
			ensure_visible();
			dirty = 1;
		}
		if (follow && !(use_stdin && stdin_eof)) {
			int stick = nv > 0 && cur >= nv - 1;
			size_t old = nlines;
			int got = pump_follow();
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

		/* paint pending changes before waiting; skipped while
		 * keystrokes are queued so key repeats coalesce */
		if (dirty && !key_pending()) {
			render();
			dirty = 0;
		}

		if (!key_pending()) {
			struct pollfd pp[2] = {
				{ .fd = kfd,	.events = POLLIN },
				{ .fd = STDIN_FILENO,	.events = POLLIN },
			};
			int np = follow && use_stdin && !stdin_eof ? 2 : 1;
			poll(pp, np, 200);
			/* woke for data (or timed out): lap around; woke for
			 * a key: fall through and read it */
			if (!(pp[0].revents & POLLIN))
				continue;
		}

		key = read_key();
		if (key == K_EOF)
			break;
		msg[0] = 0;

		if (editing)
			edit_key(key);
		else
			view_action(key_action(key));
		ensure_visible();
		dirty = 1;
	}

	restore_terminal();
	/* a live feeder would outlive us and keep the pipeline (and the
	 * shell waiting on it) alive; take down the job's group like
	 * Ctrl-C would. Best effort: root-owned feeders ignore it. */
	if (use_stdin && !stdin_eof) {
		tio_saved = 0;	/* on_sigexit must not repaint the leave sequence */
		kill(0, SIGINT);
	}
	return 0;
}
#endif /* COMB_TEST */
