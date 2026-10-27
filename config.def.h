/* See LICENSE file for copyright and license details. */

/* fonts: the first file that exists wins */
static const char *fonts[] = {
	"vendor/DejaVuSans.ttf",
	"/usr/local/share/sit/fonts/DejaVuSans.ttf",  /* make install (matches PREFIX in config.mk) */
	"C:/Windows/Fonts/segoeui.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/noto/NotoSans-Regular.ttf",
	"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
	"/System/Library/Fonts/Supplemental/Arial.ttf",
};
static const char *boldfonts[] = {
	"vendor/DejaVuSans-Bold.ttf",
	"/usr/local/share/sit/fonts/DejaVuSans-Bold.ttf",  /* make install (matches PREFIX in config.mk) */
	"C:/Windows/Fonts/seguisb.ttf",
	"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
	"/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
	"/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
	"/System/Library/Fonts/Supplemental/Arial Bold.ttf",
};

/* glyphs the fonts above lack (Arabic, Hebrew, symbols) come from the
 * first of these that has them */
static const char *fallbackfonts[] = {
	"C:/Windows/Fonts/seguisym.ttf",
	"C:/Windows/Fonts/arial.ttf",
	"C:/Windows/Fonts/tahoma.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/noto/NotoSansArabic-Regular.ttf",
	"/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
	"/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
};

/* sizes in px at 96 dpi; everything is scaled on HiDPI screens
 * (override the scale with the SIT_SCALE environment variable) */
static const float textsize  = 15;
static const float titlesize = 22;
static const float smallsize = 12.5;
static const float rowh      = 38;
static const float radius    = 8;
static const float pad       = 20;
static const float sidebarw  = 220;
static const float previewf  = 0.42;  /* share of the main area for the preview */
static const float hidedelay = 2.5;   /* seconds until the player overlay fades */

static const unsigned int colors[2][ColLast] = {
	/* dark (Catppuccin Mocha) */
	[0] = {
		[ColBg]     = 0x1e1e2e,
		[ColPanel]  = 0x181825,
		[ColFg]     = 0xcdd6f4,
		[ColDim]    = 0x7f849c,
		[ColAccent] = 0x89b4fa,
		[ColSel]    = 0x313244,
		[ColLive]   = 0xf38ba8,
		[ColGood]   = 0xa6e3a1,
	},
	/* light (Catppuccin Latte) */
	[1] = {
		[ColBg]     = 0xeff1f5,
		[ColPanel]  = 0xe6e9ef,
		[ColFg]     = 0x4c4f69,
		[ColDim]    = 0x6c6f85,
		[ColAccent] = 0x1e66f5,
		[ColSel]    = 0xccd0da,
		[ColLive]   = 0xd20f39,
		[ColGood]   = 0x40a02b,
	},
};
static const int theme = 0;    /* 0 dark, 1 light */

static const char *mpvopts[][2] = {
	{ "hwdec",             "auto-safe" },
	{ "cache",             "yes" },
	{ "demuxer-max-bytes", "32MiB" },       /* buffered ahead */
	{ "demuxer-max-back-bytes", "16MiB" },  /* kept for seeking back */
	{ "volume",            "80" },
	/* { "user-agent",     "..." },  some providers only serve known players */
};

/* sources, switched with Alt+1..9. A file of tab-separated lines
 *   name	xtream	http://host:port	user	pass
 * in %APPDATA%\sit\sources or ~/.config/sit/sources (or given with -s)
 * replaces this list, so credentials need not be compiled in. */
static const Source sources[] = {
	/* name     type        url                              user     pass */
	{ "Demo",   SrcDemo,    NULL,                            NULL,    NULL },
	/* { "Home", SrcXtream, "http://provider.example:8080",  "user",  "pass" }, */
};

static const char *liveext   = "ts";      /* or "m3u8"; falls back to what the account allows */
static const int   cachettl  = 6 * 3600;  /* seconds before channel lists are refetched */
static const char *useragent = NULL;      /* for API requests; NULL: libcurl's */

/* the Demo source: public test streams */
static const Chan demo[] = {
	{ "Big Buck Bunny",   "Demo",       "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8", 0 },
	{ "Tears of Steel",   "Demo",       "https://demo.unified-streaming.com/k8s/features/stable/video/tears-of-steel/tears-of-steel.ism/.m3u8", 0 },
	{ "Apple bipbop",     "Demo",       "https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_16x9/bipbop_16x9_variant.m3u8", 0 },
	{ "Akamai live test", "Live tests", "https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8", 1 },
	{ "Unified live",     "Live tests", "https://demo.unified-streaming.com/k8s/live/stable/live.isml/.m3u8", 1 },
};

#define VB ViewBrowse
#define VP ViewPlayer
#define VA (ViewBrowse | ViewPlayer)

static const Key keys[] = {
	/* views modifier    key                function     argument */
	{ VB,   0,          SDLK_UP,           move,        { .i = -1 } },
	{ VB,   0,          SDLK_k,            move,        { .i = -1 } },
	{ VB,   0,          SDLK_DOWN,         move,        { .i = +1 } },
	{ VB,   0,          SDLK_j,            move,        { .i = +1 } },
	{ VB,   0,          SDLK_PAGEUP,       page,        { .i = -1 } },
	{ VB,   0,          SDLK_PAGEDOWN,     page,        { .i = +1 } },
	{ VB,   0,          SDLK_HOME,         jump,        { .i = 0 } },
	{ VB,   0,          SDLK_g,            jump,        { .i = 0 } },
	{ VB,   0,          SDLK_END,          jump,        { .i = 1 } },
	{ VB,   KMOD_SHIFT, SDLK_g,            jump,        { .i = 1 } },
	{ VB,   0,          SDLK_LEFT,         focus,       { .i = -1 } },
	{ VB,   0,          SDLK_h,            focus,       { .i = -1 } },
	{ VB,   0,          SDLK_RIGHT,        focus,       { .i = +1 } },
	{ VB,   0,          SDLK_l,            focus,       { .i = +1 } },
	{ VB,   0,          SDLK_r,            refresh,     { 0 } },
	{ VB,   0,          SDLK_SLASH,        search,      { 0 } },
	{ VB,   KMOD_CTRL,  SDLK_f,            search,      { 0 } },
	{ VB,   0,          SDLK_f,            favorite,    { 0 } },
	{ VA,   0,          SDLK_e,            guide,       { 0 } },
	{ VB,   0,          SDLK_TAB,          kind,        { .i = +1 } },
	{ VB,   KMOD_SHIFT, SDLK_TAB,          kind,        { .i = -1 } },
	{ VB,   KMOD_ALT,   SDLK_1,            source,      { .i = 0 } },
	{ VB,   KMOD_ALT,   SDLK_2,            source,      { .i = 1 } },
	{ VB,   KMOD_ALT,   SDLK_3,            source,      { .i = 2 } },
	{ VB,   KMOD_ALT,   SDLK_4,            source,      { .i = 3 } },
	{ VB,   KMOD_ALT,   SDLK_5,            source,      { .i = 4 } },
	{ VB,   KMOD_ALT,   SDLK_6,            source,      { .i = 5 } },
	{ VB,   KMOD_ALT,   SDLK_7,            source,      { .i = 6 } },
	{ VB,   KMOD_ALT,   SDLK_8,            source,      { .i = 7 } },
	{ VB,   KMOD_ALT,   SDLK_9,            source,      { .i = 8 } },
	{ VB,   0,          SDLK_0,            number,      { .i = 0 } },
	{ VB,   0,          SDLK_1,            number,      { .i = 1 } },
	{ VB,   0,          SDLK_2,            number,      { .i = 2 } },
	{ VB,   0,          SDLK_3,            number,      { .i = 3 } },
	{ VB,   0,          SDLK_4,            number,      { .i = 4 } },
	{ VB,   0,          SDLK_5,            number,      { .i = 5 } },
	{ VB,   0,          SDLK_6,            number,      { .i = 6 } },
	{ VB,   0,          SDLK_7,            number,      { .i = 7 } },
	{ VB,   0,          SDLK_8,            number,      { .i = 8 } },
	{ VB,   0,          SDLK_9,            number,      { .i = 9 } },
	{ VB,   0,          SDLK_KP_0,         number,      { .i = 0 } },
	{ VB,   0,          SDLK_KP_1,         number,      { .i = 1 } },
	{ VB,   0,          SDLK_KP_2,         number,      { .i = 2 } },
	{ VB,   0,          SDLK_KP_3,         number,      { .i = 3 } },
	{ VB,   0,          SDLK_KP_4,         number,      { .i = 4 } },
	{ VB,   0,          SDLK_KP_5,         number,      { .i = 5 } },
	{ VB,   0,          SDLK_KP_6,         number,      { .i = 6 } },
	{ VB,   0,          SDLK_KP_7,         number,      { .i = 7 } },
	{ VB,   0,          SDLK_KP_8,         number,      { .i = 8 } },
	{ VB,   0,          SDLK_KP_9,         number,      { .i = 9 } },
	{ VB,   0,          SDLK_RETURN,       play,        { 0 } },
	{ VB,   0,          SDLK_KP_ENTER,     play,        { 0 } },
	{ VP,   0,          SDLK_UP,           zap,         { .i = -1 } },
	{ VP,   0,          SDLK_DOWN,         zap,         { .i = +1 } },
	{ VP,   0,          SDLK_LEFT,         seek,        { .f = -10 } },
	{ VP,   0,          SDLK_RIGHT,        seek,        { .f = +10 } },
	{ VP,   KMOD_SHIFT, SDLK_LEFT,         seek,        { .f = -60 } },
	{ VP,   KMOD_SHIFT, SDLK_RIGHT,        seek,        { .f = +60 } },
	{ VA,   0,          SDLK_LEFTBRACKET,  zap,         { .i = -1 } },
	{ VA,   0,          SDLK_RIGHTBRACKET, zap,         { .i = +1 } },
	{ VA,   0,          SDLK_SPACE,        togglepause, { 0 } },
	{ VA,   0,          SDLK_a,            cycle,       { .i = 0 } },
	{ VA,   0,          SDLK_s,            cycle,       { .i = 1 } },
	{ VA,   0,          SDLK_MINUS,        volume,      { .f = -5 } },
	{ VA,   0,          SDLK_EQUALS,       volume,      { .f = +5 } },
	{ VA,   0,          SDLK_m,            mute,        { 0 } },
	{ VA,   0,          SDLK_x,            stop,        { 0 } },
	{ VA,   0,          SDLK_F11,          fullscreen,  { 0 } },
	{ VA,   KMOD_SHIFT, SDLK_f,            fullscreen,  { 0 } },
	{ VA,   0,          SDLK_ESCAPE,       back,        { 0 } },
	{ VA,   0,          SDLK_BACKSPACE,    back,        { 0 } },
	{ VA,   0,          SDLK_q,            quit,        { 0 } },
};
