/* comb - keys: decoding, prompt editing, commands */

#define _GNU_SOURCE
#include "comb.h"

#include <poll.h>
#include <string.h>
#include <unistd.h>

static int mdir = -1;	/* space/x sweep direction: -1 up, 1 down */


int key_action(int key)
{
	for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
		if (keymap[i].key == key)
			return keymap[i].act;
	return A_NONE;
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
int key_pending(void)
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

int read_key(void)
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

void usage(FILE *out)
{
	fprintf(	out,
"usage: comb [-e REGEX] [--no-color] [FILE]\n"
"\n"
"View, filter and copy log files. Reads FILE, or stdin when piped.\n"
"Filters match literal text by default (smart case).\n"
"\n"
"options:\n"
"  -e REGEX               start with REGEX as the filter\n"
"  --no-color, -C         disable syntax coloring (also honors NO_COLOR)\n"
"  -t, --threads N        worker threads for load/filter/search\n"
"                         (default: online CPUs - 2, min 1, max %d)\n"
"  -h, --help             show this help\n"
"\n"
"keys:\n", MAX_THREADS);
	print_keys(out);
}

/* filter prompt editing; Up/Down/PgUp/PgDn stay live to scroll the
 * results -- terminal wheel scroll arrives as these keys too */
void edit_key(int key)
{
	size_t elen = strlen(edit);

	switch (key) {
	case 0x1b:	case '\r': case '\n':
			if (pending_update)	/* commit a still-deferred query */
				apply_edit();
			force_render = 1;	/* show the committed result */
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
				defer_update();
			}
			break;
		case 0x7f: case CTL('h'):	/* backspace */
			if (ecur > 0) {
				size_t p = u8_prev(edit, ecur);
				memmove(edit + p, edit + ecur, elen - ecur + 1);
				ecur = p;
				defer_update();
			}
			break;
		case CTL('u'):
			edit[0] = 0;
			ecur = 0;
			defer_update();
			break;
		case CTL('r'):	/* toggle literal/regex filter */
			re_mode = !re_mode;
			defer_update();
			if (!*msg)	/* bad-regex notice wins over the mode notice */
				snprintf(msg, sizeof msg, re_mode ? "regex mode"
							  : "literal mode");
			break;
		case CTL('v'):	/* exclude instead of include matches */
			if (editing_search)
				break;	/* filter-only toggle */
			filter_inv = !filter_inv;
			defer_update();
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
				defer_update();
			}
			break;
	}
}

void view_action(int act)
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
			size_t cw = line_cols(view_at(cur));
			hscroll = cw > (size_t)cols
					  ? (int)(cw - (size_t)cols)
					  : 0;
		}
		break;
	case A_FILTER:
		editing = 1;
		editing_search = 0;
		filter_anchor = nv ? view_at(cur) : 0;
		filter_row = cur - top;
		snprintf(edit, sizeof edit, "%s", filter_pat.text);
		ecur = strlen(edit);
		break;
	case A_MARK:
	case A_UNMARK: {
		int want = (act == A_MARK);
		if (nv) {
			size_t lx = view_at(cur);
			if (lt_is_marked(lx) != want) {
				lt_mark(lx, want);
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
		if (filter_pat.active) {
			edit[0] = 0;
			update_filter("");
		}
		break;
	case A_SEARCH:
		editing = 1;
		editing_search = 1;
		snprintf(edit, sizeof edit, "%s", search_pat.text);
		ecur = strlen(edit);
		break;
	case A_SNEXT:
	case A_SPREV: {
		if (!search_pat.active || !nlines || !nv)
			break;
		int dir = act == A_SNEXT ? 1 : -1;
		/* walk the visible view[], not the file: a hit bit is set on every
		 * matching line, so scanning the raw lines would land on a hit hidden
		 * by the filter and snap cur to a nearby visible, non-match line. */
		size_t li = cur;
		size_t i = li;
		do
			i = dir > 0 ? (i + 1 < nv ? i + 1 : 0)
				    : (i > 0 ? i - 1 : nv - 1);
		while (!hit_at(view_at(i)) && i != li);
		if (hit_at(view_at(i))) {
			Line lz;
			size_t lx = view_at(i);
			cur = i;
			/* the hit may sit beyond the right edge: bring its span
			 * into view, keeping a margin clear of the scrollbar */
			regmatch_t sm;
			lt_fill(&lz, lx);
			if (search_match(lz.s, lz.len, &sm)) {
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
		if (search_pat.active) {
			update_search("");
			snprintf(msg, sizeof msg, "search cleared");
		}
		break;
	case A_CANCEL:
		if (nmarked) {
			clear_marks();
			snprintf(msg, sizeof msg, "marks cleared");
		} else if (filter_pat.active) {
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
