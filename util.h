/* See LICENSE file for copyright and license details. */
#define LEN(a)          (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) (MIN(MAX((x), (lo)), (hi)))

typedef struct Block Block;
typedef struct { Block *head; } Arena;

enum { DirConfig, DirCache, DirData };

void die(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
void *arena_alloc(Arena *a, size_t n);
char *arena_strdup(Arena *a, const char *s);
void arena_free(Arena *a);
unsigned char *readfile(const char *path, size_t *len);
int fileexists(const char *path);
double fileage(const char *path);
int mkdirp(const char *path);
const char *appdir(int which);
unsigned long long fnv1a(const char *s);
void urlenc(char *dst, size_t n, const char *s);
int utf8dec(const char **s);
int utf8ok(const char *s);
int b64dec(const char *s, char *dst, size_t n);
double now(void);
float approach(float cur, float target, float dt, float speed, float eps);
void fmttime(char *buf, size_t n, double secs);
