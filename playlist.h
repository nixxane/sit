/* See LICENSE file for copyright and license details. */
enum { SrcDemo, SrcUrls, SrcXtream, SrcM3u };
enum { KindLive, KindVod, KindSeries, KindEpisode, KindProg };

typedef struct {
	const char *name;
	int type;
	const char *url, *user, *pass;
} Source;

/* strings are "" when the provider sent nothing */
typedef struct {
	const char *name, *logo, *epgid;
	const char *ext;      /* container for movies and episodes */
	const char *url;      /* direct url; NULL for Xtream, built when played */
	const char *rating, *date, *genre, *plot, *cast, *backdrop;
	const char *sname;    /* series name, for episodes */
	const JVal *raw;      /* episodes only: the provider's object */
	int id, group, kind, live, archive;
	int series;           /* series id, for episodes */
} Item;

/* one browsable list: live channels, movies, series or episodes */
typedef struct {
	Arena arena;          /* items, groups and their strings */
	Item *items;
	const char **groups;  /* groups[0] is "All ..." */
	int *gcount;
	int nitems, ngroups;
	int loaded, loading;  /* loaded: a list arrived at least once */
	char err[256];
	Job *res[2];          /* Xtream answers waiting for their other half */
} Catalog;

typedef struct {
	const Source *src;
	Catalog cat[3];       /* KindLive, KindVod, KindSeries */

	/* Xtream account */
	Job *auth;
	int authok, authloading, active, maxconn;
	char status[32], exp[16], ext[8], autherr[256];
} Playlist;
