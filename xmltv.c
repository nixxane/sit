/* See LICENSE file for copyright and license details.
 *
 * XMLTV guide, plain or gzip, read in 64 KB pieces so a guide of several
 * hundred MB never sits in memory. Runs on the net worker. Only the
 * channels listed in the job's aux (one id per line) are kept, and only
 * programmes between a few hours ago and three days ahead.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zlib.h>

#include "util.h"
#include "json.h"
#include "net.h"
#include "playlist.h"
#include "epg.h"
#include "xmltv.h"

#define PAST   (6 * 3600)
#define FUTURE (3 * 86400)
#define MAXTAIL (1 << 20)
#define MAXTEXT (1 << 16)

typedef struct {
	int ch;
	Prog p;
} Row;

typedef struct {
	Xml *x;
	Arena *a;
	char *buf;            /* input not consumed yet */
	size_t len, cap;
	char *txt;            /* text of the title or desc being read */
	size_t tlen, tcap;
	int into;             /* 0 nothing, 1 title, 2 desc */
	Row *rows, cur;
	size_t nrows, caprows;
	long long lo, hi;
	int inprog;
} St;

static size_t
slotof(const Xml *x, const char *id)
{
	size_t i = fnv1a(id) & x->mask;

	while (x->keys[i] && strcmp(x->keys[i], id))
		i = (i + 1) & x->mask;
	return i;
}

static void
lowercopy(char *d, size_t n, const char *s)
{
	size_t i;

	for (i = 0; s[i] && i + 1 < n; i++)
		d[i] = tolower((unsigned char)s[i]);
	d[i] = '\0';
}

const Prog *
xml_chan(const Xml *x, const char *id, int *n)
{
	char l[256];
	size_t i;

	if (!x || !*id)
		return NULL;
	lowercopy(l, sizeof l, id);
	i = slotof(x, l);
	if (!x->keys[i] || !x->cnt[x->vals[i]])
		return NULL;
	*n = x->cnt[x->vals[i]];
	return x->progs + x->first[x->vals[i]];
}

static void
want(Xml *x, Arena *a, char *ids)
{
	char *line, *next, l[256];
	size_t n = 1, i;

	for (line = ids; *line; line++)
		n += *line == '\n';
	for (x->mask = 15; x->mask < n * 2; x->mask = x->mask * 2 + 1)
		;
	x->keys = arena_alloc(a, (x->mask + 1) * sizeof *x->keys);
	x->vals = arena_alloc(a, (x->mask + 1) * sizeof *x->vals);
	for (line = ids; line && *line; line = next) {
		if ((next = strchr(line, '\n')))
			*next++ = '\0';
		lowercopy(l, sizeof l, line);
		i = slotof(x, l);
		if (*l && !x->keys[i]) {
			x->keys[i] = arena_strdup(a, l);
			x->vals[i] = x->nch++;
		}
	}
}

static long long
daynum(int y, int m, int d)
{
	long long era, yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

/* "20251027183000 +0100"; no zone means UTC */
static long long
xtime(const char *s)
{
	int y, mo, d, h = 0, mi = 0, se = 0, th = 0, tm = 0;
	long long tz = 0;

	if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &se) < 4)
		return 0;
	while (isdigit((unsigned char)*s))
		s++;
	while (*s == ' ')
		s++;
	if ((*s == '+' || *s == '-') && sscanf(s + 1, "%2d%2d", &th, &tm) >= 1)
		tz = (*s == '-' ? -1 : 1) * (th * 3600LL + tm * 60);
	return daynum(y, mo, d) * 86400 + h * 3600 + mi * 60 + se - tz;
}

/* value of name="..." in a tag, or "" */
static const char *
attr(const char *tag, const char *name, char *buf, size_t n)
{
	size_t l = strlen(name);
	const char *p, *e;

	for (p = tag; (p = strstr(p, name)); p += l) {
		if (p > tag && !isspace((unsigned char)p[-1]))
			continue;
		if (p[l] != '=' || (p[l + 1] != '"' && p[l + 1] != '\''))
			continue;
		p += l + 2;
		if (!(e = strchr(p, p[-1])))
			break;
		snprintf(buf, n, "%.*s", (int)(e - p), p);
		return buf;
	}
	buf[0] = '\0';
	return buf;
}

static int
isname(const char *t, const char *name)
{
	size_t l = strlen(name);

	return !strncmp(t, name, l) && (!t[l] || isspace((unsigned char)t[l]) || t[l] == '/');
}

static void
unent(char *s)
{
	char *d = s, *e;
	long v;

	while (*s) {
		if (*s != '&') {
			*d++ = *s++;
		} else if (!strncmp(s, "&amp;", 5)) {
			*d++ = '&', s += 5;
		} else if (!strncmp(s, "&lt;", 4)) {
			*d++ = '<', s += 4;
		} else if (!strncmp(s, "&gt;", 4)) {
			*d++ = '>', s += 4;
		} else if (!strncmp(s, "&quot;", 6)) {
			*d++ = '"', s += 6;
		} else if (!strncmp(s, "&apos;", 6)) {
			*d++ = '\'', s += 6;
		} else if (s[1] == '#' && (v = s[2] == 'x' || s[2] == 'X' ? strtol(s + 3, &e, 16) :
		           strtol(s + 2, &e, 10)) > 0 && v < 0x110000 && *e == ';') {
			if (v < 0x80) {
				*d++ = (char)v;
			} else if (v < 0x800) {
				*d++ = (char)(0xc0 | v >> 6);
				*d++ = (char)(0x80 | (v & 0x3f));
			} else if (v < 0x10000) {
				*d++ = (char)(0xe0 | v >> 12);
				*d++ = (char)(0x80 | (v >> 6 & 0x3f));
				*d++ = (char)(0x80 | (v & 0x3f));
			} else {
				*d++ = (char)(0xf0 | v >> 18);
				*d++ = (char)(0x80 | (v >> 12 & 0x3f));
				*d++ = (char)(0x80 | (v >> 6 & 0x3f));
				*d++ = (char)(0x80 | (v & 0x3f));
			}
			s = e + 1;
		} else {
			*d++ = *s++;
		}
	}
	*d = '\0';
}

static void
addtext(St *s, const char *p, size_t n)
{
	if (!s->into || s->tlen + n > MAXTEXT)
		return;
	if (s->tlen + n + 1 > s->tcap) {
		s->tcap = (s->tlen + n + 1) * 2;
		s->txt = xrealloc(s->txt, s->tcap);
	}
	memcpy(s->txt + s->tlen, p, n);
	s->tlen += n;
}

/* the finished text of a title or desc, entities resolved and trimmed */
static const char *
takedown(St *s)
{
	char *t = s->txt;
	size_t n;

	if (!t)
		return "";
	t[s->tlen] = '\0';
	unent(t);
	while (isspace((unsigned char)*t))
		t++;
	for (n = strlen(t); n && isspace((unsigned char)t[n - 1]); )
		t[--n] = '\0';
	return arena_strdup(s->a, t);
}

static void
tag(St *s, const char *t)
{
	char c[256], b[64];
	long long st, en;
	size_t n;
	int selfclose = *t && t[strlen(t) - 1] == '/';

	if (*t == '/') {
		t++;
		if ((isname(t, "title") && s->into == 1) || (isname(t, "desc") && s->into == 2)) {
			if (s->into == 1)
				s->cur.p.title = takedown(s);
			else
				s->cur.p.desc = takedown(s);
			s->into = 0;
		} else if (isname(t, "programme") && s->inprog) {
			s->inprog = 0;
			if (s->nrows == s->caprows) {
				s->caprows = s->caprows ? s->caprows * 2 : 1024;
				s->rows = xrealloc(s->rows, s->caprows * sizeof *s->rows);
			}
			if (!s->cur.p.title)
				s->cur.p.title = "";
			if (!s->cur.p.desc)
				s->cur.p.desc = "";
			s->rows[s->nrows++] = s->cur;
		}
		return;
	}
	if (isname(t, "programme")) {
		s->inprog = 0;
		if (selfclose)
			return;
		lowercopy(c, sizeof c, attr(t, "channel", c, sizeof c));
		n = slotof(s->x, c);
		st = xtime(attr(t, "start", b, sizeof b));
		en = xtime(attr(t, "stop", b, sizeof b));
		if (!s->x->keys[n] || !st || en <= st || en < s->lo || st > s->hi)
			return;
		memset(&s->cur, 0, sizeof s->cur);
		s->cur.ch = s->x->vals[n];
		s->cur.p.start = st;
		s->cur.p.stop = en;
		s->cur.p.local = "";
		s->inprog = 1;
	} else if (s->inprog && !selfclose && (isname(t, "title") || isname(t, "desc"))) {
		s->into = isname(t, "title") ? 1 : 2;
		s->tlen = 0;
		if ((s->into == 1 && s->cur.p.title) || (s->into == 2 && s->cur.p.desc))
			s->into = 0;    /* the first language wins */
	}
}

/* CDATA is text as is; '&' is escaped so unent() leaves it alone */
static void
cdata(St *s, const char *p, const char *e)
{
	for (; p < e; p++)
		if (*p == '&')
			addtext(s, "&amp;", 5);
		else
			addtext(s, p, 1);
}

static void
feed(St *s, const char *in, size_t n)
{
	char *p, *e, *end, *q, t[2048];
	char quote;
	size_t rest;

	if (s->len + n + 1 > s->cap) {
		s->cap = (s->len + n + 1) * 2;
		s->buf = xrealloc(s->buf, s->cap);
	}
	memcpy(s->buf + s->len, in, n);
	s->len += n;
	s->buf[s->len] = '\0';
	p = s->buf;
	end = s->buf + s->len;
	while (p < end) {
		for (e = p; e < end && *e != '<'; e++)
			;
		addtext(s, p, e - p);
		if ((p = e) == end)
			break;
		rest = end - p;
		if (rest >= 9 || memchr(p, '>', rest)) {
			if (!strncmp(p, "<!--", 4)) {
				if (!(q = strstr(p + 4, "-->")))
					break;
				p = q + 3;
				continue;
			} else if (!strncmp(p, "<![CDATA[", 9)) {
				if (!(q = strstr(p + 9, "]]>")))
					break;
				cdata(s, p + 9, q);
				p = q + 3;
				continue;
			} else if (!strncmp(p, "<?", 2)) {
				if (!(q = strstr(p, "?>")))
					break;
				p = q + 2;
				continue;
			}
		}
		for (quote = 0, e = p + 1; e < end && (quote || *e != '>'); e++)
			if (quote ? *e == quote : (*e == '"' || *e == '\''))
				quote = quote ? 0 : *e;
		if (e == end)
			break;
		snprintf(t, sizeof t, "%.*s", (int)MIN((size_t)(e - p - 1), sizeof t - 1), p + 1);
		if (*p == '<' && p[1] != '!')
			tag(s, t);
		p = e + 1;
	}
	s->len = end - p;
	if (s->len > MAXTAIL)
		s->len = 0;
	memmove(s->buf, p, s->len);
}

static int
bystart(const void *a, const void *b)
{
	const Row *x = a, *y = b;

	if (x->ch != y->ch)
		return x->ch - y->ch;
	return (x->p.start > y->p.start) - (x->p.start < y->p.start);
}

/* worker: j->data is the guide (plain or gzip), j->aux the wanted ids;
 * the answer is a Xml in j->res */
void
xml_parse(Job *j)
{
	unsigned char out[65536];
	z_stream z;
	St s;
	Xml *x = arena_alloc(&j->arena, sizeof *x);
	size_t i;
	int r;

	memset(&s, 0, sizeof s);
	s.x = x;
	s.a = &j->arena;
	s.lo = (long long)time(NULL) - PAST;
	s.hi = (long long)time(NULL) + FUTURE;
	want(x, &j->arena, j->aux ? j->aux : "");
	if (j->len > 2 && (unsigned char)j->data[0] == 0x1f && (unsigned char)j->data[1] == 0x8b) {
		memset(&z, 0, sizeof z);
		if (inflateInit2(&z, 15 + 16) != Z_OK) {
			snprintf(j->err, sizeof j->err, "zlib failed to start");
		} else {
			z.next_in = (unsigned char *)j->data;
			z.avail_in = (uInt)j->len;
			do {
				z.next_out = out;
				z.avail_out = sizeof out;
				if ((r = inflate(&z, Z_NO_FLUSH)) != Z_OK && r != Z_STREAM_END) {
					snprintf(j->err, sizeof j->err, "guide is corrupt (gzip)");
					break;
				}
				feed(&s, (char *)out, sizeof out - z.avail_out);
			} while (r == Z_OK && (z.avail_in || !z.avail_out));
			inflateEnd(&z);
		}
	} else {
		feed(&s, j->data, j->len);
	}
	free(j->data);
	j->data = NULL;
	free(s.buf);
	free(s.txt);

	qsort(s.rows, s.nrows, sizeof *s.rows, bystart);
	x->progs = arena_alloc(&j->arena, (s.nrows + 1) * sizeof *x->progs);
	x->first = arena_alloc(&j->arena, (x->nch + 1) * sizeof *x->first);
	x->cnt = arena_alloc(&j->arena, (x->nch + 1) * sizeof *x->cnt);
	for (i = 0; i < s.nrows; i++) {
		x->progs[i] = s.rows[i].p;
		if (!x->cnt[s.rows[i].ch])
			x->first[s.rows[i].ch] = (int)i;
		x->cnt[s.rows[i].ch]++;
	}
	free(s.rows);
	j->res = x;
}
