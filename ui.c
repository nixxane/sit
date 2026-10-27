/* See LICENSE file for copyright and license details. */
#include <math.h>
#include <stddef.h>

#include "util.h"
#include "gfx.h"
#include "ui.h"

float ui_dt;

void
ui_list_reset(List *l, int n)
{
	l->n = n;
	l->sel = 0;
	l->follow = 1;
	l->scroll = l->target = l->pill = 0;
}

void
ui_list_move(List *l, int d)
{
	if (!l->n)
		return;
	l->sel = CLAMP(l->sel + d, 0, l->n - 1);
	l->follow = 1;
}

void
ui_list_wheel(List *l, float rows, float rowh)
{
	l->target -= rows * rowh * 3;
	l->follow = 0;
}

int
ui_list_hit(const List *l, Rect r, float rowh, float x, float y)
{
	int i;

	if (!inrect(r, x, y))
		return -1;
	i = (int)((y - r.y + floorf(l->scroll)) / rowh);
	return i < l->n ? i : -1;
}

/* row-major grid of cols columns; l->sel indexes cells, fn draws one
 * cell and its selection. Returns nonzero while still scrolling */
int
ui_grid(List *l, Rect r, int cols, float cellh, float gap, const ListStyle *st, Rowfn fn)
{
	float rowh = cellh + gap, cw = (r.w - gap * (cols - 1)) / cols, maxs, m, off, th;
	int rows = (l->n + cols - 1) / cols, srow = l->sel / cols, i, last;

	maxs = MAX(0, rows * rowh - gap - r.h);
	if (l->follow) {
		m = MIN(gap * 2, MAX(0, (r.h - cellh) / 2));
		if (srow * rowh - m < l->target)
			l->target = srow * rowh - m;
		if (srow * rowh + cellh + m > l->target + r.h)
			l->target = srow * rowh + cellh + m - r.h;
	}
	l->target = CLAMP(l->target, 0, maxs);
	l->scroll = approach(l->scroll, l->target, ui_dt, 20, 0.5f);

	gfx_clip(r);
	off = floorf(l->scroll);
	last = MIN(l->n, ((int)((off + r.h) / rowh) + 1) * cols);
	for (i = (int)(off / rowh) * cols; i < last; i++)
		fn(i, (Rect){ roundf(r.x + (i % cols) * (cw + gap)), r.y + (i / cols) * rowh - off,
		              floorf(cw), cellh }, i == l->sel);
	gfx_noclip();
	if (maxs > 0) {    /* just right of the grid */
		th = MAX(r.h * r.h / (rows * rowh), rowh / 2);
		gfx_rect((Rect){ r.x + r.w + st->accentw, r.y + (r.h - th) * off / maxs,
		                 st->accentw, th }, st->bar, st->accentw / 2);
	}
	return l->scroll != l->target;
}

int
ui_grid_hit(const List *l, Rect r, int cols, float cellh, float gap, float x, float y)
{
	float rowh = cellh + gap, cw = (r.w - gap * (cols - 1)) / cols;
	float lx = x - r.x, ly = y - r.y + floorf(l->scroll);
	int i;

	if (!inrect(r, x, y) || fmodf(lx, cw + gap) > cw || fmodf(ly, rowh) > cellh)
		return -1;
	i = (int)(ly / rowh) * cols + (int)(lx / (cw + gap));
	return i < l->n ? i : -1;
}

/* draw a scrolling list with an animated selection pill; returns
 * nonzero while still animating */
int
ui_list(List *l, Rect r, float rowh, const ListStyle *st, Rowfn fn)
{
	float maxs = MAX(0, l->n * rowh - r.h), m, off, py, th;
	int i, last;

	if (l->follow) {
		m = MIN(rowh * 2, MAX(0, (r.h - rowh) / 2));
		if (l->sel * rowh - m < l->target)
			l->target = l->sel * rowh - m;
		if ((l->sel + 1) * rowh + m > l->target + r.h)
			l->target = (l->sel + 1) * rowh + m - r.h;
	}
	l->target = CLAMP(l->target, 0, maxs);
	l->scroll = approach(l->scroll, l->target, ui_dt, 20, 0.5f);
	l->pill = approach(l->pill, l->sel * rowh, ui_dt, 28, 0.5f);

	gfx_clip(r);
	off = floorf(l->scroll);
	if (l->n) {
		py = r.y + floorf(l->pill) - off;
		gfx_rect((Rect){ r.x, py, r.w, rowh }, st->pill, st->radius);
		gfx_rect((Rect){ r.x, py + rowh * 0.3f, st->accentw, rowh * 0.4f },
		         st->accent, st->accentw / 2);
	}
	last = MIN(l->n, (int)((off + r.h) / rowh) + 1);
	for (i = (int)(off / rowh); i < last; i++)
		fn(i, (Rect){ r.x, r.y + i * rowh - off, r.w, rowh }, i == l->sel);
	if (maxs > 0) {
		th = MAX(r.h * r.h / (l->n * rowh), rowh);
		gfx_rect((Rect){ r.x + r.w - st->accentw, r.y + (r.h - th) * off / maxs,
		                 st->accentw, th }, st->bar, st->accentw / 2);
	}
	gfx_noclip();
	return l->scroll != l->target || l->pill != l->sel * rowh;
}
