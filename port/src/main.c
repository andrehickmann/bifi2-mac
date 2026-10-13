/*
 * Bi-Fi 2 - Action in Hollywood, native macOS host.
 *
 * Runs the game like BIFI.BAT did: title program XP00.EXE, and when it
 * exits with code 0 the main program XP01.EXE. Rendering, input and sound
 * go through SDL2.
 */
#include "host.h"

#include <SDL.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <execinfo.h>

#define WINDOW_W       960
#define WINDOW_H       720
#define AUDIO_RATE     44100

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    SDL_AudioDeviceID audio;
    uint32_t pixels[64000];
} Video;

/* SDL scancode -> PC/XT set 1 make code, plus the BIOS ASCII value. */
static uint8_t xt_scancode(SDL_Scancode s)
{
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03;
    case SDL_SCANCODE_3: return 0x04; case SDL_SCANCODE_4: return 0x05;
    case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09;
    case SDL_SCANCODE_9: return 0x0A; case SDL_SCANCODE_0: return 0x0B;
    case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11;
    case SDL_SCANCODE_E: return 0x12; case SDL_SCANCODE_R: return 0x13;
    case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17;
    case SDL_SCANCODE_O: return 0x18; case SDL_SCANCODE_P: return 0x19;
    case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F;
    case SDL_SCANCODE_D: return 0x20; case SDL_SCANCODE_F: return 0x21;
    case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25;
    case SDL_SCANCODE_L: return 0x26; case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D;
    case SDL_SCANCODE_C: return 0x2E; case SDL_SCANCODE_V: return 0x2F;
    case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_F1: return 0x3B; case SDL_SCANCODE_F2: return 0x3C;
    case SDL_SCANCODE_F3: return 0x3D; case SDL_SCANCODE_F4: return 0x3E;
    case SDL_SCANCODE_F5: return 0x3F; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42;
    case SDL_SCANCODE_F9: return 0x43; case SDL_SCANCODE_F10: return 0x44;
    case SDL_SCANCODE_UP: return 0x48; case SDL_SCANCODE_LEFT: return 0x4B;
    case SDL_SCANCODE_RIGHT: return 0x4D; case SDL_SCANCODE_DOWN: return 0x50;
    default: return 0;
    }
}

static uint16_t bios_key(SDL_Keycode k, uint8_t scan)
{
    if (k == SDLK_RETURN) return scan << 8 | 13;
    if (k == SDLK_ESCAPE) return scan << 8 | 27;
    if (k == SDLK_SPACE) return scan << 8 | ' ';
    if (k == SDLK_BACKSPACE) return scan << 8 | 8;
    if (k >= SDLK_a && k <= SDLK_z) return scan << 8 | (uint8_t)k;
    if (k >= SDLK_0 && k <= SDLK_9) return scan << 8 | (uint8_t)k;
    return scan << 8;
}

/* Mouse events arrive in the renderer's logical 320x240 space; the game
 * screen is 320x200 stretched to 4:3. */
static void window_to_game(Video *v, int wx, int wy, int *gx, int *gy)
{
    (void)v;
    *gx = wx < 0 ? 0 : wx > 319 ? 319 : wx;
    int y = wy * 200 / 240;
    *gy = y < 0 ? 0 : y > 199 ? 199 : y;
}

static void handle_events(Host *h, Video *v)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            h->quit = true;
            break;
        case SDL_MOUSEMOTION: {
            int gx, gy;
            window_to_game(v, e.motion.x, e.motion.y, &gx, &gy);
            h->mouse.target_x = gx;
            h->mouse.target_y = gy;
            h->mouse.has_target = true;
            break;
        }
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int bit = e.button.button == SDL_BUTTON_LEFT ? 1 :
                      e.button.button == SDL_BUTTON_RIGHT ? 2 : 4;
            int b = h->mouse.buttons;
            b = e.type == SDL_MOUSEBUTTONDOWN ? (b | bit) : (b & ~bit);
            mouse_set_buttons(h, b);
            break;
        }
        case SDL_KEYDOWN: case SDL_KEYUP: {
            SDL_Keycode k = e.key.keysym.sym;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_GUI) && k == SDLK_f) {
                uint32_t full = SDL_GetWindowFlags(v->win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(v->win, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                break;
            }
            if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_GUI) && k == SDLK_q) {
                h->quit = true;
                break;
            }
            if (e.key.repeat)
                break;
            uint8_t sc = xt_scancode(e.key.keysym.scancode);
            if (sc)
                kbd_press(h, sc, e.type == SDL_KEYUP, bios_key(k, sc));
            break;
        }
        }
    }
}

/*
 * Test automation, driven by environment variables:
 *   BIFI_SHOTS=dir      save a BMP every two seconds
 *   BIFI_EVENTS=list    "program@seconds:x,y[:click]" entries separated by ';'
 *   BIFI_SECONDS=n      stop each program after n seconds
 */
typedef struct {
    char program[16];
    double at;
    int x, y;
    int button;            /* 0 none, 1 left, 2 right */
    int state;
} TestEvent;

static TestEvent test_events[32];
static int n_test_events;
static const char *shot_dir;
static double program_start, last_shot;
static int shot_no;

static void parse_test_events(void)
{
    const char *s = getenv("BIFI_EVENTS");
    shot_dir = getenv("BIFI_SHOTS");
    if (!s)
        return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    char *save, *tok = strtok_r(buf, ";", &save);
    while (tok && n_test_events < 32) {
        TestEvent *e = &test_events[n_test_events];
        char click[16] = "";
        if (sscanf(tok, "%15[^@]@%lf:%d,%d:%15s", e->program, &e->at, &e->x, &e->y, click) >= 4) {
            e->button = !strcmp(click, "click") ? 1 : !strcmp(click, "rclick") ? 2 : 0;
            n_test_events++;
        }
        tok = strtok_r(NULL, ";", &save);
    }
}

static void run_test_events(Host *h, Video *v)
{
    double el = now_seconds() - program_start;
    /* BIFI_KEYS: characters typed one per second, starting after 2 s */
    const char *keys = getenv("BIFI_KEYS");
    static int keys_sent;
    static uint8_t release_sc;
    static double release_at;
    if (release_sc && el > release_at) {
        kbd_press(h, release_sc, true, 0);
        release_sc = 0;
    }
    if (keys && keys_sent < (int)strlen(keys) && el > 2 + keys_sent) {
        char c = keys[keys_sent++];
        static const char *digits = "1234567890";
        uint8_t sc = 0;
        const char *d = strchr(digits, c);
        if (d) sc = 2 + (d - digits);
        else if (c == 'x') sc = 0x2D;
        else if (c == 'j') sc = 0x24;
        else if (c == ' ') sc = 0x39;
        else if (c == '~') { sc = 0x1C; c = 13; }
        else if (c == '!') { sc = 0x01; c = 27; }
        kbd_press(h, sc, false, sc << 8 | (uint8_t)c);
        release_sc = sc;
        release_at = el + 0.2;
    }
    for (int i = 0; i < n_test_events; i++) {
        TestEvent *e = &test_events[i];
        if (strcmp(e->program, h->program))
            continue;
        if (e->state == 0 && el >= e->at) {
            h->mouse.target_x = e->x;
            h->mouse.target_y = e->y;
            h->mouse.has_target = true;
            e->state = 1;
        } else if (e->state == 1 && el >= e->at + 0.5) {
            if (e->button)
                mouse_set_buttons(h, h->mouse.buttons | e->button);
            e->state = 2;
        } else if (e->state == 2 && el >= e->at + 0.8) {
            if (e->button)
                mouse_set_buttons(h, h->mouse.buttons & ~e->button);
            e->state = 3;
        }
    }
    if (shot_dir && now_seconds() - last_shot >= 2.0) {
        last_shot = now_seconds();
        SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(v->pixels, 320, 200, 32, 320 * 4,
                                                               SDL_PIXELFORMAT_ARGB8888);
        char path[1200];
        snprintf(path, sizeof path, "%s/%s_%03d.bmp", shot_dir, h->program, shot_no++);
        SDL_SaveBMP(surf, path);
        SDL_FreeSurface(surf);
    }
}

static void present(Host *h, Video *v)
{
    vga_render(h, v->pixels);
    run_test_events(h, v);
    SDL_UpdateTexture(v->tex, NULL, v->pixels, 320 * 4);
    SDL_RenderClear(v->ren);
    SDL_RenderCopy(v->ren, v->tex, NULL, NULL);
    SDL_RenderPresent(v->ren);
}

static void queue_audio(Host *h, Video *v, double seconds)
{
    if (!v->audio)
        return;
    int frames = (int)(seconds * AUDIO_RATE);
    if (frames <= 0)
        return;
    if (SDL_GetQueuedAudioSize(v->audio) > AUDIO_RATE * 4 / 10)  /* keep ~100 ms */
        return;
    static int16_t buf[AUDIO_RATE * 2];
    if (frames > AUDIO_RATE)
        frames = AUDIO_RATE;
    sb_render(h, buf, frames, AUDIO_RATE);
    SDL_QueueAudio(v->audio, buf, frames * 4);
    static FILE *wav;                              /* BIFI_WAV: raw PCM dump for tests */
    if (!wav && getenv("BIFI_WAV"))
        wav = fopen(getenv("BIFI_WAV"), "wb");
    if (wav)
        fwrite(buf, 4, frames, wav), fflush(wav);
}

/*
 * Run one DOS program to completion; returns its exit code.
 *
 * The guest runs in small slices paced against the host clock, so timer
 * and sound interrupts arrive evenly (the sound driver refills a 256-byte
 * DMA buffer every 6 ms). Frames are presented at 60 Hz in between.
 */
static int run_program(Host *h, Video *v, const char *exe)
{
    if (host_load(h, exe, "") < 0)
        return 255;
    /* BIFI_LOADSTATE / BIFI_SAVESTATE=path@seconds: snapshots for tests */
    const char *load_state = getenv("BIFI_LOADSTATE");
    if (load_state && !snapshot_load(h, load_state))
        host_log("cannot load snapshot %s", load_state);
    char save_state[1024] = "";
    double save_at = -1;
    if (getenv("BIFI_SAVESTATE")) {
        snprintf(save_state, sizeof save_state, "%s", getenv("BIFI_SAVESTATE"));
        char *at = strrchr(save_state, '@');
        if (at) {
            *at = 0;
            save_at = atof(at + 1);
        }
    }
    double start = now_seconds();
    double next_frame = start, next_events = start, audio_clock = start;
    program_start = start;
    shot_no = 0;
    const char *limit = getenv("BIFI_SECONDS");
    /* The emulated clock (h->vtime) advances with executed code and port
     * accesses; keep it in step with the host clock. The game calibrates
     * delay loops at start-up, so emulated time must stay consistent. */
    if (getenv("BIFI_SPEED"))
        h->speed = atof(getenv("BIFI_SPEED"));
    host_set_slice(h, getenv("BIFI_SLICE") ? atoi(getenv("BIFI_SLICE")) : 150);
    while (!h->exited && !h->quit) {
        double now = now_seconds();
        if (limit && now - start > atof(limit))
            break;
        if (save_at >= 0 && now - start >= save_at) {
            host_log(snapshot_save(h, save_state) ? "snapshot saved" : "snapshot failed");
            save_at = -1;
        }
        if (now >= next_events) {
            handle_events(h, v);
            next_events = now + 0.004;
        }
        double wall = now_seconds() - start;
        if (h->vtime > wall + 0.0003) {
            /* short naps: devices are updated between runs */
            double ahead = h->vtime - wall;
            usleep((useconds_t)((ahead > 0.0004 ? 0.0004 : ahead) * 1e6));
        } else {
            host_slice(h);
            if (h->vtime < wall - 0.05)        /* host too slow: skip ahead */
                h->vtime = wall - 0.05;
        }
        static double stat_t;
        static uint64_t stat_b;
        static uint32_t stat_irq0;
        if (h->trace && now - stat_t > 3) {
            uint32_t late, total, under;
            sb_stats(h, &late, &total, &under);
            host_log("guest code rate %.1f MB/s, timer irqs %.0f/s (skipped %u merged %u reprog %u), sound irqs %u late %u, stale blocks %u",
                     (h->raw_bytes - stat_b) / (now - stat_t) / 1e6,
                     (h->irq_delivered[0] - stat_irq0) / (now - stat_t), h->irq_lost[0], h->irq_merged, h->pit_reprograms, total, late, under);
            stat_t = now;
            stat_b = h->raw_bytes;
            stat_irq0 = h->irq_delivered[0];
        }
        if (now >= next_frame) {
            queue_audio(h, v, now - audio_clock);
            audio_clock = now;
            present(h, v);
            next_frame += 1.0 / 60.0;
            if (next_frame < now)
                next_frame = now + 1.0 / 60.0;
        }
    }
    return h->exit_code;
}

static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* The game needs a sound configuration; create one if it is missing. */
static void ensure_sound_config(const char *game)
{
    char path[1100];
    snprintf(path, sizeof path, "%s/PSMCFG4.DAT", game);
    if (file_exists(path))
        return;
    uint8_t cfg[36] = "PSM2CFG, VERSION 4.00";
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(cfg, 1, sizeof cfg, f);
        fclose(f);
    }
}

/* Copy a directory tree (used once to set up the writable game directory). */
static void copy_tree(const char *src, const char *dst)
{
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "/bin/mkdir -p '%s' && /bin/cp -R '%s/' '%s/'", dst, src, dst);
    if (system(cmd) != 0)
        fprintf(stderr, "copying game data failed\n");
}

/*
 * Game root (the directory that plays drive C:). Explicit argument for
 * development; inside the app bundle the pristine files live in
 * Resources/game and are copied to Application Support on first start, so
 * savegames and settings are writable.
 */
static void find_root(int argc, char **argv, char *root, size_t n)
{
    if (argc > 1 && argv[1][0] != '-') {
        snprintf(root, n, "%s", argv[1]);
        return;
    }
    char *base = SDL_GetBasePath();
    char *pref = SDL_GetPrefPath("", "Bi-Fi 2");
    char bundled[1100];
    snprintf(bundled, sizeof bundled, "%sgame", base ? base : "./");
    snprintf(root, n, "%sC", pref ? pref : "./");
    char marker[1200];
    snprintf(marker, sizeof marker, "%s/BIFI/XP01.EXE", root);
    if (!file_exists(marker))
        copy_tree(bundled, root);
    SDL_free(base);
    SDL_free(pref);
}

/* Print a backtrace on fatal signals, so crash reports are useful. */
static void crash_handler(int sig)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    fprintf(stderr, "fatal signal %d\n", sig);
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char **argv)
{
    signal(SIGABRT, crash_handler);
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS, crash_handler);
    char root[1100];
    find_root(argc, argv, root, sizeof root);
    char game[1100];
    snprintf(game, sizeof game, "%s/BIFI", root);
    if (!file_exists(game)) {
        fprintf(stderr, "game directory %s not found\n", game);
        return 1;
    }
    ensure_sound_config(game);
    parse_test_events();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    Video v = {0};
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    v.win = SDL_CreateWindow("Bi-Fi 2 – Action in Hollywood", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, WINDOW_W, WINDOW_H,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    v.ren = SDL_CreateRenderer(v.win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(v.ren, 320, 240);        /* 4:3, like a CRT */
    v.tex = SDL_CreateTexture(v.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200);
    SDL_ShowCursor(SDL_DISABLE);

    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    v.audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (v.audio)
        SDL_PauseAudioDevice(v.audio, 0);

    const char *programs[] = {"XP00.EXE", "XP01.EXE"};
    int nprog = 2;
    if (getenv("BIFI_PROGRAM")) {
        programs[0] = getenv("BIFI_PROGRAM");
        nprog = 1;
    }
    for (int i = 0; i < nprog; i++) {
        Host *h = calloc(1, sizeof(Host));
        host_init(h, root);
        h->trace = getenv("BIFI_TRACE") != NULL;
        char exe[1200];
        snprintf(exe, sizeof exe, "%s/%s", game, programs[i]);
        int code = run_program(h, &v, exe);
        bool quit = h->quit;
        host_free(h);
        free(h);
        if (quit || code != 0)
            break;
    }
    SDL_Quit();
    return 0;
}
