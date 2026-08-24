/* Unit tests for comb's internals.
 *
 * Includes the implementation directly, so every static is reachable
 * without header surgery; comb.c is built with COMB_TEST to drop its
 * main(). These cover the pure logic -- tag detection, sanitizing,
 * literal matching, widths, key decoding, span collection, severity
 * boundaries -- which needs no terminal and runs in microseconds.
 * Screen-level behavior lives in tests/harness.py (--check/--compare).
 *
 *   make check      (or: cc -O2 -Wall -o tests/selftest tests/selftest.c)
 */
#define COMB_TEST

/* comb.c's main() is compiled out here, so its helpers go unreferenced;
 * the noise is expected and says nothing about the code under test.
 * Pragmas must precede the include to cover its parse. */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../comb.c"

static int fails;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fails++; \
		printf("FAIL %d: %s\n", __LINE__, #cond); \
	} \
} while (0)

static void test_tag_span(void)
{
	int so, eo;
	const char *l;

	l = "Aug 22 23:53:27 host NetworkManager[508]: <info> thing";
	CHECK(tag_span(l, strlen(l), &so, &eo) == 1);
	CHECK(so == 21 && eo == 35);	/* pid stripped, prose untouched */

	l = "2026-08-22T23:53:27.7 localhost NetworkManager[508]: <info> x";
	CHECK(tag_span(l, strlen(l), &so, &eo) == 1);
	CHECK(so == 32 && eo == 46);

	/* lone ':': absorb the preceding word for one stable tag */
	l = "[    0.163450] Spectre V2 : Enabling thing 0 Mitigation: x";
	CHECK(tag_span(l, strlen(l), &so, &eo) == 1);
	CHECK(so == 23 && eo == 25);	/* "V2", not "Mitigation" */

	/* no timestamp-ish preamble: prose never reads as "tag: text" */
	l = "some plain text Mitigation: whatever";
	CHECK(tag_span(l, strlen(l), &so, &eo) == 0);

	/* verbosity token is not a tag */
	l = "Aug 22 23:53:27 host <info>: hello";
	CHECK(tag_span(l, strlen(l), &so, &eo) == 0);

	/* overlong field rejected */
	l = "Aug 22 23:53:27 h "
	    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	    "b: tail";
	CHECK(tag_span(l, strlen(l), &so, &eo) == 0);

	/* bare colon-only tag after timestamp */
	l = "Aug 22 23:53:27 host foo: bar";
	CHECK(tag_span(l, strlen(l), &so, &eo) == 1);
	CHECK(!strncmp(l + so, "foo", 3));
}

static void test_sanitize(void)
{
	size_t n;
	char *o;

	o = sanitize("a\x1b[31mb\x1b[0m", 10, &n);
	CHECK(n == 2 && !strcmp(o, "ab"));

	o = sanitize("\x1b]0;title\x07tail", 14, &n);
	CHECK(n == 4 && !strcmp(o, "tail"));

	o = sanitize("\x1b]0;t\x1b\\tail", 11, &n);
	CHECK(n == 4 && !strcmp(o, "tail"));

	o = sanitize("a\rb\tc", 5, &n);
	CHECK(n == 7 && !strcmp(o, "ab    c"));

	o = sanitize("a\x1bZb", 4, &n);	/* lone Esc eats one final byte */
	CHECK(n == 2 && !strcmp(o, "ab"));

	o = sanitize("a\x1bxb", 4, &n);	/* not a final byte: only Esc goes */
	CHECK(n == 3 && !strcmp(o, "axb"));
}

static void test_lit_find(void)
{
	char big[300];

	memset(big, 'x', sizeof big - 1);
	big[sizeof big - 1] = 0;

	lit_update("", 0);
	CHECK(lit_len == 0);
	CHECK(lit_find("anything", 8) == 0);

	lit_update("^err", 0);
	CHECK(lit_find("error", 5) == 0);
	CHECK(lit_find("xerror", 6) == -1);

	lit_update("err$", 0);
	CHECK(lit_find("xerr", 4) == 1);
	CHECK(lit_find("xerra", 5) == -1);

	lit_update("^err$", 0);
	CHECK(lit_find("err", 3) == 0);
	CHECK(lit_find("errs", 4) == -1);

	lit_update("Ab", 1);
	CHECK(lit_find("zabc", 4) == 1);
	CHECK(lit_find("ZABC", 4) == 1);

	lit_update("needle", 0);
	snprintf(big, sizeof big, "%s needle", big + 290);
	CHECK(lit_find(big, strlen(big)) > 0);
}

static void test_widths(void)
{
	size_t cl;

	CHECK(glyph_width('a') == 1);
	CHECK(glyph_width(0x3042) == 2);	/* hiragana A */
	CHECK(glyph_width(0x0301) == 0);	/* combining acute */

	CHECK(u8_decode("a", 1, &cl) == 'a' && cl == 1);
	CHECK(u8_decode("\xc3\xa9", 2, &cl) == 0xe9 && cl == 2);
	CHECK(u8_decode("\xffx", 2, &cl) == 0xff && cl == 1); /* malformed */

	CHECK(str_cols("abc", 3) == 3);
	CHECK(str_cols("a\xe3\x81\x82" "b", 5) == 4);
}

/* read_key() reads the global kfd; point it at a pipe */
static int key_w = -1;
static void keys_feed(const char *s)
{
	int p[2];
	if (key_w >= 0)
		close(key_w);
	if (pipe(p) != 0)
		exit(2);
	kfd = p[0];
	key_w = p[1];
	peeked = -1;
	if (*s)
		write(key_w, s, strlen(s));
}

static void test_read_key(void)
{
	keys_feed("\x1b[A\x1b[B\x1b[C\x1b[D");
	CHECK(read_key() == K_UP);
	CHECK(read_key() == K_DOWN);
	CHECK(read_key() == K_RIGHT);
	CHECK(read_key() == K_LEFT);

	keys_feed("\x1b[5~\x1b[6~\x1b[H\x1b[F\x1b[3~\x1bOA");
	CHECK(read_key() == K_PGUP);
	CHECK(read_key() == K_PGDN);
	CHECK(read_key() == K_HOME);
	CHECK(read_key() == K_END);
	CHECK(read_key() == K_DEL);
	CHECK(read_key() == K_UP);	/* SS3 application arrows */

	keys_feed("\x1bjq");		/* Alt-chord: j must survive */
	CHECK(read_key() == 0x1b);
	CHECK(read_key() == 'j');
	CHECK(read_key() == 'q');

	keys_feed("\x1b[1;5A");		/* ctrl-up: modifier params undecoded */
	CHECK(read_key() == K_UP);

	keys_feed("");
	close(key_w);
	key_w = -1;
	CHECK(read_key() == K_EOF);
	kfd = 0;
}

static void test_add_span(void)
{
	Span sp[LINE_SPANS];
	int n = 0;

	add_span(sp, &n, 5, 9, DIM);
	add_span(sp, &n, 0, 3, QUOTE_COLOR);	/* out-of-order insert sorts */
	CHECK(n == 2 && sp[0].so == 0 && sp[1].so == 5);

	add_span(sp, &n, 1, 2, DIM);	/* inside earlier span: rejected */
	CHECK(n == 2);
	add_span(sp, &n, 2, 6, DIM);	/* straddles two spans: rejected */
	CHECK(n == 2);
	add_span(sp, &n, 20, 24, DIM);
	CHECK(n == 3);

	while (n < LINE_SPANS)
		add_span(sp, &n, 100 + n * 2, 101 + n * 2, DIM);
	CHECK(n == LINE_SPANS);
	add_span(sp, &n, 500, 600, DIM);	/* full table: ignored */
	CHECK(n == LINE_SPANS);
}

static Line mkline(const char *s)
{
	Line L;
	memset(&L, 0, sizeof L);
	L.s = s;
	L.len = strlen(s);
	return L;
}

static const Span *find_attr(const Span *sp, int n, const char *attr)
{
	for (int i = 0; i < n; i++)
		if (sp[i].attr == attr)
			return &sp[i];
	return NULL;
}

static void test_collect_spans(void)
{
	Span sp[LINE_SPANS];
	int n;
	Line L;

	nocolor = 0;

	/* dmesg-padded epoch stamp dims from column 0 */
	L = mkline("[    0.403686] LVT offset 0 assigned");
	n = collect_spans(&L, sp);
	CHECK(find_attr(sp, n, DIM) && find_attr(sp, n, DIM)->so == 0 &&
	      find_attr(sp, n, DIM)->se == 14);

	/* quoted values, apostrophes stay plain */
	L = mkline("don't panic and 'quoted' end");
	n = collect_spans(&L, sp);
	const Span *v = find_attr(sp, n, QUOTE_COLOR);
	CHECK(v && v->so == 16 && v->se == 24);

	L = mkline("say \"double quoted\" ok");
	n = collect_spans(&L, sp);
	v = find_attr(sp, n, QUOTE_COLOR);
	CHECK(v && v->so == 4 && v->se == 19);

	/* nested parens form one span */
	L = mkline("pre (nest (ed)) post");
	n = collect_spans(&L, sp);
	const Span *p = find_attr(sp, n, PAREN_COLOR);
	CHECK(p && p->so == 4 && p->se == 15 && L.s[p->se - 1] == ')');

	/* <info> token hue after the service tag */
	L = mkline("Aug 22 23:53:27 host foo[1]: <info> hello");
	assign_service(&L);
	n = collect_spans(&L, sp);
	const char *tok = "\x1b[38;5;110m";	/* cornflower, matches token_attr */
	const Span *t = find_attr(sp, n, tok);
	CHECK(t && t->so == 29 && t->se == 35);

	/* preamble dimmed up to the tag */
	const Span *d = find_attr(sp, n, DIM);
	CHECK(d && d->so == 0 && d->se == L.tag_so);

	nocolor = 1;
	L = mkline("Aug 22 23:53:27 host foo[1]: <warn> \"x\"");
	CHECK(collect_spans(&L, sp) == 0);
	nocolor = 0;
}

static void test_severity(void)
{
	CHECK(!strcmp(severity("level=debug now", 15), "\x1b[2m"));
	CHECK(!strcmp(severity("<warn>", 6), "\x1b[1;93m"));
	CHECK(!strcmp(severity("errors found", 12), "\x1b[1;91m"));
	CHECK(!strcmp(severity("EMERGENCY stop", 14), "\x1b[1;95m"));
	CHECK(*severity("terrain report", 14) == 0);
	CHECK(*severity("--debug flag", 12) == 0);
	CHECK(*severity("/var/debug log", 14) == 0);
}

static void test_b64enc(void)
{
	char out[64];

	b64enc("", 0, out);
	CHECK(*out == 0);
	b64enc("f", 1, out);
	CHECK(!strcmp(out, "Zg=="));
	b64enc("fo", 2, out);
	CHECK(!strcmp(out, "Zm8="));
	b64enc("foo", 3, out);
	CHECK(!strcmp(out, "Zm9v"));
	b64enc("foobar", 6, out);
	CHECK(!strcmp(out, "Zm9vYmFy"));
}

static void test_update_filter_state(void)
{
	filtered = filtered_re = 0;
	re_mode = 1;

	update_filter("a.*[0-9]+");	/* valid regex */
	CHECK(filtered && filtered_re);

	msg[0] = 0;
	update_filter("[a+b(c)|");	/* bad regex keeps the old view */
	CHECK(strstr(msg, "bad regex"));
	CHECK(filtered && filtered_re && !strcmp(query, "a.*[0-9]+"));

	update_filter("");		/* clear */
	CHECK(!filtered && !filtered_re);
}

int main(void)
{
	test_tag_span();
	test_sanitize();
	test_lit_find();
	test_widths();
	test_read_key();
	test_add_span();
	test_collect_spans();
	test_severity();
	test_b64enc();
	test_update_filter_state();

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("all selftests passed\n");
	return 0;
}
