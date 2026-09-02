/* comb config */

/* max bytes of a filter/search query*/
#define MAX_QUERY 256

/* below this the UI stops trying: a smaller window still works (position,
 * keys) but the pane is unreadable, so render() paints a clear notice
 * until the window is big enough. 6 rows = 4 line pane + status + input. */
#define MIN_COLS 30
#define MIN_ROWS 6

/* upper bound on threads, probably doesn't scale well above this */
#define MAX_THREADS 64

#define MAX_LINE_LEN 4096

/* suffix painted (dim) after a truncated line; U+2026 is one cell wide so
 * it folds into the wrap/scroll width accounting like any other glyph.
 * LINE_TRUNC_COLS must match str_cols(LINE_TRUNC_TAIL) */
#define LINE_TRUNC_TAIL  "\xe2\x80\xa6"
#define LINE_TRUNC_COLS  1

/* show the filter/search job spinner only after a scan has run this long;
 * fast scans finish inside a batch or two and shouldn't flash it */
#define JOB_PROG_MS 1000

/* ms between spinner redraws once it is shown; a batch is only a few ms,
 * so without a cap a fast scan would repaint it every batch */
#define JOB_SPIN_MS 100

#define TTY_ENTER "\x1b[?1049h\x1b[?25l\x1b[2J"
#define TTY_LEAVE "\x1b[0m\x1b[?25h\x1b[?1049l"

/* One escape sequence per colour, per render mode. The ANSI/256-colour
 * form (theme-dependent) is the fallback; the RGB/truecolor form draws the
 * exact colour and is used on terminals that advertise support. Both carry
 * the same modifier prefix, so bold/dim weight matches. colof() picks the
 * active form once, off the runtime truecolor flag. */
typedef struct { const char *ansi, *rgb; } Color;
extern int truecolor;
static inline const char *colof(const Color *c)
{
	return truecolor ? c->rgb : c->ansi;
}

/* current-line background */
static const Color CUR_BG = { "\x1b[100m", "\x1b[48;2;56;60;72m" };
/* marked-line background */
static const Color MARK_BG = { "\x1b[48;5;238m", "\x1b[48;2;68;68;68m" };
/* status/input/help bars: an explicit dark strip + light text instead of
 * reverse video, so the bar colour is terminal-independent (Konsole drew
 * reverse-video as a light gray). nocolor keeps the plain inverse bar. */
static const Color BAR_BG = { "\x1b[48;5;236m", "\x1b[48;2;44;47;54m" };
static const Color BAR_FG = { "\x1b[97m", "\x1b[38;2;221;221;226m" };
/* regex-mode accent for the prompt label (drawn as a fg on the bar bg) */
static const Color PROMPT_ACCENT = { "\x1b[38;5;173m", "\x1b[38;2;215;135;95m" };
static const Color QUOTE_COLOR = { "\x1b[38;5;223m", "\x1b[38;2;255;215;175m" };
static const Color PAREN_COLOR = { "\x1b[38;5;115m", "\x1b[38;2;135;215;175m" };
#define DIM "\x1b[2m"

/* service-tag hues; the RGB form is the indexed colour's exact value */
static const Color svc_palette[] = {
	{ "\x1b[38;5;215m", "\x1b[38;2;255;175;95m" },	/* peach */
	{ "\x1b[38;5;79m",  "\x1b[38;2;95;215;175m" },	/* seafoam */
	{ "\x1b[38;5;110m", "\x1b[38;2;135;175;215m" },	/* cornflower */
	{ "\x1b[38;5;146m", "\x1b[38;2;175;175;215m" },	/* lilac */
	{ "\x1b[38;5;151m", "\x1b[38;2;175;215;175m" },	/* sage */
	{ "\x1b[38;5;179m", "\x1b[38;2;215;175;95m" },	/* honey */
	{ "\x1b[38;5;173m", "\x1b[38;2;215;135;95m" },	/* coral */
	{ "\x1b[38;5;80m",  "\x1b[38;2;95;215;215m" },	/* sky */
	{ "\x1b[38;5;182m", "\x1b[38;2;215;175;215m" },	/* mauve */
	{ "\x1b[38;5;144m", "\x1b[38;2;175;175;135m" },	/* sand */
	{ "\x1b[38;5;115m", "\x1b[38;2;135;215;175m" },	/* celadon */
	{ "\x1b[38;5;250m", "\x1b[38;2;188;188;188m" },	/* silver */
};
#define NSVC_COLORS (sizeof svc_palette / sizeof svc_palette[0])

/* severity keyword hues */
static const struct { const char *w; Color a; } sev_palette[] = {
	{ "fatal", { "\x1b[1;95m", "\x1b[1;38;2;212;101;222m" } },	/* magenta */
	{ "panic", { "\x1b[1;95m", "\x1b[1;38;2;212;101;222m" } },
	{ "emerg", { "\x1b[1;95m", "\x1b[1;38;2;212;101;222m" } },
	{ "alert", { "\x1b[1;95m", "\x1b[1;38;2;212;101;222m" } },
	{ "crit",  { "\x1b[1;95m", "\x1b[1;38;2;212;101;222m" } },
	{ "error", { "\x1b[1;91m", "\x1b[1;38;2;230;74;74m" } },	/* red */
	{ "err",   { "\x1b[1;91m", "\x1b[1;38;2;230;74;74m" } },
	{ "warn",  { "\x1b[1;93m", "\x1b[1;38;2;219;179;79m" } },	/* yellow */
	{ "debug", { DIM, DIM } },
	{ "trace", { DIM, DIM } },
};

/* <level> token hues */
static const struct { const char *w; Color a; } token_palette[] = {
	{ "info",   { "\x1b[38;5;110m", "\x1b[38;2;135;175;215m" } },	/* cornflower */
	{ "notice", { "\x1b[38;5;79m",  "\x1b[38;2;95;215;175m" } },	/* seafoam */
	{ "audit",  { "\x1b[38;5;146m", "\x1b[38;2;175;175;215m" } },	/* lilac */
};

#define CTL(x)	((x) & 0x1f)
enum {
	K_NONE = 0x100, K_EOF, K_UP, K_DOWN, K_LEFT, K_RIGHT,
	K_PGUP, K_PGDN, K_HOME, K_END, K_DEL
};

enum {
	A_NONE, A_QUIT, A_REPAINT,
	A_DOWN, A_UP, A_PGDOWN, A_PGUP, A_TOP, A_BOT,
	A_LEFT, A_RIGHT, A_HSTART, A_HEND,
	A_FILTER, A_CLEAR_FILTER, A_CANCEL,
	A_MARK, A_UNMARK, A_COPY,
	A_FOLLOW, A_RELOAD, A_WRAP, A_STOP,
	A_SEARCH, A_SNEXT, A_SPREV, A_CLEAR_SEARCH,
	A_HELP,
	A_DEBUG,
};

static const struct { int key; int act; } keymap[] = {
	{ 'q',	      A_QUIT },
	{ CTL('c'),   A_QUIT },

	{ 'j',	      A_DOWN },
	{ 'e',	      A_DOWN },
	{ '\r',      A_DOWN },
	{ '\n',      A_DOWN },
	{ K_DOWN,    A_DOWN },
	{ 'k',	      A_UP },
	{ K_UP,	     A_UP },
	{ 'd',	      A_PGDOWN },
	{ CTL('d'),  A_PGDOWN },
	{ K_PGDN,    A_PGDOWN },
	{ CTL('f'),  A_PGDOWN },
	{ 'u',	      A_PGUP },
	{ CTL('u'),  A_PGUP },
	{ K_PGUP,    A_PGUP },
	{ 'b',	      A_PGUP },
	{ CTL('b'),  A_PGUP },
	{ 'g',	      A_TOP },
	{ K_HOME,    A_TOP },
	{ 'G',	      A_BOT },
	{ K_END,     A_BOT },

	{ 'h',	      A_LEFT },
	{ K_LEFT,    A_LEFT },
	{ 'l',	      A_RIGHT },
	{ K_RIGHT,   A_RIGHT },
	{ '0',	      A_HSTART },
	{ '^',	      A_HSTART },
	{ '$',	      A_HEND },

	{ '/',	      A_FILTER },
	{ '?',	      A_CLEAR_FILTER },
	{ 0x1b,	     A_CANCEL },
	{ ' ',	      A_MARK },
	{ 'x',	      A_UNMARK },
	{ 'c',	      A_COPY },
	{ 'y',	      A_COPY },

	{ CTL('z'),  A_STOP },
	{ '\\',	     A_SEARCH },
	{ '|',	      A_CLEAR_SEARCH },
	{ 'n',	      A_SNEXT },
	{ 'N',	      A_SPREV },

	{ 'f',	      A_FOLLOW },
	{ 'w',	      A_WRAP },
	{ 'r',	      A_RELOAD },
	{ CTL('l'),  A_REPAINT },
	{ CTL('h'),  A_HELP },
	{ CTL('o'),  A_DEBUG },
};
