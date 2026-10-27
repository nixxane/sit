/* See LICENSE file for copyright and license details.
 *
 * One worker thread runs every transfer through curl multi. Finished
 * jobs come back to the main loop as SDL user events (data1 = Job *).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <curl/curl.h>

#include "util.h"
#include "json.h"
#include "net.h"

#define MAXBODY (512u << 20)

static SDL_Thread *thr;
static SDL_mutex *mu;
static SDL_cond *cv;
static CURLM *multi;
static Job *qhead, *qtail;
static int quitting, active;
static unsigned int evdone;
static const char *ua;

Job *
net_job(const char *url, const char *cache, int maxage, int json)
{
	Job *j = xcalloc(1, sizeof *j);

	j->url = xstrdup(url);
	j->cache = cache ? xstrdup(cache) : NULL;
	j->maxage = maxage;
	j->json = json;
	return j;
}

void
net_free(Job *j)
{
	if (!j)
		return;
	free(j->url);
	free(j->cache);
	free(j->data);
	free(j->aux);
	free(j->px);
	arena_free(&j->arena);
	free(j);
}

static void
deliver(Job *j)
{
	SDL_Event e;
	const char *err;

	if (j->work && j->data)
		j->work(j);
	if (j->json && j->data) {
		if (!(j->root = json_parse(&j->arena, j->data, &err)))
			snprintf(j->err, sizeof j->err, "bad JSON from server: %s", err);
		free(j->data);    /* the tree holds copies */
		j->data = NULL;
	}
	SDL_zero(e);
	e.type = evdone;
	e.user.data1 = j;
	SDL_PushEvent(&e);
}

static int
loadcache(Job *j)
{
	return j->cache && (j->data = (char *)readfile(j->cache, &j->len)) != NULL;
}

static void
savecache(Job *j)
{
	char dir[1024], tmp[1100], *slash;
	FILE *f;

	if (!j->cache)
		return;
	snprintf(dir, sizeof dir, "%s", j->cache);
	if ((slash = strrchr(dir, '/')))
		*slash = '\0';
	mkdirp(dir);
	snprintf(tmp, sizeof tmp, "%s.tmp", j->cache);
	if (!(f = fopen(tmp, "wb")))
		return;
	if (fwrite(j->data, 1, j->len, f) != j->len) {
		fclose(f);
		remove(tmp);
		return;
	}
	fclose(f);
	remove(j->cache);    /* rename() does not replace on Windows */
	rename(tmp, j->cache);
}

static size_t
onbody(char *p, size_t sz, size_t n, void *ud)
{
	Job *j = ud;
	size_t len = sz * n;

	if (j->len + len + 1 > MAXBODY)
		return 0;
	if (j->len + len + 1 > j->cap) {
		j->cap = MAX(j->cap * 2, j->len + len + 1 + 65536);
		j->data = xrealloc(j->data, j->cap);
	}
	memcpy(j->data + j->len, p, len);
	j->len += len;
	j->data[j->len] = '\0';
	return len;
}

static void
start(Job *j)
{
	double age = fileage(j->cache);
	Job *c;
	CURL *e;

	if (age >= 0 && j->maxage > 0) {
		if (age < j->maxage && loadcache(j)) {
			j->fromcache = 1;
			deliver(j);
			return;
		}
		/* old copy now, fresh one when it arrives */
		c = net_job(j->url, j->cache, 0, j->json);
		c->owner = j->owner;
		c->kind = j->kind;
		c->sub = j->sub;
		c->id = j->id;
		c->work = j->work;
		c->w = j->w;
		c->ud = j->ud;
		c->aux = j->aux ? xstrdup(j->aux) : NULL;
		c->fromcache = c->stale = 1;
		if (loadcache(c))
			deliver(c);
		else
			net_free(c);
	}
	if (!(e = curl_easy_init())) {
		snprintf(j->err, sizeof j->err, "curl_easy_init failed");
		deliver(j);
		return;
	}
	curl_easy_setopt(e, CURLOPT_URL, j->url);
	curl_easy_setopt(e, CURLOPT_PRIVATE, (void *)j);
	curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, onbody);
	curl_easy_setopt(e, CURLOPT_WRITEDATA, (void *)j);
	curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(e, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
#ifdef _WIN32
	curl_easy_setopt(e, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
	if (ua)
		curl_easy_setopt(e, CURLOPT_USERAGENT, ua);
	curl_multi_add_handle(multi, e);
	active++;
}

static void
finish(CURL *e, CURLcode rc)
{
	char *priv;
	long code = 0;
	Job *j;

	curl_easy_getinfo(e, CURLINFO_PRIVATE, &priv);
	curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
	curl_multi_remove_handle(multi, e);
	curl_easy_cleanup(e);
	active--;
	j = (Job *)priv;
	j->fromcache = j->stale = 0;
	if (rc != CURLE_OK)
		snprintf(j->err, sizeof j->err, "%s", curl_easy_strerror(rc));
	else if (code != 200 && strncmp(j->url, "file:", 5))
		snprintf(j->err, sizeof j->err, "server answered HTTP %ld", code);
	else if (!j->data)
		snprintf(j->err, sizeof j->err, "empty response");
	if (j->err[0]) {
		free(j->data);
		j->data = NULL;
	} else {
		savecache(j);
	}
	deliver(j);
}

static int
worker(void *ud)
{
	CURLMsg *m;
	Job *q, *j;
	int n;

	(void)ud;
	for (;;) {
		SDL_LockMutex(mu);
		while (!qhead && !active && !quitting)
			SDL_CondWait(cv, mu);
		q = qhead;
		qhead = qtail = NULL;
		n = quitting;
		SDL_UnlockMutex(mu);
		if (n)
			break;
		while ((j = q)) {
			q = q->next;
			start(j);
		}
		if (!active)
			continue;
		curl_multi_perform(multi, &n);
		while ((m = curl_multi_info_read(multi, &n)))
			if (m->msg == CURLMSG_DONE)
				finish(m->easy_handle, m->data.result);
		if (active)
			curl_multi_poll(multi, NULL, 0, 1000, NULL);
	}
	return 0;
}

void
net_init(unsigned int ev, const char *useragent)
{
	evdone = ev;
	ua = useragent;
	if (curl_global_init(CURL_GLOBAL_DEFAULT))
		die("curl_global_init failed");
	if (!(multi = curl_multi_init()) || !(mu = SDL_CreateMutex()) || !(cv = SDL_CreateCond()))
		die("cannot set up networking");
	/* posters come in bursts; be polite to image hosts */
	curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);
	if (!(thr = SDL_CreateThread(worker, "net", NULL)))
		die("SDL_CreateThread: %s", SDL_GetError());
}

void
net_submit(Job *j)
{
	SDL_LockMutex(mu);
	j->next = NULL;
	if (qtail)
		qtail->next = j;
	else
		qhead = j;
	qtail = j;
	SDL_CondSignal(cv);
	SDL_UnlockMutex(mu);
	curl_multi_wakeup(multi);
}

void
net_quit(void)
{
	SDL_LockMutex(mu);
	quitting = 1;
	SDL_CondSignal(cv);
	SDL_UnlockMutex(mu);
	curl_multi_wakeup(multi);
	SDL_WaitThread(thr, NULL);
	curl_multi_cleanup(multi);
	curl_global_cleanup();
}
