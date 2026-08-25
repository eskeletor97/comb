/* comb config */

#define MAX_QUERY 256

/* show the filter/search job spinner only after a scan has run this long;
 * fast scans finish inside a batch or two and shouldn't flash it */
#define JOB_PROG_MS 1000

#define TTY_ENTER "\x1b[?1049h\x1b[?25l\x1b[2J"
#define TTY_LEAVE "\x1b[0m\x1b[?25h\x1b[?1049l"

#define MARK_BG		"\x1b[48;5;238m"	/* marked-line background */
#define QUOTE_COLOR "\x1b[38;5;223m"
#define PAREN_COLOR	"\x1b[38;5;115m"	/* (...) context */
#define DIM		"\x1b[2m"

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

/* severity keyword hues */
static const struct { const char *w, *a; } sev_palette[] = {
	{ "fatal", "\x1b[1;95m" },	/* bright magenta */
	{ "panic", "\x1b[1;95m" },
	{ "emerg", "\x1b[1;95m" },
	{ "alert", "\x1b[1;95m" },
	{ "crit",  "\x1b[1;95m" },
	{ "error", "\x1b[1;91m" },	/* bright red */
	{ "err",   "\x1b[1;91m" },
	{ "warn",  "\x1b[1;93m" },	/* bright yellow */
	{ "debug", DIM,	},
	{ "trace", DIM,	},
};

/* <level> token hues */
static const struct { const char *w, *a; } token_palette[] = {
	{ "info",   "\x1b[38;5;110m" },	/* cornflower */
	{ "notice", "\x1b[38;5;79m" },	/* seafoam */
	{ "audit",  "\x1b[38;5;146m" },	/* lilac */
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
};
