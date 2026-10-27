/* See LICENSE file for copyright and license details. */
typedef struct {
	long long start, stop;
	const char *title, *desc;
	const char *local;    /* the provider's own "YYYY-MM-DD HH:MM:SS", for catch-up */
	int archive;          /* the provider keeps a recording */
} Prog;

/* the guide of one channel */
typedef struct {
	const Playlist *p;
	int id, full, loading, pin, gen;
	long long fetched;
	unsigned long used;
	Arena arena;
	Prog *progs;
	int n;
} Epg;

Epg *epg_find(const Playlist *p, int id);
Epg *epg_want(Playlist *p, int id, int full);
void epg_result(Job *j);
void epg_xmltv(Playlist *p);
void epg_xml_result(Job *j);
const Prog *epg_now(const Epg *e, long long t, const Prog **next);
const Prog *epg_prog(const Epg *e, long long start);
int epg_catalog(const Epg *e, Catalog *c, int chan, int archdays, long long t);
void epg_day(long long t, long long now, char *buf, size_t n);
