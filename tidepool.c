/*
 * tidepool — a quiet, keyboard-shaped Gray–Scott reaction–diffusion garden.
 *
 * Portable C99 / POSIX.  No libraries are needed beyond libc and libm.
 * The terminal is restored on normal exit and handled termination signals.
 *
 * Copyright (c) 2026. MIT license; see LICENSE.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PI 3.14159265358979323846
#define MAX_KEYS 64
#define OUTPUT_WRITE_CHUNK 32768
#define OUTPUT_BYTES_PER_SECOND 256000
#define OUTPUT_INDEXED_BYTES_PER_SECOND 1024000
#define PALETTE_LEVELS 32
#define PALETTE_INDEXED 0x80000000u
#define MIN_COLS 24
#define MIN_ROWS 8
#define MAX_GRAZERS 10

/* The nine-point Laplacian is less grid-directional than four neighbors. */
#define DU .16f
#define DV .08f
#define FEED .035f
#define KILL .060f

typedef struct { unsigned char r, g, b; } RGB;
typedef struct { const char *name; RGB stops[6]; } Palette;
static const Palette palettes[] = {
    { "lagoon", {{10,22,32}, {16,43,53}, {31,82,89}, {78,139,132}, {155,190,163}, {236,221,174}} },
    { "orchid", {{20,18,33}, {42,34,60}, {86,64,109}, {147,112,151}, {209,173,189}, {244,224,202}} },
    { "moss",   {{13,26,23}, {27,52,44}, {54,98,73}, {113,151,107}, {182,197,143}, {236,225,182}} },
    { "ember",  {{29,21,29}, {62,41,54}, {118,70,79}, {179,112,103}, {225,161,122}, {254,225,167}} }
};
#define N_PALETTES ((int)(sizeof(palettes)/sizeof(palettes[0])))

typedef enum { COLOR_AUTO, COLOR_TRUE, COLOR_256, COLOR_16, COLOR_MONO } ColorMode;
typedef enum { LAYOUT_US, LAYOUT_FI } KeyLayout;
typedef enum { RENEWAL_OFF, RENEWAL_GENTLE, RENEWAL_ACTIVE } RenewalMode;

typedef struct {
    int fps, mouse, hud, ascii, utf8;
    ColorMode color;
    KeyLayout layout;
    int palette;
    RenewalMode renewal;
    unsigned int seed;
    int seed_given;
} Config;

typedef struct { uint32_t cp; float xn, yn; char utf8[5]; } Key;
typedef struct {
    float x, y, direction, hunger;
} Grazer;
typedef struct { uint32_t fg, bg, glyph; } Cell;
typedef struct { char *bytes; size_t capacity, len; int failed; } Output;
typedef struct {
    int cols, rows, w, h, picture_rows;
    float *u, *v, *next_u, *next_v, *trace;
    Cell *last;
    uint32_t *key_overlay;
    int redraw;
    char *frame_bytes;
    size_t frame_capacity;
} Garden;

typedef struct {
    Config config;
    Garden garden;
    Key keys[MAX_KEYS];
    int key_count;
    int paused, speed, keys_visible, help;
    uint32_t last_key;
    double last_key_at, last_mouse_at;
    int mouse_down, last_mouse_x, last_mouse_y;
    int resized;
    int input_state;
    char escape[96];
    int escape_length;
    double esc_at;
    int utf_expected;
    uint32_t utf_value, utf_min;
    uint32_t rng;
    Grazer grazers[MAX_GRAZERS];
    int grazer_count;
    unsigned long ecology_steps;
    float garden_cover;
    char last_footer[384];
    Output output;
    size_t output_offset;
    double next_output_at;
    int indexed_colors, palette_report_entry, palette_report_count;
    int output_palette;
    char control_bytes[1024];
} App;

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t winch_requested = 0;
static struct termios saved_tio;
static int saved_tio_valid = 0;
static int saved_output_flags = -1;
static int terminal_active = 0;
static int terminal_mouse = 0;
static int terminal_palette_saved = 0;
static int installed_palette = -1;

static void signal_stop(int signo) { (void)signo; stop_requested = 1; }
static void signal_winch(int signo) { (void)signo; winch_requested = 1; }

static double seconds_now(void);
static void app_read_input(App *a);

static int install_signal_handlers(void) {
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = signal_stop;
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0 ||
        sigaction(SIGHUP, &action, NULL) != 0) return 0;
    action.sa_handler = signal_winch;
    if (sigaction(SIGWINCH, &action, NULL) != 0) return 0;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &action, NULL) == 0;
}

static int write_all(const char *bytes, size_t len) {
    /* Only terminal setup/cleanup use this bounded synchronous writer. */
    double deadline = seconds_now() + .10;
    while (len) {
        if (seconds_now() >= deadline) return 0;
        size_t chunk = len < OUTPUT_WRITE_CHUNK ? len : OUTPUT_WRITE_CHUNK;
        ssize_t n = write(STDOUT_FILENO, bytes, chunk);
        if (n > 0) {
            bytes += n; len -= (size_t)n;
        }
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set output;
            FD_ZERO(&output); FD_SET(STDOUT_FILENO, &output);
            double wait = deadline - seconds_now();
            if (wait <= 0.0) return 0;
            struct timeval tv = {0, (suseconds_t)(wait * 1e6)};
            int ready = select(STDOUT_FILENO + 1, NULL, &output, NULL, &tv);
            if (ready < 0 && errno != EINTR) { stop_requested = 1; return 0; }
        } else { stop_requested = 1; return 0; }
    }
    return 1;
}

static void terminal_restore(void) {
    /* Restore input even when the terminal is no longer draining output. */
    if (saved_tio_valid) (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    if (terminal_active) {
        /* Discard unfinished frames so reset sequences fit even if the PTY's
           reader stays paused until after this process exits. */
        (void)tcflush(STDOUT_FILENO, TCOFLUSH);
        /* CAN also cancels a control sequence interrupted by resize/exit. */
        (void)write_all("\030", 1);
        if (terminal_palette_saved) {
            (void)write_all("\033[#Q", 4);
            terminal_palette_saved = 0;
        }
        const char *cleanup = terminal_mouse ?
            "\033[?1006l\033[?1002l\033[?1000l\033[0m\033[?25h\033[?1049l" :
            "\033[0m\033[?25h\033[?1049l";
        (void)write_all(cleanup, strlen(cleanup));
        terminal_active = 0;
    }
    if (saved_output_flags != -1)
        (void)fcntl(STDOUT_FILENO, F_SETFL, saved_output_flags);
}

static int terminal_start(int enable_mouse) {
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &saved_tio) != 0) return 0;
    saved_tio_valid = 1;
    saved_output_flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (saved_output_flags == -1 ||
        fcntl(STDOUT_FILENO, F_SETFL, saved_output_flags | O_NONBLOCK) == -1) return 0;
    raw = saved_tio;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
    /* Ctrl-C must interrupt computation as well as reads, independently of
       any unfinished mouse/escape sequence in the input parser. */
    raw.c_lflag |= ISIG;
    raw.c_cc[VINTR] = 3;
    raw.c_cc[VQUIT] = _POSIX_VDISABLE;
    raw.c_cc[VSUSP] = _POSIX_VDISABLE;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return 0;
    /* stdin/stdout may share an open-file description. The input loop and
       output writer both handle EAGAIN if this also makes stdin nonblocking. */
    terminal_active = 1;
    (void)write_all("\033[?1049h\033[?25l\033[2J\033[H",
        strlen("\033[?1049h\033[?25l\033[2J\033[H"));
    if (enable_mouse) {
        terminal_mouse = 1;
        (void)write_all("\033[?1000h\033[?1002h\033[?1006h",
            strlen("\033[?1000h\033[?1002h\033[?1006h"));
    }
    return 1;
}

static double seconds_now(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int ci_contains(const char *s, const char *part) {
    size_t i, j;
    if (!s || !part) return 0;
    for (i = 0; s[i]; ++i) {
        for (j = 0; part[j] && s[i+j] &&
            tolower((unsigned char)s[i+j]) == tolower((unsigned char)part[j]); ++j) {}
        if (!part[j]) return 1;
    }
    return 0;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static uint32_t random_u32(App *a) {
    uint32_t x = a->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return a->rng = x ? x : 0x91e10da5u;
}
static float random_unit(App *a) {
    return (float)(random_u32(a) >> 8) * (1.f / 16777216.f);
}

static void output_bytes(Output *o, const char *bytes, size_t n) {
    if (o->failed) return;
    if (n > o->capacity - o->len) { o->failed = 1; return; }
    memcpy(o->bytes + o->len, bytes, n);
    o->len += n;
}
static void output_str(Output *o, const char *s) { output_bytes(o, s, strlen(s)); }
static void output_fmt(Output *o, const char *fmt, ...) {
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) output_bytes(o, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp)-1);
}

static void app_flush_output(App *a) {
    if (!a->output.len) return;
    size_t left = a->output.len - a->output_offset;
    size_t chunk = left < OUTPUT_WRITE_CHUNK ? left : OUTPUT_WRITE_CHUNK;
    ssize_t n = write(STDOUT_FILENO, a->output.bytes + a->output_offset, chunk);
    if (n > 0) {
        a->output_offset += (size_t)n;
        if (a->output_offset == a->output.len) {
            if (a->indexed_colors) installed_palette = a->output_palette;
            a->output.len = 0;
            a->output_offset = 0;
        }
    } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
        stop_requested = 1;
    }
}

/* Terminal output uses a 24-bit, 256-color, 16-color, or monochrome fallback. */
static uint32_t encode_color(RGB c, ColorMode mode) {
    if (mode == COLOR_TRUE) return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
    if (mode == COLOR_256) {
        int r = (c.r * 5 + 127) / 255, g = (c.g * 5 + 127) / 255, b = (c.b * 5 + 127) / 255;
        int cube = 16 + 36 * r + 6 * g + b;
        int cr = r ? 55 + 40*r : 0, cg = g ? 55 + 40*g : 0, cb = b ? 55 + 40*b : 0;
        int dc = (c.r-cr)*(c.r-cr) + (c.g-cg)*(c.g-cg) + (c.b-cb)*(c.b-cb);
        int gray = ((int)c.r + c.g + c.b) / 3;
        int gi = (gray - 8 + 5) / 10;
        if (gi < 0) gi = 0;
        if (gi > 23) gi = 23;
        int gv = 8 + 10 * gi;
        int dg = (c.r-gv)*(c.r-gv) + (c.g-gv)*(c.g-gv) + (c.b-gv)*(c.b-gv);
        return (uint32_t)(dg < dc ? 232 + gi : cube);
    }
    if (mode == COLOR_16) {
        /* xterm-compatible 16-color approximation, tuned for subdued hues. */
        static const RGB colors[16] = {
            {0,0,0},{128,0,0},{0,128,0},{128,128,0},
            {0,0,128},{128,0,128},{0,128,128},{192,192,192},
            {128,128,128},{255,0,0},{0,255,0},{255,255,0},
            {0,0,255},{255,0,255},{0,255,255},{255,255,255}
        };
        int best = 0; unsigned int dist = UINT32_MAX;
        for (int k = 0; k < 16; ++k) {
            int dr = (int)c.r-colors[k].r, dg = (int)c.g-colors[k].g, db = (int)c.b-colors[k].b;
            unsigned int d = (unsigned int)(dr*dr + dg*dg + db*db);
            if (d < dist) { dist = d; best = k; }
        }
        return (uint32_t)best;
    }
    return 0;
}
static void emit_color(Output *o, uint32_t c, ColorMode m, int bg) {
    if (m == COLOR_MONO) return;
    if (c & PALETTE_INDEXED)
        output_fmt(o, "\033[%d;5;%um", bg ? 48 : 38, c & ~PALETTE_INDEXED);
    else if (m == COLOR_TRUE)
        output_fmt(o, "\033[%d;2;%u;%u;%um", bg ? 48 : 38,
            (c >> 16) & 255u, (c >> 8) & 255u, c & 255u);
    else if (m == COLOR_256)
        output_fmt(o, "\033[%d;5;%um", bg ? 48 : 38, c);
    else
        output_fmt(o, "\033[%um", (unsigned)(bg ? (c < 8 ? 40+c : 100+c-8) : (c < 8 ? 30+c : 90+c-8)));
}

static RGB mix_rgb(RGB a, RGB b, float t) {
    t = clampf(t, 0.f, 1.f);
    RGB c = {
        (unsigned char)(a.r + (b.r - a.r)*t + .5f),
        (unsigned char)(a.g + (b.g - a.g)*t + .5f),
        (unsigned char)(a.b + (b.b - a.b)*t + .5f)
    };
    return c;
}

static int palette_level(float v, float trace) {
    float t = sqrtf(clampf(v / .44f + trace * .22f, 0.f, 1.f));
    return (int)(t*PALETTE_LEVELS+.5f);
}

static RGB palette_gradient(int palette, int level) {
    const Palette *p = &palettes[palette];
    static const float at[6] = {0.f, .18f, .36f, .54f, .76f, 1.f};
    /* Nearby samples share colors, avoiding a new SGR command for every cell. */
    float t = (float)level/PALETTE_LEVELS;
    for (int i = 0; i < 5; ++i) {
        if (t <= at[i+1])
            return mix_rgb(p->stops[i], p->stops[i+1], (t-at[i]) / (at[i+1]-at[i]));
    }
    return p->stops[5];
}

static RGB palette_sample(int palette, float v, float trace) {
    return palette_gradient(palette, palette_level(v, trace));
}

static void output_palette(Output *o, int palette) {
    output_str(o, "\033]4");
    for (int level = 0; level <= PALETTE_LEVELS; ++level) {
        RGB c = palette_gradient(palette, level);
        output_fmt(o, ";%d;rgb:%02x/%02x/%02x", 16+level, c.r, c.g, c.b);
    }
    output_str(o, "\033\\");
}

static void garden_dispose(Garden *g) {
    free(g->u); free(g->v); free(g->next_u); free(g->next_v);
    free(g->trace); free(g->last); free(g->key_overlay);
    free(g->frame_bytes);
    memset(g, 0, sizeof(*g));
}

/* One deterministic Gray–Scott Euler step.  Toroidal boundaries avoid seams. */
static void garden_step(Garden *g) {
    const int w = g->w, h = g->h;
    const float *u = g->u, *v = g->v;
    float *nu = g->next_u, *nv = g->next_v;
    for (int y = 0; y < h; ++y) {
        const int ay = (y ? y-1 : h-1)*w;
        const int by = (y+1 < h ? y+1 : 0)*w;
        const int yy = y*w;
        for (int x = 0; x < w; ++x) {
            const int lx = x ? x-1 : w-1;
            const int rx = x+1 < w ? x+1 : 0;
            const int i = yy+x;
            const float uc = u[i], vc = v[i];
            const float lapu = -uc + .2f*(u[yy+lx]+u[yy+rx]+u[ay+x]+u[by+x])
                +.05f*(u[ay+lx]+u[ay+rx]+u[by+lx]+u[by+rx]);
            const float lapv = -vc + .2f*(v[yy+lx]+v[yy+rx]+v[ay+x]+v[by+x])
                +.05f*(v[ay+lx]+v[ay+rx]+v[by+lx]+v[by+rx]);
            const float reaction = uc*vc*vc;
            nu[i] = clampf(uc + DU*lapu - reaction + FEED*(1.f-uc), 0.f, 1.f);
            nv[i] = clampf(vc + DV*lapv + reaction - (FEED+KILL)*vc, 0.f, 1.f);
        }
    }
    g->u = nu; g->next_u = (float *)u;
    g->v = nv; g->next_v = (float *)v;
}

static void garden_seed_blob(Garden *g, float x, float y, float r, float vstrength) {
    int w = g->w, h = g->h;
    if (!w || !h) return;
    for (int yy = 0; yy < h; ++yy) {
        float dy = fabsf(yy-y); if (dy > h*.5f) dy = h-dy;
        if (dy > r*1.45f) continue;
        for (int xx = 0; xx < w; ++xx) {
            float dx = fabsf(xx-x); if (dx > w*.5f) dx = w-dx;
            float d = sqrtf(dx*dx + dy*dy) / r;
            if (d > 1.45f) continue;
            /* Sharpen the initial seed just enough to survive diffusion. */
            float bump = expf(-powf(d*1.05f, 4.f));
            int i = yy*w + xx;
            g->v[i] = fmaxf(g->v[i], vstrength*bump);
            g->u[i] = fminf(g->u[i], 1.f-.52f*bump);
        }
    }
}

/* Invisible grazers are a local, density-dependent disturbance, not a
   synchronized fade or a global reset. They follow established V-rich areas,
   slowly removing V and restoring U. Existing reaction fronts recolonize the
   resulting spaces. 'Off' leaves the original Gray–Scott equations intact. */
static float wrap_position(float v, int size) {
    if (v < 0.f || v >= size) {
        v = fmodf(v, (float)size);
        if (v < 0.f) v += size;
    }
    return v;
}

/* Average the local food level without blurring the chemistry itself. */
static float grazer_food(const Garden *g, float x, float y) {
    int cx = (int)lroundf(x), cy = (int)lroundf(y);
    static const int offsets[9][2] = {
        {0,0}, {-3,0}, {3,0}, {0,-3}, {0,3},
        {-3,-3}, {3,-3}, {-3,3}, {3,3}
    };
    float total = 0.f;
    for (int k = 0; k < 9; ++k) {
        int xx = (cx+offsets[k][0]+g->w*2)%g->w;
        int yy = (cy+offsets[k][1]+g->h*2)%g->h;
        total += g->v[yy*g->w+xx];
    }
    return total * (1.f/9.f);
}

/* Find a well-established patch for each grazer; grazer appearance itself
   does not change the image. Selection is deterministic with --seed. */
static void grazer_find_food(App *a, Grazer *p) {
    Garden *g = &a->garden;
    float best = -1.f;
    for (int k = 0; k < 28; ++k) {
        float x = random_unit(a)*g->w, y = random_unit(a)*g->h;
        float food = grazer_food(g, x, y);
        if (food > best) { best = food; p->x = x; p->y = y; }
    }
    p->direction = random_unit(a)*(2.f*PI);
    p->hunger = 0.f;
}

static void grazer_setup(App *a) {
    Garden *g = &a->garden;
    a->grazer_count = 0;
    a->ecology_steps = 0;
    a->garden_cover = 0.f;
    if (a->config.renewal == RENEWAL_OFF || !g->w) return;
    /* Scale with area, not with terminal height alone. */
    int count = (g->w*g->h+1900)/3200;
    if (count < 1) count = 1;
    if (count > MAX_GRAZERS/2) count = MAX_GRAZERS/2;
    if (a->config.renewal == RENEWAL_ACTIVE) count *= 2;
    a->grazer_count = count;
    for (int i = 0; i < count; ++i) grazer_find_food(a, &a->grazers[i]);
}

static void grazer_eat(Garden *g, float x, float y, float strength) {
    /* Smooth, limited consumption: no square tiles or perfectly erased holes.
       Leave faint residual V so locally surviving chemistry can regrow. */
    float radius = clampf(fminf(g->w,g->h)*.065f, 3.0f, 7.0f);
    int reach = (int)ceilf(radius*1.9f);
    int cx = (int)lroundf(x), cy = (int)lroundf(y);
    for (int dy = -reach; dy <= reach; ++dy) {
        int yy = (cy+dy+g->h*2)%g->h;
        for (int dx = -reach; dx <= reach; ++dx) {
            float r2 = (float)(dx*dx+dy*dy)/(radius*radius);
            if (r2 > 3.61f) continue;
            int xx = (cx+dx+g->w*2)%g->w;
            int i = yy*g->w+xx;
            float amount = strength*expf(-1.6f*r2);
            /* Consume V, with the consumed fraction slowly returning to U. */
            float taken = g->v[i]*amount;
            g->v[i] -= taken;
            g->u[i] += (1.f-g->u[i])*amount*.6f;
        }
    }
}

static void garden_ecology_step(App *a) {
    if (!a->grazer_count) return;
    Garden *g = &a->garden;
    ++a->ecology_steps;
    /* Grazing starts only as the garden gets crowded and stops naturally if
       too much V has been consumed. The average is sampled at low frequency. */
    if (a->ecology_steps == 1 || a->ecology_steps % 48 == 0) {
        double sum = 0.0;
        size_t n = (size_t)g->w*g->h;
        for (size_t i = 0; i < n; ++i) sum += g->v[i];
        a->garden_cover = (float)(sum/(double)n);
    }
    float awake = clampf((a->garden_cover-.080f)/.055f, 0.f, 1.f);
    if (awake <= 0.f) return;
    for (int k = 0; k < a->grazer_count; ++k) {
        Grazer *p = &a->grazers[k];
        /* Compare chemical density ahead and to the sides. A little stochastic
           turning prevents a locked orbit on symmetric patterns. */
        float ahead = p->direction;
        float dist = 9.f;
        float left = grazer_food(g,
            p->x+dist*cosf(ahead-.65f),p->y+dist*sinf(ahead-.65f));
        float right = grazer_food(g,
            p->x+dist*cosf(ahead+.65f),p->y+dist*sinf(ahead+.65f));
        float steer = clampf((right-left)*1.6f,-.055f,.055f);
        float wander = (random_unit(a)-.5f)*.037f;
        p->direction += steer + wander;
        p->x = wrap_position(p->x+cosf(p->direction)*.17f, g->w);
        p->y = wrap_position(p->y+sinf(p->direction)*.17f, g->h);
        float food = grazer_food(g,p->x,p->y);
        if (food < .052f) p->hunger += 1.f;
        else p->hunger = fmaxf(0.f,p->hunger-5.f);
        if (p->hunger > 180.f) { grazer_find_food(a,p); continue; }
        float appetite = clampf((food-.045f)/.10f,0.f,1.f);
        if (appetite > 0.f) {
            float vigor = a->config.renewal == RENEWAL_ACTIVE ? 1.5f : 1.f;
            grazer_eat(g,p->x,p->y,.027f*awake*appetite*vigor);
        }
    }
}

static void garden_renew(App *a) {
    Garden *g = &a->garden;
    if (!g->w || !g->h) return;
    size_t n = (size_t)g->w*g->h;
    for (size_t i = 0; i < n; ++i) {
        g->u[i] = 1.f; g->v[i] = 0.f;
        g->trace[i] = 0.f;
    }
    /* Offset centers and scale seed size to fit even small terminals. */
    int count = g->w * g->h < 1000 ? 3 : 6;
    float radius = clampf(fminf(g->w, g->h) * .115f, 3.f, 12.f);
    for (int j = 0; j < count; ++j) {
        float u = .10f + random_unit(a)*.80f;
        float v = .13f + random_unit(a)*.74f;
        float size = radius*(.73f + random_unit(a)*.46f);
        garden_seed_blob(g, u*g->w, v*g->h, size, .30f);
    }
    /* Enter an already-evolving garden instead of an empty screen. */
    int warm = 490 + (int)(random_u32(a) % 150u);
    for (int i = 0; i < warm && !stop_requested; ++i) garden_step(g);
    grazer_setup(a);
    g->redraw = 1;
    a->last_key = 0;
}

static void garden_touch(Garden *g, float px, float py, float force, int quiet) {
    if (!g->w || !g->h) return;
    const float r = clampf(fminf(g->w, g->h)*.080f, 3.2f, 7.0f);
    const int reach = (int)ceilf(2.2f*r);
    int cx = (int)lroundf(px), cy = (int)lroundf(py);
    for (int dy = -reach; dy <= reach; ++dy) {
        int y = (cy+dy+g->h*3) % g->h;
        for (int dx = -reach; dx <= reach; ++dx) {
            int x = (cx+dx+g->w*3) % g->w;
            float d2 = (float)(dx*dx+dy*dy)/(r*r);
            if (d2 > 4.f) continue;
            float z = force*expf(-2.f*d2);
            int i = y*g->w+x;
            if (quiet) {
                g->v[i] *= 1.f-.65f*z;
                g->u[i] += (1.f-g->u[i])*.25f*z;
            } else {
                g->v[i] = clampf(g->v[i]+.28f*z*(1.f-g->v[i]), 0.f, 1.f);
                g->u[i] *= 1.f-.38f*z;
            }
            g->trace[i] = fmaxf(g->trace[i], .30f*z);
        }
    }
}

static void garden_trace_fade(Garden *g) {
    size_t n = (size_t)g->w*g->h;
    for (size_t i = 0; i < n; ++i) {
        g->trace[i] *= .92f;
        if (g->trace[i] < .001f) g->trace[i] = 0.f;
    }
}

static uint32_t decode_one_utf8(const unsigned char *s, int *consumed) {
    uint32_t cp;
    if (s[0] < 0x80) { *consumed = 1; return s[0]; }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
        cp = ((s[0]&31u)<<6) | (s[1]&63u); *consumed = 2;
        return cp >= 0x80 ? cp : '?';
    }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        cp = ((s[0]&15u)<<12) | ((s[1]&63u)<<6) | (s[2]&63u); *consumed = 3;
        return cp >= 0x800 && !(cp >= 0xd800 && cp <= 0xdfff) ? cp : '?';
    }
    if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 &&
        (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        cp = ((s[0]&7u)<<18) | ((s[1]&63u)<<12) | ((s[2]&63u)<<6) | (s[3]&63u); *consumed = 4;
        return cp >= 0x10000 && cp <= 0x10ffff ? cp : '?';
    }
    *consumed = 1; return '?';
}

static void encode_utf8(uint32_t cp, char b[5]) {
    if (cp < 0x80) { b[0] = (char)cp; b[1] = 0; }
    else if (cp < 0x800) {
        b[0] = (char)(0xc0|(cp>>6)); b[1] = (char)(0x80|(cp&63)); b[2] = 0;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xe0|(cp>>12)); b[1] = (char)(0x80|((cp>>6)&63));
        b[2] = (char)(0x80|(cp&63)); b[3] = 0;
    } else {
        b[0] = (char)(0xf0|(cp>>18)); b[1] = (char)(0x80|((cp>>12)&63));
        b[2] = (char)(0x80|((cp>>6)&63)); b[3] = (char)(0x80|(cp&63)); b[4] = 0;
    }
}

static void make_keys(App *a) {
    /* Each character represents a physical key, not a screen text position.
       Staggering emulates the offset rows of a QWERTY keyboard. */
    static const char *us[4] = {
        "1234567890-=", "qwertyuiop[]\\", "asdfghjkl;'", "zxcvbnm,./"
    };
    static const char *fi[4] = {
        "1234567890+´", "qwertyuiopå¨", "asdfghjklöä'", "zxcvbnm,.-"
    };
    const char *const *rows = a->config.layout == LAYOUT_FI ? fi : us;
    const float offsets[4] = { 0.f, 0.f, .51f, 1.02f };
    const float yn[4] = { .16f, .36f, .56f, .77f };
    a->key_count = 0;
    for (int row = 0; row < 4; ++row) {
        const unsigned char *str = (const unsigned char *)rows[row];
        for (int col = 0; *str && a->key_count < MAX_KEYS; ++col) {
            int consumed;
            uint32_t cp = decode_one_utf8(str, &consumed);
            str += consumed;
            Key *k = &a->keys[a->key_count++];
            k->cp = cp;
            k->xn = clampf(.075f + (col+offsets[row])*(.850f/9.f), .028f, .971f);
            k->yn = yn[row];
            encode_utf8(cp, k->utf8);
        }
    }
}

static void garden_make_overlay(App *a) {
    Garden *g = &a->garden;
    if (!g->key_overlay) return;
    memset(g->key_overlay, 0, (size_t)g->cols*g->picture_rows*sizeof(uint32_t));
    for (int k = 0; k < a->key_count; ++k) {
        int x = (int)lroundf(a->keys[k].xn*(g->cols-1));
        int y = (int)lroundf(a->keys[k].yn*(g->picture_rows-1));
        if (x >= 0 && x < g->cols && y >= 0 && y < g->picture_rows)
            g->key_overlay[y*g->cols+x] = a->keys[k].cp;
    }
}

static int garden_resize(App *a, int cols, int rows) {
    Garden *g = &a->garden, newg = {0};
    if (cols < MIN_COLS || rows < MIN_ROWS) {
        garden_dispose(g);
        a->grazer_count = 0;
        a->output = (Output){a->control_bytes, sizeof(a->control_bytes), 0, 0};
        a->output_offset = 0;
        a->next_output_at = 0;
        a->resized = 1;
        return 1;
    }
    int picture_rows = rows-(a->config.hud ? 1 : 0);
    if (g->cols == cols && g->rows == rows && g->w) return 1;
    newg.cols = cols; newg.rows = rows; newg.picture_rows = picture_rows;
    newg.w = cols; newg.h = 2*picture_rows;
    size_t n = (size_t)newg.w * newg.h;
    size_t pixels = (size_t)cols * picture_rows;
    if (n > 180000u) return 0; /* protective guard for absurd PTY sizes */
    newg.u = calloc(n, sizeof(float));
    newg.v = calloc(n, sizeof(float));
    newg.next_u = calloc(n, sizeof(float));
    newg.next_v = calloc(n, sizeof(float));
    newg.trace = calloc(n, sizeof(float));
    newg.last = malloc(pixels*sizeof(Cell));
    newg.key_overlay = calloc(pixels, sizeof(uint32_t));
    /* Each cell needs at most a cursor, two RGB controls, and a UTF-8 glyph. */
    newg.frame_capacity = pixels*64 + 4096;
    newg.frame_bytes = malloc(newg.frame_capacity);
    if (!newg.u || !newg.v || !newg.next_u || !newg.next_v ||
        !newg.trace || !newg.last || !newg.key_overlay || !newg.frame_bytes) {
        garden_dispose(&newg); return 0;
    }
    for (size_t i = 0; i < pixels; ++i) {
        newg.last[i].fg = UINT32_MAX;
        newg.last[i].bg = UINT32_MAX;
        newg.last[i].glyph = UINT32_MAX;
    }
    newg.redraw = 1;
    if (g->w) {
        /* Bilinear resampling keeps existing formations across terminal resize. */
        for (int y = 0; y < newg.h; ++y) {
            float fy = (y+.5f)*g->h/newg.h-.5f;
            int y0 = (int)floorf(fy); float ty = fy-y0;
            int y1 = y0+1;
            y0 = (y0%g->h+g->h)%g->h;
            y1 = (y1%g->h+g->h)%g->h;
            for (int x = 0; x < newg.w; ++x) {
                float fx = (x+.5f)*g->w/newg.w-.5f;
                int x0 = (int)floorf(fx); float tx = fx-x0;
                int x1 = x0+1;
                x0 = (x0%g->w+g->w)%g->w;
                x1 = (x1%g->w+g->w)%g->w;
                int i = y*newg.w+x, i00=y0*g->w+x0, i10=y0*g->w+x1;
                int i01=y1*g->w+x0, i11=y1*g->w+x1;
                newg.u[i] = (1-ty)*((1-tx)*g->u[i00]+tx*g->u[i10])
                    +ty*((1-tx)*g->u[i01]+tx*g->u[i11]);
                newg.v[i] = (1-ty)*((1-tx)*g->v[i00]+tx*g->v[i10])
                    +ty*((1-tx)*g->v[i01]+tx*g->v[i11]);
                newg.trace[i] = (1-ty)*((1-tx)*g->trace[i00]+tx*g->trace[i10])
                    +ty*((1-tx)*g->trace[i01]+tx*g->trace[i11]);
            }
        }
    }
    int fresh = !g->w;
    if (!fresh) {
        for (int k = 0; k < a->grazer_count; ++k) {
            a->grazers[k].x = wrap_position(a->grazers[k].x*newg.w/g->w,newg.w);
            a->grazers[k].y = wrap_position(a->grazers[k].y*newg.h/g->h,newg.h);
        }
    }
    garden_dispose(g);
    *g = newg;
    a->output = (Output){g->frame_bytes, g->frame_capacity, 0, 0};
    a->output_offset = 0;
    a->next_output_at = 0;
    garden_make_overlay(a);
    a->last_footer[0] = 0;
    a->resized = 1;
    if (fresh) garden_renew(a);
    return 1;
}

static void garden_invalidate(Garden *g) {
    if (!g->last) return;
    size_t n = (size_t)g->cols*g->picture_rows;
    for (size_t i = 0; i < n; ++i) g->last[i].glyph = UINT32_MAX;
    g->redraw = 1;
}

static void garden_key_touch(App *a, uint32_t cp) {
    /* Fold capitals, then prefer the literal layout key over US Shift aliases.
       In particular, '+' is a real key on a Finnish/Swedish keyboard. */
    if (cp >= 'A' && cp <= 'Z') cp += 32;
    if (cp == 0x00c5 || cp == 0x00c4 || cp == 0x00d6) cp += 32;
    const Key *matched = NULL;
    for (int i = 0; i < a->key_count; ++i)
        if (a->keys[i].cp == cp) { matched = &a->keys[i]; break; }
    if (!matched) {
        const char *shifted = "!@#$%^&*()_+{}|:\"<>?~";
        const char *base    = "1234567890-=[]\\;',./`";
        for (size_t i = 0; shifted[i]; ++i)
            if (cp == (uint32_t)(unsigned char)shifted[i]) {
                cp = (uint32_t)(unsigned char)base[i];
                break;
            }
        for (int i = 0; i < a->key_count; ++i)
            if (a->keys[i].cp == cp) { matched = &a->keys[i]; break; }
    }
    if (matched) {
        Garden *g = &a->garden;
        if (g->w) garden_touch(g, matched->xn*(g->w-1),
            matched->yn*(g->h-1), .92f, 0);
        a->last_key = matched->cp;
        a->last_key_at = seconds_now();
        return;
    }
    /* Unmapped printable symbols still give a small, predictable touch.
       Their code point selects a stable screen region (no random leaps). */
    if (cp >= 33 && cp <= 0x10ffff && a->garden.w) {
        uint32_t hash = cp * 2654435761u;
        float xx = .12f + (float)(hash & 0xffffu) / 65535.f * .76f;
        float yy = .20f + (float)((hash >> 16)&0xffffu) / 65535.f * .62f;
        garden_touch(&a->garden, xx*a->garden.w, yy*a->garden.h, .65f, 0);
    }
}

static void app_mouse(App *a, int code, int x, int y, int released) {
    Garden *g = &a->garden;
    if (!g->w || released) { if (released) a->mouse_down = 0; return; }
    if ((code & 64) != 0) return; /* wheel: do not steal scrolling gestures */
    int button = code & 3;
    int motion = code & 32;
    if (!motion) {
        if (button != 0 && button != 2) return;
        a->mouse_down = button == 0 ? 1 : 2;
    } else if (!a->mouse_down) return;
    if (x < 1 || x > g->cols || y < 1 || y > g->picture_rows) return;
    double now = seconds_now();
    int dist = abs(x-a->last_mouse_x)+abs(y-a->last_mouse_y);
    if (motion && (now-a->last_mouse_at < .045 || dist < 2)) return;
    int quiet = a->mouse_down == 2;
    if (motion && now-a->last_mouse_at < .35 && a->last_mouse_x > 0 &&
        a->last_mouse_y > 0 && dist > 1) {
        int count = dist/3;
        if (count > 8) count = 8;
        for (int i = 1; i <= count; ++i) {
            float t = (float)i/(count+1);
            float cx = (a->last_mouse_x-1)*(1.f-t) + (x-1)*t;
            float cy = (a->last_mouse_y-1)*(1.f-t) + (y-1)*t;
            garden_touch(g, cx, 2.f*cy+.5f, .25f, quiet);
        }
    }
    garden_touch(g, (float)(x-1), 2.f*(y-1)+.5f,
        motion ? .35f : .92f, quiet);
    a->last_mouse_x = x;
    a->last_mouse_y = y;
    a->last_mouse_at = now;
}

static void app_arrow(App *a, char direction) {
    if (direction == 'C') { if (a->speed < 3) ++a->speed; }
    else if (direction == 'D') { if (a->speed > 0) --a->speed; }
    else if (direction == 'A') a->config.palette = (a->config.palette+1)%N_PALETTES;
    else if (direction == 'B') a->config.palette = (a->config.palette+N_PALETTES-1)%N_PALETTES;
    garden_invalidate(&a->garden);
}

static void app_character(App *a, uint32_t cp) {
    if (cp == 3 || cp == 4) { stop_requested = 1; return; }
    if (cp == 9) { a->keys_visible = !a->keys_visible; garden_invalidate(&a->garden); return; }
    if (cp == 10 || cp == 13) { garden_renew(a); return; }
    if (cp == 32) { a->paused = !a->paused; return; }
    if (cp == '?') { a->help = !a->help; return; }
    if (cp == 12) { garden_invalidate(&a->garden); return; }
    if (cp == 127 || cp == 8) return;
    if (cp >= 33) garden_key_touch(a, cp);
}

static void app_input_byte(App *a, unsigned char byte) {
    if (byte == 3 || byte == 4) { stop_requested = 1; return; }
    if (a->input_state == 1) {
        if (byte == '[') { a->input_state = 2; a->escape_length = 0; return; }
        if (byte == 'O') { a->input_state = 3; a->escape_length = 0; return; }
        a->input_state = 0;
        /* Alt+key: just touch the key. Bare Esc exits after a short timeout. */
    }
    if (a->input_state == 2) {
        if (a->escape_length < (int)sizeof(a->escape)-1)
            a->escape[a->escape_length++] = (char)byte;
        else { a->input_state = 0; return; }
        if (byte >= '@' && byte <= '~') {
            a->escape[a->escape_length] = 0;
            if (byte == 'A' || byte == 'B' || byte == 'C' || byte == 'D') app_arrow(a, (char)byte);
            else if (byte == 'M' || byte == 'm') {
                int b, x, y;
                if (sscanf(a->escape, "<%d;%d;%d", &b, &x, &y) == 3)
                    app_mouse(a, b, x, y, byte == 'm');
            } else if (byte == 'Q') {
                unsigned int entry, count;
                int consumed = 0;
                if (sscanf(a->escape, "?%2u;%2u#Q%n", &entry, &count, &consumed) == 2 &&
                    consumed == a->escape_length && entry <= 10 && count <= 10 && entry <= count) {
                    a->palette_report_entry = (int)entry;
                    a->palette_report_count = (int)count;
                }
            }
            a->input_state = 0;
        }
        return;
    }
    if (a->input_state == 3) {
        a->input_state = 0; return;
    }
    if (byte == 27) {
        a->input_state = 1;
        a->esc_at = seconds_now();
        a->utf_expected = 0;
        return;
    }
    if (a->utf_expected) {
        if ((byte & 0xc0) == 0x80) {
            a->utf_value = (a->utf_value << 6) | (byte & 63);
            if (--a->utf_expected == 0) {
                if (a->utf_value >= a->utf_min && a->utf_value <= 0x10ffff &&
                    !(a->utf_value >= 0xd800 && a->utf_value <= 0xdfff))
                    app_character(a, a->utf_value);
            }
            return;
        }
        a->utf_expected = 0;
    }
    if (byte >= 0xc2 && byte <= 0xdf) {
        a->utf_expected = 1; a->utf_value = byte & 31u; a->utf_min = 0x80;
    } else if (byte >= 0xe0 && byte <= 0xef) {
        a->utf_expected = 2; a->utf_value = byte & 15u; a->utf_min = 0x800;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
        a->utf_expected = 3; a->utf_value = byte & 7u; a->utf_min = 0x10000;
    } else if (byte < 128) {
        app_character(a, byte);
    }
}

static void app_read_input(App *a) {
    /* One bounded batch keeps a continuous stream of mouse reports from
       starving rendering, resize handling, and shutdown. VMIN/VTIME are zero. */
    unsigned char buf[512];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        stop_requested = 1;
        return;
    }
    for (ssize_t i = 0; i < n && !stop_requested; ++i) app_input_byte(a, buf[i]);
    if (a->input_state == 1 && seconds_now() - a->esc_at > .10) stop_requested = 1;
}

static int terminal_palette_report(App *a) {
    a->palette_report_entry = -1;
    if (!write_all("\033[#R", 4)) return 0;
    double deadline = seconds_now() + .12;
    while (!stop_requested && a->palette_report_entry < 0) {
        double remaining = deadline - seconds_now();
        if (remaining <= 0) return 0;
        fd_set input;
        FD_ZERO(&input); FD_SET(STDIN_FILENO, &input);
        struct timeval tv = {0, (suseconds_t)(remaining*1e6)};
        int ready = select(STDIN_FILENO+1, &input, NULL, NULL, &tv);
        if (ready > 0) app_read_input(a);
        else if (ready < 0 && errno != EINTR) return 0;
    }
    return !stop_requested && a->palette_report_entry >= 0;
}

static void terminal_prepare_palette(App *a, int automatic_color) {
    /* Probe the reversible palette stack, rather than guessing from TERM.
       Other terminals keep the ordinary RGB renderer. No colors change until
       the terminal confirms that our saved palette is on its stack. */
    if ((a->config.color != COLOR_TRUE && !automatic_color) || !terminal_palette_report(a) ||
        a->palette_report_entry >= 10) return;
    int previous = a->palette_report_entry;
    if (!write_all("\033[#P", 4)) return;
    terminal_palette_saved = 1;
    if (terminal_palette_report(a) && a->palette_report_entry == previous+1) {
        a->indexed_colors = 1;
        a->config.color = COLOR_TRUE;
    }
}

static const char *speed_name(int i) {
    static const char *names[4] = { "still slow", "drift", "flow", "current" };
    return names[i < 0 ? 0 : i > 3 ? 3 : i];
}

static void app_footer(App *a, Output *o) {
    Garden *g = &a->garden;
    if (!a->config.hud || !g->w) return;
    char line[384];
    const char *pal = palettes[a->config.palette].name;
    if (a->help) {
        if (g->cols >= 97) snprintf(line, sizeof(line),
            " tidepool  |  left click: bloom  right click: soften  |  arrows: pace / palette  |  space: pause  enter: renew  esc: exit");
        else if (g->cols >= 70) snprintf(line, sizeof(line),
            " left: bloom  right: soften  arrows: pace / color  space: pause");
        else snprintf(line, sizeof(line), " arrows: speed/color  enter: renew  esc: exit");
    } else if (g->cols >= 105) {
        snprintf(line, sizeof(line), " tidepool / %-7s / %-10s %s  type or click to bloom   Tab keys   ? help   Esc leave",
            pal, a->paused ? "paused" : speed_name(a->speed),
            a->config.renewal == RENEWAL_OFF ? "" :
            a->config.renewal == RENEWAL_GENTLE ? "/ grazing" : "/ lively");
    } else if (g->cols >= 71) {
        snprintf(line, sizeof(line), " tidepool / %s / %s%s   type / click   ? help",
            pal, a->paused ? "paused" : speed_name(a->speed),
            a->config.renewal == RENEWAL_OFF ? "" : " / grazing");
    } else {
        snprintf(line, sizeof(line), " tidepool / %s / %s     ? help", pal,
            a->paused ? "paused" : speed_name(a->speed));
    }
    line[g->cols < (int)sizeof(line)-1 ? g->cols : (int)sizeof(line)-1] = 0;
    if (strcmp(line, a->last_footer) == 0 && !g->redraw) return;
    strncpy(a->last_footer, line, sizeof(a->last_footer)-1);
    a->last_footer[sizeof(a->last_footer)-1] = 0;
    output_fmt(o, "\033[%d;1H\033[0m\033[2K", g->rows);
    if (a->config.color != COLOR_MONO) {
        RGB base = palettes[a->config.palette].stops[0];
        RGB fg = mix_rgb(base, palettes[a->config.palette].stops[3], .55f);
        emit_color(o, encode_color(fg, a->config.color), a->config.color, 0);
        emit_color(o, encode_color(base, a->config.color), a->config.color, 1);
    }
    output_str(o, line);
}

static uint32_t stable_palette_color(uint32_t before, uint32_t after) {
    /* Hold a one-shade change until it accumulates. Stabilize both colors
       before choosing the glyph, so tiny changes cannot alternate a solid
       space with a half-block and force an otherwise invisible repaint. */
    if ((before & after & PALETTE_INDEXED) &&
        abs((int)(before & ~PALETTE_INDEXED)-(int)(after & ~PALETTE_INDEXED)) <= 1)
        return before;
    return after;
}

static int colors_close(uint32_t before, uint32_t after, ColorMode mode) {
    if ((before | after) & PALETTE_INDEXED) return before == after;
    if (mode != COLOR_TRUE) return before == after;
    for (int shift = 0; shift <= 16; shift += 8) {
        int delta = (int)((before >> shift)&255u) - (int)((after >> shift)&255u);
        if (delta < -3 || delta > 3) return 0;
    }
    return 1;
}

static unsigned int app_output_rate(const App *a) {
    return a->indexed_colors ? OUTPUT_INDEXED_BYTES_PER_SECOND : OUTPUT_BYTES_PER_SECOND;
}

static void app_render(App *a) {
    Garden *g = &a->garden;
    if (a->output.len || seconds_now() < a->next_output_at) return;
    Output *o = &a->output;
    o->failed = 0;
    a->output_palette = installed_palette;
    if (a->resized) {
        output_str(o, "\030\033[0m\033[H\033[2J");
        a->resized = 0;
        garden_invalidate(g);
        if (!g->w) output_str(o, "tidepool: enlarge the terminal to continue");
    }
    if (!g->w) {
        a->next_output_at = seconds_now() + (double)o->len / app_output_rate(a);
        return;
    }
    if (g->redraw) garden_invalidate(g);
    if (a->indexed_colors && installed_palette != a->config.palette) {
        output_palette(o, a->config.palette);
        a->output_palette = a->config.palette;
    }
    app_footer(a, o);
    g->redraw = 0;
    uint32_t current_fg = UINT32_MAX, current_bg = UINT32_MAX;
    double now = seconds_now();
    int last_written_row = -1, last_written_col = -1;
    const char *chars = " .,:-=+*#%@";
    /* Capture one simulation state and send its changes together. Pacing is
       between frames, rather than a visible scan across successive states. */
    for (int y = 0; y < g->picture_rows; ++y) {
        for (int x = 0; x < g->cols; ++x) {
            Cell *prev = &g->last[y*g->cols+x];
            int top = (2*y)*g->w+x;
            int bottom = (2*y+1)*g->w+x;
            RGB t = palette_sample(a->config.palette, g->v[top], g->trace[top]);
            RGB b = palette_sample(a->config.palette, g->v[bottom], g->trace[bottom]);
            uint32_t fg = encode_color(t, a->config.color);
            uint32_t bg = encode_color(b, a->config.color);
            if (a->indexed_colors) {
                fg = PALETTE_INDEXED | (uint32_t)(16+palette_level(g->v[top], g->trace[top]));
                bg = PALETTE_INDEXED | (uint32_t)(16+palette_level(g->v[bottom], g->trace[bottom]));
                if (prev->glyph != UINT32_MAX) {
                    if (fg == bg) {
                        bg = stable_palette_color(prev->bg,bg);
                        fg = bg;
                    } else {
                        fg = stable_palette_color(prev->fg,fg);
                        bg = stable_palette_color(prev->bg,bg);
                    }
                }
            }
            uint32_t glyph = 0x2580; /* U+2580 upper-half block */
            if (fg == bg) glyph = ' '; /* A solid cell needs only its background. */
            if (a->config.ascii || a->config.color == COLOR_MONO || !a->config.utf8) {
                float concentration = (g->v[top]+g->v[bottom])*.5f;
                float density = sqrtf(clampf(concentration/.43f, 0.f, 1.f));
                int ci = (int)(density*10.f+.5f);
                if (ci > 10) ci = 10;
                glyph = (unsigned char)chars[ci];
                /* Text needs a readable foreground and a quiet solid backdrop. */
                fg = encode_color(mix_rgb(t,b,.5f), a->config.color);
                bg = encode_color(palettes[a->config.palette].stops[0], a->config.color);
            }
            if (a->keys_visible && g->key_overlay[y*g->cols+x]) {
                uint32_t cp = g->key_overlay[y*g->cols+x];
                if (cp <= 127 || a->config.utf8) {
                    glyph = cp;
                    float k = cp == a->last_key && now-a->last_key_at < .50 ? .90f : .46f;
                    RGB label = mix_rgb(b, palettes[a->config.palette].stops[5], k);
                    fg = encode_color(label, a->config.color);
                }
            }
            if (prev->glyph == glyph && colors_close(prev->fg, fg, a->config.color) &&
                colors_close(prev->bg, bg, a->config.color))
                continue;
            if (last_written_row != y || last_written_col != x-1) {
                if (last_written_row == y) {
                    int gap = x-last_written_col-1;
                    if (gap == 1) output_str(o,"\033[C");
                    else output_fmt(o,"\033[%dC",gap);
                } else output_fmt(o, "\033[%d;%dH", y+1, x+1);
            }
            if (glyph != ' ' && fg != current_fg && bg != current_bg &&
                (fg & bg & PALETTE_INDEXED)) {
                output_fmt(o,"\033[38;5;%u;48;5;%um",fg & ~PALETTE_INDEXED,bg & ~PALETTE_INDEXED);
                current_fg = fg; current_bg = bg;
            }
            if (glyph != ' ' && fg != current_fg) {
                emit_color(o, fg, a->config.color, 0); current_fg = fg;
            }
            if (bg != current_bg) { emit_color(o, bg, a->config.color, 1); current_bg = bg; }
            if (glyph == 0x2580) output_str(o, "▀");
            else if (glyph < 128) {
                char c = (char)glyph;
                output_bytes(o, &c, 1);
            } else {
                char encoded[5]; encode_utf8(glyph, encoded); output_str(o, encoded);
            }
            prev->fg = fg; prev->bg = bg; prev->glyph = glyph;
            last_written_row = y; last_written_col = x;
        }
    }
    if (o->failed) stop_requested = 1;
    a->next_output_at = seconds_now() + (double)o->len / app_output_rate(a);
}

static void read_size(int *cols, int *rows) {
    struct winsize ws = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || !ws.ws_col || !ws.ws_row) {
        *cols = 80; *rows = 24;
    } else { *cols = ws.ws_col; *rows = ws.ws_row; }
}

static ColorMode detected_color(void) {
    const char *ct = getenv("COLORTERM"), *term = getenv("TERM");
    if (ci_contains(ct, "truecolor") || ci_contains(ct, "24bit") ||
        ci_contains(term, "direct") || getenv("KITTY_WINDOW_ID") || getenv("WEZTERM_PANE"))
        return COLOR_TRUE;
    if (ci_contains(term, "256color")) return COLOR_256;
    return COLOR_16;
}

static int parse_color(const char *str, ColorMode *mode) {
    if (!strcmp(str, "auto")) *mode = COLOR_AUTO;
    else if (!strcmp(str, "truecolor") || !strcmp(str, "24")) *mode = COLOR_TRUE;
    else if (!strcmp(str, "256")) *mode = COLOR_256;
    else if (!strcmp(str, "16")) *mode = COLOR_16;
    else if (!strcmp(str, "mono") || !strcmp(str, "none")) *mode = COLOR_MONO;
    else return 0;
    return 1;
}

static void print_help(FILE *out) {
    fputs(
        "tidepool — a quiet reaction–diffusion garden for your terminal\n\n"
        "Usage: tidepool [options]\n\n"
        "  --palette NAME      lagoon (default), orchid, moss, ember\n"
        "  --layout us|fi      QWERTY / Finnish-Swedish physical keyboard (default: us)\n"
        "  --color MODE        auto (default), truecolor, 256, 16, mono\n"
        "  --ascii             Use ASCII shades instead of Unicode half-blocks\n"
        "  --mono              ASCII without color\n"
        "  --fps N             Simulation rate 5–30 (default: 14)\n"
        "  --renewal MODE      off (default), gentle, active: local grazing\n"
        "  --seed N            Repeat a garden's initial random seed\n"
        "  --no-mouse          Don't enable terminal mouse reporting\n"
        "  --no-hud            Fill the bottom line with the garden\n"
        "  -h, --help          Show this help\n\n"
        "While running:\n"
        "  Any letter / number / symbol   Touch its position in a keyboard-shaped map\n"
        "  Mouse left / drag             Bloom gently at a position\n"
        "  Mouse right / drag            Soften a region\n"
        "  Tab                           Show / hide the keyboard map\n"
        "  Space                         Pause / resume\n"
        "  Left / Right arrows           Slower / faster evolution\n"
        "  Up / Down arrows              Cycle color palettes\n"
        "  Enter                         Create another garden\n"
        "  ?                             Show / hide expanded help\n"
        "  Esc or Ctrl-C                 Leave, restoring terminal\n\n"
        "Requires a Unix-like terminal with ANSI cursor controls. Best in UTF-8.\n"
        "No network, dependencies, sound, or files written at runtime.\n"
        "With mouse reporting enabled, use Shift+drag to select terminal text.\n",
        out);
}

static int parse_args(int argc, char **argv, Config *cfg) {
    cfg->fps = 14; cfg->mouse = 1; cfg->hud = 1; cfg->ascii = 0;
    cfg->color = COLOR_AUTO; cfg->layout = LAYOUT_US; cfg->palette = 0;
    cfg->seed = 0; cfg->seed_given = 0; cfg->renewal = RENEWAL_OFF;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) { print_help(stdout); return 2; }
        if (!strcmp(arg, "--ascii")) cfg->ascii = 1;
        else if (!strcmp(arg, "--mono")) { cfg->color = COLOR_MONO; cfg->ascii = 1; }
        else if (!strcmp(arg, "--no-mouse")) cfg->mouse = 0;
        else if (!strcmp(arg, "--no-hud")) cfg->hud = 0;
        else if (!strcmp(arg, "--palette") && i+1 < argc) {
            ++i; int found = 0;
            for (int p = 0; p < N_PALETTES; ++p)
                if (!strcmp(argv[i],palettes[p].name)) { cfg->palette = p; found = 1; break; }
            if (!found) { fprintf(stderr,"tidepool: unknown palette: %s\n",argv[i]); return 0; }
        } else if (!strcmp(arg, "--layout") && i+1 < argc) {
            ++i;
            if (!strcmp(argv[i], "us")) cfg->layout = LAYOUT_US;
            else if (!strcmp(argv[i], "fi")) cfg->layout = LAYOUT_FI;
            else { fprintf(stderr,"tidepool: unknown layout: %s\n",argv[i]); return 0; }
        } else if (!strcmp(arg, "--color") && i+1 < argc) {
            if (!parse_color(argv[++i],&cfg->color)) {
                fprintf(stderr,"tidepool: unknown color mode: %s\n",argv[i]); return 0;
            }
        } else if (!strcmp(arg, "--renewal") && i+1 < argc) {
            const char *mode = argv[++i];
            if (!strcmp(mode, "off")) cfg->renewal = RENEWAL_OFF;
            else if (!strcmp(mode, "gentle")) cfg->renewal = RENEWAL_GENTLE;
            else if (!strcmp(mode, "active")) cfg->renewal = RENEWAL_ACTIVE;
            else { fprintf(stderr,"tidepool: unknown renewal mode: %s\n",mode); return 0; }
        } else if (!strcmp(arg, "--fps") && i+1 < argc) {
            char *end; long fps = strtol(argv[++i], &end, 10);
            if (*end || fps < 5 || fps > 30) { fputs("tidepool: --fps must be 5–30\n",stderr); return 0; }
            cfg->fps = (int)fps;
        } else if (!strcmp(arg, "--seed") && i+1 < argc) {
            char *end; unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end || value > UINT32_MAX) { fputs("tidepool: invalid seed\n",stderr); return 0; }
            cfg->seed = (unsigned int)value; cfg->seed_given = 1;
        } else {
            fprintf(stderr,"tidepool: unknown or incomplete option: %s\n",arg);
            fputs("Try: tidepool --help\n",stderr);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    App a = {0};
    int args = parse_args(argc, argv, &a.config);
    if (args != 1) return args == 2 ? 0 : 2;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("tidepool: run in an interactive terminal (TTY).\n",stderr); return 2;
    }
    const char *term = getenv("TERM");
    if (term && !strcmp(term, "dumb")) {
        fputs("tidepool: TERM=dumb cannot draw an interactive screen.\n",stderr); return 2;
    }
    setlocale(LC_CTYPE, "");
    const char *codeset = nl_langinfo(CODESET);
    a.config.utf8 = ci_contains(codeset,"UTF-8") || ci_contains(codeset,"UTF8");
    if (!a.config.utf8) a.config.ascii = 1;
    int automatic_color = a.config.color == COLOR_AUTO;
    if (automatic_color) a.config.color = detected_color();
    if (a.config.color == COLOR_MONO) a.config.ascii = 1;
    a.rng = a.config.seed_given ? a.config.seed :
        (unsigned int)time(NULL) ^ ((unsigned int)getpid()<<11) ^ 0x9e3779b9u;
    if (!a.rng) a.rng = 0x1234abcd;
    a.speed = 1;
    make_keys(&a);
    if (atexit(terminal_restore) != 0) { fputs("tidepool: cannot install cleanup\n",stderr); return 2; }
    if (!install_signal_handlers()) {
        fputs("tidepool: cannot install signal handlers\n", stderr); return 2;
    }
    if (!terminal_start(a.config.mouse)) {
        fputs("tidepool: could not enter raw terminal mode\n",stderr); return 2;
    }
    int cols, rows;
    read_size(&cols, &rows);
    if (!garden_resize(&a, cols, rows)) {
        fputs("tidepool: terminal too large or allocation failed\n",stderr); return 2;
    }
    terminal_prepare_palette(&a, automatic_color);
    double next_frame = seconds_now();
    const double interval = 1.0/a.config.fps;
    while (!stop_requested) {
        app_read_input(&a);
        if (stop_requested) break;
        if (winch_requested) {
            winch_requested = 0;
            read_size(&cols,&rows);
            if (!garden_resize(&a,cols,rows)) { stop_requested = 1; break; }
        }
        if (a.input_state == 1 && seconds_now()-a.esc_at > .10) {
            stop_requested = 1; break;
        }
        double now = seconds_now();
        if (now >= next_frame) {
            if (!a.garden.w) {
                /* When the window becomes sufficiently large again, it grows. */
                read_size(&cols,&rows);
                if (cols >= MIN_COLS && rows >= MIN_ROWS) {
                    if (!garden_resize(&a,cols,rows)) { stop_requested = 1; break; }
                }
            }
            if (a.garden.w) {
                if (!a.paused) {
                    static const int steps[4] = {1,2,4,7};
                    for (int i=0;i<steps[a.speed];++i) {
                        garden_step(&a.garden);
                        garden_ecology_step(&a);
                    }
                    garden_trace_fade(&a.garden);
                }
            }
            if (now-next_frame > interval*1.5) next_frame = now + interval;
            else next_frame += interval;
        }
        if (a.input_state != 1) app_render(&a);
        app_flush_output(&a);
        now = seconds_now();
        double wait_sec = next_frame-now;
        int output_pending = a.output.len != 0;
        if (!output_pending && a.next_output_at > now && a.next_output_at-now < wait_sec)
            wait_sec = a.next_output_at-now;
        if (a.input_state == 1) {
            double esc_wait = .10-(now-a.esc_at);
            if (esc_wait < wait_sec) wait_sec = esc_wait;
        }
        if (wait_sec < .001) wait_sec = .001;
        if (wait_sec > .10) wait_sec = .10;
        struct timeval tv;
        tv.tv_sec = (long)wait_sec;
        tv.tv_usec = (suseconds_t)((wait_sec-tv.tv_sec)*1e6);
        fd_set fds, output;
        FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        FD_ZERO(&output);
        int wait_output = output_pending;
        if (wait_output) FD_SET(STDOUT_FILENO, &output);
        int ready = select(STDOUT_FILENO+1, &fds, wait_output ? &output : NULL, NULL, &tv);
        if (ready > 0) {
            if (FD_ISSET(STDIN_FILENO, &fds)) app_read_input(&a);
        } else if (ready < 0 && errno != EINTR) {
            stop_requested = 1;
        }
    }
    garden_dispose(&a.garden);
    return 0;
}
