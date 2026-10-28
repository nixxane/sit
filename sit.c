/* See LICENSE file for copyright and license details.
 *
 * sit - suckless iptv. One window: groups on the left, channels or
 * posters in the middle, preview or details on the right; video through
 * libmpv's OpenGL renderer. Keys, colours and sources live in config.h.
 */
#include <ctype.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "util.h"
#include "gfx.h"
#include "ui.h"
#include "json.h"
#include "net.h"
#include "playlist.h"
#include "xtream.h"
#include "m3u.h"
#include "img.h"
#include "store.h"
#include "epg.h"
#include "player.h"

enum { ViewBrowse = 1, ViewPlayer = 2 };
/* the first three are Kind*; the episode and guide pages sit over a tab */
enum { PageLive, PageVod, PageSeries, PageFav, PageRecent, PageEpisodes, PageGuide, PageLast };
enum { FontText, FontBold, FontTitle, FontSmall };
enum { ColBg, ColPanel, ColFg, ColDim, ColAccent, ColSel, ColLive, ColGood, ColLast };

typedef union {
	int i;
	float f;
} Arg;

typedef struct {
	int views, mod;
	SDL_Keycode sym;
	void (*func)(const Arg *);
	Arg arg;
} Key;

typedef struct {
	const char *name, *group, *url;
	int live;    /* live HLS reports its playlist window as duration */
} Chan;          /* entries of the built-in Demo source */

static void back(const Arg *a);
static void cycle(const Arg *a);
static int drawonboarding(void);
static void favorite(const Arg *a);
static void focus(const Arg *a);
static void fullscreen(const Arg *a);
static void guide(const Arg *a);
static void jump(const Arg *a);
static void kind(const Arg *a);
static void move(const Arg *a);
static void mute(const Arg *a);
static void number(const Arg *a);
static void obkey(const SDL_Keysym *k);
static void obtext(const char *s);
static void page(const Arg *a);
static void play(const Arg *a);
static void quit(const Arg *a);
static void refresh(const Arg *a);
static void search(const Arg *a);
static void seek(const Arg *a);
static int seekable(void);
static void source(const Arg *a);
static void stop(const Arg *a);
static void togglepause(const Arg *a);
static void volume(const Arg *a);
static void zap(const Arg *a);

#include "config.h"

#define MAXFOUND 60

static SDL_Window *win;
static SDL_GLContext glctx;
static unsigned int evmpv;
static float S = 1, ratio = 1;      /* ui scale; drawable px per window unit */
static int W, H, running = 1, view = ViewBrowse, fs;
static float mx = -1, my = -1;

static Source *srcs;
static Playlist *lists;             /* one per source, loaded on first use */
static int nsrcs, cur;
static const char **args;           /* urls from the command line */
static int nargs, skipped;
#define P (&lists[cur])

/* pages: a tab of P, or the episodes of one series */
static int tab, inseries;
static Catalog eps, marks[2];       /* episodes; favorites and recent of P */
static unsigned long marksver[2] = { (unsigned long)-1, (unsigned long)-1 };
static int markssrc[2] = { -1, -1 };
static int epsid = -1;
static char epsname[256], epscover[1024];
static List gls[PageLast], cls[PageLast];
static int keep[PageLast] = { -1, -1, -1, -1, -1, -1, -1 };    /* selection of pages not shown */
#define PG (inseries ? PageEpisodes : inguide ? PageGuide : tab)
#define glist gls[PG]
#define clist cls[PG]

/* the guide page: one channel's programmes, its Epg pinned meanwhile */
static int inguide, guidegen = -1;
static Item guidech;
static char guidename[256], guidelogo[1024], guidegroup[256];
static Catalog guidecat;
static Epg *gepg;

static int *vis, nvis, logos;       /* items of the page in the selected group */
static int pane = 1;                /* 0: groups, 1: items */

/* the search palette */
static int searching, skipslash;
static char query[256];
static Catalog found;
static List slist;
static Rect searchr;

/* channel number entry, like a TV remote */
static int numbering;
static char numberbuf[8];

/* first-run onboarding: one source, appended to the sources file */
enum { ObName, ObType, ObUrl, ObUser, ObPass, ObXml, ObFieldLast };
static int onboarding, obfield, obtype;
static char obname[256], oburl[1024], obuser[256], obpass[256], obxml[1024];

/* movie details, fetched when the selection rests on one */
typedef struct {
	const Playlist *p;
	int id;
	Job *j;
} Info;
static Info infos[48];
static int ninfos, lastsel = -2, lastpg = -1;
static double selt;

/* what plays; survives page and source switches and list rebuilds */
static int playsrc = -1, playkind, playid, playlive, playseries = -1;
static char playname[256], playgroup[300];
static double lastsave, lastflush;

/* layout, recomputed every frame */
static Rect sider, glr, clr, vidr, gridr, tabr[5], backr;
static float gcellw, gcellh, ggap;
static int gcols = 1;
static double lastinput, osdtime, nextwake, t;
static float ovl = 1;
static char osdmsg[128];

static Color
col(int c, float a)
{
	return rgba(colors[theme][c], a);
}

static void
osd(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(osdmsg, sizeof osdmsg, fmt, ap);
	va_end(ap);
	osdtime = now();
}

static void
wake(double at)
{
	if (at > t && (nextwake <= 0 || at < nextwake))
		nextwake = at;
}

static void
append(char *buf, size_t n, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf + len, n - len, fmt, ap);
	va_end(ap);
}

static void
fmtcount(char *buf, size_t n, int v)
{
	if (v >= 1000000)
		snprintf(buf, n, "%d,%03d,%03d", v / 1000000, v / 1000 % 1000, v % 1000);
	else if (v >= 1000)
		snprintf(buf, n, "%d,%03d", v / 1000, v % 1000);
	else
		snprintf(buf, n, "%d", v);
}

static void
fmtdur(char *buf, size_t n, long long secs)
{
	if (secs >= 3600)
		snprintf(buf, n, "%lldh %02lldm", secs / 3600, secs / 60 % 60);
	else
		snprintf(buf, n, "%lld min", (secs + 59) / 60);
}

static void
fmtago(char *buf, size_t n, long long when)
{
	long long d = (long long)time(NULL) - when;

	if (d < 60)
		snprintf(buf, n, "just now");
	else if (d < 3600)
		snprintf(buf, n, "%lld min ago", d / 60);
	else if (d < 86400)
		snprintf(buf, n, "%lldh ago", d / 3600);
	else
		snprintf(buf, n, "%lld day%s ago", d / 86400, d < 2 * 86400 ? "" : "s");
}

static int
hastabs(void)
{
	return P->src->type == SrcXtream;
}

/* an Xtream account or an M3U playlist with an XMLTV guide */
static int
hasguide(const Playlist *p)
{
	return p->src->type == SrcXtream || (p->src->type == SrcM3u && *p->guide);
}

/* tabs a source has: Movies and Series need Xtream */
static int
tabok(int k)
{
	return k == PageLive || k == PageFav || k == PageRecent || hastabs();
}

static Catalog *
catalog(void)
{
	if (inseries)
		return &eps;
	if (inguide)
		return &guidecat;
	return tab < PageFav ? &P->cat[tab] : &marks[tab - PageFav];
}

static int
isgrid(void)
{
	return PG == PageVod || PG == PageSeries;
}

static const char *
noun(int n)
{
	static const char *one[] = { "channel", "movie", "series", "favorite", "item", "episode", "programme" };
	static const char *many[] = { "channels", "movies", "series", "favorites", "items", "episodes",
	                              "programmes" };

	return n == 1 ? one[PG] : many[PG];
}

static const char *
kindname(int k)
{
	static const char *names[] = { "Channel", "Movie", "Series", "Episode", "Programme" };

	return names[k];
}

static const Item *
focused(void)
{
	return nvis ? &catalog()->items[vis[clist.sel]] : NULL;
}

static int
selid(void)
{
	return nvis ? catalog()->items[vis[clist.sel]].id : -1;
}

static int
isfav(const Item *it)
{
	return it->kind != KindEpisode && store_find(ListFav, P->src->name, it->kind, it->id, 0);
}

/* recompute vis for the selected group; keep item keepid selected */
static void
filter(int keepid)
{
	Catalog *c = catalog();
	int i;

	glist.n = c->ngroups;
	if (glist.sel >= c->ngroups)
		glist.sel = 0;
	free(vis);
	vis = xcalloc(c->nitems + 1, sizeof *vis);
	logos = 0;
	for (nvis = i = 0; i < c->nitems; i++) {
		if (glist.sel && c->items[i].group != glist.sel)
			continue;
		vis[nvis++] = i;
		logos |= c->items[i].logo && *c->items[i].logo;
	}
	ui_list_reset(&clist, nvis);
	for (i = 0; keepid >= 0 && i < nvis; i++)
		if (c->items[vis[i]].id == keepid) {
			clist.sel = i;
			clist.pill = i * rowh * S;
		}
}

/* selection by name and id, to survive a rebuild */
static void
remember(char *gname, size_t n, int *id)
{
	Catalog *c = catalog();

	snprintf(gname, n, "%s", c->ngroups ? c->groups[glist.sel] : "");
	*id = selid();
}

static void
restore(const char *gname, int id)
{
	Catalog *c = catalog();
	int g;

	ui_list_reset(&glist, c->ngroups);
	for (g = 0; g < c->ngroups; g++)
		if (!strcmp(c->groups[g], gname)) {
			glist.sel = g;
			glist.pill = g * rowh * S;
		}
	filter(id);
}

static void
blank(Item *it)
{
	it->name = it->logo = it->epgid = it->ext = "";
	it->rating = it->date = it->genre = it->plot = it->cast = it->backdrop = it->sname = "";
}

static void
loadstatic(Playlist *p)
{
	Catalog *c = &p->cat[KindLive];
	int n = p->src->type == SrcDemo ? (int)LEN(demo) : nargs, i, g;
	const char *grp;
	Item *it;

	c->items = arena_alloc(&c->arena, (n + 1) * sizeof *c->items);
	c->groups = arena_alloc(&c->arena, (n + 1) * sizeof *c->groups);
	c->gcount = arena_alloc(&c->arena, (n + 1) * sizeof *c->gcount);
	c->groups[0] = "All channels";
	c->ngroups = 1;
	for (i = 0; i < n; i++) {
		it = &c->items[i];
		blank(it);
		it->id = i;
		it->kind = KindLive;
		if (p->src->type == SrcDemo) {
			it->name = demo[i].name;
			it->url = demo[i].url;
			it->live = demo[i].live;
			grp = demo[i].group;
		} else {
			it->name = it->url = args[i];
			grp = "Command line";
		}
		for (g = 1; g < c->ngroups && strcmp(c->groups[g], grp); g++)
			;
		if (g == c->ngroups)
			c->groups[c->ngroups++] = grp;
		it->group = g;
		c->gcount[g]++;
	}
	c->nitems = c->gcount[0] = n;
	c->loaded = 1;
}

/* a channel of a static source by id, for its url */
static const char *
staticurl(int id)
{
	Catalog *c = &P->cat[KindLive];

	return id >= 0 && id < c->nitems ? c->items[id].url : NULL;
}

/* favorites or recent of the current source as a catalog, grouped by kind */
static void
buildmarks(int list)
{
	static const char *names[2][4] = {
		{ "All favorites", "Channels", "Movies", "Series" },
		{ "All recent", "Channels", "Movies", "Episodes" },
	};
	Catalog *c = &marks[list];
	int n, i, k, gk[4] = { 0 }, ng = 1, cnt[4] = { 0 };
	char ago[32];
	Mark *m;
	Item *it;

	xt_free(c);
	n = store_marks(list, &m);
	for (i = 0; i < n; i++)
		if (!strcmp(m[i].src, P->src->name))
			cnt[MIN(m[i].kind, KindSeries) + 1]++;
	c->items = arena_alloc(&c->arena, (n + 1) * sizeof *c->items);
	c->groups = arena_alloc(&c->arena, 4 * sizeof *c->groups);
	c->gcount = arena_alloc(&c->arena, 4 * sizeof *c->gcount);
	c->groups[0] = names[list][0];
	for (k = 1; k < 4; k++)
		if (cnt[k]) {
			gk[k] = ng;
			c->groups[ng] = names[list][k];
			c->gcount[ng++] = cnt[k];
		}
	c->ngroups = ng;
	for (i = 0; i < n; i++) {
		if (strcmp(m[i].src, P->src->name))
			continue;
		it = &c->items[c->nitems++];
		blank(it);
		it->name = m[i].name;
		it->logo = m[i].logo;
		it->ext = m[i].ext;
		it->sname = m[i].sname;
		it->id = m[i].id;
		it->kind = m[i].kind;
		it->series = m[i].series;
		it->live = it->kind == KindLive;
		it->group = gk[MIN(it->kind, KindSeries) + 1];
		if (P->src->type != SrcXtream && it->kind == KindLive)
			it->url = staticurl(it->id);
		if (list == ListRecent) {
			fmtago(ago, sizeof ago, m[i].time);
			it->date = arena_strdup(&c->arena, ago);
		}
	}
	c->gcount[0] = c->nitems;
	c->loaded = 1;
	marksver[list] = store_version();
	markssrc[list] = cur;
}

/* rebuild the favorites or recent page when its list changed */
static void
freshmarks(void)
{
	char gname[256];
	int list = tab - PageFav, id;

	if (inseries || tab < PageFav || (marksver[list] == store_version() && markssrc[list] == cur))
		return;
	remember(gname, sizeof gname, &id);
	buildmarks(list);
	restore(gname, id);
}

static void
usesource(int i)
{
	Playlist *p = &lists[i];
	int k;

	cur = i;
	tab = PageLive;
	inseries = 0;
	xt_free(&eps);
	epsid = -1;
	for (k = 0; k < PageLast; k++) {
		ui_list_reset(&gls[k], 0);
		keep[k] = -1;
	}
	if (p->src->type == SrcXtream) {
		if (!p->auth && !p->authloading)
			xt_account(p, cachettl, 0);
		if (!p->cat[KindLive].loaded && !p->cat[KindLive].loading)
			xt_load(p, KindLive, cachettl, 0);
	} else if (p->src->type == SrcM3u) {
		if (!p->cat[KindLive].loaded && !p->cat[KindLive].loading)
			m3u_load(p, cachettl, 0);
	} else if (!p->cat[KindLive].loaded) {
		loadstatic(p);
	}
	filter(-1);
	pane = 1;
}

static void
settab(int k)
{
	Catalog *c;

	if ((k == tab && !inseries) || !tabok(k))
		return;
	keep[PG] = selid();
	inseries = 0;
	tab = k;
	if (k < PageFav) {
		c = &P->cat[k];
		if (!c->loaded && !c->loading)
			xt_load(P, k, cachettl, 0);
	} else {
		buildmarks(k - PageFav);
	}
	filter(keep[k]);
	pane = 1;
}

static void
openseries(int id, const char *name, const char *cover)
{
	if (tab == PageSeries)
		keep[PageSeries] = selid();
	if (epsid != id) {
		xt_free(&eps);
		epsid = id;
		snprintf(epsname, sizeof epsname, "%s", name);
		snprintf(epscover, sizeof epscover, "%s", cover);
		eps.loading = 1;
		xt_info(P, KindSeries, id);
		ui_list_reset(&gls[PageEpisodes], 0);
		keep[PageEpisodes] = -1;
	}
	inseries = 1;
	filter(keep[PageEpisodes]);
	pane = 1;
}

static void
closeseries(void)
{
	keep[PageEpisodes] = selid();
	inseries = 0;
	filter(keep[tab]);
}

static const JVal *
vodinfo(int id, int fetch)
{
	Info *in;
	size_t i;

	for (i = 0; i < LEN(infos); i++)
		if (infos[i].p == P && infos[i].id == id)
			return infos[i].j ? json_get(infos[i].j->root, "info") : NULL;
	if (!fetch || P->src->type != SrcXtream)
		return NULL;
	in = &infos[ninfos++ % LEN(infos)];
	net_free(in->j);
	in->p = P;
	in->id = id;
	in->j = NULL;
	xt_info(P, KindVod, id);
	return NULL;
}

static void
oninfo(Job *j)
{
	char gname[256];
	int id = -1, first, loading;
	size_t i;

	if (j->sub == KindVod) {
		for (i = 0; i < LEN(infos); i++)
			if (infos[i].p == j->ud && infos[i].id == j->id && j->root && !j->err[0]) {
				net_free(infos[i].j);
				infos[i].j = j;
				return;
			}
		net_free(j);
		return;
	}
	if (j->ud != P || j->id != epsid) {
		net_free(j);    /* the series was left meanwhile */
		return;
	}
	loading = eps.loading - !j->stale;
	if (j->err[0] || !j->root) {
		eps.loading = loading;
		if (!eps.loaded)
			snprintf(eps.err, sizeof eps.err, "%s", j->err[0] ? j->err : "no data");
		net_free(j);
		return;
	}
	first = !eps.loaded;
	if (inseries)
		remember(gname, sizeof gname, &id);
	xt_episodes(&eps, j, epsid, epsname);
	eps.loading = loading;
	if (first) {
		/* open on the first season rather than all of them */
		ui_list_reset(&gls[PageEpisodes], eps.ngroups);
		gls[PageEpisodes].sel = eps.ngroups > 1;
		gls[PageEpisodes].pill = gls[PageEpisodes].sel * rowh * S;
		if (inseries)
			filter(-1);
	} else if (inseries) {
		restore(gname, id);
	}
}

static int lower(int c);
static void runsearch(void);

/* a network job finished; rebuild views without losing the selection */
static void
onjob(Job *j)
{
	Playlist *p = j->ud;
	char gname[256] = "";
	int k, id = -1, here;

	if (j->owner == JobImg) {
		img_result(j);
		return;
	}
	if (j->owner == JobXml) {
		epg_xml_result(j);
		return;
	}
	if (j->kind == ResInfo) {
		oninfo(j);
		return;
	}
	if (j->kind == ResEpg) {
		epg_result(j);
		return;
	}
	here = p == P && j->kind != ResAuth && j->sub == tab && !inseries;
	if (here)
		remember(gname, sizeof gname, &id);
	if ((k = j->owner == JobM3u ? m3u_result(p, j) : xt_result(p, j)) < 0 || p != P)
		return;
	if (here)
		restore(gname, id);
	else
		ui_list_reset(&gls[k], p->cat[k].ngroups);
	if (searching)
		runsearch();
}

static int
playing(const Item *it)
{
	if (playsrc != cur)
		return 0;
	if (it->kind == KindSeries)
		return playkind == KindEpisode && playseries == it->id;
	return playkind == it->kind && playid == it->id &&
	       ((it->kind != KindEpisode && it->kind != KindProg) || playseries == it->series);
}

/* remember how far into a movie or episode we are */
static void
saveprogress(int flush)
{
	if (playsrc >= 0 && (playkind == KindVod || playkind == KindEpisode) && pl.loaded)
		store_set(srcs[playsrc].name, playkind, playid, pl.ended ? pl.dur : pl.pos, pl.dur);
	if (flush) {
		store_save();
		lastflush = t;
	}
}

/* episode titles often repeat the series name: "Show - S01E02 - Title" */
static const char *
eptitle(const char *name, const char *sname)
{
	size_t n = strlen(sname);

	if (n && !strncmp(name, sname, n) && name[n]) {
		for (name += n; *name == ' ' || *name == '-' || *name == ':'; name++)
			;
		if (*name)
			return name;
	}
	return name;
}

static void
playitem(const Item *it)
{
	Catalog *c = catalog();
	char url[2048], enc[256], ts[32];
	const char *u = it->url;
	double start = 0, dur;
	Mark m;

	saveprogress(1);
	playsrc = cur;
	playkind = it->kind;
	playid = it->id;
	playlive = it->live;
	playseries = it->kind == KindEpisode ? it->series : -1;
	if (it->kind == KindEpisode) {
		snprintf(playname, sizeof playname, "%s", eptitle(it->name, it->sname));
		snprintf(playgroup, sizeof playgroup, "%s", it->sname);
		if (inseries)
			append(playgroup, sizeof playgroup, " \xc2\xb7 %s", c->groups[it->group]);
	} else {
		snprintf(playname, sizeof playname, "%s", it->name);
		snprintf(playgroup, sizeof playgroup, "%s", PG < PageFav ? c->groups[it->group] :
		         inguide ? guidegroup : kindname(it->kind));
	}
	if (it->kind == KindVod || it->kind == KindEpisode) {
		start = store_get(P->src->name, it->kind, it->id, &dur);
		if (start < 30 || start >= dur - 1)    /* barely started or watched */
			start = 0;
		if (start > 0) {
			fmttime(ts, sizeof ts, start);
			osd("Resuming at %s", ts);
		}
	}

	m.src = (char *)P->src->name;
	m.name = (char *)it->name;
	m.logo = (char *)it->logo;
	m.ext = (char *)it->ext;
	m.sname = (char *)it->sname;
	m.kind = it->kind;
	m.id = it->id;
	m.series = it->kind == KindEpisode ? it->series : 0;
	store_add(ListRecent, &m);

	if (!u && P->src->type != SrcXtream)
		u = staticurl(it->id);
	if (!u) {
		player_mask(P->src->pass);
		urlenc(enc, sizeof enc, P->src->pass);
		player_mask(enc);
		xt_url(P, it->kind, it, url, sizeof url);
		u = url;
	}
	player_load(u, start);
}

/* rebuild the guide page when its Epg changed; the first time, open on
 * today and what is on now */
static void
freshguide(void)
{
	const Prog *now;
	char gname[256];
	int id, today, first;

	if (!inguide || !gepg || gepg->gen == guidegen)
		return;
	first = !guidecat.nitems;
	if (!first)
		remember(gname, sizeof gname, &id);
	today = epg_catalog(gepg, &guidecat, guidech.id, guidech.archive, time(NULL));
	guidecat.loading = gepg->loading;
	guidegen = gepg->gen;
	if (!first) {
		restore(gname, id);
		return;
	}
	ui_list_reset(&glist, guidecat.ngroups);
	glist.sel = today;
	glist.pill = today * rowh * S;
	now = epg_now(gepg, time(NULL), NULL);
	filter(now ? (int)now->start : -1);
}

static void
openguide(const Item *ch)
{
	if (!hasguide(P) || ch->kind != KindLive) {
		osd("No guide for this channel");
		return;
	}
	keep[PG] = selid();
	guidech = *ch;
	snprintf(guidename, sizeof guidename, "%s", ch->name);
	snprintf(guidelogo, sizeof guidelogo, "%s", ch->logo ? ch->logo : "");
	snprintf(guidegroup, sizeof guidegroup, "%s",
	         tab == PageLive && ch->group < P->cat[KindLive].ngroups ?
	         P->cat[KindLive].groups[ch->group] : "Channel");
	guidech.name = guidename;
	guidech.logo = guidelogo;
	if (gepg)
		gepg->pin = 0;
	if ((gepg = epg_want(P, ch->id, 1)))
		gepg->pin = 1;
	xt_free(&guidecat);
	guidecat.loading = 1;
	guidegen = -1;
	inguide = 1;
	ui_list_reset(&gls[PageGuide], 0);
	filter(-1);
	freshguide();
	pane = 1;
}

static void
closeguide(void)
{
	inguide = 0;
	if (gepg)
		gepg->pin = 0;
	gepg = NULL;
	filter(keep[tab]);
}

/* a recording from the guide, played like a movie */
static void
playcatchup(const Prog *pg)
{
	char url[2048], enc[256], when[64], hm[16];
	time_t tt = (time_t)pg->start;

	saveprogress(1);
	playsrc = cur;
	playkind = KindProg;
	playid = (int)pg->start;
	playseries = guidech.id;
	playlive = 0;
	epg_day(pg->start, time(NULL), when, sizeof when);
	strftime(hm, sizeof hm, "%H:%M", localtime(&tt));
	snprintf(playname, sizeof playname, "%s", pg->title);
	snprintf(playgroup, sizeof playgroup, "%s \xc2\xb7 %s %s", guidech.name, when, hm);
	player_mask(P->src->pass);
	urlenc(enc, sizeof enc, P->src->pass);
	player_mask(enc);
	xt_catchup(P, guidech.id, pg->local, pg->start, (int)((pg->stop - pg->start + 59) / 60),
	           url, sizeof url);
	player_load(url, 0);
	view = ViewPlayer;
}

/* Enter on an item, wherever it is listed */
static void
activate(const Item *it)
{
	const Prog *pg;
	long long t1 = time(NULL);

	if (it->kind == KindProg) {
		pg = epg_prog(gepg, it->id);
		if (pg && pg->start <= t1 && t1 < pg->stop)
			activate(&guidech);    /* on now: the channel itself */
		else if (pg && it->archive)
			playcatchup(pg);
		else
			osd(pg && pg->start > t1 ? "Not on yet" : "No recording of this one");
		return;
	}
	if (it->kind == KindSeries) {
		openseries(it->id, it->name, it->logo);
	} else if (playing(it)) {
		view = ViewPlayer;
	} else {
		playitem(it);
		if (it->kind != KindLive)
			view = ViewPlayer;
	}
}

static List *
curlist(void)
{
	return pane ? &clist : &glist;
}

static void
back(const Arg *a)
{
	if (view == ViewPlayer) {
		view = ViewBrowse;
		if (fs)
			fullscreen(a);
	} else if (fs) {
		fullscreen(a);
	} else if (inseries) {
		closeseries();
	} else if (inguide) {
		closeguide();
	} else {
		pane = 0;
	}
}

static void
cycle(const Arg *a)
{
	const char *prop = a->i ? "sid" : "aid";
	char buf[64];

	player_cycle(prop);
	osd("%s track: %s", a->i ? "Subtitle" : "Audio", player_prop(prop, buf, sizeof buf));
}

static void
favorite(const Arg *a)
{
	const Item *it = focused();
	Mark *old, m;

	(void)a;
	if (!pane || !it)
		return;
	if (it->kind == KindEpisode) {
		osd("Favorite the series instead");
		return;
	}
	if ((old = store_find(ListFav, P->src->name, it->kind, it->id, 0))) {
		osd("Removed %s from favorites", it->name);
		store_del(ListFav, old);
		return;
	}
	m.src = (char *)P->src->name;
	m.name = (char *)it->name;
	m.logo = (char *)it->logo;
	m.ext = (char *)it->ext;
	m.sname = "";
	m.kind = it->kind;
	m.id = it->id;
	m.series = 0;
	store_add(ListFav, &m);
	osd("Added %s to favorites", it->name);
}

/* left/right: between panes, or within a row of posters */
static void
focus(const Arg *a)
{
	if (pane == 1 && isgrid()) {
		if (a->i < 0 && clist.sel % gcols == 0)
			pane = 0;
		else if (a->i < 0 || (clist.sel % gcols < gcols - 1 && clist.sel < nvis - 1))
			ui_list_move(&clist, a->i);
		return;
	}
	pane = CLAMP(pane + a->i, 0, 1);
}

static void
fullscreen(const Arg *a)
{
	(void)a;
	fs = !fs;
	SDL_SetWindowFullscreen(win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	if (fs && playsrc >= 0)
		view = ViewPlayer;
}

/* the guide of the focused channel, or of the one playing; again to close */
static void
guide(const Arg *a)
{
	const Item *it = focused();
	Catalog *c = &P->cat[KindLive];
	int i;

	(void)a;
	if (view == ViewPlayer) {
		if (playsrc != cur || playkind != KindLive)
			return;
		view = ViewBrowse;
		for (i = 0; i < c->nitems; i++)
			if (c->items[i].id == playid) {
				openguide(&c->items[i]);
				return;
			}
		return;
	}
	if (inguide)
		closeguide();
	else if (it && !inseries)
		openguide(it);
}

static void
jump(const Arg *a)
{
	List *l = curlist();

	ui_list_move(l, a->i ? l->n : -l->n);
	if (!pane)
		filter(-1);
}

static void
kind(const Arg *a)
{
	int k = tab, i;

	for (i = 0; i < PageFav + 2; i++) {
		k = (k + a->i + PageFav + 2) % (PageFav + 2);
		if (tabok(k))
			break;
	}
	settab(k);
}

static void
move(const Arg *a)
{
	ui_list_move(curlist(), pane && isgrid() ? a->i * gcols : a->i);
	if (!pane)
		filter(-1);
}

static void
mute(const Arg *a)
{
	(void)a;
	player_cycle("mute");
	osd("%s", pl.mute ? "Unmuted" : "Muted");
}

static void
page(const Arg *a)
{
	int rows = pane && isgrid() ? MAX(1, (int)(gridr.h / (gcellh + ggap))) * gcols
	                            : MAX(1, (int)(clr.h / (rowh * S)) - 1);

	ui_list_move(curlist(), a->i * rows);
	if (!pane)
		filter(-1);
}

static void
play(const Arg *a)
{
	(void)a;
	if (!pane) {
		pane = 1;
		return;
	}
	if (nvis)
		activate(focused());
}

static void
quit(const Arg *a)
{
	(void)a;
	running = 0;
}

static void
refresh(const Arg *a)
{
	(void)a;
	if (tab >= PageFav || P->cat[tab].loading)
		return;
	if (P->src->type == SrcM3u) {
		m3u_load(P, cachettl, 1);
	} else if (P->src->type == SrcXtream) {
		xt_account(P, cachettl, 1);
		xt_load(P, tab, cachettl, 1);
	} else {
		return;
	}
	osd("Refreshing %s", P->src->name);
}

static void
search(const Arg *a)
{
	int k;

	(void)a;
	searching = skipslash = 1;
	query[0] = '\0';
	SDL_StartTextInput();
	/* search everything, so load what was not opened yet */
	for (k = KindVod; hastabs() && k <= KindSeries; k++)
		if (!P->cat[k].loaded && !P->cat[k].loading)
			xt_load(P, k, cachettl, 0);
	runsearch();
}

static void
seek(const Arg *a)
{
	if (seekable())
		player_seek(a->f);
}

static void
source(const Arg *a)
{
	if (a->i >= nsrcs || a->i == cur)
		return;
	usesource(a->i);
	osd("%s", P->src->name);
}

static void
stop(const Arg *a)
{
	(void)a;
	saveprogress(1);
	player_stop();
	playsrc = -1;
	view = ViewBrowse;
}

static void
togglepause(const Arg *a)
{
	(void)a;
	if (playsrc >= 0)
		player_cycle("pause");
}

static void
volume(const Arg *a)
{
	double v = CLAMP(pl.volume + a->f, 0, 130);

	player_setvol(v);
	osd("Volume %.0f%%", v);
}

static void
zap(const Arg *a)
{
	Catalog *c = catalog();
	int i, pos = -1, n;

	if (!nvis || inguide)
		return;
	for (i = 0; i < nvis; i++)
		if (playing(&c->items[vis[i]]) && c->items[vis[i]].kind != KindSeries)
			pos = i;
	if (pos < 0) {
		pos = clist.sel;
	} else {
		/* step over series, which do not play */
		for (n = 0; n < nvis; n++) {
			pos = (pos + a->i + nvis) % nvis;
			if (c->items[vis[pos]].kind != KindSeries)
				break;
		}
	}
	if (c->items[vis[pos]].kind == KindSeries)
		return;
	clist.sel = pos;
	clist.follow = 1;
	playitem(&c->items[vis[pos]]);
}

static int
seekable(void)
{
	return playsrc >= 0 && !playlive && pl.dur > 0;
}

/* search: a subsequence match of q in s, ASCII case-insensitive. -1 for
 * none; more for substrings, word starts and runs, less for long names.
 * hit[i] is set for matched bytes */
static int
lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int
fuzzy(const char *s, const char *q0, unsigned char *hit, size_t nhit)
{
	const char *q;
	int score = 0, run = 0, prev = -2;
	size_t i = 0, k, len = strlen(s), qn = strlen(q0);

	for (q = q0; *q; q++) {
		while (s[i] && lower((unsigned char)s[i]) != lower((unsigned char)*q))
			i++;
		if (!s[i])
			return -1;
		score++;
		if ((int)i == prev + 1)
			score += 4 + run++;
		else
			run = 0;
		if (i == 0 || strchr(" -_:([/|.", s[i - 1]))
			score += 6;
		if (hit && i < nhit)
			hit[i] = 1;
		prev = (int)i++;
	}
	for (i = 0; i + qn <= len; i++) {
		for (k = 0; k < qn && lower((unsigned char)s[i + k]) == lower((unsigned char)q0[k]); k++)
			;
		if (k == qn) {
			score += i == 0 ? 30 : 20;
			break;
		}
	}
	return score - (int)(len / 8);
}

static void
runsearch(void)
{
	static int score[MAXFOUND];
	Catalog *c;
	const Item *it;
	int k, i, n = 0, sc, j;

	xt_free(&found);
	found.items = arena_alloc(&found.arena, MAXFOUND * sizeof *found.items);
	if (*query) {
		for (k = KindLive; k <= KindSeries; k++) {
			c = &P->cat[k];
			for (i = 0; i < c->nitems; i++) {
				it = &c->items[i];
				if ((sc = fuzzy(it->name, query, NULL, 0)) < 0)
					continue;
				if (n == MAXFOUND && sc <= score[n - 1])
					continue;
				/* insertion into the sorted top list */
				for (j = MIN(n, MAXFOUND - 1); j > 0 && score[j - 1] < sc; j--) {
					score[j] = score[j - 1];
					found.items[j] = found.items[j - 1];
				}
				score[j] = sc;
				found.items[j] = *it;
				n = MIN(n + 1, MAXFOUND);
			}
		}
	}
	found.nitems = n;
	ui_list_reset(&slist, n);
}

static void
closesearch(void)
{
	searching = 0;
	SDL_StopTextInput();
}

/* show the chosen result in its tab and play or open it */
static void
choose(void)
{
	Item it;

	if (!found.nitems)
		return;
	it = found.items[slist.sel];
	closesearch();
	settab(it.kind);
	if (tab != it.kind)
		return;
	glist.sel = 0;
	filter(it.id);
	activate(&it);
}

static void
searchkey(const SDL_Keysym *k)
{
	size_t n = strlen(query);

	if (k->sym == SDLK_v && (k->mod & KMOD_CTRL)) {
		char *t = SDL_GetClipboardText();
		if (t) {
			n = strlen(query);
			snprintf(query + n, sizeof query - n, "%s", t);
			SDL_free(t);
			runsearch();
		}
		return;
	}
	switch (k->sym) {
	case SDLK_ESCAPE:
		closesearch();
		break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		choose();
		break;
	case SDLK_UP:
		ui_list_move(&slist, -1);
		break;
	case SDLK_DOWN:
		ui_list_move(&slist, 1);
		break;
	case SDLK_PAGEUP:
		ui_list_move(&slist, -8);
		break;
	case SDLK_PAGEDOWN:
		ui_list_move(&slist, 8);
		break;
	case SDLK_BACKSPACE:
		if (k->mod & KMOD_CTRL)
			n = 0;
		else    /* a whole UTF-8 sequence */
			while (n > 0 && (query[--n] & 0xc0) == 0x80)
				;
		query[n] = '\0';
		runsearch();
		break;
	case SDLK_u:
		if (k->mod & KMOD_CTRL) {
			query[0] = '\0';
			runsearch();
		}
		break;
	}
}

static int
digitkey(SDL_Keycode sym)
{
	if (sym >= SDLK_0 && sym <= SDLK_9)
		return sym - SDLK_0;
	if (sym == SDLK_KP_0)
		return 0;
	if (sym >= SDLK_KP_1 && sym <= SDLK_KP_9)
		return sym - SDLK_KP_1 + 1;
	return -1;
}

/* a digit from keys[]: start number entry on Live, or append to it */
static void
number(const Arg *a)
{
	if (a->i < 0 || a->i > 9)
		return;
	if (!numbering) {
		if (view != ViewBrowse || tab != PageLive || inseries || inguide || searching)
			return;
		numbering = 1;
		numberbuf[0] = '\0';
	}
	if (strlen(numberbuf) >= 4)
		return;
	append(numberbuf, sizeof numberbuf, "%d", a->i);
}

/* tune the number typed so far; out of range just cancels */
static void
tune(void)
{
	Catalog *c = catalog();
	int n = atoi(numberbuf);

	numbering = 0;
	numberbuf[0] = '\0';
	if (n < 1 || n > nvis) {
		osd("No channel %d", n);
		return;
	}
	pane = 1;
	clist.sel = n - 1;
	clist.follow = 1;
	playitem(&c->items[vis[clist.sel]]);
}

/* keys that steer number entry; digits append through number() */
static void
numberkey(const SDL_Keysym *k)
{
	size_t n = strlen(numberbuf);
	int d = digitkey(k->sym);

	if (d >= 0) {
		number(&(Arg){ .i = d });
		return;
	}
	switch (k->sym) {
	case SDLK_ESCAPE:
		numbering = 0;
		break;
	case SDLK_BACKSPACE:
		if (n > 0)
			numberbuf[n - 1] = '\0';
		if (!numberbuf[0])
			numbering = 0;
		break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		tune();
		break;
	}
}

static const char *
status(int *c)
{
	if (pl.err[0]) {
		*c = ColLive;
		return pl.err;
	}
	*c = ColAccent;
	if (!pl.loaded)
		return "Connecting\xe2\x80\xa6";
	if (pl.buffering)
		return "Buffering\xe2\x80\xa6";
	if (pl.ended)
		return "Ended";
	*c = pl.paused ? ColDim : ColGood;
	return pl.paused ? "Paused" : "Playing";
}

/* y of text vertically centred in a box at y of height h */
static float
mid(int font, float y, float h)
{
	return roundf(y + (h - gfx_lineh(font)) / 2);
}

static int
hover(Rect r)
{
	return view == ViewBrowse && !searching && inrect(r, mx, my);
}

/* fill r, cropping the image to keep its aspect */
static void
drawcover(const Img *im, Rect r, float rad)
{
	float ia = (float)im->w / im->h, ra = r.w / r.h;
	Rect uv = { 0, 0, 1, 1 };

	if (ia > ra) {
		uv.w = ra / ia;
		uv.x = (1 - uv.w) / 2;
	} else {
		uv.h = ia / ra;
		uv.y = (1 - uv.h) / 2;
	}
	gfx_image_uv(im->tex, r, uv, rgba(0xffffff, 1), rad);
}

/* fit inside r, keeping the aspect */
static void
drawcontain(const Img *im, Rect r)
{
	float s = MIN(r.w / im->w, r.h / im->h), w = im->w * s, h = im->h * s;

	gfx_image(im->tex, (Rect){ roundf(r.x + (r.w - w) / 2), roundf(r.y + (r.h - h) / 2), w, h },
	          rgba(0xffffff, 1), 0);
}

/* stand-in for a missing poster: the first letters of two words */
static void
drawinitials(const char *name, Rect r)
{
	char buf[16] = "";
	const char *p = name, *q;
	size_t n = 0;
	int words = 0;

	while (*p && words < 2) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		q = p;
		utf8dec(&q);
		if (n + (q - p) < sizeof buf) {
			memcpy(buf + n, p, q - p);
			n += q - p;
			buf[n] = '\0';
		}
		words++;
		while (*p && *p != ' ')
			p++;
	}
	gfx_text(FontTitle, roundf(r.x + (r.w - gfx_textw(FontTitle, buf)) / 2),
	         mid(FontTitle, r.y, r.h), buf, col(ColDim, 1), 0);
}

/* a small label at the right of a row; returns the new right edge */
static float
rlabel(float right, Rect r, const char *s, int c)
{
	float w;

	if (!*s)
		return right;
	w = MIN(gfx_textw(FontSmall, s), r.w * 0.35f);
	gfx_text(FontSmall, right - w, mid(FontSmall, r.y, r.h), s, col(c, 1), w);
	return right - w - 16 * S;
}

static void
rowgroup(int i, Rect r, int sel)
{
	Catalog *c = catalog();
	char n[16];
	float right = r.x + r.w - 14 * S;

	if (!sel && hover(r))
		gfx_rect(r, col(ColSel, 0.45f), radius * S);
	fmtcount(n, sizeof n, c->gcount[i]);
	right -= gfx_textw(FontSmall, n);
	gfx_text(FontSmall, right, mid(FontSmall, r.y, r.h), n, col(ColDim, 1), 0);
	gfx_text(sel ? FontBold : FontText, r.x + 14 * S, mid(FontText, r.y, r.h), c->groups[i],
	         col(sel ? ColFg : ColDim, 1), right - r.x - 26 * S);
}

/* channels, favorites and recent: number, logo, name, labels */
static void
rowitem(const Item *it, int num, Rect r, int sel, int haslogos, int page)
{
	int on = playing(it);
	float x = r.x + 12 * S, right = r.x + r.w - 14 * S, ls = 26 * S, frac;
	char n[16], buf[300];
	const Prog *pg;
	double pos, dur;
	Epg *e;
	Img *im;

	if (!sel && hover(r))
		gfx_rect(r, col(ColSel, 0.45f), radius * S);
	snprintf(n, sizeof n, "%d", num);
	gfx_text(FontSmall, x + 26 * S - gfx_textw(FontSmall, n), mid(FontSmall, r.y, r.h),
	         n, col(ColDim, 1), 0);
	x += 40 * S;
	if (on)
		gfx_rect((Rect){ x - 11 * S, r.y + r.h / 2 - 3 * S, 6 * S, 6 * S },
		         col(ColAccent, 1), 3 * S);
	if (haslogos) {
		if ((im = img_get(it->logo, (int)ls)) && im->state == ImgReady)
			drawcontain(im, (Rect){ x, r.y + (r.h - ls) / 2, ls, ls });
		x += ls + 12 * S;
	}

	if (page == PageRecent)
		right = rlabel(right, r, it->date, ColDim);
	if (page >= PageFav) {
		snprintf(buf, sizeof buf, "%s", it->kind == KindEpisode ? it->sname : kindname(it->kind));
		right = rlabel(right, r, buf, ColDim);
	} else if (glist.sel == 0 && page != PageFav) {
		right = rlabel(right, r, catalog()->groups[it->group], ColDim);
	}
	if ((it->kind == KindVod || it->kind == KindEpisode) &&
	    (pos = store_get(P->src->name, it->kind, it->id, &dur)) > 0 && pos >= dur)
		right = rlabel(right, r, "Watched", ColGood);
	if (page != PageFav && isfav(it))
		right = rlabel(right, r, "\xe2\x98\x85", ColAccent);

	/* what is on, once its guide is known */
	if (it->kind == KindLive && (e = epg_find(P, it->id)) &&
	    (pg = epg_now(e, (long long)time(NULL), NULL))) {
		x = gfx_text(on ? FontBold : FontText, x, mid(FontText, r.y, r.h), it->name,
		             col(on ? ColAccent : ColFg, 1), (right - x) * 0.55f);
		gfx_text(FontSmall, x + 12 * S, mid(FontSmall, r.y, r.h), pg->title, col(ColDim, 1),
		         right - x - 12 * S);
		frac = (float)(time(NULL) - pg->start) / (pg->stop - pg->start);
		gfx_rect((Rect){ x + 12 * S, r.y + r.h - 6 * S, (right - x - 12 * S) * frac, 2 * S },
		         col(ColDim, 0.5f), S);
		return;
	}
	gfx_text(on ? FontBold : FontText, x, mid(FontText, r.y, r.h),
	         it->kind == KindEpisode ? eptitle(it->name, it->sname) : it->name,
	         col(on ? ColAccent : ColFg, 1), right - x);
	if ((it->kind == KindVod || it->kind == KindEpisode) &&
	    (pos = store_get(P->src->name, it->kind, it->id, &dur)) > 0 && pos < dur) {
		gfx_rect((Rect){ x, r.y + r.h - 6 * S, right - x, 3 * S }, col(ColSel, 1), 1.5f * S);
		gfx_rect((Rect){ x, r.y + r.h - 6 * S, (right - x) * pos / dur, 3 * S },
		         col(ColAccent, 1), 1.5f * S);
	}
}

static void
rowchan(int i, Rect r, int sel)
{
	rowitem(&catalog()->items[vis[i]], i + 1, r, sel, logos, PG);
}

/* episode length: "duration" text, else duration_secs */
static void
eplength(const JVal *info, char *buf, size_t n)
{
	long long secs = json_int(json_get(info, "duration_secs"), 0);

	if (secs > 0)
		fmtdur(buf, n, secs);
	else
		snprintf(buf, n, "%s", json_str(json_get(info, "duration")));
}

static void
rowep(int i, Rect r, int sel)
{
	const Item *it = &eps.items[vis[i]];
	int on = playing(it);
	float x = r.x + 12 * S, right = r.x + r.w - 14 * S;
	char n[24], d[32];
	double pos, dur;

	if (!sel && hover(r))
		gfx_rect(r, col(ColSel, 0.45f), radius * S);
	snprintf(n, sizeof n, "%lld", json_int(json_get(it->raw, "episode_num"), i + 1));
	gfx_text(FontSmall, x + 26 * S - gfx_textw(FontSmall, n), mid(FontSmall, r.y, r.h),
	         n, col(ColDim, 1), 0);
	x += 40 * S;
	if (on)
		gfx_rect((Rect){ x - 11 * S, r.y + r.h / 2 - 3 * S, 6 * S, 6 * S },
		         col(ColAccent, 1), 3 * S);
	pos = store_get(P->src->name, KindEpisode, it->id, &dur);
	if (pos > 0 && pos >= dur)
		right = rlabel(right, r, "Watched", ColGood);
	eplength(json_get(it->raw, "info"), d, sizeof d);
	right = rlabel(right, r, d, ColDim);
	gfx_text(on ? FontBold : FontText, x, mid(FontText, r.y, r.h), eptitle(it->name, it->sname),
	         col(on ? ColAccent : ColFg, 1), right - x);
	if (pos > 0 && pos < dur) {
		gfx_rect((Rect){ x, r.y + r.h - 6 * S, right - x, 3 * S }, col(ColSel, 1), 1.5f * S);
		gfx_rect((Rect){ x, r.y + r.h - 6 * S, (right - x) * pos / dur, 3 * S },
		         col(ColAccent, 1), 1.5f * S);
	}
}

/* first 19xx or 20xx standing alone in s */
static int
year(const char *s, char *out)
{
	const char *p;

	for (p = s; *p; p++)
		if (((p[0] == '1' && p[1] == '9') || (p[0] == '2' && p[1] == '0')) &&
		    isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) &&
		    !isdigit((unsigned char)p[4]) && (p == s || !isdigit((unsigned char)p[-1]))) {
			memcpy(out, p, 4);
			out[4] = '\0';
			return 1;
		}
	return 0;
}

/* a small pill with text, top-left or top-right of a poster */
static float
badge(float x, float y, const char *s, int bg, int fg, float a)
{
	float w = gfx_textw(FontSmall, s) + 12 * S;
	Rect b = { x, y, w, 20 * S };

	gfx_rect(b, col(bg, a), 5 * S);
	gfx_text(FontSmall, b.x + 6 * S, mid(FontSmall, b.y, b.h), s, col(fg, 1), 0);
	return w;
}

static void
cellposter(int i, Rect r, int sel)
{
	Catalog *c = catalog();
	const Item *it = &c->items[vis[i]];
	Rect pr = { r.x, r.y, r.w, roundf(r.w * 1.5f) }, b;
	float y = pr.y + pr.h + 8 * S, x = pr.x + 6 * S, rad = radius * S;
	char buf[32];
	double pos, dur;
	Img *im;

	if (sel)
		gfx_rect((Rect){ pr.x - 3 * S, pr.y - 3 * S, pr.w + 6 * S, pr.h + 6 * S },
		         col(pane ? ColAccent : ColDim, 1), rad + 3 * S);
	gfx_rect(pr, col(ColPanel, 1), rad);
	if ((im = img_get(it->logo, (int)pr.w)) && im->state == ImgReady)
		drawcover(im, pr, rad);
	else
		drawinitials(it->name, pr);
	if (!sel && hover(pr))
		gfx_rect(pr, rgba(0xffffff, 0.06f), rad);

	if (atof(it->rating) > 0) {
		snprintf(buf, sizeof buf, "%.1f", atof(it->rating));
		badge(pr.x + pr.w - gfx_textw(FontSmall, buf) - 18 * S, pr.y + 6 * S, buf, ColBg, ColFg, 0.85f);
	}
	if (playing(it))
		x += badge(x, pr.y + 6 * S, "Playing", ColAccent, ColBg, 1) + 4 * S;
	if (isfav(it))
		badge(x, pr.y + 6 * S, "\xe2\x98\x85", ColBg, ColAccent, 0.85f);
	if (it->kind == KindVod && (pos = store_get(P->src->name, KindVod, it->id, &dur)) > 0) {
		if (pos >= dur) {
			badge(pr.x + 6 * S, pr.y + pr.h - 26 * S, "Watched", ColBg, ColGood, 0.85f);
		} else {
			b = (Rect){ pr.x + 8 * S, pr.y + pr.h - 10 * S, pr.w - 16 * S, 4 * S };
			gfx_rect(b, col(ColBg, 0.85f), 2 * S);
			b.w *= pos / dur;
			gfx_rect(b, col(ColAccent, 1), 2 * S);
		}
	}

	gfx_text(sel ? FontBold : FontText, r.x, y, it->name, col(ColFg, 1), r.w);
	y += gfx_lineh(FontText) + 2 * S;
	if (year(it->date, buf) || year(it->name, buf))
		gfx_text(FontSmall, r.x, y, buf, col(ColDim, 1), r.w);
}

static void
progress(float x, float y, float w)
{
	char a[32], b[32], buf[80];
	float frac = pl.dur > 0 ? CLAMP(pl.pos / pl.dur, 0, 1) : 0;

	gfx_rect((Rect){ x, y, w, 4 * S }, col(ColSel, 1), 2 * S);
	gfx_rect((Rect){ x, y, w * frac, 4 * S }, col(ColAccent, 1), 2 * S);
	fmttime(a, sizeof a, pl.pos);
	fmttime(b, sizeof b, pl.dur);
	snprintf(buf, sizeof buf, "%s / %s", a, b);
	gfx_text(FontSmall, x, y + 10 * S, buf, col(ColDim, 1), 0);
}

static void
rowprog(int i, Rect r, int sel)
{
	const Item *it = &guidecat.items[vis[i]];
	const Prog *pg = epg_prog(gepg, it->id);
	long long t1 = time(NULL);
	int live = pg && pg->start <= t1 && t1 < pg->stop, past = pg && pg->stop <= t1;
	int on = playing(it) || (live && playsrc == cur && playkind == KindLive && playid == guidech.id);
	float x = r.x + 12 * S, right = r.x + r.w - 14 * S;
	char d[32];

	if (!sel && hover(r))
		gfx_rect(r, col(ColSel, 0.45f), radius * S);
	gfx_text(FontSmall, x + 40 * S - gfx_textw(FontSmall, it->date), mid(FontSmall, r.y, r.h),
	         it->date, col(live ? ColAccent : ColDim, 1), 0);
	x += 54 * S;
	if (on)
		gfx_rect((Rect){ x - 11 * S, r.y + r.h / 2 - 3 * S, 6 * S, 6 * S },
		         col(ColAccent, 1), 3 * S);
	if (pg) {
		fmtdur(d, sizeof d, pg->stop - pg->start);
		right = rlabel(right, r, d, ColDim);
	}
	if (live)
		right = rlabel(right, r, "Now", ColLive);
	else if (it->archive)
		right = rlabel(right, r, "Replay", ColAccent);
	gfx_text(live || on ? FontBold : FontText, x, mid(FontText, r.y, r.h), it->name,
	         col(on ? ColAccent : past && !it->archive ? ColDim : ColFg, 1), right - x);
	if (live) {
		gfx_rect((Rect){ x, r.y + r.h - 6 * S, right - x, 3 * S }, col(ColSel, 1), 1.5f * S);
		gfx_rect((Rect){ x, r.y + r.h - 6 * S,
		                 (right - x) * (float)(t1 - pg->start) / (pg->stop - pg->start), 3 * S },
		         col(ColLive, 1), 1.5f * S);
	}
}

/* "Now 21:00 - 22:00", the title, a progress bar and what comes next;
 * returns the y below it */
static float
drawnow(const Epg *e, float x, float y, float w)
{
	const Prog *pg, *next;
	long long t1 = time(NULL);
	char buf[300], a[16], b[16];
	time_t ta, tb;

	if (!(pg = epg_now(e, t1, &next)))
		return y;
	ta = (time_t)pg->start;
	tb = (time_t)pg->stop;
	strftime(a, sizeof a, "%H:%M", localtime(&ta));
	strftime(b, sizeof b, "%H:%M", localtime(&tb));
	snprintf(buf, sizeof buf, "NOW  %s \xe2\x80\x93 %s  \xc2\xb7  %lld min left", a, b,
	         (pg->stop - t1 + 59) / 60);
	gfx_text(FontSmall, x, y, buf, col(ColAccent, 1), w);
	y += gfx_lineh(FontSmall) + 2 * S;
	y += gfx_wrap(FontBold, x, y, w, pg->title, col(ColFg, 1), 2) * gfx_lineh(FontBold) + 6 * S;
	gfx_rect((Rect){ x, y, w, 3 * S }, col(ColSel, 1), 1.5f * S);
	gfx_rect((Rect){ x, y, w * (float)(t1 - pg->start) / (pg->stop - pg->start), 3 * S },
	         col(ColAccent, 1), 1.5f * S);
	y += 12 * S;
	if (next) {
		ta = (time_t)next->start;
		strftime(a, sizeof a, "%H:%M", localtime(&ta));
		snprintf(buf, sizeof buf, "Next  %s  %s", a, next->title);
		gfx_text(FontSmall, x, y, buf, col(ColDim, 1), w);
		y += gfx_lineh(FontSmall) + 8 * S;
	}
	return y;
}

/* the right pane on the guide page */
static void
drawprog(const Item *it)
{
	const Prog *pg = it ? epg_prog(gepg, it->id) : NULL;
	long long t1 = time(NULL);
	int live = pg && pg->start <= t1 && t1 < pg->stop, n;
	float y, bottom = clr.y + clr.h, rad = radius * S;
	char buf[128], day[32], a[16], b[16];
	time_t ta, tb;
	Rect v = vidr;
	Img *im;

	gfx_rect(v, col(ColPanel, 1), rad);
	if (playsrc == cur && pl.loaded && ((playkind == KindLive && playid == guidech.id) ||
	    (it && playing(it))))
		gfx_image(pl.fb.tex, v, rgba(0xffffff, 1), rad);
	else if ((im = img_get(guidech.logo, (int)(v.h * 0.5f))) && im->state == ImgReady)
		drawcontain(im, (Rect){ v.x + v.w * 0.3f, v.y + v.h * 0.25f, v.w * 0.4f, v.h * 0.5f });
	else
		drawinitials(guidech.name, v);
	y = v.y + v.h + 18 * S;
	if (!pg) {
		gfx_text(FontTitle, v.x, y, guidech.name, col(ColFg, 1), v.w);
		return;
	}
	y += gfx_wrap(FontTitle, v.x, y, v.w, pg->title, col(ColFg, 1), 2) * gfx_lineh(FontTitle) + 2 * S;
	ta = (time_t)pg->start;
	tb = (time_t)pg->stop;
	epg_day(pg->start, t1, day, sizeof day);
	strftime(a, sizeof a, "%H:%M", localtime(&ta));
	strftime(b, sizeof b, "%H:%M", localtime(&tb));
	snprintf(buf, sizeof buf, "%s \xc2\xb7 %s %s \xe2\x80\x93 %s \xc2\xb7 %lld min", guidech.name,
	         day, a, b, (pg->stop - pg->start + 59) / 60);
	gfx_text(FontSmall, v.x, y, buf, col(ColDim, 1), v.w);
	y += gfx_lineh(FontSmall) + 10 * S;
	if (live) {
		gfx_rect((Rect){ v.x, y, v.w, 3 * S }, col(ColSel, 1), 1.5f * S);
		gfx_rect((Rect){ v.x, y, v.w * (float)(t1 - pg->start) / (pg->stop - pg->start), 3 * S },
		         col(ColLive, 1), 1.5f * S);
		y += 13 * S;
	}
	n = (int)((bottom - y) / gfx_lineh(FontText)) - 2;
	if (*pg->desc && n > 0)
		gfx_wrap(FontText, v.x, y, v.w, pg->desc, col(ColFg, 0.85f), n);
	gfx_text(FontSmall, v.x, bottom - gfx_lineh(FontSmall),
	         live ? "Enter: watch live" : it->archive ? "Enter: watch the recording" :
	         pg->start > t1 ? "Not on yet" : "No recording of this one",
	         col(ColDim, 0.7f), 0);
}

static void
drawpreview(void)
{
	Rect v = vidr;
	const Item *it;
	const char *s;
	float y = v.y + v.h + 20 * S;
	Epg *e;
	int c;

	if (playsrc < 0) {
		gfx_rect(v, col(ColPanel, 1), radius * S);
		s = "Pick a channel and press Enter";
		gfx_text(FontText, roundf(v.x + (v.w - gfx_textw(FontText, s)) / 2),
		         mid(FontText, v.y, v.h), s, col(ColDim, 1), 0);
		if ((it = focused()) && it->kind == KindLive && (e = epg_find(P, it->id))) {
			gfx_text(FontTitle, v.x, y, it->name, col(ColFg, 1), v.w);
			drawnow(e, v.x, y + gfx_lineh(FontTitle) + 12 * S, v.w);
		}
		return;
	}
	gfx_rect(v, rgba(0, 1), radius * S);
	if (pl.loaded)
		gfx_image(pl.fb.tex, v, rgba(0xffffff, 1), radius * S);
	gfx_text(FontTitle, v.x, y, playname, col(ColFg, 1), v.w);
	y += gfx_lineh(FontTitle) + 2 * S;
	gfx_text(FontSmall, v.x, y, playgroup, col(ColDim, 1), v.w);
	y += gfx_lineh(FontSmall) + 16 * S;
	s = status(&c);
	gfx_rect((Rect){ v.x, y + gfx_lineh(FontText) / 2 - 4 * S, 8 * S, 8 * S }, col(c, 1), 4 * S);
	gfx_text(FontText, v.x + 16 * S, y, s, col(ColFg, 1), v.w - 16 * S);
	y += gfx_lineh(FontText) + 16 * S;
	if (seekable())
		progress(v.x, y, v.w);
	else if (playkind == KindLive && hasguide(&lists[playsrc]))
		drawnow(epg_want(&lists[playsrc], playid, 0), v.x, y, v.w);
}

/* a string, or the first string of an array (backdrop_path is either) */
static const char *
firststr(const JVal *v)
{
	return v && v->type == JArr ? json_str(v->child) : json_str(v);
}

static const char *
orstr(const char *a, const char *b)
{
	return a && *a ? a : b ? b : "";
}

/* the right pane for movies, series and episodes */
static void
drawdetails(const Item *it)
{
	const JVal *info = NULL, *sinfo = NULL;
	const char *img, *title, *sub = NULL, *plot, *date, *genre, *cast, *rating;
	char meta[256] = "", buf[96], a[32], b[32], len[32] = "";
	float y, bottom = clr.y + clr.h, rad = radius * S;
	double pos = 0, dur = 0;
	int k = it ? it->kind : KindEpisode, n;
	Rect v = vidr;
	Img *im;

	if (k == KindEpisode) {
		/* the episode page knows the series; recent episodes do not */
		if (inseries && eps.res[1])
			sinfo = json_get(eps.res[1]->root, "info");
		img = orstr(firststr(json_get(sinfo, "backdrop_path")), json_str(json_get(sinfo, "cover")));
		if (inseries)
			img = orstr(img, epscover);
		img = orstr(img, it ? it->logo : NULL);
		title = it ? it->sname : epsname;
		sub = it ? eptitle(it->name, it->sname) : NULL;
		if (it)
			eplength(json_get(it->raw, "info"), len, sizeof len);
		plot = orstr(it ? it->plot : NULL, json_str(json_get(sinfo, "plot")));
		date = json_str(json_get(sinfo, "releaseDate"));
		genre = json_str(json_get(sinfo, "genre"));
		cast = json_str(json_get(sinfo, "cast"));
		rating = it ? it->rating : "";
	} else if (k == KindVod) {
		info = vodinfo(it->id, t - selt > 0.25);
		if (t - selt <= 0.25)
			wake(selt + 0.26);
		img = orstr(firststr(json_get(info, "backdrop_path")), it->logo);
		title = it->name;
		eplength(info, len, sizeof len);
		plot = orstr(json_str(json_get(info, "plot")), json_str(json_get(info, "description")));
		date = orstr(json_str(json_get(info, "releasedate")), it->date);
		genre = json_str(json_get(info, "genre"));
		cast = orstr(json_str(json_get(info, "cast")), json_str(json_get(info, "actors")));
		rating = orstr(it->rating, json_str(json_get(info, "rating")));
	} else {
		img = orstr(it->backdrop, it->logo);
		title = it->name;
		plot = it->plot;
		date = it->date;
		genre = it->genre;
		cast = it->cast;
		rating = it->rating;
	}

	gfx_rect(v, col(ColPanel, 1), rad);
	if (it && k != KindSeries && playing(it) && pl.loaded)
		gfx_image(pl.fb.tex, v, rgba(0xffffff, 1), rad);
	else if ((im = img_get(img, (int)v.w)) && im->state == ImgReady)
		drawcover(im, v, rad);
	else
		drawinitials(title, v);

	y = v.y + v.h + 18 * S;
	y += gfx_wrap(FontTitle, v.x, y, v.w, title, col(ColFg, 1), 2) * gfx_lineh(FontTitle) + 2 * S;
	if (sub)
		y += gfx_wrap(FontBold, v.x, y, v.w, sub, col(ColFg, 1), 2) * gfx_lineh(FontBold) + 2 * S;

	/* year · genre · length · rating */
	if (year(date, buf) || (k != KindEpisode && year(title, buf)))
		append(meta, sizeof meta, "%s", buf);
	if (*genre)
		append(meta, sizeof meta, "%s%s", *meta ? " \xc2\xb7 " : "", genre);
	if (*len)
		append(meta, sizeof meta, "%s%s", *meta ? " \xc2\xb7 " : "", len);
	if (atof(rating) > 0)
		append(meta, sizeof meta, "%s%.1f/10", *meta ? " \xc2\xb7 " : "", atof(rating));
	if (*meta) {
		gfx_text(FontSmall, v.x, y, meta, col(ColDim, 1), v.w);
		y += gfx_lineh(FontSmall) + 10 * S;
	}

	if (it && (k == KindVod || k == KindEpisode) &&
	    (pos = store_get(P->src->name, k, it->id, &dur)) > 0) {
		if (pos >= dur) {
			gfx_text(FontSmall, v.x, y, "Watched", col(ColGood, 1), 0);
		} else {
			fmttime(a, sizeof a, pos);
			fmttime(b, sizeof b, dur);
			snprintf(buf, sizeof buf, "Resume from %s of %s", a, b);
			gfx_text(FontSmall, v.x, y, buf, col(ColDim, 1), 0);
			gfx_rect((Rect){ v.x, y + gfx_lineh(FontSmall) + 4 * S, v.w, 3 * S },
			         col(ColSel, 1), 1.5f * S);
			gfx_rect((Rect){ v.x, y + gfx_lineh(FontSmall) + 4 * S, v.w * pos / dur, 3 * S },
			         col(ColAccent, 1), 1.5f * S);
			y += 7 * S;
		}
		y += gfx_lineh(FontSmall) + 10 * S;
	}

	n = (int)((bottom - y) / gfx_lineh(FontText)) - 3;
	if (*plot && n > 0)
		y += gfx_wrap(FontText, v.x, y, v.w, plot, col(ColFg, 0.85f), n) *
		     gfx_lineh(FontText) + 10 * S;
	if (*cast && y + 2 * gfx_lineh(FontSmall) < bottom) {
		snprintf(meta, sizeof meta, "With %s", cast);
		gfx_wrap(FontSmall, v.x, y, v.w, meta, col(ColDim, 1), 2);
	}
	if (!it)
		return;
	gfx_text(FontSmall, v.x, bottom - gfx_lineh(FontSmall),
	         k == KindSeries ? "Enter: episodes   f: favorite" :
	         k == KindEpisode ? "Enter: play" : "Enter: play   f: favorite",
	         col(ColDim, 0.7f), 0);
}

static void
drawhints(Rect r)
{
	static const char *hints[][2] = {
		{ "Enter", "play" },
		{ "/", "search" },
		{ "f", "favorite" },
		{ "e", "guide" },
		{ "Tab", "tabs" },
		{ "\xe2\x86\x91\xe2\x86\x93\xe2\x86\x90\xe2\x86\x92", "move" },
		{ "Space", "pause" },
		{ "r", "refresh" },
		{ "F11", "fullscreen" },
		{ "Esc", "back" },
		{ "q", "quit" },
	};
	float x = r.x, kh = 20 * S, kw;
	size_t i;

	for (i = 0; i < LEN(hints); i++) {
		kw = gfx_textw(FontSmall, hints[i][0]) + 12 * S;
		if (x + kw + gfx_textw(FontSmall, hints[i][1]) > r.x + r.w)
			break;
		gfx_rect((Rect){ x, r.y + (r.h - kh) / 2, kw, kh }, col(ColSel, 1), 5 * S);
		gfx_text(FontSmall, x + 6 * S, mid(FontSmall, r.y, r.h), hints[i][0], col(ColFg, 1), 0);
		x = gfx_text(FontSmall, x + kw + 6 * S, mid(FontSmall, r.y, r.h), hints[i][1],
		             col(ColDim, 1), 0) + 18 * S;
	}
}

/* source, account and item count, right aligned; returns its width */
static float
drawaccount(Rect r)
{
	Playlist *p = P;
	Catalog *c = catalog();
	char buf[256] = "", n[32], off[300] = "";
	float x, w;

	append(buf, sizeof buf, "%s", p->src->name);
	if (p->src->type == SrcXtream && p->authok) {
		if (*p->status)
			append(buf, sizeof buf, " \xc2\xb7 %s", p->status);
		if (*p->exp)
			append(buf, sizeof buf, " \xc2\xb7 expires %s", p->exp);
		else
			append(buf, sizeof buf, " \xc2\xb7 no expiry");
		if (p->maxconn)
			append(buf, sizeof buf, " \xc2\xb7 %d/%d connections", p->active, p->maxconn);
	}
	if (c->loading || p->authloading) {
		append(buf, sizeof buf, " \xc2\xb7 updating\xe2\x80\xa6");
	} else if (c->loaded) {
		fmtcount(n, sizeof n, c->nitems);
		append(buf, sizeof buf, " \xc2\xb7 %s %s", n, noun(c->nitems));
	}
	/* a failed refresh with an older list still on screen */
	if (c->err[0] && c->nitems)
		snprintf(off, sizeof off, "Offline: %s   ", c->err);
	w = MIN(gfx_textw(FontSmall, off), r.w * 0.4f) + gfx_textw(FontSmall, buf);
	w = MIN(w, r.w);
	x = r.x + r.w - w;
	if (*off)
		x = gfx_text(FontSmall, x, mid(FontSmall, r.y, r.h), off, col(ColLive, 1), r.w * 0.4f);
	gfx_text(FontSmall, x, mid(FontSmall, r.y, r.h), buf, col(ColDim, 1), r.x + r.w - x);
	return w;
}

/* what the middle pane shows when there is nothing to list */
static void
drawempty(void)
{
	Catalog *c = catalog();
	const char *err = c->err[0] ? c->err : P->autherr;
	char msg[300];
	float w, y = clr.y + clr.h / 3;
	int failed = !c->loading && *err && tab < PageFav;

	if (c->loading)
		snprintf(msg, sizeof msg, "Loading %s\xe2\x80\xa6", noun(2));
	else if (PG == PageGuide)
		snprintf(msg, sizeof msg, "No guide for this channel");
	else if (failed)
		snprintf(msg, sizeof msg, "%s", err);
	else if (PG == PageFav)
		snprintf(msg, sizeof msg, "Press f on a channel, movie or series to keep it here");
	else if (PG == PageRecent)
		snprintf(msg, sizeof msg, "What you play shows up here");
	else
		snprintf(msg, sizeof msg, "No %s", noun(2));
	w = MIN(gfx_textw(FontText, msg), clr.w - 40 * S);
	gfx_text(FontText, roundf(clr.x + (clr.w - w) / 2), y, msg,
	         col(failed ? ColLive : ColDim, 1), w);
	if (failed) {
		snprintf(msg, sizeof msg, "Press r to try again");
		gfx_text(FontSmall, roundf(clr.x + (clr.w - gfx_textw(FontSmall, msg)) / 2),
		         y + gfx_lineh(FontText) + 8 * S, msg, col(ColDim, 1), 0);
	}
}

/* tabs, or the series name on its episode page, and where we are */
static void
drawheader(void)
{
	static const char *names[] = { "Live", "Movies", "Series", "Favorites", "Recent" };
	Catalog *c = catalog();
	float p = pad * S, x = clr.x + 12 * S, x0, base = p + gfx_ascent(FontTitle);
	char buf[300] = "", n[32];
	int k;

	memset(tabr, 0, sizeof tabr);
	backr = (Rect){ 0, 0, 0, 0 };
	if (inseries || inguide) {
		x0 = x;
		x = gfx_text(FontTitle, x, p, "\xe2\x86\x90  ", col(ColDim, 1), 0);
		x = gfx_text(FontTitle, x, p, inguide ? guidename : epsname, col(ColFg, 1), clr.w * 0.6f);
		backr = (Rect){ x0 - 6 * S, p - 4 * S, x - x0 + 12 * S, gfx_lineh(FontTitle) + 12 * S };
		x += 14 * S;
	} else {
		for (k = 0; k < (int)LEN(names); k++) {
			if (!tabok(k))
				continue;
			x0 = x;
			x = gfx_text(FontTitle, x, p, names[k], col(k == tab ? ColFg : ColDim, 1), 0);
			if (k == tab)
				gfx_rect((Rect){ x0, p + gfx_lineh(FontTitle) + 3 * S, x - x0, 3 * S },
				         col(ColAccent, 1), 1.5f * S);
			tabr[k] = (Rect){ x0 - 6 * S, p - 4 * S, x - x0 + 12 * S, gfx_lineh(FontTitle) + 12 * S };
			x += 24 * S;
		}
	}
	if (!c->loaded || x > clr.x + clr.w - 60 * S)
		return;
	if (glist.sel > 0 || inseries || inguide)
		append(buf, sizeof buf, "%s \xc2\xb7 ", c->groups[glist.sel]);
	fmtcount(n, sizeof n, nvis);
	append(buf, sizeof buf, "%s %s", n, noun(nvis));
	gfx_text(FontSmall, x + 4 * S, base - gfx_ascent(FontSmall), buf, col(ColDim, 1),
	         clr.x + clr.w - x - 4 * S);
}

static void
rowfound(int i, Rect r, int sel)
{
	const Item *it = &found.items[i];
	unsigned char hit[256] = { 0 };
	char seg[256], label[300];
	float x = r.x + 14 * S, right = r.x + r.w - 14 * S, ls = 26 * S;
	const char *s = it->name;
	size_t a, b, n = strlen(s);
	Img *im;
	int rtl = 0;

	(void)sel;
	if (hover(r))
		gfx_rect(r, col(ColSel, 0.45f), radius * S);
	if ((im = img_get(it->logo, (int)ls)) && im->state == ImgReady)
		drawcontain(im, (Rect){ x, r.y + (r.h - ls) / 2, ls, ls });
	x += ls + 12 * S;
	if (it->group > 0 && it->group < P->cat[it->kind].ngroups)
		snprintf(label, sizeof label, "%s \xc2\xb7 %s", kindname(it->kind),
		         P->cat[it->kind].groups[it->group]);
	else
		snprintf(label, sizeof label, "%s", kindname(it->kind));
	right = rlabel(right, r, label, ColDim);

	/* matched letters in the accent colour; right-to-left names are
	 * drawn whole, since pieces would be reordered on their own */
	for (a = 0; a < n; a++)
		rtl |= (unsigned char)s[a] >= 0xd6 && (unsigned char)s[a] <= 0xdb;
	if (rtl || n >= sizeof hit || gfx_textw(FontText, s) > right - x) {
		gfx_text(FontText, x, mid(FontText, r.y, r.h), s, col(ColFg, 1), right - x);
		return;
	}
	fuzzy(s, query, hit, sizeof hit);
	for (a = 0; a < n; a = b) {
		for (b = a; b < n && hit[b] == hit[a]; b++)
			;
		memcpy(seg, s + a, b - a);
		seg[b - a] = '\0';
		x = gfx_text(hit[a] ? FontBold : FontText, x, mid(FontText, r.y, r.h), seg,
		             col(hit[a] ? ColAccent : ColFg, 1), 0);
	}
}

static int
drawsearch(void)
{
	float p = pad * S, w = MIN(W - 2 * p, 760 * S), x = roundf((W - w) / 2), y = roundf(H * 0.12f);
	float ih = 54 * S, cx;
	int n = found.nitems, anim = 0;
	Rect in = { x, y, w, ih }, lr;
	ListStyle st;
	const char *hint;

	gfx_rect((Rect){ 0, 0, W, H }, col(ColBg, 0.7f), 0);
	gfx_rect(in, col(ColPanel, 1), radius * S);
	gfx_rect((Rect){ in.x, in.y + in.h - 2 * S, in.w, 2 * S }, col(ColAccent, 1), 1);
	cx = in.x + 20 * S;
	if (*query)
		cx = gfx_text(FontTitle, cx, mid(FontTitle, in.y, in.h), query, col(ColFg, 1), in.w - 40 * S);
	else
		gfx_text(FontTitle, cx, mid(FontTitle, in.y, in.h), "Search channels, movies, series",
		         col(ColDim, 0.6f), in.w - 40 * S);
	gfx_rect((Rect){ cx + 2 * S, in.y + 14 * S, 2 * S, in.h - 28 * S }, col(ColAccent, 1), 1);

	searchr = in;
	if (!*query)
		return 0;
	lr = (Rect){ x, in.y + in.h + 10 * S, w, MIN(n, 10) * rowh * S + 12 * S };
	if (!n) {
		hint = hastabs() && (P->cat[KindVod].loading || P->cat[KindSeries].loading)
		       ? "Nothing yet, still loading movies and series\xe2\x80\xa6" : "Nothing matches";
		lr.h = 52 * S;
		gfx_rect(lr, col(ColPanel, 1), radius * S);
		gfx_text(FontText, lr.x + 20 * S, mid(FontText, lr.y, lr.h), hint, col(ColDim, 1), 0);
	} else {
		gfx_rect(lr, col(ColPanel, 1), radius * S);
		st.radius = radius * S;
		st.accentw = 3 * S;
		st.bar = col(ColSel, 1);
		st.pill = col(ColSel, 1);
		st.accent = col(ColAccent, 1);
		anim = ui_list(&slist, (Rect){ lr.x + 6 * S, lr.y + 6 * S, lr.w - 12 * S, lr.h - 12 * S },
		               rowh * S, &st, rowfound);
	}
	searchr.h = lr.y + lr.h - in.y;
	return anim;
}

/* channel number entry: a small prompt over the bottom of the list */
static void
drawnumber(void)
{
	char buf[32];
	float h = 36 * S, w;
	Rect r;

	snprintf(buf, sizeof buf, "Channel %s", numberbuf);
	w = gfx_textw(FontText, buf) + 28 * S;
	r = (Rect){ clr.x + clr.w - w - 16 * S, clr.y + clr.h - h - 10 * S, w, h };
	gfx_rect(r, col(ColPanel, 1), radius * S);
	gfx_rect((Rect){ r.x, r.y, 3 * S, r.h }, col(ColAccent, 1), 1.5f * S);
	gfx_text(FontText, r.x + 14 * S, mid(FontText, r.y, r.h), buf, col(ColFg, 1), 0);
}

static void
layout(void)
{
	float p = pad * S, sw = sidebarw * S, top = 76 * S, sb = 40 * S;
	float mainx = sw + p, mainw = W - sw - 2 * p, pw = 0;

	if (mainw > 700 * S)
		pw = floorf(mainw * previewf);
	sider = (Rect){ 0, 0, sw, H - sb };
	glr = (Rect){ p / 2, top + gfx_lineh(FontSmall) + 8 * S, sw - p, 0 };
	glr.h = H - sb - glr.y - p / 2;
	clr = (Rect){ mainx - 12 * S, top, mainw + 12 * S - (pw ? pw + p : 0), H - sb - top - p / 2 };
	vidr = pw ? (Rect){ W - p - pw, top, pw, floorf(pw * 9 / 16) } : (Rect){ 0, 0, 0, 0 };

	/* posters: about 150pt wide, room for the selection ring */
	gridr = (Rect){ clr.x + 16 * S, clr.y + 4 * S, clr.w - 28 * S, clr.h - 4 * S };
	ggap = 18 * S;
	gcols = MAX(1, (int)((gridr.w + ggap) / (150 * S + ggap)));
	gcellw = (gridr.w - ggap * (gcols - 1)) / gcols;
	gcellh = roundf(gcellw * 1.5f) + 10 * S + gfx_lineh(FontText) + gfx_lineh(FontSmall);

	if (view == ViewPlayer)
		vidr = (Rect){ 0, 0, W, H };
}

static int
drawbrowse(void)
{
	float p = pad * S, x, y;
	const Item *it = focused();
	ListStyle st;
	int anim;

	SDL_ShowCursor(SDL_ENABLE);
	gfx_rect(sider, col(ColPanel, 1), 0);
	x = gfx_text(FontTitle, p, p, "sit", col(ColAccent, 1), 0);
	gfx_text(FontSmall, x + 8 * S, p + gfx_ascent(FontTitle) - gfx_ascent(FontSmall),
	         "suckless iptv", col(ColDim, 1), 0);
	gfx_text(FontSmall, p, glr.y - gfx_lineh(FontSmall) - 8 * S,
	         inseries ? "Seasons" : inguide ? "Days" : P->src->name, col(ColDim, 1), sider.w - 2 * p);
	drawheader();

	st.radius = radius * S;
	st.accentw = 3 * S;
	st.bar = col(ColSel, 1);
	st.pill = col(ColSel, pane == 0 ? 1 : 0.5f);
	st.accent = col(ColAccent, pane == 0);
	anim = ui_list(&glist, glr, rowh * S, &st, rowgroup);
	st.pill = col(ColSel, pane == 1 ? 1 : 0.5f);
	st.accent = col(ColAccent, pane == 1);
	if (isgrid())
		anim |= ui_grid(&clist, gridr, gcols, gcellh, ggap, &st, cellposter);
	else
		anim |= ui_list(&clist, clr, rowh * S, &st,
		                PG == PageEpisodes ? rowep : PG == PageGuide ? rowprog : rowchan);
	if (!nvis)
		drawempty();
	if (vidr.w > 0) {
		if (inguide)
			drawprog(it);
		else if ((it && it->kind != KindLive) || (!it && inseries))
			drawdetails(it);
		else
			drawpreview();
	}

	y = H - 40 * S;
	gfx_rect((Rect){ 0, y, W, 40 * S }, col(ColPanel, 1), 0);
	x = drawaccount((Rect){ p, y, W - 2 * p, 40 * S });
	drawhints((Rect){ p, y, W - 2 * p - x - 24 * S, 40 * S });
	if (numbering)
		drawnumber();
	if (searching)
		anim |= drawsearch();
	return anim;
}

static int
drawplayer(void)
{
	float p = pad * S, cw = MIN(W - 2 * p, 960 * S), ch = 92 * S, x, y, w, target, a, frac;
	Rect card = { roundf((W - cw) / 2), H - p - ch, cw, ch };
	const char *s, *nowtitle = NULL;
	char buf[80], b1[32], b2[32];
	const Prog *pg, *next;
	int c, anim = 0;
	time_t tt;

	/* mpv keeps the old channel's last frame until the new one decodes */
	if (pl.loaded)
		gfx_image(pl.fb.tex, vidr, rgba(0xffffff, 1), 0);

	target = pl.paused || !pl.loaded || pl.err[0] || t - lastinput < hidedelay;
	if (t - lastinput < hidedelay)
		wake(lastinput + hidedelay);
	ovl = approach(ovl, target, ui_dt, 10, 0.01f);
	anim |= ovl != target;
	SDL_ShowCursor(ovl > 0 ? SDL_ENABLE : SDL_DISABLE);

	if (ovl > 0) {
		gfx_rect(card, col(ColPanel, 0.92f * ovl), radius * S);
		x = card.x + 22 * S;
		y = card.y + 18 * S;
		w = card.w - 44 * S;
		if (seekable()) {
			fmttime(b1, sizeof b1, pl.pos);
			fmttime(b2, sizeof b2, pl.dur);
			snprintf(buf, sizeof buf, "%s / %s", b1, b2);
			gfx_text(FontSmall, x + w - gfx_textw(FontSmall, buf), y + 4 * S, buf,
			         col(ColDim, ovl), 0);
			gfx_rect((Rect){ x, card.y + ch - 20 * S, w, 4 * S }, col(ColSel, ovl), 2 * S);
			gfx_rect((Rect){ x, card.y + ch - 20 * S, w * CLAMP(pl.pos / pl.dur, 0, 1), 4 * S },
			         col(ColAccent, ovl), 2 * S);
		} else if (pl.loaded && playkind == KindLive && hasguide(&lists[playsrc]) &&
		           (pg = epg_now(epg_want(&lists[playsrc], playid, 0), (long long)time(NULL), &next))) {
			/* the programme on now, its progress and what follows */
			frac = (float)(time(NULL) - pg->start) / (pg->stop - pg->start);
			gfx_rect((Rect){ x, card.y + ch - 20 * S, w, 4 * S }, col(ColSel, ovl), 2 * S);
			gfx_rect((Rect){ x, card.y + ch - 20 * S, w * frac, 4 * S }, col(ColLive, ovl), 2 * S);
			if (next) {
				tt = (time_t)next->start;
				strftime(b1, sizeof b1, "%H:%M", localtime(&tt));
				snprintf(buf, sizeof buf, "Next %s  %.*s", b1, 40, next->title);
				gfx_text(FontSmall, x + w - MIN(gfx_textw(FontSmall, buf), w * 0.4f), y + 4 * S,
				         buf, col(ColDim, ovl), w * 0.4f);
			}
			nowtitle = pg->title;
		} else if (pl.loaded) {
			a = gfx_textw(FontSmall, "LIVE") + 14 * S;
			gfx_rect((Rect){ x + w - a, y + 2 * S, a, 20 * S }, col(ColLive, ovl), 5 * S);
			gfx_text(FontSmall, x + w - a + 7 * S, mid(FontSmall, y + 2 * S, 20 * S), "LIVE",
			         col(ColBg, ovl), 0);
		}
		gfx_text(FontBold, x, y, playname, col(ColFg, ovl), w * 0.55f);
		y += gfx_lineh(FontBold) + 4 * S;
		s = status(&c);
		gfx_rect((Rect){ x, y + gfx_lineh(FontSmall) / 2 - 3 * S, 6 * S, 6 * S }, col(c, ovl), 3 * S);
		x = gfx_text(FontSmall, x + 14 * S, y, s, col(ColDim, ovl), w - 14 * S);
		gfx_text(FontSmall, x + 14 * S, y, nowtitle ? nowtitle : playgroup,
		         col(nowtitle ? ColFg : ColDim, ovl), card.x + card.w - 22 * S - x - 14 * S);
	}

	/* connecting/buffering stays visible even with the overlay hidden */
	if (!pl.err[0] && (!pl.loaded || pl.buffering)) {
		s = status(&c);
		w = gfx_textw(FontText, s) + 32 * S;
		card = (Rect){ roundf((W - w) / 2), roundf(H / 2 - 20 * S), w, 40 * S };
		gfx_rect(card, col(ColPanel, 0.85f), 20 * S);
		gfx_text(FontText, card.x + 16 * S, mid(FontText, card.y, card.h), s, col(ColFg, 1), 0);
	}
	return anim;
}

/* volume and track messages, top right, in both views */
static int
drawosd(void)
{
	float p = pad * S, w, a;
	double age = t - osdtime;
	Rect r;

	if (!osdmsg[0] || age > 2)
		return 0;
	a = (float)MIN(1, (2 - age) * 4);
	w = MIN(gfx_textw(FontText, osdmsg) + 32 * S, W - 2 * p);
	r = (Rect){ W - p - w, p, w, 40 * S };
	gfx_rect(r, col(ColPanel, 0.92f * a), 20 * S);
	gfx_text(FontText, r.x + 16 * S, mid(FontText, r.y, r.h), osdmsg, col(ColFg, a), r.w - 32 * S);
	if (age < 1.75)
		wake(osdtime + 1.75);
	return age >= 1.75;
}

static int
draw(void)
{
	const Item *it;
	int anim, id;

	SDL_GL_GetDrawableSize(win, &W, &H);
	t = now();
	nextwake = 0;
	img_tick();
	if (!onboarding) {
		freshmarks();
		freshguide();
		if (view == ViewPlayer && playsrc < 0)
			view = ViewBrowse;
		if ((id = selid()) != lastsel || PG != lastpg) {
			lastsel = id;
			lastpg = PG;
			selt = t;
		}
		if (!inguide && hasguide(P) && (it = focused()) && it->kind == KindLive) {
			if (t - selt > 0.3)
				epg_want(P, it->id, 0);
			else
				wake(selt + 0.31);
		}
	}
	if (playsrc >= 0 && t - lastsave > 5) {
		saveprogress(t - lastflush > 30);
		lastsave = t;
	}
	layout();
	if (playsrc >= 0)
		player_render((int)vidr.w, (int)vidr.h);
	gfx_begin(W, H, col(ColBg, 1));
	anim = onboarding ? drawonboarding()
	     : view == ViewPlayer ? drawplayer() : drawbrowse();
	anim |= drawosd();
	gfx_end();
	SDL_GL_SwapWindow(win);
	player_swapped();
	return anim;
}

static int
cleanmod(int m)
{
	int r = 0;

	if (m & KMOD_CTRL)
		r |= KMOD_CTRL;
	if (m & KMOD_SHIFT)
		r |= KMOD_SHIFT;
	if (m & KMOD_ALT)
		r |= KMOD_ALT;
	return r;
}

static void
click(int clicks)
{
	int i, k;

	if (onboarding)
		return;
	if (view == ViewPlayer) {
		if (clicks == 2)
			fullscreen(NULL);
		return;
	}
	if (searching) {
		i = ui_list_hit(&slist, (Rect){ searchr.x + 6 * S, searchr.y + 64 * S + 6 * S,
		                                searchr.w - 12 * S, searchr.h - 64 * S - 12 * S },
		                rowh * S, mx, my);
		if (i >= 0) {
			slist.sel = i;
			choose();
		} else if (!inrect(searchr, mx, my)) {
			closesearch();
		}
		return;
	}
	if (numbering)
		numbering = 0;
	for (k = 0; k < (int)LEN(tabr); k++)
		if (tabr[k].w && inrect(tabr[k], mx, my)) {
			settab(k);
			return;
		}
	if (backr.w && inrect(backr, mx, my)) {
		if (inguide)
			closeguide();
		else
			closeseries();
		return;
	}
	if ((i = ui_list_hit(&glist, glr, rowh * S, mx, my)) >= 0) {
		pane = 0;
		if (i != glist.sel) {
			glist.sel = i;
			filter(-1);
		}
		return;
	}
	i = isgrid() ? ui_grid_hit(&clist, gridr, gcols, gcellh, ggap, mx, my)
	             : ui_list_hit(&clist, clr, rowh * S, mx, my);
	if (i >= 0) {
		pane = 1;
		clist.sel = i;
		if (clicks == 2)
			play(NULL);
	} else if (playsrc >= 0 && inrect(vidr, mx, my) &&
	           (PG == PageLive || (nvis && playing(focused())))) {
		view = ViewPlayer;
	}
}

static void
handle(SDL_Event *e)
{
	size_t i, n;
	int m;

	switch (e->type) {
	case SDL_QUIT:
		running = 0;
		break;
	case SDL_TEXTINPUT:
		if (onboarding) {
			obtext(e->text.text);
			break;
		}
		if (!searching)
			break;
		/* the "/" that opened the palette arrives as text too */
		if (skipslash && !strcmp(e->text.text, "/")) {
			skipslash = 0;
			break;
		}
		skipslash = 0;
		n = strlen(query);
		snprintf(query + n, sizeof query - n, "%s", e->text.text);
		runsearch();
		break;
	case SDL_KEYDOWN:
		lastinput = now();
		if (onboarding) {
			obkey(&e->key.keysym);
			break;
		}
		if (searching) {
			searchkey(&e->key.keysym);
			break;
		}
		if (numbering) {
			numberkey(&e->key.keysym);
			break;
		}
		m = cleanmod(e->key.keysym.mod);
		for (i = 0; i < LEN(keys); i++)
			if ((keys[i].views & view) && keys[i].mod == m && keys[i].sym == e->key.keysym.sym) {
				keys[i].func(&keys[i].arg);
				break;
			}
		break;
	case SDL_MOUSEMOTION:
		mx = e->motion.x * ratio;
		my = e->motion.y * ratio;
		lastinput = now();
		break;
	case SDL_MOUSEBUTTONDOWN:
		mx = e->button.x * ratio;
		my = e->button.y * ratio;
		lastinput = now();
		if (e->button.button == SDL_BUTTON_LEFT)
			click(e->button.clicks);
		break;
	case SDL_MOUSEWHEEL:
		if (onboarding)
			break;
		if (view == ViewPlayer)
			volume(&(Arg){ .f = e->wheel.preciseY > 0 ? 5 : -5 });
		else if (searching)
			ui_list_wheel(&slist, e->wheel.preciseY, rowh * S);
		else if (inrect(glr, mx, my))
			ui_list_wheel(&glist, e->wheel.preciseY, rowh * S);
		else if (isgrid() && inrect(clr, mx, my))
			ui_list_wheel(&clist, e->wheel.preciseY, (gcellh + ggap) / 3);
		else if (inrect(clr, mx, my))
			ui_list_wheel(&clist, e->wheel.preciseY, rowh * S);
		break;
	default:
		if (e->type == evmpv)
			player_events();
		else if (e->type == evmpv + 2)
			onjob(e->user.data1);
		break;
	}
}

static const char *
findfont(const char **cands, size_t n)
{
	char buf[1100], *base = SDL_GetBasePath();
	size_t i;

	/* relative entries are looked up next to the executable first, so a
	 * shortcut or installer need not set the working directory */
	for (i = 0; base && i < n; i++) {
		if (cands[i][0] == '/' || strchr(cands[i], ':'))
			continue;
		snprintf(buf, sizeof buf, "%s%s", base, cands[i]);
		if (fileexists(buf)) {
			SDL_free(base);
			return xstrdup(buf);
		}
	}
	SDL_free(base);
	for (i = 0; i < n; i++)
		if (fileexists(cands[i]))
			return cands[i];
	return NULL;
}

static void
setscale(void)
{
	int ww, wh, dw, dh;
	float dpi;
	const char *env = getenv("SIT_SCALE");

	SDL_GetWindowSize(win, &ww, &wh);
	SDL_GL_GetDrawableSize(win, &dw, &dh);
	ratio = ww > 0 ? (float)dw / ww : 1;
	if (env && atof(env) > 0)
		S = atof(env);
	else if (ratio > 1.01f)
		S = ratio;
#ifdef _WIN32
	else if (SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(win), &dpi, NULL, NULL) == 0)
		S = roundf(dpi / 96 * 4) / 4;
#else
	(void)dpi;
#endif
	S = CLAMP(S, 1, 4);
}

static void
addsource(const char *name, int type, const char *url, const char *user, const char *pass)
{
	srcs = xrealloc(srcs, (nsrcs + 1) * sizeof *srcs);
	srcs[nsrcs++] = (Source){ name, type, url, user, pass };
}

/* tab-separated: name, type, url, user, pass (m3u: name, m3u, url or file);
 * returns sources added */
static int
loadsources(const char *path, int required)
{
	char *buf, *line, *next, *f[5], *t;
	int nf, n = 0;

	if (!(buf = (char *)readfile(path, NULL))) {
		if (required)
			die("cannot read %s", path);
		return 0;
	}
	for (line = buf; line && *line; line = next) {
		if ((next = strchr(line, '\n')))
			*next++ = '\0';
		if ((t = strchr(line, '\r')))
			*t = '\0';
		if (!strncmp(line, "\xef\xbb\xbf", 3))    /* Notepad's BOM */
			line += 3;
		if (!*line || *line == '#')
			continue;
		f[0] = line;
		for (nf = 1; nf < 5 && (t = strchr(f[nf - 1], '\t')); nf++) {
			*t = '\0';
			f[nf] = t + 1;
		}
		if (nf == 5 && !strcmp(f[1], "xtream"))
			addsource(f[0], SrcXtream, f[2], f[3], f[4]);
		else if (nf >= 3 && !strcmp(f[1], "m3u"))
			addsource(f[0], SrcM3u, f[2], nf >= 4 ? f[3] : "", "");
		else if (nf >= 2 && !strcmp(f[1], "demo"))
			addsource(f[0], SrcDemo, NULL, NULL, NULL);
		else {
			fprintf(stderr, "sit: %s: skipping '%s': want name, xtream, url, user, "
			        "pass separated by tabs\n", path, f[0]);
			skipped++;
			continue;
		}
		n++;
	}
	return n;
}

static int
obvisible(int f)
{
	if (f == ObName || f == ObType || f == ObUrl)
		return 1;
	return obtype == 0 ? f == ObUser || f == ObPass : f == ObXml;
}

static void
obmove(int d)
{
	do
		obfield = (obfield + d + ObFieldLast) % ObFieldLast;
	while (!obvisible(obfield));
}

/* the buffer of the focused field; NULL for the Type selector */
static char *
obfieldbuf(size_t *n)
{
	switch (obfield) {
	case ObName: *n = sizeof obname; return obname;
	case ObUrl: *n = sizeof oburl; return oburl;
	case ObUser: *n = sizeof obuser; return obuser;
	case ObPass: *n = sizeof obpass; return obpass;
	case ObXml: *n = sizeof obxml; return obxml;
	}
	*n = 0;
	return NULL;
}

static void
obtext(const char *s)
{
	char *b;
	size_t n, len;

	if (!(b = obfieldbuf(&n)))
		return;
	len = strlen(b);
	if (len < n)
		snprintf(b + len, n - len, "%s", s);
}

static void
obbackspace(void)
{
	char *b;
	size_t n, len;

	if (!(b = obfieldbuf(&n)))
		return;
	len = strlen(b);
	while (len > 0 && (b[--len] & 0xc0) == 0x80)
		;
	b[len] = '\0';
}

static void
obhost(char *dst, size_t n, const char *u)
{
	const char *p, *e, *at;
	size_t len;

	p = strstr(u, "://");
	p = p ? p + 3 : u;
	for (e = p; *e && *e != '/' && *e != '?' && *e != '#'; e++)
		;
	for (at = p; at < e; at++)
		if (*at == '@')
			p = at + 1;
	for (at = p; at < e && *at != ':' && *at != '\\'; at++)
		;
	len = at - p;
	if (!len)
		snprintf(dst, n, "Source");
	else
		snprintf(dst, n, "%.*s", (int)MIN(len, n - 1), p);
}

/* keep tabs and newlines from breaking the sources file */
static void
santab(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

/* the password is stored in plaintext here, as it already is in the file */
static void
obsave(void)
{
	const char *dir = appdir(DirConfig);
	char path[1100], line[4096];
	FILE *f;
	int fd, i, type;

	if (!*oburl) {
		osd("Enter a URL");
		return;
	}
	if (!*obname)
		obhost(obname, sizeof obname, oburl);
	santab(obname);
	santab(oburl);
	santab(obuser);
	santab(obpass);
	santab(obxml);
	if (obtype == 0)
		snprintf(line, sizeof line, "%s\txtream\t%s\t%s\t%s\n",
		         obname, oburl, obuser, obpass);
	else if (*obxml)
		snprintf(line, sizeof line, "%s\tm3u\t%s\t%s\n", obname, oburl, obxml);
	else
		snprintf(line, sizeof line, "%s\tm3u\t%s\n", obname, oburl);
	mkdirp(dir);
	snprintf(path, sizeof path, "%s/sources", dir);
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0) {
		osd("Cannot write %s", path);
		return;
	}
	if (!(f = fdopen(fd, "a"))) {
		osd("Cannot write %s", path);
		return;
	}
	if (fputs(line, f) == EOF) {
		fclose(f);
		osd("Cannot write %s", path);
		return;
	}
	if (fclose(f) != 0) {
		osd("Cannot write %s", path);
		return;
	}
	type = obtype == 0 ? SrcXtream : SrcM3u;
	if (type == SrcXtream)
		addsource(obname, type, oburl, obuser, obpass);
	else
		addsource(obname, type, oburl, obxml, "");
	lists = xrealloc(lists, nsrcs * sizeof *lists);
	for (i = 0; i < nsrcs - 1; i++)    /* addsource may have moved srcs */
		lists[i].src = &srcs[i];
	lists[nsrcs - 1] = (Playlist){0};
	lists[nsrcs - 1].src = &srcs[nsrcs - 1];
	snprintf(lists[nsrcs - 1].ext, sizeof lists[nsrcs - 1].ext, "%s", liveext);
	onboarding = 0;
	SDL_StopTextInput();
	usesource(nsrcs - 1);
	osd("%s added", obname);
}

static void
obkey(const SDL_Keysym *k)
{
	if (k->sym == SDLK_v && (k->mod & KMOD_CTRL)) {
		char *t = SDL_GetClipboardText();
		if (t) {
			obtext(t);
			SDL_free(t);
		}
		return;
	}
	switch (k->sym) {
	case SDLK_ESCAPE:
		if (nsrcs) {
			onboarding = 0;
			SDL_StopTextInput();
			break;
		}
		SDL_StopTextInput();
		quit(NULL);
		break;
	case SDLK_q:
		SDL_StopTextInput();
		quit(NULL);
		break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		obsave();
		break;
	case SDLK_UP:
		obmove(-1);
		break;
	case SDLK_DOWN:
		obmove(1);
		break;
	case SDLK_TAB:
		obmove((k->mod & KMOD_SHIFT) ? -1 : 1);
		break;
	case SDLK_LEFT:
	case SDLK_RIGHT:
		if (obfield == ObType)
			obtype = !obtype;
		break;
	case SDLK_BACKSPACE:
		obbackspace();
		break;
	}
}

static int
drawonboarding(void)
{
	static const char *labels[] = {
		"Name", "Type", "URL", "Username", "Password", "XMLTV URL"
	};
	char stars[sizeof obpass];
	const char *s;
	float p = pad * S, w = MIN(W - 2 * p, 720 * S), x = roundf((W - w) / 2);
	float lx = x + w * 0.24f, y = roundf(H * 0.12f), rh = 44 * S;
	size_t i, n;
	int k;

	SDL_ShowCursor(SDL_ENABLE);
	gfx_text(FontTitle, x, y, "Welcome to sit", col(ColAccent, 1), w);
	y += gfx_lineh(FontTitle) + 8 * S;
	gfx_text(FontText, x, y, "Add your provider (Esc to skip):", col(ColDim, 1), w);
	y += gfx_lineh(FontText) + 18 * S;
	for (k = 0; k < ObFieldLast; k++) {
		Rect r = { x, y, w, rh };

		if (!obvisible(k))
			continue;
		if (k == obfield) {
			gfx_rect(r, col(ColSel, 1), radius * S);
			gfx_rect((Rect){ r.x, r.y, 3 * S, r.h }, col(ColAccent, 1), 1.5f * S);
		}
		gfx_text(FontSmall, x + 16 * S, mid(FontSmall, y, rh), labels[k],
		         col(k == obfield ? ColFg : ColDim, 1), lx - x - 24 * S);
		switch (k) {
		case ObType:
			s = obtype ? (k == obfield ? "< M3U >" : "M3U")
			           : (k == obfield ? "< Xtream >" : "Xtream");
			break;
		case ObPass:
			n = strlen(obpass);
			for (i = 0; i < n; i++)
				stars[i] = '*';
			stars[n] = '\0';
			s = stars;
			break;
		case ObName: s = obname; break;
		case ObUrl: s = oburl; break;
		case ObUser: s = obuser; break;
		case ObXml: s = obxml; break;
		default: s = "";
		}
		gfx_text(FontText, lx, mid(FontText, y, rh), s,
		         col(ColFg, k == obfield ? 1 : 0.85f), r.x + r.w - lx - 16 * S);
		y += rh + 8 * S;
	}
	y = H - 40 * S;
	gfx_rect((Rect){ 0, y, W, 40 * S }, col(ColPanel, 1), 0);
	gfx_text(FontSmall, p, mid(FontSmall, y, 40 * S),
	         "Enter: save   Esc: skip   Up/Down: field   Left/Right: type",
	         col(ColDim, 1), W - 2 * p);
	return 0;
}

static void
usage(void)
{
	die("usage: sit [-v] [-s sources] [url ...]");
}

int
main(int argc, char *argv[])
{
	SDL_Event ev;
	const char *reg, *bold, *srcfile = NULL;
	char path[1100];
	double last;
	int i, anim = 1, ms;

	args = xcalloc(argc, sizeof *args);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			puts("sit-" VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-s")) {
			if (++i == argc)
				usage();
			srcfile = argv[i];
		} else if (argv[i][0] == '-' && argv[i][1]) {
			usage();
		} else {
			args[nargs++] = argv[i];
		}
	}
	if (nargs)
		addsource("Command line", SrcUrls, NULL, NULL, NULL);
	snprintf(path, sizeof path, "%s/sources", appdir(DirConfig));
	if (!srcfile && !fileexists(path))    /* Notepad likes to add .txt */
		snprintf(path, sizeof path, "%s/sources.txt", appdir(DirConfig));
	if (srcfile) {
		loadsources(srcfile, 1);
	} else if (loadsources(path, 0) == 0) {
		if (fileexists(path))
			die("%s has no usable lines.\n\nEach line needs five fields separated by tabs:\n"
			    "name, xtream, url, username, password", path);
		for (i = 0; i < (int)LEN(sources); i++)
			addsource(sources[i].name, sources[i].type, sources[i].url,
			          sources[i].user, sources[i].pass);
	}
	/* first run: no source file, no command line urls, only the Demo source */
	onboarding = !srcfile && !nargs && !fileexists(path);
	for (i = 0; i < nsrcs; i++)
		if (srcs[i].type != SrcDemo)
			onboarding = 0;
	if (!nsrcs) {
		if (srcfile)
			die("no usable sources in %s", srcfile);
		onboarding = 1;    /* nothing configured: offer the form instead of dying */
	}

	setlocale(LC_NUMERIC, "C");    /* libmpv requires it */
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		die("SDL_Init: %s", SDL_GetError());
	SDL_StopTextInput();    /* on by default; only search and onboarding want it */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	if (!(win = SDL_CreateWindow("sit", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                             1280, 800, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
	                             SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN)))
		die("SDL_CreateWindow: %s", SDL_GetError());
	if (!(glctx = SDL_GL_CreateContext(win)))
		die("OpenGL 3.3 unavailable: %s", SDL_GetError());
	SDL_GL_SetSwapInterval(1);
	setscale();
	if (ratio < 1.01f && S > 1) {
		SDL_SetWindowSize(win, (int)(1280 * S), (int)(800 * S));
		SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	}
	SDL_SetWindowMinimumSize(win, (int)(640 * S / ratio), (int)(400 * S / ratio));
	SDL_ShowWindow(win);
	if (onboarding)
		SDL_StartTextInput();

	gfx_init();
	if (!(reg = findfont(fonts, LEN(fonts))))
		die("no font found; edit fonts[] in config.h");
	if (!(bold = findfont(boldfonts, LEN(boldfonts))))
		bold = reg;
	for (i = 0; i < (int)LEN(fallbackfonts); i++)
		if (fileexists(fallbackfonts[i]))
			gfx_fallback(fallbackfonts[i]);
	gfx_font(reg, textsize * S);
	gfx_font(bold, textsize * S);
	gfx_font(bold, titlesize * S);
	gfx_font(reg, smallsize * S);
	gfx_bake();

	/* mpv wakeup, mpv frame, finished network job */
	evmpv = SDL_RegisterEvents(3);
	player_init(evmpv, mpvopts, LEN(mpvopts));
	net_init(evmpv + 2, useragent);
	store_load();

	lists = xcalloc(MAX(nsrcs, 1), sizeof *lists);
	for (i = 0; i < nsrcs; i++) {
		lists[i].src = &srcs[i];
		snprintf(lists[i].ext, sizeof lists[i].ext, "%s", liveext);
	}
	if (nsrcs)
		usesource(0);
	if (skipped)
		osd("Skipped %d unreadable line%s in sources", skipped, skipped == 1 ? "" : "s");

	last = now();
	while (running) {
		if (!anim) {
			ms = nextwake > 0 ? (int)((nextwake - now()) * 1000) + 1 : -1;
			if (ms < 0 ? SDL_WaitEvent(&ev) : SDL_WaitEventTimeout(&ev, MAX(ms, 1)))
				handle(&ev);
			last = now() - 1.0 / 60;    /* no time passes while idle */
		}
		while (SDL_PollEvent(&ev))
			handle(&ev);
		ui_dt = (float)MIN(now() - last, 1.0 / 30);
		last = now();
		anim = draw();
	}

	saveprogress(1);
	net_quit();
	player_quit();
	SDL_GL_DeleteContext(glctx);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
