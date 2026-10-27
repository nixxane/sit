/* See LICENSE file for copyright and license details.
 *
 * What sit remembers, as tab-separated text in $XDG_DATA_HOME/sit
 * (%APPDATA%\sit on Windows):
 *   resume     source kind id seconds duration
 *              (a position equal to the duration means watched)
 *   favorites  source kind id series time name logo ext seriesname
 *   recent     the same, newest first
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"
#include "store.h"

#define MAXRECENT 100

typedef struct {
	char *src;
	int kind, id;
	double pos, dur;
} Entry;

typedef struct {
	Mark *v;
	int n, cap;
	const char *file;
} Marks;

static Entry *ents;
static int nents, cap, dirty;
static Marks lists[] = { [ListFav] = { 0, 0, 0, "favorites" }, [ListRecent] = { 0, 0, 0, "recent" } };
static unsigned long version;

static Entry *
find(const char *src, int kind, int id, int add)
{
	int i;

	for (i = 0; i < nents; i++)
		if (ents[i].id == id && ents[i].kind == kind && !strcmp(ents[i].src, src))
			return &ents[i];
	if (!add)
		return NULL;
	if (nents == cap) {
		cap = cap ? cap * 2 : 64;
		ents = xrealloc(ents, cap * sizeof *ents);
	}
	ents[nents].src = xstrdup(src);
	ents[nents].kind = kind;
	ents[nents].id = id;
	return &ents[nents++];
}

static char *
path(const char *name)
{
	static char buf[1100];

	snprintf(buf, sizeof buf, "%s/%s", appdir(DirData), name);
	return buf;
}

/* split a line on tabs into at most n fields; returns how many */
static int
split(char *line, char **f, int n)
{
	char *t;
	int nf;

	f[0] = line;
	for (nf = 1; nf < n && (t = strchr(f[nf - 1], '\t')); nf++) {
		*t = '\0';
		f[nf] = t + 1;
	}
	return nf;
}

/* the lines of a data file, handed to fn */
static void
eachline(const char *name, void (*fn)(char *line, void *ud), void *ud)
{
	char *buf, *line, *next;

	if (!(buf = (char *)readfile(path(name), NULL)))
		return;
	for (line = buf; line && *line; line = next) {
		if ((next = strchr(line, '\n')))
			*next++ = '\0';
		fn(line, ud);
	}
	free(buf);
}

static void
loadresume(char *line, void *ud)
{
	char *f[5];
	Entry *e;

	(void)ud;
	if (split(line, f, 5) < 5)
		return;
	e = find(f[0], atoi(f[1]), atoi(f[2]), 1);
	e->pos = atof(f[3]);
	e->dur = atof(f[4]);
}

static void
push(Marks *l, const Mark *m, int front)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 32;
		l->v = xrealloc(l->v, l->cap * sizeof *l->v);
	}
	if (front) {
		memmove(l->v + 1, l->v, l->n * sizeof *l->v);
		l->v[0] = *m;
	} else {
		l->v[l->n] = *m;
	}
	l->n++;
}

static void
loadmark(char *line, void *ud)
{
	char *f[9];
	Mark m;

	if (split(line, f, 9) < 9)
		return;
	m.src = xstrdup(f[0]);
	m.kind = atoi(f[1]);
	m.id = atoi(f[2]);
	m.series = atoi(f[3]);
	m.time = atoll(f[4]);
	m.name = xstrdup(f[5]);
	m.logo = xstrdup(f[6]);
	m.ext = xstrdup(f[7]);
	m.sname = xstrdup(f[8]);
	push(ud, &m, 0);
}

void
store_load(void)
{
	size_t i;

	eachline("resume", loadresume, NULL);
	for (i = 0; i < LEN(lists); i++)
		eachline(lists[i].file, loadmark, &lists[i]);
}

/* seconds watched, 0 if never started */
double
store_get(const char *src, int kind, int id, double *dur)
{
	Entry *e = find(src, kind, id, 0);

	if (dur)
		*dur = e ? e->dur : 0;
	return e ? e->pos : 0;
}

void
store_set(const char *src, int kind, int id, double pos, double dur)
{
	Entry *e;

	if (dur <= 0)
		return;
	if (pos > dur * 0.95)
		pos = dur;    /* close enough to the credits */
	e = find(src, kind, id, 1);
	if ((int)e->pos != (int)pos || (int)e->dur != (int)dur)
		dirty = 1;
	e->pos = pos;
	e->dur = dur;
}

static FILE *
create(const char *name, char *tmp, size_t n)
{
	mkdirp(appdir(DirData));
	snprintf(tmp, n, "%s.tmp", path(name));
	return fopen(tmp, "w");
}

static void
commit(const char *name, FILE *f, const char *tmp)
{
	if (fclose(f) == 0) {
		remove(path(name));
		rename(tmp, path(name));
	}
}

void
store_save(void)
{
	char tmp[1200];
	FILE *f;
	int i;

	if (!dirty || !(f = create("resume", tmp, sizeof tmp)))
		return;
	for (i = 0; i < nents; i++)
		fprintf(f, "%s\t%d\t%d\t%.0f\t%.0f\n", ents[i].src, ents[i].kind, ents[i].id,
		        ents[i].pos, ents[i].dur);
	commit("resume", f, tmp);
	dirty = 0;
}

/* names come from providers; keep tabs and newlines out of the file */
static void
field(FILE *f, const char *s, int last)
{
	for (; *s; s++)
		fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
	fputc(last ? '\n' : '\t', f);
}

static void
savemarks(int list)
{
	Marks *l = &lists[list];
	char tmp[1200];
	FILE *f;
	int i;

	version++;
	if (!(f = create(l->file, tmp, sizeof tmp)))
		return;
	for (i = 0; i < l->n; i++) {
		fprintf(f, "%s\t%d\t%d\t%d\t%lld\t", l->v[i].src, l->v[i].kind, l->v[i].id,
		        l->v[i].series, l->v[i].time);
		field(f, l->v[i].name, 0);
		field(f, l->v[i].logo, 0);
		field(f, l->v[i].ext, 0);
		field(f, l->v[i].sname, 1);
	}
	commit(l->file, f, tmp);
}

int
store_marks(int list, Mark **marks)
{
	*marks = lists[list].v;
	return lists[list].n;
}

Mark *
store_find(int list, const char *src, int kind, int id, int series)
{
	Marks *l = &lists[list];
	int i;

	for (i = 0; i < l->n; i++)
		if (l->v[i].id == id && l->v[i].kind == kind && l->v[i].series == series &&
		    !strcmp(l->v[i].src, src))
			return &l->v[i];
	return NULL;
}

static void
freemark(Mark *m)
{
	free(m->src);
	free(m->name);
	free(m->logo);
	free(m->ext);
	free(m->sname);
}

void
store_del(int list, Mark *m)
{
	Marks *l = &lists[list];

	freemark(m);
	memmove(m, m + 1, (l->v + l->n - m - 1) * sizeof *m);
	l->n--;
	savemarks(list);
}

/* to the front, replacing an older mark of the same item */
void
store_add(int list, const Mark *m)
{
	Marks *l = &lists[list];
	Mark c, *old;

	if ((old = store_find(list, m->src, m->kind, m->id, m->series))) {
		freemark(old);
		memmove(old, old + 1, (l->v + l->n - old - 1) * sizeof *old);
		l->n--;
	}
	c.src = xstrdup(m->src);
	c.name = xstrdup(m->name);
	c.logo = xstrdup(m->logo ? m->logo : "");
	c.ext = xstrdup(m->ext ? m->ext : "");
	c.sname = xstrdup(m->sname ? m->sname : "");
	c.kind = m->kind;
	c.id = m->id;
	c.series = m->series;
	c.time = (long long)time(NULL);
	push(l, &c, 1);
	if (list == ListRecent && l->n > MAXRECENT)
		freemark(&l->v[--l->n]);
	savemarks(list);
}

/* changes whenever a list changes, so views built from it can rebuild */
unsigned long
store_version(void)
{
	return version;
}
