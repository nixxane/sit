/* See LICENSE file for copyright and license details. */
typedef struct { float x, y, w, h; } Rect;
typedef struct { unsigned char r, g, b, a; } Color;
typedef struct { unsigned int fbo, tex; int w, h; } Target;

Color rgba(unsigned int rgb, float a);
int inrect(Rect r, float x, float y);

void gfx_init(void);
int gfx_font(const char *path, float px);
void gfx_fallback(const char *path);
void gfx_bake(void);

void gfx_begin(int w, int h, Color bg);
void gfx_end(void);
void gfx_rect(Rect r, Color c, float radius);
void gfx_image(unsigned int tex, Rect r, Color tint, float radius);
void gfx_image_uv(unsigned int tex, Rect r, Rect uv, Color tint, float radius);
unsigned int gfx_texture(const unsigned char *rgba, int w, int h);
void gfx_freetex(unsigned int tex);
float gfx_text(int font, float x, float y, const char *s, Color c, float maxw);
int gfx_wrap(int font, float x, float y, float w, const char *s, Color c, int maxlines);
float gfx_textw(int font, const char *s);
float gfx_lineh(int font);
float gfx_ascent(int font);
void gfx_clip(Rect r);
void gfx_noclip(void);

void gfx_target(Target *t, int w, int h);
void gfx_target_clear(Target *t);
