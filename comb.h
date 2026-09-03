/* comb - shared types and the cross-module interface.
 * Each module owns a slice of the viewer (banner atop each .c); anything
 * reaching across slices is declared here. */
#ifndef COMB_H
#define COMB_H

#include "config.h"

#include <limits.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>

/* minimum output buffer for human_bytes() (defined for its callers too) */
#define HUMAN_BYTES_BUF 32

/* one materialised line: s points either into the arena, the mmap, or a
 * sanitising copy made on demand. A Line is a *transient* view built by
 * lt_fill() for lines currently on screen (or being tested); the only
 * persistent per-line state lives in lidx[] and the marker/search bitmaps. */
typedef struct {
	const char *s;
	size_t len;
	int tag_so, tag_eo;	/* byte span of the service tag, -1 if none */
	int slot;		/* svc_palette index, -1 if no tag */
	unsigned char marked;
	unsigned char srchit;	/* highlight-search hit, for n/N + scrollbar */
	unsigned char trunc;	/* line was cut at MAX_LINE_LEN; draw the tail marker */
	size_t wcols;		/* display width cache, 0 = uncomputed */
	const char *sev;	/* severity SGR cache, NULL = unscanned */
} Line;

/* the persistent, compact per-line index. Everything the viewer needs to
 * reach line i on demand: where its raw bytes live and how long they are.
 * Display width, service-tag span, palette slot and severity are *not*
 * stored here -- they are recomputed by lt_fill() only for lines that are
 * actually drawn. raw points into the mmap (clean file lines) or into the
 * arena (stdin / appended lines, and the sanitised copy lt_text() writes
 * back the first time a dirty line is touched). Both are stable until the
 * next reset_lines(). */
typedef struct {
	const char *raw;
	uint32_t len;		/* byte length, excluding the '\n'; <= MAX_LINE_LEN */
	unsigned char dirty;	/* L_CLEAN/L_TRIMCR/L_SANITIZE (see load.c);
				 * lt_text() materialises the line and clears it */
	unsigned char slot;	/* service palette slot, 0xFF = no tag */
	unsigned char trunc;	/* line exceeded MAX_LINE_LEN: only raw[0..len) kept */
} LineIdx;

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

extern LineIdx *lidx;
extern size_t nlines, lcap;

/* per-line bitset flags; lines are transient, so marks and highlight-search
 * hits live here instead of in a Line. */
extern unsigned char *mark_bit;
extern unsigned char *hit_bit;

extern size_t *view;
extern size_t nv, vcap;

/* the visible view is an identity range when no filter is active
 * (view==NULL, view_at(k)==k), so a huge unfiltered log needs no array; a
 * filtered view materialises view[] of matching line indices. */
size_t view_at(size_t k);

extern char path[PATH_MAX];
extern int use_stdin;
extern int fd;		/* log file */
extern int kfd;		/* keyboard: stdin, or /dev/tty when stdin isn't a tty */
extern off_t fsize;
extern int stdin_eof;	/* pipe closed: no more input will ever come */

/* zero-copy window onto a regular file */
extern char *fmap;
extern size_t fmap_len, fmap_pos;

/* running max of raw line lengths; maintained at index time (push_raw),
 * refined with measured widths on materialisation. reset_lines rewinds it. */
extern size_t wc_max;

/* One pattern, in one of three roles. Lifecycle: update_filter /
 * update_search parse a query into pending_pat (workers test it while a
 * scan job flies); on commit install_pattern() swaps it wholesale into
 * the committed instance, so a cancelled attempt costs nothing. */
typedef struct {
	char text[MAX_QUERY];	/* the query this spec was parsed from */
	LitSpec lit;		/* plain literal, when neither is_re nor is_alt */
	AltSpec alt;		/* alternation of literals */
	regex_t re;		/* compiled ERE, owned iff is_re */
	unsigned char active;	/* matchers consult this spec */
	unsigned char is_re, is_alt;
	unsigned char enable_on_commit;	/* pending search only: text non-empty,
					 * i.e. the commit leaves the search on */
} Pat;

extern Pat filter_pat;		/* committed filter: narrows view[] */
extern Pat search_pat;		/* highlight-only search: marks hit_bit */

/* candidate pattern for in-flight scans; at most one job (filter scan or
 * search sweep) flies at a time, so a single slot serves both roles */
extern Pat pending_pat;

extern int re_mode;	/* UI toggle for the next filter: literal vs ERE */
extern int filter_inv;		/* live filter excludes matching lines */
extern char edit[MAX_QUERY];
extern int editing;
extern size_t ecur;	/* insertion point: byte offset into edit[] */
extern int editing_search;	/* prompt currently edits search, not filter */

/* viewport / UI state */
extern size_t cur, top;
extern size_t filter_anchor;	/* line selected when filtering began */
extern size_t filter_row;	/* its screen row, restored on clear */
extern int hscroll;
extern int rows, cols;
extern int wrap;
extern int help_open;
extern size_t help_top;	/* scrolling the in-pane help page */
extern int debug_open;
extern int follow;
extern int nocolor;
extern int truecolor;
extern int linuxcolor;
extern const char *mark_bg;
extern int running, dirty;
extern char msg[160];
extern size_t nmarked;
extern struct termios saved_tio;
extern int tio_saved;
extern volatile sig_atomic_t got_winch;

/* bumped whenever view[] membership or the search-hit bits change
 * (filter/search commit, follow extend, reload); render caches derive
 * from it */
extern uint64_t view_epoch;

/* Worker threads; 0 means "auto": effective_threads() falls back to nproc-2 clamped to [1,MAX]. */
extern int max_threads;
int effective_threads(void);

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

/* lazy per-line access (see load.c): materialise line i or fetch just its
 * displayed text. lt_text returns a stable pointer (raw for clean lines,
 * an arena-copied sanitised string for dirty ones) and the byte length. */
void lt_fill(Line *L, size_t i);
const char *lt_text(size_t i, size_t *len);
void lt_mark(size_t i, int on);
int lt_is_marked(size_t i);
void set_hit(size_t i, int on);
int hit_at(size_t i);

/* --- match.c --------------------------------------------------------- */
int smart_case(const char *q);
void lit_parse(const char *q, int icase, LitSpec *ls);
int alt_parse(const char *q, int icase, AltSpec *as);
int query_match(const char *s, size_t len, regmatch_t *m);
int search_match(const char *s, size_t len, regmatch_t *m);
int par_threads(size_t len);
void par_run(size_t lo, size_t hi, int nthreads, par_work work, void *ctx);
void scan_collect(size_t lo, size_t hi, size_t (*pos_line)(size_t),
		  int (*match)(const char *s, size_t len, regmatch_t *m), int inv,
		  size_t **arr, size_t *n, size_t *cap);
size_t pos_ident(size_t p);
size_t pos_view(size_t p);

/* --- jobs.c ---------------------------------------------------------- */
int have_status_bar(void);
int have_input_bar(void);
int terminal_too_small(void);
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
const char *job_spin_text(void);	/* spinner chip text, NULL inside grace */

/* --- render.c -------------------------------------------------------- */
size_t str_cols(const char *s, size_t n);
size_t widest_col(void);
void paint_chips(void);		/* partial chip-zone repaint (job fast path) */
size_t line_cols(size_t i);
size_t line_rows(size_t i);
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
size_t help_count(void);
const char *help_line(size_t i);
int help_section(size_t i);	/* nonzero if help_line(i) is a section header */
size_t chip_keys(char *buf, size_t n, const int *acts, size_t na);

#endif /* COMB_H */
