/* See LICENSE file for copyright and license details. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include <SDL.h>

#include "util.h"

/* blocks double from 64K to 8M: small parses stay small, and big ones
 * get blocks large enough that the OS takes them back when freed */
#define MINBLOCK (64u << 10)
#define MAXBLOCK (8u << 20)

struct Block {
	Block *next;
	size_t used, cap;
	union { void *p; long long l; double d; } data[];
};

void
die(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fprintf(stderr, "sit: %s\n", buf);
	/* GUI builds on Windows have no console; make the error visible */
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "sit", buf, NULL);
	exit(1);
}

void *
xmalloc(size_t n)
{
	void *p;

	if (!(p = malloc(n)))
		die("out of memory");
	return p;
}

void *
xcalloc(size_t n, size_t sz)
{
	void *p;

	if (!(p = calloc(n, sz)))
		die("out of memory");
	return p;
}

void *
xrealloc(void *p, size_t n)
{
	if (!(p = realloc(p, n)))
		die("out of memory");
	return p;
}

char *
xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;

	return memcpy(xmalloc(n), s, n);
}

/* zeroed, 8-byte aligned; everything goes at once with arena_free() */
void *
arena_alloc(Arena *a, size_t n)
{
	Block *b = a->head;
	void *p;

	n = (n + 7) & ~(size_t)7;
	if (!b || b->used + n > b->cap) {
		size_t cap = MAX(n, b ? MIN(b->cap * 2, MAXBLOCK) : MINBLOCK);

		b = xmalloc(sizeof *b + cap);
		b->cap = cap;
		b->used = 0;
		b->next = a->head;
		a->head = b;
	}
	p = (char *)b->data + b->used;
	b->used += n;
	return memset(p, 0, n);
}

char *
arena_strdup(Arena *a, const char *s)
{
	size_t n = strlen(s) + 1;

	return memcpy(arena_alloc(a, n), s, n);
}

void
arena_free(Arena *a)
{
	Block *b, *next;

	for (b = a->head; b; b = next) {
		next = b->next;
		free(b);
	}
	a->head = NULL;
}

/* whole file, NUL terminated; *len (if given) excludes the NUL */
unsigned char *
readfile(const char *path, size_t *len)
{
	FILE *f;
	long n;
	unsigned char *buf;

	if (!(f = fopen(path, "rb")))
		return NULL;
	if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return NULL;
	}
	buf = xmalloc(n + 1);
	if (fread(buf, 1, n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	} else {
		buf[n] = '\0';
		if (len)
			*len = n;
	}
	fclose(f);
	return buf;
}

int
fileexists(const char *path)
{
	FILE *f;

	if (!(f = fopen(path, "rb")))
		return 0;
	fclose(f);
	return 1;
}

/* seconds since the file was modified, -1 if it does not exist */
double
fileage(const char *path)
{
	struct stat st;

	if (!path || stat(path, &st) < 0)
		return -1;
	return difftime(time(NULL), st.st_mtime);
}

int
mkdirp(const char *path)
{
	char buf[1024], *p;

	snprintf(buf, sizeof buf, "%s", path);
	for (p = buf + 1; ; p++) {
		if (*p != '/' && *p != '\\' && *p)
			continue;
		if (p[-1] != ':') {    /* skip drive letters */
			char c = *p;
			*p = '\0';
#ifdef _WIN32
			if (_mkdir(buf) < 0 && errno != EEXIST)
#else
			if (mkdir(buf, 0755) < 0 && errno != EEXIST)
#endif
				return -1;
			*p = c;
		}
		if (!*p)
			return 0;
	}
}

/* $XDG_{CONFIG,CACHE,DATA}_HOME/sit, else %APPDATA% or %LOCALAPPDATA%
 * on Windows, else ~/.config, ~/.cache, ~/.local/share */
const char *
appdir(int which)
{
	static const char *xdg[] = { "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME" };
	static const char *home[] = { ".config", ".cache", ".local/share" };
	static char buf[3][1024];
	const char *x;

	if ((x = getenv(xdg[which])) && *x)
		snprintf(buf[which], sizeof buf[which], "%s/sit", x);
#ifdef _WIN32
	else if ((x = getenv(which == DirCache ? "LOCALAPPDATA" : "APPDATA")))
		snprintf(buf[which], sizeof buf[which], "%s/sit", x);
#endif
	else if ((x = getenv("HOME")))
		snprintf(buf[which], sizeof buf[which], "%s/%s/sit", x, home[which]);
	else
		snprintf(buf[which], sizeof buf[which], ".sit");
	return buf[which];
}

unsigned long long
fnv1a(const char *s)
{
	unsigned long long h = 14695981039346656037ULL;

	while (*s)
		h = (h ^ (unsigned char)*s++) * 1099511628211ULL;
	return h;
}

/* percent-encode everything but RFC 3986 unreserved characters */
void
urlenc(char *dst, size_t n, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t i = 0;
	unsigned char c;

	for (; *s && i + 4 < n; s++) {
		c = *s;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '.' || c == '_' || c == '~') {
			dst[i++] = c;
		} else {
			dst[i++] = '%';
			dst[i++] = hex[c >> 4];
			dst[i++] = hex[c & 15];
		}
	}
	dst[i] = '\0';
}

/* decode one UTF-8 sequence, advance *s; invalid input yields U+FFFD */
int
utf8dec(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	int c = *p++, n, i;

	if (c < 0x80)
		n = 0;
	else if ((c & 0xe0) == 0xc0)
		c &= 0x1f, n = 1;
	else if ((c & 0xf0) == 0xe0)
		c &= 0x0f, n = 2;
	else if ((c & 0xf8) == 0xf0)
		c &= 0x07, n = 3;
	else {
		*s = (const char *)p;
		return 0xfffd;
	}
	for (i = 0; i < n; i++) {
		if ((p[i] & 0xc0) != 0x80) {
			*s = (const char *)p + i;
			return 0xfffd;
		}
		c = (c << 6) | (p[i] & 0x3f);
	}
	*s = (const char *)p + n;
	return c;
}

/* valid UTF-8 without control characters (tabs and newlines allowed) */
int
utf8ok(const char *s)
{
	int c;

	while (*s) {
		c = utf8dec(&s);
		if (c == 0xfffd || (c < 0x20 && c != '\t' && c != '\n' && c != '\r'))
			return 0;
	}
	return 1;
}

/* decode base64 s into dst (n bytes, NUL terminated); -1 unless s is
 * clean, padded base64 */
int
b64dec(const char *s, char *dst, size_t n)
{
	static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t len = strlen(s), i, o = 0;
	unsigned long v = 0;
	const char *p;
	int bits = 0;

	if (!len || len % 4)
		return -1;
	for (i = 0; i < len; i++) {
		if (s[i] == '=') {
			if (i + 2 < len || (i + 1 < len && s[i + 1] != '='))
				return -1;
			break;
		}
		if (!(p = strchr(tab, s[i])))
			return -1;
		v = (v << 6 | (unsigned long)(p - tab)) & 0xffffff;
		if ((bits += 6) >= 8) {
			bits -= 8;
			if (o + 1 >= n)
				return -1;
			dst[o++] = (char)(v >> bits & 0xff);
		}
	}
	dst[o] = '\0';
	return (int)o;
}

double
now(void)
{
	return (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency();
}

/* frame-rate independent exponential ease towards target */
float
approach(float cur, float target, float dt, float speed, float eps)
{
	cur += (target - cur) * (1 - expf(-speed * dt));
	if (fabsf(target - cur) < eps)
		cur = target;
	return cur;
}

void
fmttime(char *buf, size_t n, double secs)
{
	int s = secs > 0 ? (int)secs : 0;

	if (s >= 3600)
		snprintf(buf, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(buf, n, "%d:%02d", s / 60, s % 60);
}
