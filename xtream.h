/* See LICENSE file for copyright and license details. */
enum { ResAuth, ResCats, ResItems, ResInfo, ResEpg };

void xt_account(Playlist *p, int ttl, int force);
void xt_load(Playlist *p, int kind, int ttl, int force);
void xt_info(Playlist *p, int kind, int id);
void xt_epg(Playlist *p, int id, int full);
int xt_result(Playlist *p, Job *j);
void xt_episodes(Catalog *c, Job *info, int sid, const char *sname);
void xt_free(Catalog *c);
void xt_url(const Playlist *p, int kind, const Item *it, char *buf, size_t n);
void xt_catchup(const Playlist *p, int id, const char *local, long long start, int minutes,
                char *buf, size_t n);
