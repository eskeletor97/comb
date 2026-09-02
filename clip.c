/* comb - clipboard: OSC 52 escape plus helper programs */

#define _GNU_SOURCE
#include "comb.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	char full[PATH_MAX];
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
	/* OSC 52*/
	printf("\x1b]52;c;%s\a", b);
	fflush(stdout);
	free(b);
	clip_helper(s, len);
}

void clear_marks(void)
{
	if (nlines)
		memset(mark_bit, 0, nlines / 8 + 1);
	nmarked = 0;
}

void copy_current(void)
{
	if (nmarked > 0) {
		size_t total = 0;
		for (size_t i = 0; i < nlines; i++)
			if (lt_is_marked(i)) {
				size_t len;
				lt_text(i, &len);
				total += len + 1;
			}
		char *buf = xrealloc(NULL, total + 1);
		size_t off = 0;
		for (size_t i = 0; i < nlines; i++) {
			if (!lt_is_marked(i))
				continue;
			size_t len;
			const char *s = lt_text(i, &len);
			memcpy(buf + off, s, len);
			off += len;
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
	size_t len;
	const char *s = lt_text(view_at(cur), &len);
	copy_text(s, len);
	snprintf(msg, sizeof msg, "copied line %zu (%zu bytes)", cur + 1, len);
}
