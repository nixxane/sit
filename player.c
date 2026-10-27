/* See LICENSE file for copyright and license details. */
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include "util.h"
#include "gfx.h"
#include "player.h"

enum { PPause = 1, PBuffering, PPos, PDur, PTitle, PVolume, PMute };

Player pl;
static mpv_handle *mpv;
static mpv_render_context *rctx;
static unsigned int evbase;
static char lasterr[256];
static char *secrets[8];

/* mpv calls these from its own threads; just poke the main loop */
static void
push(unsigned int type)
{
	SDL_Event e;

	SDL_zero(e);
	e.type = type;
	SDL_PushEvent(&e);
}

static void
onwakeup(void *ud)
{
	(void)ud;
	push(evbase);
}

static void
onupdate(void *ud)
{
	(void)ud;
	push(evbase + 1);
}

static void *
getproc(void *ud, const char *name)
{
	(void)ud;
	return SDL_GL_GetProcAddress(name);
}

void
player_init(unsigned int ev, const char *opts[][2], int nopts)
{
	mpv_opengl_init_params gl = { .get_proc_address = getproc };
	mpv_render_param params[] = {
		{ MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL },
		{ MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl },
		{ 0, NULL },
	};
	int i;

	evbase = ev;
	if (!(mpv = mpv_create()))
		die("mpv_create failed");
	mpv_set_option_string(mpv, "vo", "libmpv");
	mpv_set_option_string(mpv, "idle", "yes");
	mpv_set_option_string(mpv, "osd-level", "0");
	mpv_set_option_string(mpv, "input-default-bindings", "no");
	mpv_set_option_string(mpv, "ytdl", "no");
	for (i = 0; i < nopts; i++)
		if (mpv_set_option_string(mpv, opts[i][0], opts[i][1]) < 0)
			fprintf(stderr, "sit: bad mpv option %s=%s\n", opts[i][0], opts[i][1]);
	if (mpv_initialize(mpv) < 0)
		die("mpv_initialize failed");
	mpv_request_log_messages(mpv, "error");

	mpv_observe_property(mpv, PPause, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv, PBuffering, "paused-for-cache", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv, PPos, "time-pos", MPV_FORMAT_DOUBLE);
	mpv_observe_property(mpv, PDur, "duration", MPV_FORMAT_DOUBLE);
	mpv_observe_property(mpv, PTitle, "media-title", MPV_FORMAT_STRING);
	mpv_observe_property(mpv, PVolume, "volume", MPV_FORMAT_DOUBLE);
	mpv_observe_property(mpv, PMute, "mute", MPV_FORMAT_FLAG);

	if (mpv_render_context_create(&rctx, mpv, params) < 0)
		die("cannot create mpv OpenGL renderer");
	mpv_set_wakeup_callback(mpv, onwakeup, NULL);
	mpv_render_context_set_update_callback(rctx, onupdate, NULL);
}

static void
property(int id, mpv_event_property *p)
{
	int ok = p->format != MPV_FORMAT_NONE;

	switch (id) {
	case PPause:     pl.paused = ok && *(int *)p->data; break;
	case PBuffering: pl.buffering = ok && *(int *)p->data; break;
	case PMute:      pl.mute = ok && *(int *)p->data; break;
	/* after the end, keep the last position so it can be saved as watched */
	case PPos:       if (ok || !pl.ended) pl.pos = ok ? *(double *)p->data : 0; break;
	case PDur:       if (ok || !pl.ended) pl.dur = ok ? *(double *)p->data : 0; break;
	case PVolume:    pl.volume = ok ? *(double *)p->data : 0; break;
	case PTitle:
		snprintf(pl.title, sizeof pl.title, "%s", ok ? *(char **)p->data : "");
		break;
	}
}

/* stream urls carry credentials and mpv puts urls in its errors */
void
player_mask(const char *secret)
{
	size_t i;

	if (!*secret)
		return;
	for (i = 0; i < LEN(secrets) && secrets[i]; i++)
		if (!strcmp(secrets[i], secret))
			return;
	if (i < LEN(secrets))
		secrets[i] = xstrdup(secret);
}

static void
redact(char *s, size_t n)
{
	char out[sizeof pl.err], *p;
	size_t i, k, len;

	for (i = 0; i < LEN(secrets) && secrets[i]; i++) {
		len = strlen(secrets[i]);
		for (k = 0, p = s; *p && k + 4 < sizeof out; ) {
			if (!strncmp(p, secrets[i], len)) {
				memcpy(out + k, "***", 3);
				k += 3;
				p += len;
			} else {
				out[k++] = *p++;
			}
		}
		out[k] = '\0';
		snprintf(s, n, "%s", out);
	}
}

void
player_events(void)
{
	mpv_event *e;
	mpv_event_end_file *ef;
	mpv_event_log_message *m;
	size_t n;

	while ((e = mpv_wait_event(mpv, 0))->event_id != MPV_EVENT_NONE) {
		switch (e->event_id) {
		case MPV_EVENT_START_FILE:
			pl.loaded = pl.ended = 0;
			pl.err[0] = lasterr[0] = '\0';
			gfx_target_clear(&pl.fb);
			break;
		case MPV_EVENT_FILE_LOADED:
			pl.loaded = 1;
			break;
		case MPV_EVENT_END_FILE:
			ef = e->data;
			if (ef->reason == MPV_END_FILE_REASON_ERROR) {
				snprintf(pl.err, sizeof pl.err, "%s",
				         lasterr[0] ? lasterr : mpv_error_string(ef->error));
				redact(pl.err, sizeof pl.err);
			} else if (ef->reason == MPV_END_FILE_REASON_EOF)
				pl.ended = 1;
			break;
		case MPV_EVENT_LOG_MESSAGE:
			m = e->data;
			snprintf(lasterr, sizeof lasterr, "%s", m->text);
			if ((n = strlen(lasterr)) && lasterr[n - 1] == '\n')
				lasterr[n - 1] = '\0';
			break;
		case MPV_EVENT_PROPERTY_CHANGE:
			property((int)e->reply_userdata, e->data);
			break;
		default:
			break;
		}
	}
}

/* render the current video frame into pl.fb if there is a new one or
 * the size changed; returns 1 if the texture was updated */
int
player_render(int w, int h)
{
	mpv_opengl_fbo fbo = { 0 };
	/* blocking until the frame's display time would pace the whole UI
	 * at the video's frame rate */
	int flip = 0, resized, block = 0;
	mpv_render_param params[] = {
		{ MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
		{ MPV_RENDER_PARAM_FLIP_Y, &flip },
		{ MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block },
		{ 0, NULL },
	};

	if (w <= 0 || h <= 0)
		return 0;
	resized = pl.fb.w != w || pl.fb.h != h;
	gfx_target(&pl.fb, w, h);
	if (!(mpv_render_context_update(rctx) & MPV_RENDER_UPDATE_FRAME) && !resized)
		return 0;
	fbo.fbo = (int)pl.fb.fbo;
	fbo.w = w;
	fbo.h = h;
	mpv_render_context_render(rctx, params);
	return 1;
}

void
player_swapped(void)
{
	mpv_render_context_report_swap(rctx);
}

/* start: seconds to begin at, 0 for the start (or live edge) */
void
player_load(const char *url, double start)
{
	char opt[64];
	const char *cmd[] = { "loadfile", url, "replace", "-1", opt, NULL };

	snprintf(opt, sizeof opt, "start=%.0f", start);
	if (start <= 0)
		cmd[2] = NULL;
	pl.loaded = pl.ended = 0;
	pl.pos = pl.dur = 0;
	mpv_command_async(mpv, 0, cmd);
}

void
player_stop(void)
{
	const char *cmd[] = { "stop", NULL };

	mpv_command(mpv, cmd);
	gfx_target_clear(&pl.fb);
}

void
player_cycle(const char *prop)
{
	const char *cmd[] = { "cycle", prop, NULL };

	mpv_command(mpv, cmd);
}

void
player_seek(double secs)
{
	char buf[32];
	const char *cmd[] = { "seek", buf, "relative", NULL };

	snprintf(buf, sizeof buf, "%.1f", secs);
	mpv_command_async(mpv, 0, cmd);
}

void
player_setvol(double v)
{
	mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

char *
player_prop(const char *name, char *buf, size_t n)
{
	char *s = mpv_get_property_string(mpv, name);

	snprintf(buf, n, "%s", s ? s : "");
	mpv_free(s);
	return buf;
}

void
player_quit(void)
{
	mpv_render_context_free(rctx);
	mpv_terminate_destroy(mpv);
}
