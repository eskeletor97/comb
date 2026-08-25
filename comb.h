/* comb - shared types and the cross-module interface.
 * Each module owns a slice of the viewer (banner atop each .c); anything
 * reaching across slices is declared here. */
#ifndef COMB_H
#define COMB_H

#include "config.h"

#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>

/* one ingested line: s points either into the arena or into the mmap */
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

/* alternation of literals: a "regex" that is really just a few literal
 * needles (error|panic, ^sshd, foo$). Each piece is a plain byte string
 * with its own ^/$ anchors; matched with the SIMD literal path instead of
 * glibc regexec. Set up by alt_parse, selected by alt_match. */
#define ALT_MAX 32
typedef struct {
	char buf[MAX_QUERY];		/* decoded literal bytes, one run per piece */
	size_t off[ALT_MAX];		/* start of each piece in buf */
	size_t len[ALT_MAX];		/* piece byte length */
	unsigned char bol[ALT_MAX], eol[ALT_MAX];
	int n;				/* number of pieces */
	unsigned char icase;
} AltSpec;

/* worker entry point handed to par_run/scan_collect */
typedef void (*par_work)(size_t lo, size_t hi, int slot, void *ctx);

/* --- shared viewer state (defined in state.c) ------------------------ */

extern Line *lines;
extern size_t nlines, lcap;

extern size_t *view;
extern size_t nv, vcap;

extern char path[4096];
extern int use_stdin;
extern int fd;		/* log file */
extern int kfd;		/* keyboard: stdin, or /dev/tty when stdin isn't a tty */
extern off_t fsize;
extern int stdin_eof;	/* pipe closed: no more input will ever come */

/* zero-copy window onto a regular file */
extern char *fmap;
extern size_t fmap_len, fmap_pos;

/* running max of line widths; maintained at push time, see push_line.
 * reset_lines rewinds it alongside the lines themselves. */
extern size_t wc_max;

/* filter (narrows view[]) */
extern regex_t re;
extern int filtered;
extern int re_mode;		/* UI toggle for the next filter: literal vs ERE */
extern int filtered_re;		/* the live filter is a compiled regex */
extern int filtered_alt;	/* the live filter is an alternation of literals */
extern int filter_inv;		/* live filter excludes matching lines */
extern LitSpec lit;
extern AltSpec alt;
extern char query[MAX_QUERY];
extern char edit[MAX_QUERY];
extern int editing;
extern size_t ecur;	/* insertion point: byte offset into edit[] */

/* highlight-only search: marks lines but never narrows view[] */
extern char search[MAX_QUERY];
extern int searched;
extern int editing_search;	/* prompt currently edits search, not filter */
extern regex_t sre;
extern int searched_re;
extern LitSpec slit;
extern AltSpec salt;
extern int searched_alt;	/* search is an alternation of literals */

/* pending patterns for in-flight scans: the UI keeps serving the last
 * committed view/pattern until the job lands (see step_job), so Esc can
 * drop a mistyped query wholesale. Workers test the pending specs; n/N,
 * push_line marking and the status bar stay on the committed ones. */
extern regex_t jfre, jsre;
extern LitSpec jflit, jslit;
extern AltSpec jfalt, jsalt;
extern int jf_on, jf_all, jf_re, jf_alt;	/* pending filter pattern */
extern int js_on, js_active, js_re, js_alt;	/* pending search pattern */

/* viewport / UI state */
extern size_t cur, top;
extern size_t filter_anchor;	/* line selected when filtering began */
extern size_t filter_row;	/* its screen row, restored on clear */
extern int hscroll;
extern int rows, cols;
extern int wrap;
extern int follow;
extern int nocolor;
extern const char *mark_bg;
extern int running, dirty;
extern char msg[160];
extern size_t nmarked;
extern struct termios saved_tio;
extern int tio_saved;
extern volatile sig_atomic_t got_winch;

/* While editing a filter/search prompt, keystrokes update the buffer but
 * defer the expensive per-line recompute until typing idles. After this
 * many ms of no keys, the pending query is applied once. */
#define SEARCH_DEBOUNCE_MS 150
extern int pending_update;	/* a query change awaits the debounce */
extern uint64_t debounce_due;	/* deadline for the deferred recompute */
extern int force_render;	/* commit a queued-keys coalescing render */

/* long-scan jobs: lines scanned per lap (see step_job) */
#define JOB_BATCH ((size_t)1 << 21)
extern int job_active;		/* an incremental scan is in flight */
extern int pend_ext;		/* follow append deferred past the job */
extern size_t pend_ext_from;

/* load progress (set by main, maintained in load.c) */
extern uint64_t prog_t0;	/* set in main, right before load_all() */
extern size_t prog_fed;		/* bytes handed to feed() */

/* --- base.c ---------------------------------------------------------- */
void die(const char *fmt, ...);
void die_sys(const char *fmt, ...);
void *xrealloc(void *p, size_t n);
uint64_t now_ms(void);
void restore_terminal(void);
void on_sigexit(int sig);
void on_winch(int sig);
void get_winsize(void);
void tty_enter(void);

/* --- load.c ---------------------------------------------------------- */
void flush_pending(void);
void reset_lines(void);
void load_all(void);
int pump_follow(void);			/* 0 none, 1 appended, 2 rotated */
void human_bytes(char *o, size_t v);
void group_digits(char *o, size_t v);
void prog_hide(void);

/* --- match.c --------------------------------------------------------- */
int smart_case(const char *q);
void lit_parse(const char *q, int icase, LitSpec *ls);
int alt_parse(const char *q, int icase, AltSpec *as);
int pattern_match(const Line *L, int is_re, int is_alt,
		  const regex_t *re, const LitSpec *ls,
		  const AltSpec *alt, regmatch_t *m);
int query_match(const Line *L, regmatch_t *m);
int search_match(const Line *L, regmatch_t *m);
int par_threads(size_t len);
void par_run(size_t lo, size_t hi, int nthreads, par_work work, void *ctx);
void scan_collect(size_t lo, size_t hi, size_t (*pos_line)(size_t),
		  int (*match)(const Line *L, regmatch_t *m), int inv,
		  size_t **arr, size_t *n, size_t *cap);
size_t pos_ident(size_t p);
size_t pos_view(size_t p);

/* --- jobs.c ---------------------------------------------------------- */
int have_status_bar(void);
int have_input_bar(void);
size_t pane_rows(void);
void ensure_visible(void);
void job_discard(void);
void job_finish(int commit);		/* commit 1 = apply, 0 = cancel */
void step_job(void);
void job_flush(void);
void start_extend_view(size_t from);
void update_filter(const char *q);
void update_search(const char *q);
void rebuild_view(void);
void extend_view(size_t from);
void defer_update(void);
void apply_edit(void);
int debounce_elapsed(void);
int debounce_ms_left(void);

/* --- render.c -------------------------------------------------------- */
size_t str_cols(const char *s, size_t n);
size_t widest_col(void);
size_t line_cols(Line *L);
size_t line_rows(Line *L);
void render(void);

/* --- clip.c ---------------------------------------------------------- */
void clear_marks(void);
void copy_current(void);

/* --- input.c --------------------------------------------------------- */
int key_action(int key);
int key_pending(void);
int read_key(void);
void usage(FILE *out);
void edit_key(int key);
void view_action(int act);

#endif /* COMB_H */
