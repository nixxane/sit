/* See LICENSE file for copyright and license details. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_opengl.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include "util.h"
#include "gfx.h"

#define MAXFONTS 8
#define MAXVTX   (6 * 8192)
#define NRANGE   (95 + 96 + 128 + 144 + 256 + 112 + 256 + 128 + 176 + 144)
#define NEXTRA   13
#define NGLYPHS  (NRANGE + NEXTRA)
#define MAXFB    4
#define MAXCP    1024

/* everything past OpenGL 1.1 has to be fetched at runtime on Windows */
#define GLFUNCS \
	X(PFNGLACTIVETEXTUREPROC,            glActiveTexture) \
	X(PFNGLATTACHSHADERPROC,             glAttachShader) \
	X(PFNGLBINDBUFFERPROC,               glBindBuffer) \
	X(PFNGLBINDFRAMEBUFFERPROC,          glBindFramebuffer) \
	X(PFNGLBINDVERTEXARRAYPROC,          glBindVertexArray) \
	X(PFNGLBUFFERDATAPROC,               glBufferData) \
	X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,   glCheckFramebufferStatus) \
	X(PFNGLCOMPILESHADERPROC,            glCompileShader) \
	X(PFNGLCREATEPROGRAMPROC,            glCreateProgram) \
	X(PFNGLCREATESHADERPROC,             glCreateShader) \
	X(PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray) \
	X(PFNGLFRAMEBUFFERTEXTURE2DPROC,     glFramebufferTexture2D) \
	X(PFNGLGENBUFFERSPROC,               glGenBuffers) \
	X(PFNGLGENFRAMEBUFFERSPROC,          glGenFramebuffers) \
	X(PFNGLGENVERTEXARRAYSPROC,          glGenVertexArrays) \
	X(PFNGLGETPROGRAMINFOLOGPROC,        glGetProgramInfoLog) \
	X(PFNGLGETPROGRAMIVPROC,             glGetProgramiv) \
	X(PFNGLGETSHADERINFOLOGPROC,         glGetShaderInfoLog) \
	X(PFNGLGETSHADERIVPROC,              glGetShaderiv) \
	X(PFNGLGETUNIFORMLOCATIONPROC,       glGetUniformLocation) \
	X(PFNGLLINKPROGRAMPROC,              glLinkProgram) \
	X(PFNGLSHADERSOURCEPROC,             glShaderSource) \
	X(PFNGLUNIFORM1IPROC,                glUniform1i) \
	X(PFNGLUNIFORM2FPROC,                glUniform2f) \
	X(PFNGLUSEPROGRAMPROC,               glUseProgram) \
	X(PFNGLVERTEXATTRIBPOINTERPROC,      glVertexAttribPointer)

#define X(type, name) static type p##name;
GLFUNCS
#undef X

enum { ModeRect, ModeText, ModeImage };

typedef struct {
	float x, y, u, v;      /* position, texcoord */
	float lx, ly;          /* position relative to quad centre */
	float hw, hh, mode, rad;
	unsigned char c[4];
} Vtx;

typedef struct {
	const char *path;
	unsigned char *ttf;
	stbtt_fontinfo info;
	float px, ascent, lineh;
	stbtt_packedchar ch[NGLYPHS];
	unsigned char miss[NGLYPHS];    /* in none of the fonts */
} Font;

/* ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic, Hebrew, Arabic and
 * its presentation forms (what shaping turns letters into) */
static const struct { int first, n; } ranges[] = {
	{ 0x20, 95 }, { 0xa0, 96 }, { 0x100, 128 }, { 0x370, 144 }, { 0x400, 256 },
	{ 0x590, 112 }, { 0x600, 256 }, { 0xe00, 128 }, { 0xfb50, 176 }, { 0xfe70, 144 },
};

/* punctuation, arrows and the star the UI uses */
static int extracp[NEXTRA] = {
	0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026,
	0x2190, 0x2191, 0x2192, 0x2193, 0x2605,
};

/* fonts to take glyphs from when the main one lacks them */
static unsigned char *fbttf[MAXFB];
static stbtt_fontinfo fbinfo[MAXFB];
static int nfb;

static const char *vsrc =
	"#version 330 core\n"
	"layout(location = 0) in vec2 apos;\n"
	"layout(location = 1) in vec2 auv;\n"
	"layout(location = 2) in vec2 aloc;\n"
	"layout(location = 3) in vec4 arect;\n"
	"layout(location = 4) in vec4 acol;\n"
	"uniform vec2 screen;\n"
	"out vec2 uv;\n"
	"out vec2 loc;\n"
	"flat out vec4 rect;\n"
	"out vec4 col;\n"
	"void main() {\n"
	"	uv = auv; loc = aloc; rect = arect; col = acol;\n"
	"	gl_Position = vec4(apos.x / screen.x * 2.0 - 1.0,\n"
	"	                   1.0 - apos.y / screen.y * 2.0, 0.0, 1.0);\n"
	"}\n";

/* rect.xy: half size, rect.z: mode, rect.w: corner radius */
static const char *fsrc =
	"#version 330 core\n"
	"in vec2 uv;\n"
	"in vec2 loc;\n"
	"flat in vec4 rect;\n"
	"in vec4 col;\n"
	"uniform sampler2D atlas;\n"
	"uniform sampler2D img;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"	vec4 c = col;\n"
	"	if (rect.z > 1.5)\n"
	"		c *= texture(img, uv);\n"
	"	else if (rect.z > 0.5)\n"
	"		c.a *= texture(atlas, uv).r;\n"
	"	if (rect.w > 0.0) {\n"
	"		vec2 q = abs(loc) - rect.xy + rect.w;\n"
	"		float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rect.w;\n"
	"		c.a *= clamp(0.5 - d, 0.0, 1.0);\n"
	"	}\n"
	"	frag = c;\n"
	"}\n";

static GLuint prog, vao, vbo, atlas, curimg;
static GLint uscreen;
static Vtx vtx[MAXVTX];
static int nvtx, W, H, atlasw;
static Font fonts[MAXFONTS];
static int nfonts;

Color
rgba(unsigned int rgb, float a)
{
	Color c;

	c.r = rgb >> 16 & 0xff;
	c.g = rgb >> 8 & 0xff;
	c.b = rgb & 0xff;
	c.a = (unsigned char)(CLAMP(a, 0, 1) * 255 + 0.5f);
	return c;
}

int
inrect(Rect r, float x, float y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = pglCreateShader(type);
	GLint ok;
	char log[1024];

	pglShaderSource(s, 1, &src, NULL);
	pglCompileShader(s);
	pglGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		pglGetShaderInfoLog(s, sizeof log, NULL, log);
		die("shader: %s", log);
	}
	return s;
}

void
gfx_init(void)
{
	GLint ok;
	char log[1024];
	void *p;

#define X(type, name) \
	if (!(p = SDL_GL_GetProcAddress(#name))) \
		die("OpenGL function %s missing", #name); \
	memcpy(&p##name, &p, sizeof p);
	GLFUNCS
#undef X

	prog = pglCreateProgram();
	pglAttachShader(prog, shader(GL_VERTEX_SHADER, vsrc));
	pglAttachShader(prog, shader(GL_FRAGMENT_SHADER, fsrc));
	pglLinkProgram(prog);
	pglGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		pglGetProgramInfoLog(prog, sizeof log, NULL, log);
		die("shader link: %s", log);
	}
	pglUseProgram(prog);
	pglUniform1i(pglGetUniformLocation(prog, "atlas"), 0);
	pglUniform1i(pglGetUniformLocation(prog, "img"), 1);
	uscreen = pglGetUniformLocation(prog, "screen");

	pglGenVertexArrays(1, &vao);
	pglBindVertexArray(vao);
	pglGenBuffers(1, &vbo);
	pglBindBuffer(GL_ARRAY_BUFFER, vbo);
	pglEnableVertexAttribArray(0);
	pglEnableVertexAttribArray(1);
	pglEnableVertexAttribArray(2);
	pglEnableVertexAttribArray(3);
	pglEnableVertexAttribArray(4);
	pglVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, x));
	pglVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, u));
	pglVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, lx));
	pglVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)offsetof(Vtx, hw));
	pglVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void *)offsetof(Vtx, c));
}

/* queue a font; glyphs are rasterised by gfx_bake() */
int
gfx_font(const char *path, float px)
{
	Font *f;
	stbtt_fontinfo info;
	int asc, desc, gap;
	float scale;

	if (nfonts == MAXFONTS)
		die("too many fonts");
	f = &fonts[nfonts];
	if (!(f->ttf = readfile(path, NULL)))
		die("cannot read font %s", path);
	if (!stbtt_InitFont(&f->info, f->ttf, stbtt_GetFontOffsetForIndex(f->ttf, 0)))
		die("cannot parse font %s", path);
	info = f->info;
	scale = stbtt_ScaleForMappingEmToPixels(&info, px);
	stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
	f->path = path;
	f->px = px;
	f->ascent = roundf(asc * scale);
	f->lineh = roundf((asc - desc) * scale);
	return nfonts++;
}

void
gfx_fallback(const char *path)
{
	if (nfb == MAXFB || !(fbttf[nfb] = readfile(path, NULL)))
		return;
	if (stbtt_InitFont(&fbinfo[nfb], fbttf[nfb], stbtt_GetFontOffsetForIndex(fbttf[nfb], 0)))
		nfb++;
}

/* the codepoint of glyph slot i */
static int
cpat(int i)
{
	size_t k;

	for (k = 0; k < LEN(ranges); i -= ranges[k++].n)
		if (i < ranges[k].n)
			return ranges[k].first + i;
	return extracp[i];
}

/* 0: the main font has cp, k: fallback k-1 has it, -1: none does */
static int
fontfor(Font *f, int cp)
{
	int k;

	if (stbtt_FindGlyphIndex(&f->info, cp))
		return 0;
	for (k = 0; k < nfb; k++)
		if (stbtt_FindGlyphIndex(&fbinfo[k], cp))
			return k + 1;
	return -1;
}

/* each glyph comes from the first font that has it */
static int
pack(unsigned char *px, int size)
{
	static int owner[NGLYPHS], cps[NGLYPHS], idx[NGLYPHS];
	static stbtt_packedchar tmp[NGLYPHS];
	stbtt_pack_context pc;
	stbtt_pack_range r;
	unsigned char *ttf;
	Font *f;
	int i, j, s, n, ok = 1;

	memset(&pc, 0, sizeof pc);
	if (!stbtt_PackBegin(&pc, px, size, size, 0, 1, NULL))
		die("stbtt_PackBegin failed");
	for (i = 0; ok && i < nfonts; i++) {
		f = &fonts[i];
		for (j = 0; j < NGLYPHS; j++) {
			owner[j] = fontfor(f, cpat(j));
			f->miss[j] = owner[j] < 0;
		}
		for (s = 0; ok && s <= nfb; s++) {
			for (n = j = 0; j < NGLYPHS; j++)
				if (owner[j] == s) {
					cps[n] = cpat(j);
					idx[n++] = j;
				}
			if (!n)
				continue;
			ttf = s ? fbttf[s - 1] : f->ttf;
			memset(&r, 0, sizeof r);
			r.font_size = STBTT_POINT_SIZE(f->px);
			r.array_of_unicode_codepoints = cps;
			r.num_chars = n;
			r.chardata_for_range = tmp;
			ok = stbtt_PackFontRanges(&pc, ttf, stbtt_GetFontOffsetForIndex(ttf, 0), &r, 1);
			for (j = 0; j < n; j++)
				f->ch[idx[j]] = tmp[j];
		}
	}
	stbtt_PackEnd(&pc);
	return ok;
}

void
gfx_bake(void)
{
	unsigned char *px;

	/* big HiDPI scales need a bigger atlas */
	for (atlasw = 2048; ; atlasw *= 2) {
		px = xcalloc(atlasw, atlasw);
		if (pack(px, atlasw))
			break;
		free(px);
		if (atlasw >= 8192)
			die("font atlas full; lower the font sizes in config.h");
	}

	glGenTextures(1, &atlas);
	glBindTexture(GL_TEXTURE_2D, atlas);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlasw, atlasw, 0, GL_RED, GL_UNSIGNED_BYTE, px);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	free(px);
}

static void
flush(void)
{
	if (!nvtx)
		return;
	pglUseProgram(prog);
	pglBindVertexArray(vao);
	pglBindBuffer(GL_ARRAY_BUFFER, vbo);
	pglBufferData(GL_ARRAY_BUFFER, nvtx * sizeof *vtx, vtx, GL_STREAM_DRAW);
	pglUniform2f(uscreen, W, H);
	pglActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, curimg);
	pglActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDrawArrays(GL_TRIANGLES, 0, nvtx);
	nvtx = 0;
}

static void
vset(Vtx *v, float x, float y, float u, float t, float lx, float ly)
{
	v->x = x;
	v->y = y;
	v->u = u;
	v->v = t;
	v->lx = lx;
	v->ly = ly;
}

static void
quad(Rect r, float u0, float v0, float u1, float v1, Color c, int mode, float rad)
{
	float hw = r.w / 2, hh = r.h / 2, x1 = r.x + r.w, y1 = r.y + r.h;
	Vtx *v;
	int i;

	if (!c.a || r.w <= 0 || r.h <= 0)
		return;
	if (nvtx + 6 > MAXVTX)
		flush();
	v = vtx + nvtx;
	nvtx += 6;
	vset(v + 0, r.x, r.y, u0, v0, -hw, -hh);
	vset(v + 1, x1, r.y, u1, v0, hw, -hh);
	vset(v + 2, x1, y1, u1, v1, hw, hh);
	vset(v + 3, r.x, r.y, u0, v0, -hw, -hh);
	vset(v + 4, x1, y1, u1, v1, hw, hh);
	vset(v + 5, r.x, y1, u0, v1, -hw, hh);
	for (i = 0; i < 6; i++) {
		v[i].hw = hw;
		v[i].hh = hh;
		v[i].mode = mode;
		v[i].rad = MIN(rad, MIN(hw, hh));
		memcpy(v[i].c, &c, 4);
	}
}

void
gfx_begin(int w, int h, Color bg)
{
	W = w;
	H = h;
	nvtx = 0;
	pglBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, w, h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(bg.r / 255.0f, bg.g / 255.0f, bg.b / 255.0f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
}

void
gfx_end(void)
{
	flush();
}

void
gfx_rect(Rect r, Color c, float radius)
{
	quad(r, 0, 0, 0, 0, c, ModeRect, radius);
}

void
gfx_image(unsigned int tex, Rect r, Color tint, float radius)
{
	gfx_image_uv(tex, r, (Rect){ 0, 0, 1, 1 }, tint, radius);
}

/* uv: the part of the texture to show, in 0..1 */
void
gfx_image_uv(unsigned int tex, Rect r, Rect uv, Color tint, float radius)
{
	if (tex != curimg) {
		flush();
		curimg = tex;
	}
	quad(r, uv.x, uv.y, uv.x + uv.w, uv.y + uv.h, tint, ModeImage, radius);
}

unsigned int
gfx_texture(const unsigned char *rgba, int w, int h)
{
	GLuint t;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

void
gfx_freetex(unsigned int tex)
{
	GLuint t = tex;

	if (tex == curimg) {
		flush();
		curimg = 0;
	}
	glDeleteTextures(1, &t);
}

/* NULL for emoji, which are dropped rather than shown as '?' */
static stbtt_packedchar *
glyph(Font *f, int cp)
{
	size_t i;
	int off = 0;

	for (i = 0; i < LEN(ranges); off += ranges[i++].n)
		if (cp >= ranges[i].first && cp < ranges[i].first + ranges[i].n)
			return f->miss[off + cp - ranges[i].first] ? &f->ch['?' - 32]
			                                           : &f->ch[off + cp - ranges[i].first];
	for (i = 0; i < LEN(extracp); i++)
		if (extracp[i] == cp)
			return f->miss[NRANGE + i] ? &f->ch['?' - 32] : &f->ch[NRANGE + i];
	if (cp >= 0x1f000 || (cp >= 0x2600 && cp < 0x2800) || (cp >= 0xfe00 && cp < 0xfe10))
		return NULL;
	return &f->ch['?' - 32];
}

static float
adv(Font *f, int cp)
{
	stbtt_packedchar *g = glyph(f, cp);

	return g ? g->xadvance : 0;
}

/*
 * Right-to-left text. Arabic letters take their initial, medial, final
 * or isolated presentation form by their neighbours (lam + alef become
 * one ligature), then a reduced bidi pass orders the line: runs take the
 * direction of their letters, numbers read left to right, neutrals
 * between two runs of one direction join them, others follow the
 * paragraph (the direction of its first letter).
 */
enum { DirN, DirL, DirR };

/* isolated form and how many forms (1, 2: +final, 4: +initial, medial)
 * for U+0621..U+064A */
static const unsigned short arforms[][2] = {
	{ 0xfe80, 1 }, { 0xfe81, 2 }, { 0xfe83, 2 }, { 0xfe85, 2 }, { 0xfe87, 2 }, { 0xfe89, 4 },
	{ 0xfe8d, 2 }, { 0xfe8f, 4 }, { 0xfe93, 2 }, { 0xfe95, 4 }, { 0xfe99, 4 }, { 0xfe9d, 4 },
	{ 0xfea1, 4 }, { 0xfea5, 4 }, { 0xfea9, 2 }, { 0xfeab, 2 }, { 0xfead, 2 }, { 0xfeaf, 2 },
	{ 0xfeb1, 4 }, { 0xfeb5, 4 }, { 0xfeb9, 4 }, { 0xfebd, 4 }, { 0xfec1, 4 }, { 0xfec5, 4 },
	{ 0xfec9, 4 }, { 0xfecd, 4 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
	{ 0, 0 },    /* U+0640 tatweel */
	{ 0xfed1, 4 }, { 0xfed5, 4 }, { 0xfed9, 4 }, { 0xfedd, 4 }, { 0xfee1, 4 }, { 0xfee5, 4 },
	{ 0xfee9, 4 }, { 0xfeed, 2 }, { 0xfeef, 2 }, { 0xfef1, 4 },
};

/* Persian and Urdu letters outside that block */
static const struct { unsigned short c, form, n; } arextra[] = {
	{ 0x671, 0xfb50, 2 }, { 0x67e, 0xfb56, 4 }, { 0x686, 0xfb7a, 4 }, { 0x698, 0xfb8a, 2 },
	{ 0x6a9, 0xfb8e, 4 }, { 0x6af, 0xfb92, 4 }, { 0x6cc, 0xfbfc, 4 },
};

/* 'D' joins both sides, 'R' only the letter before, 'C' tatweel, 0 none */
static int
joins(int c, int *form, int *n)
{
	size_t i;

	*form = *n = 0;
	if (c == 0x640)
		return 'C';
	if (c >= 0x621 && c <= 0x64a && arforms[c - 0x621][1]) {
		*form = arforms[c - 0x621][0];
		*n = arforms[c - 0x621][1];
		return c == 0x621 ? 0 : *n == 4 || c == 0x649 ? 'D' : 'R';
	}
	for (i = 0; i < LEN(arextra); i++)
		if (arextra[i].c == c) {
			*form = arextra[i].form;
			*n = arextra[i].n;
			return *n == 4 ? 'D' : 'R';
		}
	return 0;
}

static int
lamalef(int c)
{
	switch (c) {
	case 0x622: return 0xfef5;
	case 0x623: return 0xfef7;
	case 0x625: return 0xfef9;
	case 0x627: return 0xfefb;
	}
	return 0;
}

static int
shape(const int *in, int n, int *out)
{
	int i, o = 0, t, form, nf, f2, n2, prev, next, k;

	for (i = 0; i < n; i++) {
		t = joins(in[i], &form, &nf);
		if (!form) {
			out[o++] = in[i];
			continue;
		}
		k = i > 0 ? joins(in[i - 1], &f2, &n2) : 0;
		prev = k == 'D' || k == 'C';
		next = t == 'D' && i + 1 < n && joins(in[i + 1], &f2, &n2);
		if (in[i] == 0x644 && i + 1 < n && (k = lamalef(in[i + 1]))) {
			out[o++] = k + prev;
			i++;
			continue;
		}
		k = prev && next ? 3 : prev ? 1 : next ? 2 : 0;
		out[o++] = form + (nf == 4 ? k : nf == 2 ? k & 1 : 0);
	}
	return o;
}

static int
isrtl(int c)
{
	return (c >= 0x590 && c <= 0x8ff) || (c >= 0xfb1d && c <= 0xfdff) || (c >= 0xfe70 && c <= 0xfefc);
}

static int
isdigitcp(int c)
{
	return (c >= '0' && c <= '9') || (c >= 0x660 && c <= 0x669) || (c >= 0x6f0 && c <= 0x6f9);
}

static int
dirof(int c)
{
	if (isdigitcp(c))
		return DirL;
	if (isrtl(c))
		return DirR;
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	    (c >= 0xc0 && !(c >= 0x2000 && c < 0x2c00)))
		return DirL;
	return DirN;
}

static int
mirror(int c)
{
	switch (c) {
	case '(': return ')';
	case ')': return '(';
	case '[': return ']';
	case ']': return '[';
	case '{': return '}';
	case '}': return '{';
	case '<': return '>';
	case '>': return '<';
	case 0xab: return 0xbb;
	case 0xbb: return 0xab;
	}
	return c;
}

static void
reverse(int *cp, unsigned char *d, int a, int b)
{
	int t;

	for (b--; a < b; a++, b--) {
		t = cp[a], cp[a] = cp[b], cp[b] = t;
		t = d[a], d[a] = d[b], d[b] = t;
	}
}

/* reorder logical cp[0..n) into display order; returns 1 for a
 * right-to-left paragraph */
static int
reorder(int *cp, int n)
{
	static unsigned char d[MAXCP];
	int open[32], i, j, k, para = DirL, a, b, want;

	for (i = 0; i < n; i++)
		d[i] = dirof(cp[i]);
	for (i = 0; i < n; i++)
		if (d[i] != DirN) {
			para = d[i];
			break;
		}
	for (i = 0; i < n; i = j) {
		for (j = i; j < n && d[j] == DirN; j++)
			;
		if (j == i) {
			j++;
			continue;
		}
		a = i > 0 ? d[i - 1] : para;
		b = j < n ? d[j] : para;
		for (k = i; k < j; k++)
			d[k] = a == b ? a : para;
	}
	/* a closing bracket goes the way its opening one went */
	for (i = 0, k = 0; i < n; i++) {
		if (cp[i] == '(' || cp[i] == '[' || cp[i] == '{') {
			if (k < (int)LEN(open))
				open[k++] = i;
		} else if ((cp[i] == ')' || cp[i] == ']' || cp[i] == '}') && k > 0) {
			d[i] = d[open[--k]];
		}
	}
	/* RTL: flip everything, then put left-to-right runs back */
	if (para == DirR)
		reverse(cp, d, 0, n);
	want = para == DirR ? DirL : DirR;
	for (i = 0; i < n; i = j) {
		for (j = i; j < n && d[j] == d[i]; j++)
			;
		if (d[i] == want)
			reverse(cp, d, i, j);
	}
	for (i = 0; i < n; i++)
		if (d[i] == DirR)
			cp[i] = mirror(cp[i]);
	return para == DirR;
}

/* s as codepoints in display order; marks and bidi controls dropped */
static int
visual(const char *s, int *out, int max, int *rtl)
{
	static int lg[MAXCP];
	int n = 0, c, any = 0;

	while (*s && n < max) {
		c = utf8dec(&s);
		if ((c >= 0x64b && c <= 0x65f) || c == 0x670 || (c >= 0x6d6 && c <= 0x6ed) ||
		    (c >= 0x591 && c <= 0x5c7 && c != 0x5be && c != 0x5c0 && c != 0x5c3 && c != 0x5c6) ||
		    (c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) ||
		    (c >= 0x2066 && c <= 0x2069))
			continue;
		any |= isrtl(c);
		lg[n++] = c;
	}
	*rtl = 0;
	if (!any) {
		memcpy(out, lg, n * sizeof *out);
		return n;
	}
	n = shape(lg, n, out);
	*rtl = reorder(out, n);
	return n;
}

static void
drawglyph(stbtt_packedchar *g, float *x, float base, Color c)
{
	float x0 = floorf(*x + g->xoff + 0.5f), y0 = floorf(base + g->yoff + 0.5f);
	float k = 1.0f / atlasw;
	Rect r;

	r.x = x0;
	r.y = y0;
	r.w = g->xoff2 - g->xoff;
	r.h = g->yoff2 - g->yoff;
	quad(r, g->x0 * k, g->y0 * k, g->x1 * k, g->y1 * k, c, ModeText, 0);
	*x += g->xadvance;
}

float
gfx_textw(int font, const char *s)
{
	static int cp[MAXCP];
	Font *f = &fonts[font];
	float w = 0;
	int i, n, rtl;

	n = visual(s, cp, MAXCP, &rtl);
	for (i = 0; i < n; i++)
		w += adv(f, cp[i]);
	return w;
}

/* draw s with its line box top at y; ellipsize if wider than maxw (> 0),
 * cutting the logical end: the right of LTR text, the left of RTL */
float
gfx_text(int font, float x, float y, const char *s, Color c, float maxw)
{
	static int cp[MAXCP];
	Font *f = &fonts[font];
	stbtt_packedchar *g, *ell = NULL;
	float base = roundf(y + f->ascent), w = 0;
	int i, n, from = 0, to, rtl;

	n = to = visual(s, cp, MAXCP, &rtl);
	for (i = 0; i < n; i++)
		w += adv(f, cp[i]);
	if (maxw > 0 && w > maxw) {
		ell = glyph(f, 0x2026);
		while (from < to && w + ell->xadvance > maxw)
			w -= adv(f, cp[rtl ? from++ : --to]);
		if (rtl)
			drawglyph(ell, &x, base, c);
	}
	for (i = from; i < to; i++)
		if ((g = glyph(f, cp[i])))
			drawglyph(g, &x, base, c);
	if (ell && !rtl)
		drawglyph(ell, &x, base, c);
	return x;
}

/* word-wrap s into width w, at most maxlines lines, the last one
 * ellipsized; newlines break lines. Returns the number of lines drawn */
int
gfx_wrap(int font, float x, float y, float w, const char *s, Color c, int maxlines)
{
	Font *f = &fonts[font];
	stbtt_packedchar *g;
	const char *p = s, *q, *prev, *brk, *end;
	char buf[2048];
	float lw;
	size_t n;
	int lines = 0, cp;

	while (*p && lines < maxlines) {
		while (*p == ' ' || *p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;
		brk = NULL;
		lw = 0;
		for (q = p; *q && *q != '\n'; ) {
			prev = q;
			cp = utf8dec(&q);
			if (cp == ' ')
				brk = prev;
			if ((g = glyph(f, cp)))
				lw += g->xadvance;
			if (lw > w) {
				q = brk ? brk : prev;
				break;
			}
		}
		end = q > p ? q : p + 1;
		/* last line: the rest of the text, ellipsized by gfx_text */
		if (++lines == maxlines)
			for (end = p; *end; end++)
				;
		n = MIN((size_t)(end - p), sizeof buf - 1);
		memcpy(buf, p, n);
		buf[n] = '\0';
		for (q = buf; *q; q++)
			if (*q == '\n' || *q == '\r')
				buf[q - buf] = ' ';
		gfx_text(font, x, y, buf, c, w);
		y += f->lineh;
		p = end;
	}
	return lines;
}

float
gfx_lineh(int font)
{
	return fonts[font].lineh;
}

float
gfx_ascent(int font)
{
	return fonts[font].ascent;
}

void
gfx_clip(Rect r)
{
	flush();
	glEnable(GL_SCISSOR_TEST);
	glScissor((int)r.x, H - (int)(r.y + r.h), (int)r.w, (int)r.h);
}

void
gfx_noclip(void)
{
	flush();
	glDisable(GL_SCISSOR_TEST);
}

/* (re)allocate an offscreen colour target of w x h pixels */
void
gfx_target(Target *t, int w, int h)
{
	if (t->tex && t->w == w && t->h == h)
		return;
	if (!t->tex) {
		glGenTextures(1, &t->tex);
		pglGenFramebuffers(1, &t->fbo);
	}
	glBindTexture(GL_TEXTURE_2D, t->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	pglBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
	if (pglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		die("framebuffer incomplete (%dx%d)", w, h);
	t->w = w;
	t->h = h;
	gfx_target_clear(t);
}

void
gfx_target_clear(Target *t)
{
	if (!t->tex)
		return;
	pglBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	glDisable(GL_SCISSOR_TEST);
	glViewport(0, 0, t->w, t->h);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	pglBindFramebuffer(GL_FRAMEBUFFER, 0);
}
