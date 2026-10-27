/* See LICENSE file for copyright and license details.
 *
 * Programme guide from Xtream: get_short_epg for what is on now and
 * next, get_simple_data_table for a channel's whole guide and archive.
 * Kept per channel in a small LRU. Titles usually arrive base64 encoded,
 * sometimes not; text that does not decode to clean UTF-8 is taken as is.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"
#include "json.h"
#include "net.h"
#include "playlist.h"
#include "xtream.h"
#include "epg.h"
#include "xmltv.h"

#define XMLTTL   (4 * 3600)    /* seconds an XMLTV download stays fresh */
#define MAXEPG   256
#define SHORTTTL 600     /* seconds before now/next is asked again */
#define FULLTTL  1800    /* and a whole guide */
#define RETRY    60      /* after a failure, or when the list ran out */

static Epg cache[MAXEPG];
static unsigned long tick;

static Epg *
find(const Playlist *p, int id)
{
	int i;

	for (i = 0; i < MAXEPG; i++)
		if (cache[i].p == p && cache[i].id == id)
			return &cache[i];
	return NULL;
}

/* what is known about a channel, without asking for more */
Epg *
epg_find(const Playlist *p, int id)
{
	Epg *e = find(p, id);

	if (!e || !e->n)
		return NULL;
	e->used = ++tick;
	return e;
}

static int
stale(const Epg *e, long long now, int full)
{
	if (full && !e->full)
		return 1;
	if (now - e->fetched < RETRY)
		return 0;
	if (now - e->fetched > (e->full ? FULLTTL : SHORTTTL))
		return 1;
	/* nothing left that is on now or later */
	return !e->n || e->progs[e->n - 1].stop <= now;
}

/* a channel's guide out of the parsed XMLTV; M3U channel ids are their
 * position in the list */
static void
xmlfill(Epg *e)
{
	const Catalog *c = &e->p->cat[KindLive];
	const Prog *src;
	Prog *pg;
	int i, n = 0;

	e->fetched = time(NULL);
	e->full = 1;
	e->gen++;
	arena_free(&e->arena);
	e->progs = NULL;
	e->n = 0;
	if (e->id < 0 || e->id >= c->nitems ||
	    !(src = xml_chan(e->p->xml->res, c->items[e->id].epgid, &n)))
		return;
	pg = arena_alloc(&e->arena, n * sizeof *pg);
	for (i = 0; i < n; i++) {
		pg[i] = src[i];
		pg[i].title = arena_strdup(&e->arena, src[i].title);
		pg[i].desc = arena_strdup(&e->arena, src[i].desc);
	}
	e->progs = pg;
	e->n = n;
}

/* ask for the guide of an M3U source once its channels are known */
void
epg_xmltv(Playlist *p)
{
	const Catalog *c = &p->cat[KindLive];
	char url[1100], path[1300], *ids, *w;
	size_t len = 1, n;
	Job *j;
	int i;

	if (!*p->guide)
		return;
	for (i = 0; i < c->nitems; i++)
		len += strlen(c->items[i].epgid) + 1;
	if (len == (size_t)c->nitems + 1)
		return;    /* no channel has an id */
	w = ids = xmalloc(len);
	for (i = 0; i < c->nitems; i++) {
		n = strlen(c->items[i].epgid);
		if (n) {
			memcpy(w, c->items[i].epgid, n);
			w += n;
			*w++ = '\n';
		}
	}
	*w = '\0';
	if (!strncmp(p->guide, "http://", 7) || !strncmp(p->guide, "https://", 8)) {
		snprintf(url, sizeof url, "%s", p->guide);
		snprintf(path, sizeof path, "%s/xmltv-%016llx/guide", appdir(DirCache), fnv1a(p->guide));
		j = net_job(url, path, XMLTTL, 0);
	} else {
		/* curl reads files too */
		snprintf(url, sizeof url, "file://%s%s", *p->guide == '/' ? "" : "/", p->guide);
		for (w = url; *w; w++)
			if (*w == '\\')
				*w = '/';
		j = net_job(url, NULL, 0, 0);
	}
	j->owner = JobXml;
	j->work = xml_parse;
	j->aux = ids;
	j->ud = p;
	net_submit(j);
	p->xmlload++;
}

void
epg_xml_result(Job *j)
{
	Playlist *p = j->ud;
	int i;

	if (!j->stale && p->xmlload > 0)
		p->xmlload--;
	if (!j->err[0] && j->res) {
		net_free(p->xml);
		p->xml = j;
	} else {
		net_free(j);
	}
	for (i = 0; i < MAXEPG; i++)
		if (cache[i].p == p) {
			if (p->xml)
				xmlfill(&cache[i]);
			else
				cache[i].gen++;
		}
}

/* the guide of channel id, fetched when missing or old; NULL only when
 * every slot is busy */
Epg *
epg_want(Playlist *p, int id, int full)
{
	Epg *e = find(p, id), *old = NULL;
	long long now = time(NULL);
	int i;

	if (!e) {
		for (i = 0; i < MAXEPG; i++)
			if (!cache[i].loading && !cache[i].pin && (!old || cache[i].used < old->used))
				old = &cache[i];
		if (!old)
			return NULL;
		arena_free(&old->arena);
		memset(old, 0, sizeof *old);
		old->p = p;
		old->id = id;
		e = old;
	}
	e->used = ++tick;
	if (p->src->type == SrcM3u) {
		if (!e->fetched && p->xml)
			xmlfill(e);
		e->loading = p->xmlload > 0 && !e->fetched;
		return e;
	}
	if (!e->loading && stale(e, now, full)) {
		/* once a whole guide is wanted, keep asking for the whole one */
		xt_epg(p, id, full || e->full);
		e->loading = 1;
	}
	return e;
}

/* base64 when it decodes to clean text, else the text itself */
static const char *
text(Arena *a, const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = arena_alloc(a, n);

	if (b64dec(s, d, n) >= 0 && *d && utf8ok(d))
		return d;
	memcpy(d, s, n);
	return d;
}

/* "YYYY-MM-DD HH:MM:SS" in local time */
static long long
parsetime(const char *s)
{
	struct tm tm;

	memset(&tm, 0, sizeof tm);
	if (sscanf(s, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
	           &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 5)
		return 0;
	tm.tm_year -= 1900;
	tm.tm_mon--;
	tm.tm_isdst = -1;
	return (long long)mktime(&tm);
}

static int
bystart(const void *a, const void *b)
{
	const Prog *x = a, *y = b;

	return (x->start > y->start) - (x->start < y->start);
}

void
epg_result(Job *j)
{
	Epg *e = find(j->ud, j->id);
	JVal *l, *v;
	Arena a = { 0 };
	Prog *pg, *progs;
	int n = 0;

	if (!e) {
		net_free(j);    /* evicted meanwhile */
		return;
	}
	if (!j->stale)
		e->loading = 0;
	e->fetched = time(NULL);
	l = json_get(j->root, "epg_listings");
	if (j->err[0] || !l || l->type != JArr) {
		net_free(j);
		return;
	}
	for (v = l->child; v; v = v->next)
		n++;
	progs = arena_alloc(&a, (n + 1) * sizeof *progs);
	for (n = 0, v = l->child; v; v = v->next) {
		pg = &progs[n];
		if (!(pg->start = json_int(json_get(v, "start_timestamp"), 0)))
			pg->start = parsetime(json_str(json_get(v, "start")));
		if (!(pg->stop = json_int(json_get(v, "stop_timestamp"), 0)))
			pg->stop = parsetime(json_str(json_get(v, "end")));
		if (pg->stop <= pg->start)
			continue;
		pg->title = text(&a, json_str(json_get(v, "title")));
		pg->desc = text(&a, json_str(json_get(v, "description")));
		pg->local = arena_strdup(&a, json_str(json_get(v, "start")));
		pg->archive = json_int(json_get(v, "has_archive"), 0) == 1;
		n++;
	}
	qsort(progs, n, sizeof *progs, bystart);
	arena_free(&e->arena);
	e->arena = a;
	e->progs = progs;
	e->n = n;
	e->full = j->sub;
	e->gen++;
	net_free(j);
}

/* what is on at t, and what follows it */
const Prog *
epg_now(const Epg *e, long long t, const Prog **next)
{
	const Prog *cur = NULL;
	int i;

	if (next)
		*next = NULL;
	if (!e)
		return NULL;
	for (i = 0; i < e->n; i++) {
		if (e->progs[i].start <= t && t < e->progs[i].stop) {
			cur = &e->progs[i];
		} else if (e->progs[i].start > t) {
			if (next)
				*next = &e->progs[i];
			break;
		}
	}
	return cur;
}

const Prog *
epg_prog(const Epg *e, long long start)
{
	int i;

	for (i = 0; e && i < e->n; i++)
		if (e->progs[i].start == start)
			return &e->progs[i];
	return NULL;
}

static long long
daykey(long long t)
{
	time_t tt = (time_t)t;
	struct tm *tm = localtime(&tt);

	return tm ? tm->tm_year * 400LL + tm->tm_yday : 0;
}

/* "Today", "Yesterday", "Tomorrow" or "Tue 28 Oct" */
void
epg_day(long long t, long long now, char *buf, size_t n)
{
	long long d = daykey(t) - daykey(now);
	time_t tt = (time_t)t;

	if (d == 0)
		snprintf(buf, n, "Today");
	else if (d == -1)
		snprintf(buf, n, "Yesterday");
	else if (d == 1)
		snprintf(buf, n, "Tomorrow");
	else
		strftime(buf, n, "%a %d %b", localtime(&tt));
}

/* the guide as a catalog, one group per day. An item's id is its start
 * time, its series the channel; archive means it can be replayed.
 * Returns the group of today */
int
epg_catalog(const Epg *e, Catalog *c, int chan, int archdays, long long t)
{
	const Prog *pg;
	char buf[64];
	time_t tt;
	Item *it;
	int i, g = 0, today = 0;
	long long day = -1;

	xt_free(c);
	c->loaded = 1;
	c->items = arena_alloc(&c->arena, (e->n + 1) * sizeof *c->items);
	c->groups = arena_alloc(&c->arena, (e->n + 2) * sizeof *c->groups);
	c->gcount = arena_alloc(&c->arena, (e->n + 2) * sizeof *c->gcount);
	c->groups[0] = "Whole guide";
	c->ngroups = 1;
	for (i = 0; i < e->n; i++) {
		pg = &e->progs[i];
		if (daykey(pg->start) != day) {
			day = daykey(pg->start);
			epg_day(pg->start, t, buf, sizeof buf);
			g = c->ngroups++;
			c->groups[g] = arena_strdup(&c->arena, buf);
			if (day == daykey(t))
				today = g;
		}
		it = &c->items[c->nitems++];
		it->name = pg->title;
		it->plot = pg->desc;
		it->logo = it->epgid = it->ext = it->rating = it->genre = "";
		it->cast = it->backdrop = it->sname = "";
		tt = (time_t)pg->start;
		strftime(buf, sizeof buf, "%H:%M", localtime(&tt));
		it->date = arena_strdup(&c->arena, buf);
		it->id = (int)pg->start;
		it->series = chan;
		it->kind = KindProg;
		it->group = g;
		it->archive = pg->stop <= t && (pg->archive ||
		              (archdays > 0 && pg->start > t - archdays * 86400LL));
		c->gcount[g]++;
	}
	c->gcount[0] = c->nitems;
	return today;
}
