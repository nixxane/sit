/* See LICENSE file for copyright and license details. */
typedef struct {
	int loaded, paused, buffering, ended, mute;
	double pos, dur, volume;
	char title[256], err[256];
	Target fb;    /* the video, rendered offscreen at display size */
} Player;

extern Player pl;

void player_init(unsigned int evbase, const char *opts[][2], int nopts);
void player_events(void);
int player_render(int w, int h);
void player_swapped(void);
void player_load(const char *url, double start);
void player_mask(const char *secret);
void player_stop(void);
void player_cycle(const char *prop);
void player_seek(double secs);
void player_setvol(double v);
char *player_prop(const char *name, char *buf, size_t n);
void player_quit(void);
