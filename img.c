/* See LICENSE file for copyright and license details.
 *
 * Posters and logos: fetched, decoded and shrunk on the net thread,
 * uploaded here, kept in an LRU of textures capped by size. Only a few loads run
 * at once so fast scrolling does not queue thousands of downloads; cards
 * that are still on screen ask again on the next frame.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include <stb_image.h>

#include "util.h"
#include "gfx.h"
#include "json.h"
#include "net.h"
#include "img.h"

#define MAXIMG    256
#define BUDGET    (64u << 20)    /* bytes of textures kept */
#define MAXFLIGHT 10
#define IMGTTL    (30 * 86400)

static Img cache[MAXIMG];
static unsigned long frame = 1;    /* used == 0 marks a free slot */
static size_t bytes;
static int inflight;

static void
drop(Img *im)
{
	if (im->tex) {
		gfx_freetex(im->tex);
		bytes -= (size_t)im->w * im->h * 4;
	}
	memset(im, 0, sizeof *im);
}

/* evict the least recently used until under budget; never what is on
 * screen this frame */
static void
trim(void)
{
	Img *old;
	int i;

	while (bytes > BUDGET) {
		old = NULL;
		for (i = 0; i < MAXIMG; i++)
			if (cache[i].tex && cache[i].used < frame && (!old || cache[i].used < old->used))
				old = &cache[i];
		if (!old)
			return;
		drop(old);
	}
}

/* box filter; posters can be 2000px tall and we show them at 200 */
static unsigned char *
shrink(const unsigned char *s, int w, int h, int nw, int nh)
{
	unsigned char *d = xmalloc((size_t)nw * nh * 4), *o = d;
	int x, y, sx, sy, x0, x1, y0, y1, c, n;
	unsigned long sum[4];

	for (y = 0; y < nh; y++) {
		y0 = y * h / nh;
		y1 = MAX(y0 + 1, (y + 1) * h / nh);
		for (x = 0; x < nw; x++) {
			x0 = x * w / nw;
			x1 = MAX(x0 + 1, (x + 1) * w / nw);
			sum[0] = sum[1] = sum[2] = sum[3] = 0;
			for (sy = y0; sy < y1; sy++)
				for (sx = x0; sx < x1; sx++)
					for (c = 0; c < 4; c++)
						sum[c] += s[((size_t)sy * w + sx) * 4 + c];
			n = (y1 - y0) * (x1 - x0);
			for (c = 0; c < 4; c++)
				*o++ = (unsigned char)(sum[c] / n);
		}
	}
	return d;
}

/* on the net thread; j->w holds the wanted width on the way in */
static void
decode(Job *j)
{
	unsigned char *p;
	int w, h, n, maxw = j->w;

	if (!(p = stbi_load_from_memory((unsigned char *)j->data, (int)j->len, &w, &h, &n, 4))) {
		snprintf(j->err, sizeof j->err, "cannot decode image");
		return;
	}
	if (maxw > 0 && w > maxw) {
		j->px = shrink(p, w, h, maxw, MAX(1, h * maxw / w));
		j->w = maxw;
		j->h = MAX(1, h * maxw / w);
		stbi_image_free(p);
	} else {
		j->px = xmalloc((size_t)w * h * 4);
		memcpy(j->px, p, (size_t)w * h * 4);
		j->w = w;
		j->h = h;
		stbi_image_free(p);
	}
}

static unsigned long long
imgkey(const char *url, int maxw)
{
	char key[2100];

	snprintf(key, sizeof key, "%d %s", maxw, url);
	return fnv1a(key);
}

/* the texture for url at most maxw pixels wide, or NULL until loaded */
Img *
img_get(const char *url, int maxw)
{
	char path[1100];
	unsigned long long k;
	Img *im, *old = NULL;
	Job *j;
	int i;

	if (!url || strncmp(url, "http", 4))
		return NULL;
	maxw = (maxw + 63) & ~63;    /* a few sizes, not one per pixel */
	k = imgkey(url, maxw);
	for (i = 0; i < MAXIMG; i++) {
		im = &cache[i];
		if (im->key == k && im->used) {
			im->used = frame;
			return im->state == ImgLoading ? NULL : im;
		}
		if (im->state != ImgLoading && (!old || im->used < old->used))
			old = im;
	}
	if (inflight >= MAXFLIGHT || !old)
		return NULL;
	drop(old);
	old->key = k;
	old->used = frame;
	old->state = ImgLoading;

	snprintf(path, sizeof path, "%s/img/%016llx", appdir(DirCache), fnv1a(url));
	j = net_job(url, path, IMGTTL, 0);
	j->owner = JobImg;
	j->work = decode;
	j->w = j->sub = maxw;
	j->ud = (void *)(intptr_t)(old - cache);
	net_submit(j);
	inflight++;
	return NULL;
}

void
img_result(Job *j)
{
	Img *im = &cache[(intptr_t)j->ud];

	if (!j->stale)
		inflight--;
	if (im->key != imgkey(j->url, j->sub)) {
		/* slot reused while a refresh was in flight */
	} else if (j->px) {
		if (im->tex) {
			gfx_freetex(im->tex);
			bytes -= (size_t)im->w * im->h * 4;
		}
		im->tex = gfx_texture(j->px, j->w, j->h);
		im->w = j->w;
		im->h = j->h;
		im->state = ImgReady;
		bytes += (size_t)im->w * im->h * 4;
		trim();
	} else if (im->state == ImgLoading) {
		im->state = ImgFailed;
	}
	net_free(j);
}

void
img_tick(void)
{
	frame++;
}
