/* See LICENSE file for copyright and license details.
 *
 * M3U / M3U8 playlists, from a url or a file. Every entry becomes a live
 * channel: #EXTINF attributes (tvg-id, tvg-logo, group-title) and the
 * #EXTGRP line are read, the rest is ignored. Ids are the position in the
 * list, which is also what static sources use to find a url again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "json.h"
#include "net.h"
#include "playlist.h"
#include "epg.h"
#include "m3u.h"

static int
isurl(const char *s)
{
	return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8);
}

/* value of key="..." or key=... in an #EXTINF line, or "" */
static const char *
attr(Arena *a, const char *line, const char *comma, const char *key)
{
	size_t kl = strlen(key);
	const char *p, *e;
	char q;

	for (p = line; p + kl < comma; p++) {
		if (strncmp(p, key, kl) || p[kl] != '=' || (p > line && p[-1] != ' ' && p[-1] != '\t'))
			continue;
		p += kl + 1;
		q = *p == '"' || *p == '\'' ? *p++ : ' ';
		for (e = p; e < comma && *e != q; e++)
			;
		return memcpy(arena_alloc(a, e - p + 1), p, e - p);
	}
	return "";
}

/* the title: after the first comma that is outside quotes */
static const char *
titleof(const char *line)
{
	int inq = 0;

	for (; *line; line++) {
		if (*line == '"')
			inq = !inq;
		else if (*line == ',' && !inq)
			return line + 1;
	}
	return NULL;
}

static int
groupof(Catalog *c, const char *name, int *last)
{
	int g;

	if (*last && !strcmp(c->groups[*last], name))
		return *last;
	for (g = 1; g < c->ngroups; g++)
		if (!strcmp(c->groups[g], name))
			return *last = g;
	c->groups[c->ngroups] = arena_strdup(&c->arena, name);
	c->gcount[c->ngroups] = 0;
	return *last = c->ngroups++;
}

static void
build(Playlist *p, char *buf)
{
	Catalog *c = &p->cat[KindLive];
	char *line, *next, *t, *title;
	const char *comma, *grp;
	char *info = NULL, *extgrp = NULL;
	size_t cap = 1;
	int last = 0;
	Item *it;

	for (line = buf; *line; line++)
		cap += *line == '\n';
	arena_free(&c->arena);
	c->items = arena_alloc(&c->arena, cap * sizeof *c->items);
	c->groups = arena_alloc(&c->arena, cap * sizeof *c->groups);
	c->gcount = arena_alloc(&c->arena, cap * sizeof *c->gcount);
	c->groups[0] = "All channels";
	c->ngroups = 1;
	c->nitems = 0;

	if (!strncmp(buf, "\xef\xbb\xbf", 3))
		buf += 3;
	for (line = buf; line && *line; line = next) {
		if ((next = strchr(line, '\n')))
			*next++ = '\0';
		if ((t = strchr(line, '\r')))
			*t = '\0';
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line)
			continue;
		if (!strncmp(line, "#EXTM3U", 7)) {
			/* the playlist may name its own guide; the sources file wins */
			if (!*p->src->user) {
				comma = line + strlen(line);
				grp = attr(&c->arena, line, comma, "x-tvg-url");
				if (!*grp)
					grp = attr(&c->arena, line, comma, "url-tvg");
				snprintf(p->guide, sizeof p->guide, "%.*s", (int)strcspn(grp, ","), grp);
			}
			continue;
		}
		if (!strncmp(line, "#EXTINF:", 8)) {
			info = line + 8;
			extgrp = NULL;
			continue;
		}
		if (!strncmp(line, "#EXTGRP:", 8)) {
			extgrp = line + 8;
			continue;
		}
		if (*line == '#')
			continue;
		it = &c->items[c->nitems];
		it->name = it->logo = it->epgid = it->ext = "";
		it->rating = it->date = it->genre = it->plot = it->cast = it->backdrop = it->sname = "";
		title = NULL;
		grp = "";
		if (info) {
			title = (char *)titleof(info);
			comma = title ? title - 1 : info + strlen(info);
			it->logo = attr(&c->arena, info, comma, "tvg-logo");
			it->epgid = attr(&c->arena, info, comma, "tvg-id");
			grp = attr(&c->arena, info, comma, "group-title");
		}
		if (!*grp && extgrp)
			grp = extgrp;
		if (!*grp)
			grp = "Ungrouped";
		while (title && (*title == ' ' || *title == '\t'))
			title++;
		it->name = title && *title ? arena_strdup(&c->arena, title) : arena_strdup(&c->arena, line);
		it->url = arena_strdup(&c->arena, line);
		it->id = c->nitems;
		it->kind = KindLive;
		it->live = 1;
		it->group = groupof(c, grp, &last);
		c->gcount[it->group]++;
		c->nitems++;
		info = extgrp = NULL;
	}
	c->gcount[0] = c->nitems;
	c->loaded = 1;
}

/* a file is read at once; a url comes back through m3u_result */
void
m3u_load(Playlist *p, int ttl, int force)
{
	Catalog *c = &p->cat[KindLive];
	char path[1300];
	unsigned char *buf;
	Job *j;

	c->err[0] = '\0';
	snprintf(p->guide, sizeof p->guide, "%s", p->src->user);
	if (!isurl(p->src->url)) {
		if (!(buf = readfile(p->src->url, NULL))) {
			snprintf(c->err, sizeof c->err, "cannot read %s", p->src->url);
			return;
		}
		build(p, (char *)buf);
		free(buf);
		epg_xmltv(p);
		return;
	}
	snprintf(path, sizeof path, "%s/m3u-%016llx/list.m3u", appdir(DirCache), fnv1a(p->src->url));
	j = net_job(p->src->url, path, force ? 0 : ttl, 0);
	j->owner = JobM3u;
	j->kind = 2;    /* ResItems: onjob treats it like a rebuilt list */
	j->sub = KindLive;
	j->ud = p;
	net_submit(j);
	c->loading++;
}

/* takes ownership of j; returns KindLive when the list was rebuilt, else -1 */
int
m3u_result(Playlist *p, Job *j)
{
	Catalog *c = &p->cat[KindLive];
	int rebuilt = -1;

	if (!j->stale)
		c->loading--;
	if (j->err[0] || !j->data) {
		if (!c->err[0])
			snprintf(c->err, sizeof c->err, "%s", j->err[0] ? j->err : "no data");
	} else {
		build(p, j->data);
		rebuilt = KindLive;
		epg_xmltv(p);
	}
	net_free(j);
	return rebuilt;
}
