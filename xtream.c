/* See LICENSE file for copyright and license details.
 *
 * Xtream Codes client: player_api.php for the account, for the
 * categories and items of live TV, movies and series, and for per-title
 * details. Providers disagree on types (ids as strings or numbers, null
 * categories, category_ids arrays, objects where arrays belong), so
 * everything is read through json_str()/json_int().
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

#define INFOTTL (3 * 86400)

static const struct {
	const char *cats, *items, *id, *logo, *all;
} kinds[] = {
	[KindLive]   = { "live_categories",   "live_streams", "stream_id", "stream_icon", "All channels" },
	[KindVod]    = { "vod_categories",    "vod_streams",  "stream_id", "stream_icon", "All movies" },
	[KindSeries] = { "series_categories", "series",       "series_id", "cover",       "All series" },
};

/* scheme://host[:port] without trailing slashes */
static void
base(const Source *s, char *buf, size_t n)
{
	size_t len;

	snprintf(buf, n, "%s%s", strstr(s->url, "://") ? "" : "http://", s->url);
	for (len = strlen(buf); len && buf[len - 1] == '/'; )
		buf[--len] = '\0';
}

static void
request(Playlist *p, const char *query, const char *cache, int maxage, int kind, int sub, int id)
{
	char b[512], u[256], pw[256], key[1024], url[2048], path[1300];
	Job *j;

	base(p->src, b, sizeof b);
	urlenc(u, sizeof u, p->src->user);
	urlenc(pw, sizeof pw, p->src->pass);
	snprintf(url, sizeof url, "%s/player_api.php?username=%s&password=%s%s", b, u, pw, query);
	/* hashed so the path does not leak credentials; the password is in
	 * it so fixing a typo does not keep serving cached rejections */
	snprintf(key, sizeof key, "%s\n%s\n%s", b, p->src->user, p->src->pass);
	snprintf(path, sizeof path, "%s/xt-%016llx/%s.json", appdir(DirCache), fnv1a(key), cache);
	j = net_job(url, path, maxage, 1);
	j->owner = JobXtream;
	j->kind = kind;
	j->sub = sub;
	j->id = id;
	j->ud = p;
	net_submit(j);
}

void
xt_account(Playlist *p, int ttl, int force)
{
	/* active connections change often */
	request(p, "", "account", force ? 0 : MIN(ttl, 300), ResAuth, 0, 0);
	p->authloading++;
	p->autherr[0] = '\0';
}

void
xt_load(Playlist *p, int kind, int ttl, int force)
{
	Catalog *c = &p->cat[kind];
	char q[64];

	snprintf(q, sizeof q, "&action=get_%s", kinds[kind].cats);
	request(p, q, kinds[kind].cats, force ? 0 : ttl, ResCats, kind, 0);
	snprintf(q, sizeof q, "&action=get_%s", kinds[kind].items);
	request(p, q, kinds[kind].items, force ? 0 : ttl, ResItems, kind, 0);
	c->loading += 2;
	c->err[0] = '\0';
}

/* get_vod_info or get_series_info; comes back as a ResInfo job */
void
xt_info(Playlist *p, int kind, int id)
{
	char q[64], cache[64];

	if (kind == KindVod) {
		snprintf(q, sizeof q, "&action=get_vod_info&vod_id=%d", id);
		snprintf(cache, sizeof cache, "vod/%d", id);
	} else {
		snprintf(q, sizeof q, "&action=get_series_info&series_id=%d", id);
		snprintf(cache, sizeof cache, "series/%d", id);
	}
	request(p, q, cache, INFOTTL, ResInfo, kind, id);
}

/* now and next, or the whole guide with its archive; ResEpg, sub = full */
void
xt_epg(Playlist *p, int id, int full)
{
	char q[96], cache[48];

	if (full) {
		snprintf(q, sizeof q, "&action=get_simple_data_table&stream_id=%d", id);
		snprintf(cache, sizeof cache, "guide/%d", id);
	} else {
		snprintf(q, sizeof q, "&action=get_short_epg&stream_id=%d&limit=8", id);
		snprintf(cache, sizeof cache, "epg/%d", id);
	}
	request(p, q, cache, full ? 1800 : 600, ResEpg, full, id);
}

static int
hasformat(const JVal *fmts, const char *f)
{
	const JVal *v;

	for (v = fmts->child; v; v = v->next)
		if (!strcmp(json_str(v), f))
			return 1;
	return 0;
}

static void
account(Playlist *p)
{
	JVal *ui = json_get(p->auth->root, "user_info"), *f;
	long long exp;
	time_t t;

	p->authok = json_int(json_get(ui, "auth"), 0) == 1;
	if (!p->authok) {
		snprintf(p->autherr, sizeof p->autherr, "Login rejected, check the username and password");
		return;
	}
	snprintf(p->status, sizeof p->status, "%s", json_str(json_get(ui, "status")));
	p->exp[0] = '\0';
	if ((exp = json_int(json_get(ui, "exp_date"), 0)) > 0) {
		t = (time_t)exp;
		strftime(p->exp, sizeof p->exp, "%Y-%m-%d", gmtime(&t));
	}
	p->active = json_int(json_get(ui, "active_cons"), 0);
	p->maxconn = json_int(json_get(ui, "max_connections"), 0);

	/* use the preferred container only if the account may */
	f = json_get(ui, "allowed_output_formats");
	if (f && f->type == JArr && f->child && !hasformat(f, p->ext))
		snprintf(p->ext, sizeof p->ext, "%s",
		         hasformat(f, "m3u8") ? "m3u8" : json_str(f->child));
}

static int
count(const JVal *v)
{
	int n = 0;

	if (v && v->type == JArr)
		for (v = v->child; v; v = v->next)
			n++;
	return n;
}

/* category id -> group index, open addressing */
typedef struct {
	const char **keys;
	int *vals;
	size_t mask;
} Map;

/* insert k, or with !add look it up only (0: unknown) */
static int *
slot(Map *m, const char *k, int add)
{
	static int none;
	size_t i = fnv1a(k) & m->mask;

	while (m->keys[i] && strcmp(m->keys[i], k))
		i = (i + 1) & m->mask;
	if (!m->keys[i] && !add) {
		none = 0;
		return &none;
	}
	m->keys[i] = k;
	return &m->vals[i];
}

/* drop empty groups, renumbering the items' */
static void
compact(Catalog *c, int ng)
{
	int *remap = arena_alloc(&c->arena, ng * sizeof *remap), g;
	Item *it;

	for (c->ngroups = g = 1; g < ng; g++) {
		if (!c->gcount[g])
			continue;
		remap[g] = c->ngroups;
		c->groups[c->ngroups] = c->groups[g];
		c->gcount[c->ngroups++] = c->gcount[g];
	}
	for (it = c->items; it < c->items + c->nitems; it++)
		it->group = remap[it->group];
}

/* a copy in the catalog's arena, so the parsed JSON can be freed */
static const char *
keep(Catalog *c, const JVal *v)
{
	const char *s = v && v->type == JArr ? json_str(v->child) : json_str(v);

	return *s ? arena_strdup(&c->arena, s) : "";
}

static const char *
either(Catalog *c, const JVal *o, const char *k1, const char *k2, const char *k3)
{
	const char *s = keep(c, json_get(o, k1));

	if (!*s && k2)
		s = keep(c, json_get(o, k2));
	if (!*s && k3)
		s = keep(c, json_get(o, k3));
	return s;
}

static void
build(Catalog *c, int kind)
{
	JVal *cats = c->res[0]->root, *items = c->res[1]->root, *s, *v;
	int ncats = count(cats), n = 0, ng = 1, unc = 0, g, *gi;
	const char *name, *cat;
	long long id;
	Item *it;
	Map m;

	arena_free(&c->arena);
	c->groups = arena_alloc(&c->arena, (ncats + 2) * sizeof *c->groups);
	c->gcount = arena_alloc(&c->arena, (ncats + 2) * sizeof *c->gcount);
	c->items = arena_alloc(&c->arena, (count(items) + 1) * sizeof *c->items);
	for (m.mask = 15; m.mask < (size_t)ncats * 2; m.mask = m.mask * 2 + 1)
		;
	m.keys = arena_alloc(&c->arena, (m.mask + 1) * sizeof *m.keys);
	m.vals = arena_alloc(&c->arena, (m.mask + 1) * sizeof *m.vals);

	c->groups[0] = kinds[kind].all;
	if (cats && cats->type == JArr) {
		for (v = cats->child; v; v = v->next) {
			gi = slot(&m, json_str(json_get(v, "category_id")), 1);
			if (*gi)
				continue;    /* duplicate id */
			name = keep(c, json_get(v, "category_name"));
			c->groups[ng] = *name ? name : "Unnamed";
			*gi = ng++;
		}
	}

	/* keep only what is shown; the JSON for a big list is several
	 * times the size of these copies */
	if (items && items->type == JArr) {
		for (s = items->child; s; s = s->next) {
			it = &c->items[n];
			id = json_int(json_get(s, kinds[kind].id), -1);
			if (!*json_str(json_get(s, "name")) || id < 0 || id > 0x7fffffff)
				continue;
			it->name = keep(c, json_get(s, "name"));
			it->id = (int)id;
			it->kind = kind;
			it->logo = keep(c, json_get(s, kinds[kind].logo));
			it->epgid = kind == KindLive ? keep(c, json_get(s, "epg_channel_id")) : "";
			it->ext = kind == KindVod ? keep(c, json_get(s, "container_extension")) : "";
			it->rating = kind != KindLive ? keep(c, json_get(s, "rating")) : "";
			it->date = kind != KindLive ? either(c, s, "year", "releaseDate", "releasedate") : "";
			it->genre = kind == KindSeries ? keep(c, json_get(s, "genre")) : "";
			it->plot = kind == KindSeries ? keep(c, json_get(s, "plot")) : "";
			it->cast = kind == KindSeries ? keep(c, json_get(s, "cast")) : "";
			it->backdrop = kind == KindSeries ? keep(c, json_get(s, "backdrop_path")) : "";
			it->sname = "";
			it->live = kind == KindLive;
			if (json_int(json_get(s, "tv_archive"), 0))
				it->archive = (int)json_int(json_get(s, "tv_archive_duration"), 0);
			cat = json_str(json_get(s, "category_id"));
			if (!*cat && (v = json_get(s, "category_ids")) && v->type == JArr)
				cat = json_str(v->child);
			if (!(g = *slot(&m, cat, 0))) {
				if (!unc) {
					unc = ng;
					c->groups[ng++] = "Uncategorized";
				}
				g = unc;
			}
			it->group = g;
			c->gcount[g]++;
			n++;
		}
	}
	c->nitems = c->gcount[0] = n;
	compact(c, ng);
	c->loaded = 1;
}

/* hold each answer until its other half arrives, build, then drop
 * both: the catalog keeps its own copies */
static int
take(Catalog *c, Job *j)
{
	int kind = j->sub, k = j->kind == ResCats ? 0 : 1;

	net_free(c->res[k]);
	c->res[k] = j;
	if (!c->res[0] || !c->res[1])
		return -1;
	build(c, kind);
	net_free(c->res[0]);
	net_free(c->res[1]);
	c->res[0] = c->res[1] = NULL;
	return kind;
}

/* takes ownership of j; returns the kind whose list was rebuilt, or -1 */
int
xt_result(Playlist *p, Job *j)
{
	Catalog *c;
	int rebuilt = -1;

	if (j->kind == ResAuth) {
		if (!j->stale)
			p->authloading--;
		if (j->err[0] || !j->root) {
			if (!p->autherr[0] && !p->auth)
				snprintf(p->autherr, sizeof p->autherr, "%s", j->err[0] ? j->err : "no data");
			net_free(j);
			return -1;
		}
		net_free(p->auth);    /* account() copies what it keeps */
		p->auth = j;
		account(p);
		return -1;
	}
	c = &p->cat[j->sub];
	if (!j->stale)
		c->loading--;
	if (j->err[0] || !j->root) {
		if (!c->err[0])
			snprintf(c->err, sizeof c->err, "%s", j->err[0] ? j->err : "no data");
		net_free(j);
	} else {
		rebuilt = take(c, j);
	}
	/* a half whose partner failed would wait forever */
	if (!c->loading) {
		net_free(c->res[0]);
		net_free(c->res[1]);
		c->res[0] = c->res[1] = NULL;
	}
	return rebuilt;
}

void
xt_free(Catalog *c)
{
	arena_free(&c->arena);
	net_free(c->res[0]);
	net_free(c->res[1]);
	memset(c, 0, sizeof *c);
}

/* episodes of a get_series_info answer, one group per season. Panels
 * send "episodes" as an object keyed by season or as an array of arrays.
 * The answer stays in c->res[1]: items point into it */
void
xt_episodes(Catalog *c, Job *info, int sid, const char *sname)
{
	JVal *eps = json_get(info->root, "episodes"), *s, *e, *in;
	const char *num;
	char *name;
	int n = 0, ns = 0, ng = 1;
	long long id;
	Item *it;

	xt_free(c);
	c->res[1] = info;
	c->loaded = 1;
	if (eps && (eps->type == JObj || eps->type == JArr))
		for (s = eps->child; s; s = s->next, ns++)
			n += count(s);
	c->groups = arena_alloc(&c->arena, (ns + 1) * sizeof *c->groups);
	c->gcount = arena_alloc(&c->arena, (ns + 1) * sizeof *c->gcount);
	c->items = arena_alloc(&c->arena, (n + 1) * sizeof *c->items);
	c->groups[0] = "All episodes";
	c->ngroups = 1;
	if (!n)
		return;
	for (s = eps->child; s; s = s->next) {
		if (s->type != JArr || !s->child)
			continue;
		num = s->key ? s->key : json_str(json_get(s->child, "season"));
		name = arena_alloc(&c->arena, 32);
		if (*num)
			snprintf(name, 32, "Season %.20s", num);
		else
			snprintf(name, 32, "Season %d", ng);
		for (e = s->child; e; e = e->next) {
			id = json_int(json_get(e, "id"), -1);
			if (id < 0 || id > 0x7fffffff)
				continue;
			it = &c->items[c->nitems++];
			in = json_get(e, "info");
			it->id = (int)id;
			it->kind = KindEpisode;
			it->series = sid;
			it->sname = sname;
			it->raw = e;
			it->name = json_str(json_get(e, "title"));
			if (!*it->name)
				it->name = "Untitled episode";
			it->ext = json_str(json_get(e, "container_extension"));
			it->logo = json_str(json_get(in, "movie_image"));
			it->plot = json_str(json_get(in, "plot"));
			it->rating = json_str(json_get(in, "rating"));
			it->epgid = it->date = it->genre = it->cast = it->backdrop = "";
			it->group = ng;
			c->gcount[ng]++;
		}
		c->groups[ng++] = name;
	}
	c->gcount[0] = c->nitems;
	compact(c, ng);
}

/* a recording: {host}/timeshift/{user}/{pass}/{minutes}/{YYYY-MM-DD:HH-MM}/{id}.ts,
 * the start in the provider's own time as its guide gave it */
void
xt_catchup(const Playlist *p, int id, const char *local, long long start, int minutes,
           char *buf, size_t n)
{
	char b[512], u[256], pw[256], when[32];
	time_t t = (time_t)start;

	base(p->src, b, sizeof b);
	urlenc(u, sizeof u, p->src->user);
	urlenc(pw, sizeof pw, p->src->pass);
	if (strlen(local) >= 16)
		snprintf(when, sizeof when, "%.10s:%.2s-%.2s", local, local + 11, local + 14);
	else
		strftime(when, sizeof when, "%Y-%m-%d:%H-%M", localtime(&t));
	snprintf(buf, n, "%s/timeshift/%s/%s/%d/%s/%d.ts", b, u, pw, minutes, when, id);
}

void
xt_url(const Playlist *p, int kind, const Item *it, char *buf, size_t n)
{
	char b[512], u[256], pw[256];
	const char *dir, *ext;

	base(p->src, b, sizeof b);
	urlenc(u, sizeof u, p->src->user);
	urlenc(pw, sizeof pw, p->src->pass);
	dir = kind == KindLive ? "live" : kind == KindVod ? "movie" : "series";
	ext = kind == KindLive ? p->ext : it->ext && *it->ext ? it->ext : "mp4";
	snprintf(buf, n, "%s/%s/%s/%s/%d.%s", b, dir, u, pw, it->id, ext);
}
