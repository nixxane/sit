/* See LICENSE file for copyright and license details. */
enum { JobXtream, JobImg, JobM3u, JobXml };

typedef struct Job Job;
struct Job {
	char *url, *cache;     /* cache: file to serve from and save to, or NULL */
	int maxage;            /* seconds a cached copy is fresh; 0 always fetches */
	int json;              /* parse the body into root on the worker */
	void (*work)(Job *);   /* or run this on the worker with the body */
	int owner, kind, sub, id;  /* for the caller */
	void *ud;
	char *aux;             /* input for work(), copied for a stale twin */
	void *res;             /* output of work(), lives in arena */
	int fromcache, stale;  /* served from disk; stale: a network result follows */
	char *data;
	size_t len, cap;
	JVal *root;
	Arena arena;
	unsigned char *px;     /* for work(): decoded RGBA image of w x h */
	int w, h;
	char err[256];
	Job *next;
};

void net_init(unsigned int evdone, const char *useragent);
Job *net_job(const char *url, const char *cache, int maxage, int json);
void net_submit(Job *j);
void net_free(Job *j);
void net_quit(void);
