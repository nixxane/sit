/* See LICENSE file for copyright and license details. */
enum { ListFav, ListRecent };

/* a favorite or a recently played item, enough to show and play it
 * without its catalog */
typedef struct {
	char *src, *name, *logo, *ext, *sname;
	int kind, id, series;
	long long time;
} Mark;

void store_load(void);
double store_get(const char *src, int kind, int id, double *dur);
void store_set(const char *src, int kind, int id, double pos, double dur);
void store_save(void);

int store_marks(int list, Mark **marks);
Mark *store_find(int list, const char *src, int kind, int id, int series);
void store_add(int list, const Mark *m);
void store_del(int list, Mark *m);
unsigned long store_version(void);
