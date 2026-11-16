/*
 * main.c - SDL2 frontend for the VMC10 Linux port.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mc10.h"
#include "debugger.h"

#define VERSION "1.0 (re-engineered from Virtual MC-10 0.73)"

/* ------------------------------------------------------------------ */
/* settings                                                            */

typedef struct {
    char rom[PATH_MAX], chr[PATH_MAX], chrx[PATH_MAX], mcxrom[PATH_MAX];
    char mcx32flash[PATH_MAX], sddir[PATH_MAX], serial[256], emcee[PATH_MAX];
    bool emcee_on;
    int  scale;
    bool fullscreen;
    bool sensible;            /* keyboard mode */
    bool debug_start;
    char config_path[PATH_MAX];
} ui_settings;

static mc10 M;
static ui_settings S;
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID adev;
static bool warp, show_help, running = true;
static uint32_t osd_until;
static char exe_dir[PATH_MAX];
static SDL_GameController *pad;
static bool dirty[6];           /* settings changed via hotkeys: scale, volume, sound, keyboard, artifact_phase, remap */

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static bool parse_rgb(const char *v, uint32_t *out)
{
    unsigned r, g, b;
    if (sscanf(v, "%u,%u,%u", &r, &g, &b) == 3 && r < 256 && g < 256 && b < 256) {
        *out = (r << 16) | (g << 8) | b;
        return true;
    }
    if (*v == '#' && strlen(v) == 7) { *out = (uint32_t)strtoul(v + 1, NULL, 16); return true; }
    return false;
}

static const char *color_names[C_NUMCOLORS] = {
    "green", "yellow", "blue", "red", "buff", "cyan", "magenta", "orange",
    "black", "darkgreen", "darkorange", "artifactblue", "artifactorange"
};

static void apply_setting(mc10_config *cfg, const char *k, const char *v)
{
    if (!strcmp(k, "rom")) snprintf(S.rom, sizeof S.rom, "%s", v);
    else if (!strcmp(k, "chr")) snprintf(S.chr, sizeof S.chr, "%s", v);
    else if (!strcmp(k, "chrx")) snprintf(S.chrx, sizeof S.chrx, "%s", v);
    else if (!strcmp(k, "ram")) cfg->expansion = atoi(v) >= 16 ? EXP_16K : EXP_NONE;
    else if (!strcmp(k, "expansion")) {
        if (!strcasecmp(v, "mcx128")) cfg->expansion = EXP_MCX128;
        else if (!strcasecmp(v, "mcx32sd")) cfg->expansion = EXP_MCX32SD;
        else if (!strcasecmp(v, "16k")) cfg->expansion = EXP_16K;
        else cfg->expansion = EXP_NONE;
    }
    else if (!strcmp(k, "mcx128rom")) snprintf(S.mcxrom, sizeof S.mcxrom, "%s", v);
    else if (!strcmp(k, "mcx32sdflash")) snprintf(S.mcx32flash, sizeof S.mcx32flash, "%s", v);
    else if (!strcmp(k, "sdcard")) snprintf(S.sddir, sizeof S.sddir, "%s", v);
    else if (!strcmp(k, "serial")) snprintf(S.serial, sizeof S.serial, "%s", v);
    else if (!strcmp(k, "emcee")) { S.emcee_on = true; if (strcmp(v, "1")) snprintf(S.emcee, sizeof S.emcee, "%s", v); }
    else if (!strcmp(k, "internal8k")) cfg->internal8k = atoi(v);
    else if (!strcmp(k, "scale")) S.scale = atoi(v);
    else if (!strcmp(k, "fullscreen")) S.fullscreen = atoi(v);
    else if (!strcmp(k, "volume")) cfg->volume = atoi(v);
    else if (!strcmp(k, "sound")) cfg->sound = atoi(v);
    else if (!strcmp(k, "keyboard")) S.sensible = !strcasecmp(v, "sensible");
    else if (!strcmp(k, "printer")) snprintf(cfg->printer_path, sizeof cfg->printer_path, "%s", v);
    else if (!strcmp(k, "tapedir")) snprintf(cfg->tape_out_dir, sizeof cfg->tape_out_dir, "%s", v);
    else if (!strcmp(k, "fast_typing")) cfg->fast_typing = atoi(v);
    else if (!strcmp(k, "artifact")) cfg->no_artifact = !atoi(v);
    else if (!strcmp(k, "artifact_phase")) cfg->artifact_phase = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "remap")) cfg->no_remap = !atoi(v);
    else if (!strcmp(k, "lowercase_inverse")) cfg->lower_as_inverse = atoi(v);
    else if (!strncmp(k, "color.", 6)) {
        for (int i = 0; i < C_NUMCOLORS; i++)
            if (!strcmp(k + 6, color_names[i])) parse_rgb(v, &cfg->palette[i]);
    }
}

static void load_config(mc10_config *cfg)
{
    FILE *f = fopen(S.config_path, "r");
    if (!f) return;
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || line[0] == ';' || line[0] == '[' || !eq) continue;
        *eq = 0;
        trim(line);
        char *v = eq + 1;
        trim(v);
        apply_setting(cfg, line, v);
    }
    fclose(f);
}

/* Persist settings the user toggles from the keyboard. */
static void save_config(void)
{
    const char *keys[] = { "scale", "volume", "sound", "keyboard", "artifact_phase", "remap" };
    char vals[6][32];
    snprintf(vals[0], 32, "%d", S.scale);
    snprintf(vals[1], 32, "%d", M.cfg.volume);
    snprintf(vals[2], 32, "%d", M.cfg.sound);
    snprintf(vals[3], 32, "%s", S.sensible ? "sensible" : "emulated");
    snprintf(vals[4], 32, "%d", M.cfg.artifact_phase);
    snprintf(vals[5], 32, "%d", !M.cfg.no_remap);

    char *out = NULL;
    size_t outlen = 0;
    FILE *mem = open_memstream(&out, &outlen);
    if (!mem) return;
    bool any = false;
    for (int i = 0; i < 6; i++) any |= dirty[i];
    if (!any) { fclose(mem); free(out); return; }
    bool done[6] = { 0 };
    for (int i = 0; i < 6; i++) done[i] = !dirty[i];
    FILE *f = fopen(S.config_path, "r");
    if (f) {
        char line[PATH_MAX + 64];
        while (fgets(line, sizeof line, f)) {
            char key[64] = "";
            sscanf(line, " %63[^= \t]", key);
            int hit = -1;
            for (int i = 0; i < 6; i++) if (!strcmp(key, keys[i]) && strchr(line, '=')) hit = i;
            if (hit >= 0 && dirty[hit]) { fprintf(mem, "%s = %s\n", keys[hit], vals[hit]); done[hit] = true; }
            else fputs(line, mem);
        }
        fclose(f);
    } else {
        fputs("# VMC10 for Linux settings.  See README.md for all keys.\n", mem);
    }
    for (int i = 0; i < 6; i++) if (!done[i]) fprintf(mem, "%s = %s\n", keys[i], vals[i]);
    fclose(mem);

    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", S.config_path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }
    f = fopen(S.config_path, "w");
    if (f) { fwrite(out, 1, outlen, f); fclose(f); }
    free(out);
}

/* ------------------------------------------------------------------ */
/* file location helpers                                               */

static bool is_dir_path(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists(const char *p)
{
    struct stat st;
    return p && *p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool find_data_file(const char *name, char *out, size_t outlen)
{
    const char *home = getenv("HOME");
    char dirs[8][PATH_MAX];
    int n = 0;
    snprintf(dirs[n++], PATH_MAX, "roms");
    snprintf(dirs[n++], PATH_MAX, "%s/roms", exe_dir);
    snprintf(dirs[n++], PATH_MAX, "%s/../share/vmc10/roms", exe_dir);
    if (home) snprintf(dirs[n++], PATH_MAX, "%s/.local/share/vmc10/roms", home);
    snprintf(dirs[n++], PATH_MAX, "/usr/local/share/vmc10/roms");
    snprintf(dirs[n++], PATH_MAX, "/usr/share/vmc10/roms");
    for (int i = 0; i < n; i++) {
        char lower[64], upper[64];
        size_t k;
        for (k = 0; name[k] && k < 63; k++) { lower[k] = (char)tolower((unsigned char)name[k]); upper[k] = (char)toupper((unsigned char)name[k]); }
        lower[k] = upper[k] = 0;
        const char *variants[3] = { name, lower, upper };
        for (int v = 0; v < 3; v++) {
            snprintf(out, outlen, "%s/%s", dirs[i], variants[v]);
            if (file_exists(out)) return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* on-screen text using the MC-10's own character generator            */

static void osd_text(uint32_t fb[FB_H][FB_W], int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    for (; *s; s++, x += 8) {
        int c = toupper((unsigned char)*s);
        int code = (c >= 0x40 && c < 0x60) ? c - 0x40 : (c >= 0x20 && c < 0x40) ? c : 0x20;
        if (x + 8 > FB_W) break;
        for (int r = 0; r < 12 && y + r < FB_H; r++) {
            uint8_t g = M.chr[code * 12 + r];
            for (int i = 0; i < 8; i++) fb[y + r][x + i] = (g & (0x80 >> i)) ? fg : bg;
        }
    }
}

static const char *help_lines[] = {
    "  VMC10 FOR LINUX - KEYS",
    "",
    "F1  THIS HELP",
    "F2  INSERT TAPE  SHIFT-F2 REWIND",
    "F3  QUICKTYPE A TEXT FILE",
    "F4  PASTE CLIPBOARD",
    "F5  KEYBOARD EMULATED/SENSIBLE",
    "F6  SOUND ON/OFF",
    "F7  WARP SPEED ON/OFF",
    "F8  DEBUGGER (IN TERMINAL)",
    "F9  RESET   SHIFT-F9 POWER OFF/ON",
    "F10 WINDOW SIZE",
    "F11 SCREENSHOT   F12 FULL SCREEN",
    "ESC BREAK   CTRL CONTROL",
    "ALT-A SWAP ARTIFACT COLOURS",
    "ALT-M MCX32-SD $4200 MAPPING",
    "ARROWS = W A S Z (GAMES)",
    "DROP FILES ON WINDOW TO LOAD",
    NULL
};

static void present(void)
{
    static uint32_t buf[FB_H][FB_W];
    memcpy(buf, M.fb, sizeof buf);
    const uint32_t white = 0xF0F0F0, dark = 0x101010;

    if (show_help) {
        int n = 0;
        while (help_lines[n]) n++;
        int y0 = (FB_H - n * 12) / 2;
        for (int y = y0 - 4; y < y0 + n * 12 + 4; y++)
            for (int x = 16; x < FB_W - 16; x++) buf[y][x] = dark;
        for (int i = 0; i < n; i++) osd_text(buf, 24, y0 + i * 12, help_lines[i], white, dark);
    }

    char line[48] = "";
    uint32_t now = SDL_GetTicks();
    if (M.status_new) { osd_until = now + 3000; M.status_new = false; }
    if (now < osd_until) {
        snprintf(line, sizeof line, "%.38s", M.status);
    } else if (M.tin.loaded && mc10_tape_busy(&M)) {
        snprintf(line, sizeof line, "TAPE %s %d%%", M.tin.name, (int)(tape_in_progress(&M.tin) * 100));
    } else if (mc10_typing(&M)) {
        snprintf(line, sizeof line, "TYPING... %zu", M.typeq_len - M.typeq_pos);
    }
    if (warp && !line[0]) snprintf(line, sizeof line, "WARP");
    if (line[0]) osd_text(buf, 4, FB_H - 14, line, white, dark);

    SDL_UpdateTexture(tex, NULL, buf, FB_W * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------------ */
/* file dialogs via zenity / kdialog                                   */

static bool pick_file(const char *title, const char *filter_name, const char *patterns,
                      char *out, size_t outlen)
{
    char cmd[1024];
    if (system("command -v zenity >/dev/null 2>&1") == 0) {
        snprintf(cmd, sizeof cmd,
                 "zenity --file-selection --title='%s' --file-filter='%s | %s' "
                 "--file-filter='All files | *' 2>/dev/null",
                 title, filter_name, patterns);
    } else if (system("command -v kdialog >/dev/null 2>&1") == 0) {
        snprintf(cmd, sizeof cmd, "kdialog --getopenfilename . '%s (%s)' 2>/dev/null",
                 patterns, filter_name);
    } else {
        mc10_status(&M, "NO ZENITY: DRAG FILES ONTO WINDOW");
        return false;
    }
    if (adev) SDL_PauseAudioDevice(adev, 1);
    FILE *p = popen(cmd, "r");
    bool ok = false;
    if (p) {
        if (fgets(out, (int)outlen, p)) { trim(out); ok = out[0] != 0; }
        pclose(p);
    }
    mc10_release_all_keys(&M);          /* key-up events were lost */
    if (adev) { SDL_ClearQueuedAudio(adev); SDL_PauseAudioDevice(adev, 0); }
    return ok;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */

static void insert_tape(const char *path)
{
    char err[256];
    if (tape_in_load(&M.tin, path, err, sizeof err))
        mc10_status(&M, "TAPE %s - TYPE CLOAD", M.tin.name);
    else
        mc10_status(&M, "TAPE ERROR: %s", err);
}

static void quicktype_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { mc10_status(&M, "CANNOT READ %s", path); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        mc10_type_text(&M, buf, (size_t)n);
        const char *base = strrchr(path, '/');
        mc10_status(&M, "QUICKTYPE %s", base ? base + 1 : path);
    }
    free(buf);
    fclose(f);
}

static void open_any(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext && (!strcasecmp(ext, ".txt") || !strcasecmp(ext, ".bas")))
        quicktype_file(path);
    else
        insert_tape(path);
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */

static int scancode_to_key(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return K_A + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return K_1 + (sc - SDL_SCANCODE_1);
    switch (sc) {
    case SDL_SCANCODE_0:            return K_0;
    case SDL_SCANCODE_MINUS:        return K_COLON;     /* MC-10 layout: ... 9 0 : - */
    case SDL_SCANCODE_EQUALS:       return K_MINUS;
    case SDL_SCANCODE_LEFTBRACKET:  return K_AT;        /* P @ */
    case SDL_SCANCODE_SEMICOLON:    return K_SEMI;
    case SDL_SCANCODE_COMMA:        return K_COMMA;
    case SDL_SCANCODE_PERIOD:       return K_PERIOD;
    case SDL_SCANCODE_SLASH:        return K_SLASH;
    case SDL_SCANCODE_SPACE:        return K_SPACE;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:     return K_ENTER;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:       return K_SHIFT;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:        return K_CONTROL;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_PAUSE:        return K_BREAK;
    case SDL_SCANCODE_UP:           return K_W;
    case SDL_SCANCODE_LEFT:         return K_A;
    case SDL_SCANCODE_RIGHT:        return K_S;
    case SDL_SCANCODE_DOWN:         return K_Z;
    default:                        return -1;
    }
}

/* in "sensible" mode only these go straight to the matrix */
static bool sensible_direct(SDL_Scancode sc)
{
    return sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_PAUSE ||
           sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN ||
           sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT;
}

static void set_scale(int s)
{
    if (s < 1) s = 1;
    if (s > 6) s = 6;
    S.scale = s;
    if (!S.fullscreen) SDL_SetWindowSize(win, FB_W * s, FB_H * s);
}

static void screenshot(void)
{
    char dir[PATH_MAX], path[PATH_MAX + 32];
    snprintf(dir, sizeof dir, "screenshots");
    mkdir(dir, 0755);
    for (int i = 1; i < 10000; i++) {
        snprintf(path, sizeof path, "%s/vmc10_%04d.bmp", dir, i);
        if (!file_exists(path)) break;
    }
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(M.fb, FB_W, FB_H, 32, FB_W * 4,
                                                        SDL_PIXELFORMAT_ARGB8888);
    if (s && SDL_SaveBMP(s, path) == 0) mc10_status(&M, "SCREENSHOT %s", path + 12);
    else mc10_status(&M, "SCREENSHOT FAILED");
    SDL_FreeSurface(s);
}

static void enter_debugger(void)
{
    if (adev) SDL_PauseAudioDevice(adev, 1);
    SDL_SetWindowTitle(win, "VMC10 - debugger active in terminal");
    if (!debugger_run(&M)) running = false;
    mc10_release_all_keys(&M);
    if (adev) { SDL_ClearQueuedAudio(adev); SDL_PauseAudioDevice(adev, 0); }
}

static void handle_fkey(SDL_Keycode k, SDL_Keymod mod)
{
    char path[PATH_MAX];
    bool shift = mod & KMOD_SHIFT;
    switch (k) {
    case SDLK_F1: show_help = !show_help; break;
    case SDLK_F2:
        if (shift) {
            if (M.tin.loaded) { tape_in_rewind(&M.tin); mc10_status(&M, "TAPE REWOUND"); }
        } else if (pick_file("Insert cassette", "Cassette images", "*.c10 *.C10 *.cas *.CAS *.k7 *.K7 *.wav *.WAV", path, sizeof path))
            insert_tape(path);
        break;
    case SDLK_F3:
        if (pick_file("Quicktype text file", "BASIC text", "*.txt *.TXT *.bas *.BAS", path, sizeof path))
            quicktype_file(path);
        break;
    case SDLK_F4:
        if (SDL_HasClipboardText()) {
            char *t = SDL_GetClipboardText();
            if (t) { mc10_type_text(&M, t, strlen(t)); mc10_status(&M, "PASTE %zu CHARS", strlen(t)); }
            SDL_free(t);
        }
        break;
    case SDLK_F5:
        S.sensible = !S.sensible;
        dirty[3] = true;
        mc10_release_all_keys(&M);
        mc10_status(&M, "KEYBOARD: %s", S.sensible ? "SENSIBLE (TYPING)" : "EMULATED (GAMES)");
        break;
    case SDLK_F6:
        M.cfg.sound = !M.cfg.sound;
        dirty[2] = true;
        mc10_status(&M, "SOUND %s", M.cfg.sound ? "ON" : "OFF");
        break;
    case SDLK_F7:
        warp = !warp;
        mc10_status(&M, "WARP %s", warp ? "ON" : "OFF");
        break;
    case SDLK_F8: enter_debugger(); break;
    case SDLK_F9:
        mc10_reset(&M, shift);
        mc10_status(&M, shift ? "POWER CYCLED" : "RESET");
        break;
    case SDLK_F10: set_scale(S.scale % 6 + 1); dirty[0] = true; break;
    case SDLK_F11: screenshot(); break;
    case SDLK_F12:
        S.fullscreen = !S.fullscreen;
        SDL_SetWindowFullscreen(win, S.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        SDL_ShowCursor(S.fullscreen ? SDL_DISABLE : SDL_ENABLE);
        if (!S.fullscreen) set_scale(S.scale);
        break;
    }
}

static void pad_update(void)
{
    if (!pad) return;
    static bool prev[5];
    const int deadzone = 12000;
    int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    bool st[5] = {
        ay < -deadzone || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP),
        ax < -deadzone || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT),
        ax >  deadzone || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
        ay >  deadzone || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN),
        SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) ||
        SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B),
    };
    static const int keys[5] = { K_W, K_A, K_S, K_Z, K_SPACE };
    for (int i = 0; i < 5; i++)
        if (st[i] != prev[i]) { mc10_set_key(&M, keys[i], st[i]); prev[i] = st[i]; }
}

static void handle_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: running = false; break;
        case SDL_DROPFILE:
            open_any(e.drop.file);
            SDL_free(e.drop.file);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) mc10_release_all_keys(&M);
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            SDL_Keycode k = e.key.keysym.sym;
            SDL_Scancode sc = e.key.keysym.scancode;
            bool down = e.type == SDL_KEYDOWN;
            if (k >= SDLK_F1 && k <= SDLK_F12) {
                if (down && !e.key.repeat) handle_fkey(k, (SDL_Keymod)e.key.keysym.mod);
                break;
            }
            if (k == SDLK_q && (e.key.keysym.mod & KMOD_ALT)) { running = false; break; }
            if (k == SDLK_m && (e.key.keysym.mod & KMOD_ALT)) {
                if (down && !e.key.repeat) {
                    M.cfg.no_remap = !M.cfg.no_remap;
                    dirty[5] = true;
                    mc10_status(&M, "$4200 MAPPING %s", M.cfg.no_remap ? "OFF (DIRECT)" : "ON");
                }
                break;
            }
            if (k == SDLK_a && (e.key.keysym.mod & KMOD_ALT)) {
                if (down && !e.key.repeat) {
                    M.cfg.artifact_phase ^= 1;
                    dirty[4] = true;
                    mc10_status(&M, "ARTIFACT: 10=%s 01=%s", M.cfg.artifact_phase ? "BLUE" : "ORANGE",
                                M.cfg.artifact_phase ? "ORANGE" : "BLUE");
                }
                break;
            }
            if (e.key.repeat) break;
            if (!S.sensible || sensible_direct(sc)) {
                int key = scancode_to_key(sc);
                if (key == K_ENTER || key == K_SPACE || !S.sensible || sensible_direct(sc))
                    mc10_set_key(&M, key, down);
                /* Backspace = CONTROL + A on the MC-10 */
                if (sc == SDL_SCANCODE_BACKSPACE && !S.sensible) {
                    mc10_set_key(&M, K_CONTROL, down);
                    mc10_set_key(&M, K_A, down);
                }
            } else if (down) {
                if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) mc10_type_text(&M, "\n", 1);
                else if (sc == SDL_SCANCODE_BACKSPACE) mc10_type_text(&M, "\b", 1);
            }
            break; }
        case SDL_TEXTINPUT:
            if (S.sensible) mc10_type_text(&M, e.text.text, strlen(e.text.text));
            break;
        }
    }
}

/* ------------------------------------------------------------------ */

/* MCX32-SD: the flash is writable (KILL MEM, WRITE MEM, firmware updates),
 * so run from a working copy in ~/.local/share/vmc10 seeded from the image
 * in roms/.  The SD card is a host folder. */
static bool setup_mcx32sd(void)
{
    const char *home = getenv("HOME");
    char work[PATH_MAX], seed[PATH_MAX], dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.local/share/vmc10", home ? home : ".");
    snprintf(work, sizeof work, "%s/mcx32sd_flash.bin", dir);

    const char *src = NULL;
    if (S.mcx32flash[0]) src = S.mcx32flash;                 /* explicit: always reseed */
    else if (!file_exists(work) && find_data_file("mcx32sd_flash.bin", seed, sizeof seed)) src = seed;

    if (src) {
        if (!mcx32sd_load_flash(&M, src)) {
            fprintf(stderr, "vmc10: cannot load MCX32-SD flash image %s (must be 128K)\n", src);
            return false;
        }
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s/.local", home ? home : ".");
        mkdir(parent, 0755);
        snprintf(parent, sizeof parent, "%s/.local/share", home ? home : ".");
        mkdir(parent, 0755);
        mkdir(dir, 0755);
        snprintf(M.mcx32.flash_path, sizeof M.mcx32.flash_path, "%s", work);
        M.mcx32.fl_dirty = true;                             /* write the working copy */
        mcx32sd_save_flash(&M);
    } else if (!mcx32sd_load_flash(&M, work)) {
        fprintf(stderr, "vmc10: MCX32-SD selected but no flash image found.\n"
                "Put mcx32sd_flash.bin (128K) in roms/ (see roms/README.md: it can be\n"
                "built from Darren Atkinson's free MCX Basic 3.1 update with\n"
                "tools/make_mcx32sd_flash.py), or pass --mcx32sd-flash FILE.\n");
        return false;
    }
    if (!S.sddir[0]) snprintf(S.sddir, sizeof S.sddir, "%s/mcx32sd", home ? home : ".");
    mkdir(S.sddir, 0755);
    snprintf(M.cfg.sd_root, sizeof M.cfg.sd_root, "%s", S.sddir);
    fprintf(stderr, "vmc10: MCX32-SD flash %s, SD card folder %s\n", M.mcx32.flash_path, S.sddir);
    return true;
}

static void usage(const char *prog)
{
    printf("VMC10 for Linux %s - TRS-80 MC-10 emulator\n\n"
           "usage: %s [options] [tape.c10|program.txt]\n\n"
           "  --rom FILE         BASIC ROM image (default: roms/MC10.BIN)\n"
           "  --alice            use the Matra Alice ROM (roms/alice.bin)\n"
           "  --chr FILE         768-byte internal character ROM\n"
           "  --chrx FILE        external character ROM (semigraphics-6 mode)\n"
           "  --ram 4|20         without / with the Tandy 16K RAM expansion (default 20)\n"
           "  --8k               internal 8K RAM mod: full-screen 256x192 graphics\n"
           "  --4k               stock 4K internal RAM (default)\n"
           "  --no-artifact      show RG6 with CSS=1 as raw 256x192 white on black,\n"
           "                     without NTSC artifact colours (default: artifacting on)\n"
           "  --artifact-phase N 0: pixel pairs 10 = orange, 01 = blue; 1: swapped (Alt-A)\n"
           "  --mcx128           MCX-128 expansion (128K RAM + MCX BASIC, roms/mcx128bas.rom)\n"
           "  --mcx128-rom FILE  MCX-128 EPROM image (16K)\n"
           "  --mcx32sd          MCX32-SD expansion (32K RAM, flash MCX BASIC 3, SD card)\n"
           "  --mcx32sd-flash F  initial 128K flash image (default roms/mcx32sd_flash.bin)\n"
           "  --sd DIR           folder used as the MCX32-SD's SD card (default ~/mcx32sd)\n"
           "  --remap            MCX32-SD: enable the board's $4200-$43FF mapping of BASIC's\n"
           "                     old variable addresses, for old programs that POKE them.\n"
           "                     Default off: PEEK/POKE reach the graphics screen (Alt-M)\n"
           "  --emcee [DIR]      built-in Emcee file server for MCX BASIC on the serial\n"
           "                     port (default folder: the SD card folder, or ~/emcee)\n"
           "  --serial HOST:PORT connect the RS-232 port to an external TCP server instead,\n"
           "                     e.g. pyDriveWire --accept --port 65504\n"
           "  --scale N          window scale 1-6 (default 3)\n"
           "  --fullscreen       start full screen\n"
           "  --tape FILE        insert cassette image (.c10 .cas .k7 .wav)\n"
           "  --quicktype FILE   type a BASIC text file after boot\n"
           "  --type TEXT        type TEXT after boot (\\n = ENTER)\n"
           "  --load FILE@ADDR   load binary file at hex ADDR after boot\n"
           "  --printer FILE     LPRINT/LLIST output file (default LPRINT.TXT)\n"
           "  --tapedir DIR      where CSAVE writes .C10 files (default .)\n"
           "  --sensible         'sensible' (character) keyboard mode\n"
           "  --volume N         0-100 (default 50)\n"
           "  --nosound          start with sound off\n"
           "  --warp             start in warp mode\n"
           "  --debug            start in the debugger\n"
           "  --config FILE      settings file (default ~/.config/vmc10/vmc10.ini)\n"
           "  --c10-to-wav IN OUT  convert a cassette image to WAV and exit\n",
           VERSION, prog);
}

int main(int argc, char **argv)
{
    /* where the executable lives, for finding roms/ */
    ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof exe_dir - 1);
    if (n > 0) { exe_dir[n] = 0; char *s = strrchr(exe_dir, '/'); if (s) *s = 0; }
    else snprintf(exe_dir, sizeof exe_dir, ".");

    mc10_config cfg = { .expansion = EXP_16K, .volume = 50, .sound = true, .fast_typing = true };
    mc10_default_palette(cfg.palette);      /* the settings file overrides single colours */
    cfg.no_remap = true;                    /* MCX32-SD $4200 mapping off unless --remap */
    snprintf(cfg.printer_path, sizeof cfg.printer_path, "LPRINT.TXT");
    snprintf(cfg.tape_out_dir, sizeof cfg.tape_out_dir, ".");
    S.scale = 3;
    const char *home = getenv("HOME");
    snprintf(S.config_path, sizeof S.config_path, "%s/.config/vmc10/vmc10.ini", home ? home : ".");

    /* pre-scan for --config / --help / --c10-to-wav */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) snprintf(S.config_path, sizeof S.config_path, "%s", argv[i + 1]);
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        if (!strcmp(argv[i], "--c10-to-wav")) {
            if (i + 2 >= argc) { usage(argv[0]); return 1; }
            if (!tape_c10_to_wav(argv[i + 1], argv[i + 2])) { fprintf(stderr, "conversion failed\n"); return 1; }
            printf("wrote %s\n", argv[i + 2]);
            return 0;
        }
    }
    load_config(&cfg);

    const char *tape = NULL, *qtype = NULL, *typetext = NULL, *loadbin = NULL;
    bool alice = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(a, name) && v && (i++, 1))
        if (ARG("--config")) ;
        else if (ARG("--rom")) snprintf(S.rom, sizeof S.rom, "%s", v);
        else if (ARG("--chr")) snprintf(S.chr, sizeof S.chr, "%s", v);
        else if (ARG("--chrx")) snprintf(S.chrx, sizeof S.chrx, "%s", v);
        else if (ARG("--ram")) cfg.expansion = atoi(v) >= 16 ? EXP_16K : EXP_NONE;
        else if (!strcmp(a, "--mcx128")) cfg.expansion = EXP_MCX128;
        else if (ARG("--mcx128-rom")) { snprintf(S.mcxrom, sizeof S.mcxrom, "%s", v); cfg.expansion = EXP_MCX128; }
        else if (!strcmp(a, "--mcx32sd")) cfg.expansion = EXP_MCX32SD;
        else if (ARG("--mcx32sd-flash")) { snprintf(S.mcx32flash, sizeof S.mcx32flash, "%s", v); cfg.expansion = EXP_MCX32SD; }
        else if (ARG("--sd")) snprintf(S.sddir, sizeof S.sddir, "%s", v);
        else if (ARG("--serial")) snprintf(S.serial, sizeof S.serial, "%s", v);
        else if (!strcmp(a, "--emcee")) {
            S.emcee_on = true;
            if (v && v[0] != '-' && is_dir_path(v)) { snprintf(S.emcee, sizeof S.emcee, "%s", v); i++; }
        }
        else if (ARG("--scale")) S.scale = atoi(v);
        else if (ARG("--tape")) tape = v;
        else if (ARG("--quicktype")) qtype = v;
        else if (ARG("--type")) typetext = v;
        else if (ARG("--load")) loadbin = v;
        else if (ARG("--printer")) snprintf(cfg.printer_path, sizeof cfg.printer_path, "%s", v);
        else if (ARG("--tapedir")) snprintf(cfg.tape_out_dir, sizeof cfg.tape_out_dir, "%s", v);
        else if (ARG("--volume")) cfg.volume = atoi(v);
        else if (!strcmp(a, "--alice")) alice = true;
        else if (!strcmp(a, "--8k")) cfg.internal8k = true;
        else if (!strcmp(a, "--no-artifact")) cfg.no_artifact = true;
        else if (ARG("--artifact-phase")) cfg.artifact_phase = atoi(v) ? 1 : 0;
        else if (!strcmp(a, "--no-remap")) cfg.no_remap = true;
        else if (!strcmp(a, "--remap")) cfg.no_remap = false;
        else if (!strcmp(a, "--4k")) cfg.internal8k = false;
        else if (!strcmp(a, "--fullscreen")) S.fullscreen = true;
        else if (!strcmp(a, "--sensible")) S.sensible = true;
        else if (!strcmp(a, "--nosound")) cfg.sound = false;
        else if (!strcmp(a, "--warp")) warp = true;
        else if (!strcmp(a, "--debug")) S.debug_start = true;
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s (try --help)\n", a); return 1; }
        else {
            const char *ext = strrchr(a, '.');
            if (ext && (!strcasecmp(ext, ".txt") || !strcasecmp(ext, ".bas"))) qtype = a;
            else tape = a;
        }
#undef ARG
    }
    if (cfg.volume < 0) cfg.volume = 0;
    if (cfg.volume > 100) cfg.volume = 100;

    mc10_init(&M, &cfg);

    char path[PATH_MAX];
    const char *rom = S.rom[0] ? S.rom : NULL;
    if (alice && !rom) rom = find_data_file("alice.bin", path, sizeof path) ? path : NULL;
    if (!rom) rom = find_data_file("MC10.BIN", path, sizeof path) ? path : NULL;
    if (!rom || !mc10_load_rom(&M, rom)) {
        fprintf(stderr, "vmc10: cannot load the BASIC ROM%s%s.\n"
                "The ROM images are not included with VMC10 for copyright reasons; see\n"
                "roms/README.md for the file names, sizes and checksums.  Put MC10.BIN in\n"
                "./roms, next to the executable, or in ~/.local/share/vmc10/roms, or pass\n"
                "--rom FILE.\n", rom ? " " : "", rom ? rom : "");
        return 1;
    }
    fprintf(stderr, "vmc10: ROM %s%s\n", rom,
            M.stock_rom ? "" : " (not MicroColor BASIC 1.0: typing goes through the key matrix)");
    if (cfg.expansion == EXP_MCX128) {
        const char *mr = S.mcxrom[0] ? S.mcxrom : NULL;
        char mpath[PATH_MAX];
        if (!mr) mr = find_data_file("mcx128bas.rom", mpath, sizeof mpath) ? mpath : NULL;
        if (!mr || !mc10_load_mcx_rom(&M, mr)) {
            fprintf(stderr, "vmc10: MCX-128 selected but its 16K EPROM image (mcx128bas.rom)\n"
                    "could not be loaded%s%s.  Put it in roms/ (see roms/README.md) or pass\n"
                    "--mcx128-rom FILE.\n",
                    mr ? " from " : "", mr ? mr : "");
            return 1;
        }
        fprintf(stderr, "vmc10: MCX-128 with %s\n", mr);
    }
    if (cfg.expansion == EXP_MCX32SD && !setup_mcx32sd()) return 1;
    if (S.chr[0] && !mc10_load_chr(&M, S.chr)) fprintf(stderr, "vmc10: cannot load %s\n", S.chr);
    if (S.chrx[0] && !mc10_load_chrx(&M, S.chrx)) fprintf(stderr, "vmc10: cannot load %s\n", S.chrx);
    mc10_reset(&M, true);
    if (S.emcee_on) {
        const char *home = getenv("HOME");
        if (!S.emcee[0]) {
            if (M.cfg.sd_root[0]) snprintf(S.emcee, sizeof S.emcee, "%s", M.cfg.sd_root);
            else snprintf(S.emcee, sizeof S.emcee, "%s/emcee", home ? home : ".");
        }
        mkdir(S.emcee, 0755);
        serial_init(&M, "builtin");
        snprintf(M.emcee.root, sizeof M.emcee.root, "%s", S.emcee);
        if (!M.cfg.sd_root[0]) snprintf(M.cfg.sd_root, sizeof M.cfg.sd_root, "%s", S.emcee);
        fprintf(stderr, "vmc10: Emcee server on the serial port, folder %s\n", S.emcee);
    } else if (S.serial[0]) serial_init(&M, S.serial);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (S.scale < 1 || S.scale > 6) S.scale = 3;
    win = SDL_CreateWindow("VMC10 - TRS-80 MC-10", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           FB_W * S.scale, FB_H * S.scale,
                           SDL_WINDOW_RESIZABLE | (S.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(ren, FB_W, FB_H);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
    if (S.fullscreen) SDL_ShowCursor(SDL_DISABLE);
    SDL_StartTextInput();

    SDL_AudioSpec want = { .freq = AUDIO_RATE, .format = AUDIO_S16SYS, .channels = 1, .samples = 512 }, have;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) fprintf(stderr, "vmc10: no audio (%s)\n", SDL_GetError());
    else SDL_PauseAudioDevice(adev, 0);

    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
        if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);

    /* let BASIC boot, then apply startup actions */
    for (int i = 0; i < 90; i++) mc10_run_frame(&M);
    if (loadbin) {
        char file[PATH_MAX];
        snprintf(file, sizeof file, "%s", loadbin);
        char *at = strrchr(file, '@');
        unsigned addr = 0x4346;
        if (at) { *at = 0; addr = (unsigned)strtoul(at + 1, NULL, 16); }
        if (!mc10_load_binary(&M, file, (uint16_t)addr)) mc10_status(&M, "CANNOT READ %s", file);
    }
    if (tape) insert_tape(tape);
    if (qtype) quicktype_file(qtype);
    if (typetext) {
        char buf[4096]; size_t k = 0;
        for (const char *p = typetext; *p && k < sizeof buf - 1; p++) {
            if (p[0] == '\\' && p[1] == 'n') { buf[k++] = '\n'; p++; }
            else buf[k++] = *p;
        }
        mc10_type_text(&M, buf, k);
    }
    if (!M.status_new) mc10_status(&M, "F1 = HELP");
    if (S.debug_start) enter_debugger();

    const double frame_ms = 1000.0 * MC10_FRAME_CYCLES / MC10_CLOCK;   /* ~16.69 ms */
    const int target_queue = (int)(AUDIO_RATE * 0.05) * 2;           /* ~50 ms in bytes */
    uint64_t freq = SDL_GetPerformanceFrequency();
    double next = (double)SDL_GetPerformanceCounter();
    char title[160] = "";
    uint32_t last_title = 0;

    while (running) {
        handle_events();
        pad_update();
        if (!running) break;

        bool fast = warp || mc10_tape_busy(&M) || (mc10_typing(&M) && !mc10_fast_typing_active(&M));
        if (fast) {
            /* run as many frames as fit in ~one display frame */
            uint64_t start = SDL_GetPerformanceCounter();
            int frames = 0;
            do {
                mc10_run_frame(&M);
                frames++;
                if (M.break_hit) break;
            } while ((SDL_GetPerformanceCounter() - start) * 1000.0 / freq < 14.0 && frames < 200);
            if (adev) SDL_ClearQueuedAudio(adev);
            next = (double)SDL_GetPerformanceCounter();
        } else {
            mc10_run_frame(&M);
            if (adev && !M.break_hit) {
                SDL_QueueAudio(adev, M.audio, (Uint32)M.audio_len * 2);
                /* pace on the audio clock, keeping ~50 ms queued */
                while ((int)SDL_GetQueuedAudioSize(adev) > target_queue) SDL_Delay(1);
            } else {
                next += frame_ms * freq / 1000.0;
                double now = (double)SDL_GetPerformanceCounter();
                if (next > now) SDL_Delay((Uint32)((next - now) * 1000.0 / freq));
                else if (now - next > freq / 4.0) next = now;
            }
        }
        if (M.break_hit) enter_debugger();

        present();

        uint32_t t = SDL_GetTicks();
        if (t - last_title > 500) {
            last_title = t;
            char nt[160];
            snprintf(nt, sizeof nt, "VMC10 - TRS-80 MC-10%s%s%s",
                     warp ? "  [WARP]" : "",
                     S.sensible ? "  [sensible keys]" : "",
                     M.tin.loaded ? "  [tape]" : "");
            if (strcmp(nt, title)) { snprintf(title, sizeof title, "%s", nt); SDL_SetWindowTitle(win, title); }
        }
    }

    save_config();
    mc10_shutdown(&M);
    if (pad) SDL_GameControllerClose(pad);
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
