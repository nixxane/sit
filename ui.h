/* See LICENSE file for copyright and license details. */
typedef struct {
	int n, sel, follow;    /* follow: keep sel scrolled into view */
	float scroll, target, pill;
} List;

typedef struct {
	Color pill, accent, bar;
	float radius, accentw;
} ListStyle;

typedef void (*Rowfn)(int i, Rect r, int sel);

extern float ui_dt;

int ui_list(List *l, Rect r, float rowh, const ListStyle *st, Rowfn fn);
int ui_list_hit(const List *l, Rect r, float rowh, float x, float y);
int ui_grid(List *l, Rect r, int cols, float cellh, float gap, const ListStyle *st, Rowfn fn);
int ui_grid_hit(const List *l, Rect r, int cols, float cellh, float gap, float x, float y);
void ui_list_move(List *l, int d);
void ui_list_wheel(List *l, float rows, float rowh);
void ui_list_reset(List *l, int n);
