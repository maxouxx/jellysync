/**
 * inkview_sdl.cpp — Émulateur InkView pour PC, basé sur SDL2 + SDL2_ttf
 *
 * Simule l'écran de la Vivlio InkPad 3 (1404×1872, 16 niveaux de gris) :
 *   • le dessin se fait dans un framebuffer hors écran, visible seulement
 *     après FullUpdate()/PartialUpdate(), comme sur l'e-ink ;
 *   • la souris joue le rôle du doigt (EVT_POINTERDOWN / EVT_POINTERUP) ;
 *   • le clavier simule les boutons physiques (voir README de l'émulateur) ;
 *   • OpenKeyboard() affiche une boîte de saisie par-dessus l'écran.
 *
 * Variables d'environnement :
 *   JELLYSYNC_SCREEN=1404x1872   résolution simulée
 *   JELLYSYNC_SCALE=0.45         zoom de la fenêtre (auto par défaut)
 *   JELLYSYNC_SHOT=fichier.bmp   enregistre l'écran après chaque rafraîchissement
 *   JELLYSYNC_WIFI=off           démarre avec le Wi-Fi coupé (F9 bascule)
 *   JELLYSYNC_PANEL_H=136        hauteur de la barre d'état simulée
 */

#include "inkview.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>

#ifndef EMU_FONT_DIR
#define EMU_FONT_DIR "fonts"
#endif

namespace {

struct QueuedEvent { int type, par1, par2; };

struct KeyboardOverlay {
    bool               active = false;
    std::string        title;
    std::string        text;
    char*              target = nullptr;
    int                maxlen = 0;
    bool               password = false;
    iv_keyboardhandler cb = nullptr;
};

int            g_sw = 1404, g_sh = 1872;
SDL_Window*    g_win = nullptr;
SDL_Renderer*  g_ren = nullptr;
SDL_Texture*   g_tex = nullptr;
SDL_Surface*   g_fb = nullptr;       // ce que l'app dessine
SDL_Surface*   g_display = nullptr;  // ce que « l'encre » affiche
iv_handler     g_handler = nullptr;
bool           g_quit = false;
bool           g_pointer_down = false;

const ifont*   g_cur_font = nullptr;
int            g_cur_color = 0x000000;
std::map<std::string, ifont*> g_fonts;

std::mutex              g_q_mutex;
std::deque<QueuedEvent> g_queue;
Uint32                  g_wake_event = 0;

KeyboardOverlay g_kb;
const char*     g_shot_path = nullptr;

// Barre d'état du système : active par défaut, comme sur la liseuse
int             g_panel_type = PANEL_ENABLED;
int             g_panel_h    = 136;

// Wi-Fi simulé. Le faux serveur (mock_jellyfin.py) coupe ses transferts tant
// que le fichier WIFI_OFF_FLAG existe.
#define WIFI_OFF_FLAG "/tmp/jellysync-emu-wifi-off"
std::atomic<bool> g_wifi(true);
int               g_wifi_pings = 0;

struct Timer { std::string name; iv_timerproc proc; Uint32 due; };
std::vector<Timer> g_timers;

// ─── Utilitaires ──────────────────────────────────────────────────────────────

// Couleur 0xRRGGBB → gris quantifié sur 16 niveaux (comme l'écran Carta)
Uint8 to_gray16(int color)
{
    int r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    int y = (r * 299 + g * 587 + b * 114) / 1000;
    int level = (y * 15 + 127) / 255;
    return (Uint8)(level * 17);
}

Uint32 map_rgb(SDL_Surface* s, int color)
{
    return SDL_MapRGB(s->format, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
}

bool file_exists(const std::string& p)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Cherche un fichier de police : dossier de l'émulateur, puis polices système
std::string find_font_file(const std::vector<std::string>& names)
{
    std::vector<std::string> dirs = {
        EMU_FONT_DIR, "fonts",
        "/usr/share/fonts/truetype/liberation", "/usr/share/fonts/truetype/dejavu",
        "/usr/share/fonts/TTF", "/Library/Fonts", "/System/Library/Fonts/Supplemental",
        "C:/Windows/Fonts",
    };
    for (auto& n : names)
        for (auto& d : dirs)
            if (file_exists(d + "/" + n)) return d + "/" + n;
    return "";
}

bool name_is_bold(const char* name)
{
    std::string lower;
    for (const char* p = name ? name : ""; *p; ++p) lower += (char)tolower((unsigned char)*p);
    return lower.find("bold") != std::string::npos;
}

// Police à utiliser pour ce texte : la fallback si un glyphe manque
TTF_Font* font_for_text(const ifont* f, const char* s)
{
    TTF_Font* primary = (TTF_Font*)f->fdata;
    TTF_Font* fb = (TTF_Font*)f->fallback;
    if (!fb || fb == primary) return primary;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        Uint32 cp; int len;
        if (*p < 0x80)      { cp = *p; len = 1; }
        else if (*p < 0xE0) { cp = *p & 0x1F; len = 2; }
        else if (*p < 0xF0) { cp = *p & 0x0F; len = 3; }
        else                { cp = *p & 0x07; len = 4; }
        for (int i = 1; i < len && p[i]; ++i) cp = (cp << 6) | (p[i] & 0x3F);
#if SDL_TTF_VERSION_ATLEAST(2, 0, 18)
        if (cp > 0x7F && !TTF_GlyphIsProvided32(primary, cp)) return fb;
#else
        if (cp > 0x7F && (cp > 0xFFFF || !TTF_GlyphIsProvided(primary, (Uint16)cp))) return fb;
#endif
        p += len;
        while (*p && (*p & 0xC0) == 0x80) ++p;
    }
    return primary;
}

void present()
{
    SDL_UpdateTexture(g_tex, nullptr, g_display->pixels, g_display->pitch);
    SDL_SetRenderDrawColor(g_ren, 40, 40, 40, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);

    if (g_kb.active) {
        // Voile + boîte de saisie
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 120);
        SDL_Rect all{0, 0, g_sw, g_sh};
        SDL_RenderFillRect(g_ren, &all);

        int bw = g_sw * 9 / 10, bh = g_sh / 6;
        SDL_Rect box{(g_sw - bw) / 2, g_sh / 4, bw, bh};
        SDL_SetRenderDrawColor(g_ren, 255, 255, 255, 255);
        SDL_RenderFillRect(g_ren, &box);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        for (int i = 0; i < 4; ++i) {
            SDL_Rect b{box.x + i, box.y + i, box.w - 2 * i, box.h - 2 * i};
            SDL_RenderDrawRect(g_ren, &b);
        }

        ifont* f = OpenFont("DejaVuSans", g_sw / 30, 1);
        ifont* fb = OpenFont("DejaVuSans-Bold", g_sw / 30, 1);
        SDL_Color black{0, 0, 0, 255}, gray{110, 110, 110, 255};
        auto draw = [&](ifont* font, const std::string& s, int x, int y, SDL_Color c) {
            if (s.empty() || !font) return;
            SDL_Surface* t = TTF_RenderUTF8_Blended(font_for_text(font, s.c_str()), s.c_str(), c);
            if (!t) return;
            SDL_Texture* tt = SDL_CreateTextureFromSurface(g_ren, t);
            int w = t->w;
            int maxw = box.w - 2 * (x - box.x);
            SDL_Rect src{w > maxw ? w - maxw : 0, 0, w > maxw ? maxw : w, t->h};
            SDL_Rect dst{x, y, src.w, t->h};
            SDL_RenderCopy(g_ren, tt, &src, &dst);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(t);
        };
        int pad = bw / 30;
        draw(fb, g_kb.title, box.x + pad, box.y + pad, black);
        std::string shown = g_kb.password ? std::string(g_kb.text.size(), '*') : g_kb.text;
        shown += "|";
        draw(f, shown, box.x + pad, box.y + bh / 2 - pad / 2, black);
        draw(f, "Entrée = valider · Échap = annuler · Ctrl/Cmd+V = coller",
             box.x + pad, box.y + bh - pad - g_sw / 28, gray);
    }
    SDL_RenderPresent(g_ren);
}

// Copie une zone du framebuffer vers « l'encre » (en gris 16 niveaux)
void ink_update(int x, int y, int w, int h)
{
    SDL_Rect r{x, y, w, h}, scr{0, 0, g_sw, g_sh}, out;
    if (!SDL_IntersectRect(&r, &scr, &out)) { present(); return; }
    // Barre d'état active : l'image est décalée de sa hauteur, avec retour en haut
    int off = (g_panel_type != PANEL_DISABLED && !(g_panel_type & PANEL_NO_FB_OFFSET)) ? g_panel_h : 0;
    for (int j = out.y; j < out.y + out.h; ++j) {
        Uint32* src = (Uint32*)((Uint8*)g_fb->pixels + j * g_fb->pitch);
        Uint32* dst = (Uint32*)((Uint8*)g_display->pixels + ((j + off) % g_sh) * g_display->pitch);
        for (int i = out.x; i < out.x + out.w; ++i) {
            Uint8 r8, g8, b8;
            SDL_GetRGB(src[i], g_fb->format, &r8, &g8, &b8);
            Uint8 v = to_gray16((r8 << 16) | (g8 << 8) | b8);
            dst[i] = SDL_MapRGB(g_display->format, v, v, v);
        }
    }
    present();
    if (g_shot_path) SDL_SaveBMP(g_display, g_shot_path);
}

void dispatch(int type, int p1, int p2)
{
    if (g_handler) g_handler(type, p1, p2);
}

void set_wifi(bool on)
{
    g_wifi = on;
    if (on) remove(WIFI_OFF_FLAG);
    else if (FILE* f = fopen(WIFI_OFF_FLAG, "w")) fclose(f);
}

void run_timers()
{
    Uint32 now = SDL_GetTicks();
    for (size_t i = 0; i < g_timers.size(); ++i) {
        if ((Sint32)(now - g_timers[i].due) < 0) continue;
        iv_timerproc p = g_timers[i].proc;
        g_timers.erase(g_timers.begin() + i);
        p();   // peut se réarmer
        return;
    }
}

void drain_queue()
{
    for (;;) {
        QueuedEvent e;
        {
            std::lock_guard<std::mutex> lk(g_q_mutex);
            if (g_queue.empty()) return;
            e = g_queue.front();
            g_queue.pop_front();
        }
        dispatch(e.type, e.par1, e.par2);
    }
}

void utf8_pop_back(std::string& s)
{
    while (!s.empty()) {
        unsigned char c = (unsigned char)s.back();
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

void keyboard_finish(bool accept)
{
    KeyboardOverlay kb = g_kb;
    g_kb.active = false;
    SDL_StopTextInput();
    if (accept && kb.target) {
        strncpy(kb.target, kb.text.c_str(), kb.maxlen - 1);
        kb.target[kb.maxlen - 1] = 0;
        if (kb.cb) kb.cb(kb.target);
    }
    present();
}

void handle_keyboard_overlay(const SDL_Event& ev)
{
    if (ev.type == SDL_TEXTINPUT) {
        if ((int)(g_kb.text.size() + strlen(ev.text.text)) < g_kb.maxlen)
            g_kb.text += ev.text.text;
        present();
    } else if (ev.type == SDL_KEYDOWN) {
        SDL_Keycode k = ev.key.keysym.sym;
        bool mod = (ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0;
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) keyboard_finish(true);
        else if (k == SDLK_ESCAPE) keyboard_finish(false);
        else if (k == SDLK_BACKSPACE) { utf8_pop_back(g_kb.text); present(); }
        else if (mod && k == SDLK_v) {
            char* clip = SDL_GetClipboardText();
            if (clip) {
                std::string s = clip;
                SDL_free(clip);
                s.erase(s.find_last_not_of("\r\n \t") + 1);
                if ((int)(g_kb.text.size() + s.size()) < g_kb.maxlen) g_kb.text += s;
            }
            present();
        } else if (mod && k == SDLK_u) { g_kb.text.clear(); present(); }
    }
}

int map_key(SDL_Keycode k)
{
    switch (k) {
        case SDLK_ESCAPE: case SDLK_BACKSPACE:      return IV_KEY_BACK;
        case SDLK_LEFT: case SDLK_PAGEUP: case SDLK_UP:    return IV_KEY_PREV;
        case SDLK_RIGHT: case SDLK_PAGEDOWN: case SDLK_DOWN: return IV_KEY_NEXT;
        case SDLK_m:                                return IV_KEY_MENU;
        case SDLK_RETURN:                           return IV_KEY_OK;
        case SDLK_p:                                return IV_KEY_POWER;
    }
    return 0;
}

void press_key(int key)
{
    dispatch(EVT_KEYPRESS, key, 0);
    dispatch(EVT_KEYRELEASE, key, 0);
}

void handle_sdl_event(const SDL_Event& ev)
{
    if (ev.type == SDL_QUIT) { g_quit = true; return; }
    if (ev.type == g_wake_event) return;
    if (ev.type == SDL_WINDOWEVENT) {
        if (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
            ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) present();
        return;
    }
    if (g_kb.active) { handle_keyboard_overlay(ev); return; }

    switch (ev.type) {
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                g_pointer_down = true;
                dispatch(EVT_POINTERDOWN, ev.button.x, ev.button.y);
            }
            break;
        case SDL_MOUSEMOTION:
            if (g_pointer_down) dispatch(EVT_POINTERMOVE, ev.motion.x, ev.motion.y);
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                g_pointer_down = false;
                dispatch(EVT_POINTERUP, ev.button.x, ev.button.y);
            }
            break;
        case SDL_MOUSEWHEEL:
            if (ev.wheel.y > 0) press_key(IV_KEY_PREV);
            else if (ev.wheel.y < 0) press_key(IV_KEY_NEXT);
            break;
        case SDL_KEYDOWN: {
            if (ev.key.keysym.sym == SDLK_F12) {
                SDL_SaveBMP(g_display, "jellysync-screenshot.bmp");
                fprintf(stderr, "[emu] capture : jellysync-screenshot.bmp\n");
                break;
            }
            if (ev.key.keysym.sym == SDLK_F5) { dispatch(EVT_REPAINT, 0, 0); break; }
            if (ev.key.keysym.sym == SDLK_F9) {
                set_wifi(!g_wifi);
                fprintf(stderr, "[emu] Wi-Fi %s\n", g_wifi ? "rétabli" : "coupé");
                dispatch(g_wifi ? EVT_NET_CONNECTED : EVT_NET_DISCONNECTED, 0, 0);
                break;
            }
            int k = map_key(ev.key.keysym.sym);
            if (k) dispatch(ev.key.repeat ? EVT_KEYREPEAT : EVT_KEYPRESS, k, 0);
            break;
        }
        case SDL_KEYUP: {
            int k = map_key(ev.key.keysym.sym);
            if (k) dispatch(EVT_KEYRELEASE, k, 0);
            break;
        }
    }
}

void put_pixel(int x, int y, Uint32 px)
{
    if (x < 0 || y < 0 || x >= g_sw || y >= g_sh) return;
    ((Uint32*)((Uint8*)g_fb->pixels + y * g_fb->pitch))[x] = px;
}

// Découpe une ligne trop longue (sans espace) caractère par caractère
void push_wrapped_word(TTF_Font* f, const std::string& word, int w,
                       std::vector<std::string>& lines, std::string& cur)
{
    std::string piece;
    size_t i = 0;
    while (i < word.size()) {
        size_t len = 1;
        unsigned char c = (unsigned char)word[i];
        if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
        std::string next = piece + word.substr(i, len);
        int tw = 0, th = 0;
        TTF_SizeUTF8(f, next.c_str(), &tw, &th);
        if (tw > w && !piece.empty()) {
            lines.push_back(piece);
            piece = word.substr(i, len);
        } else {
            piece = next;
        }
        i += len;
    }
    cur = piece;
}

} // namespace

// ─── API InkView ──────────────────────────────────────────────────────────────

extern "C" {

void InkViewMain(iv_handler h)
{
    g_handler = h;

    if (const char* s = getenv("JELLYSYNC_SCREEN")) {
        int w = 0, hh = 0;
        if (sscanf(s, "%dx%d", &w, &hh) == 2 && w > 0 && hh > 0) { g_sw = w; g_sh = hh; }
    }
    g_shot_path = getenv("JELLYSYNC_SHOT");
    { const char* s = getenv("JELLYSYNC_WIFI"); set_wifi(!(s && strcmp(s, "off") == 0)); }
    if (const char* s = getenv("JELLYSYNC_PANEL_H")) g_panel_h = atoi(s);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[emu] SDL_Init : %s\n", SDL_GetError());
        exit(1);
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "[emu] TTF_Init : %s\n", TTF_GetError());
        exit(1);
    }
    g_wake_event = SDL_RegisterEvents(1);

    double scale = 0;
    if (const char* s = getenv("JELLYSYNC_SCALE")) scale = atof(s);
    if (scale <= 0) {
        SDL_Rect usable;
        scale = 0.45;
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.h > 0)
            scale = (usable.h * 0.92) / g_sh;
        if (scale > 1) scale = 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    g_win = SDL_CreateWindow("JellySync — émulateur InkPad 3",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             (int)(g_sw * scale), (int)(g_sh * scale),
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_win || !g_ren) {
        fprintf(stderr, "[emu] fenêtre : %s\n", SDL_GetError());
        exit(1);
    }
    // Les coordonnées souris sont automatiquement converties en pixels liseuse
    SDL_RenderSetLogicalSize(g_ren, g_sw, g_sh);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, g_sw, g_sh);
    g_fb = SDL_CreateRGBSurfaceWithFormat(0, g_sw, g_sh, 32, SDL_PIXELFORMAT_ARGB8888);
    g_display = SDL_CreateRGBSurfaceWithFormat(0, g_sw, g_sh, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_FillRect(g_fb, nullptr, map_rgb(g_fb, 0xFFFFFF));
    SDL_FillRect(g_display, nullptr, map_rgb(g_display, 0xFFFFFF));
    SDL_StopTextInput();
    present();

    dispatch(EVT_INIT, 0, 0);
    dispatch(EVT_SHOW, 0, 0);

    while (!g_quit) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 100)) {
            handle_sdl_event(ev);
            while (!g_quit && SDL_PollEvent(&ev)) handle_sdl_event(ev);
        }
        drain_queue();
        run_timers();
    }

    dispatch(EVT_EXIT, 0, 0);
    for (auto& kv : g_fonts) {
        TTF_CloseFont((TTF_Font*)kv.second->fdata);
        if (kv.second->fallback) TTF_CloseFont((TTF_Font*)kv.second->fallback);
        free(kv.second->name);
        delete kv.second;
    }
    g_fonts.clear();
    SDL_FreeSurface(g_fb);
    SDL_FreeSurface(g_display);
    SDL_DestroyTexture(g_tex);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    TTF_Quit();
    SDL_Quit();
}

void CloseApp(void) { g_quit = true; }

int ScreenWidth(void)  { return g_sw; }
int ScreenHeight(void) { return g_sh; }

void ClearScreen(void)
{
    SDL_FillRect(g_fb, nullptr, map_rgb(g_fb, 0xFFFFFF));
}

void FillArea(int x, int y, int w, int h, int color)
{
    SDL_Rect r{x, y, w, h};
    SDL_FillRect(g_fb, &r, map_rgb(g_fb, color));
}

void DrawLine(int x1, int y1, int x2, int y2, int color)
{
    Uint32 px = map_rgb(g_fb, color);
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_pixel(x1, y1, px);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

const char* iv_get_default_font(int fonttype)
{
    return (fonttype == FONT_BOLD || fonttype == FONT_BOLDITALIC)
        ? "LiberationSans-Bold" : "LiberationSans";
}

ifont* OpenFont(const char* name, int size, int aa)
{
    if (size <= 0) size = 24;
    bool bold = name_is_bold(name);
    std::string key = std::string(bold ? "B" : "R") + std::to_string(size);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;

    std::string main_file = find_font_file(bold
        ? std::vector<std::string>{"LiberationSans-Bold.ttf", "Arial Bold.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"}
        : std::vector<std::string>{"LiberationSans-Regular.ttf", "Arial.ttf", "arial.ttf", "DejaVuSans.ttf"});
    std::string fb_file = find_font_file({bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf"});
    if (main_file.empty()) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "[emu] aucune police trouvée (attendu : %s/LiberationSans-Regular.ttf)\n",
                    EMU_FONT_DIR);
            warned = true;
        }
        return nullptr;
    }

    TTF_Font* tf = TTF_OpenFont(main_file.c_str(), size);
    if (!tf) return nullptr;
    TTF_Font* tfb = fb_file.empty() ? nullptr : TTF_OpenFont(fb_file.c_str(), size);
    ifont* f = new ifont();
    f->name = strdup(name ? name : "");
    f->size = size;
    f->isbold = bold;
    f->height = TTF_FontHeight(tf);
    f->linespacing = TTF_FontLineSkip(tf);
    f->baseline = TTF_FontAscent(tf);
    f->fdata = tf;
    f->fallback = tfb;
    g_fonts[key] = f;
    return f;
}

// Les polices sont mises en cache et libérées à la fermeture de l'émulateur
void CloseFont(ifont* f) { (void)f; }

void SetFont(const ifont* font, int color)
{
    if (font) g_cur_font = font;
    g_cur_color = color;
}

ifont* GetFont(void)
{
    if (!g_cur_font) g_cur_font = OpenFont(iv_get_default_font(FONT_STD), 24, 1);
    return (ifont*)g_cur_font;
}

char* DrawTextRect(int x, int y, int w, int h, const char* s, int flags)
{
    if (!s || !*s || w <= 0) return nullptr;
    const ifont* fnt = GetFont();
    if (!fnt) return nullptr;
    TTF_Font* f = font_for_text(fnt, s);
    int skip = TTF_FontLineSkip(f);

    // Retour à la ligne par mots
    std::vector<std::string> lines;
    std::string text = s;
    size_t pstart = 0;
    while (pstart <= text.size()) {
        size_t pend = text.find('\n', pstart);
        if (pend == std::string::npos) pend = text.size();
        std::string para = text.substr(pstart, pend - pstart);
        std::string cur;
        size_t i = 0;
        while (i <= para.size()) {
            size_t sp = para.find(' ', i);
            if (sp == std::string::npos) sp = para.size();
            std::string word = para.substr(i, sp - i);
            std::string cand = cur.empty() ? word : cur + " " + word;
            int tw = 0, th = 0;
            TTF_SizeUTF8(f, cand.c_str(), &tw, &th);
            if (tw <= w || cur.empty()) {
                if (tw > w) push_wrapped_word(f, word, w, lines, cur);
                else cur = cand;
            } else {
                lines.push_back(cur);
                TTF_SizeUTF8(f, word.c_str(), &tw, &th);
                if (tw > w) push_wrapped_word(f, word, w, lines, cur);
                else cur = word;
            }
            i = sp + 1;
        }
        lines.push_back(cur);
        pstart = pend + 1;
    }

    int max_lines = h > 0 ? std::max(1, h / skip) : (int)lines.size();
    if ((int)lines.size() > max_lines) lines.resize(max_lines);

    int block_h = (int)lines.size() * skip - (skip - TTF_FontHeight(f));
    int ty = y;
    if (flags & VALIGN_MIDDLE)      ty = y + (h - block_h) / 2;
    else if (flags & VALIGN_BOTTOM) ty = y + h - block_h;

    SDL_Rect clip{x, y, w, h > 0 ? h : g_sh};
    SDL_SetClipRect(g_fb, &clip);
    SDL_Color col{(Uint8)((g_cur_color >> 16) & 0xFF), (Uint8)((g_cur_color >> 8) & 0xFF),
                  (Uint8)(g_cur_color & 0xFF), 255};
    for (auto& line : lines) {
        if (!line.empty()) {
            SDL_Surface* ts = TTF_RenderUTF8_Blended(f, line.c_str(), col);
            if (ts) {
                int tx = x;
                if (flags & ALIGN_CENTER)     tx = x + (w - ts->w) / 2;
                else if (flags & ALIGN_RIGHT) tx = x + w - ts->w;
                SDL_Rect dst{tx, ty, ts->w, ts->h};
                SDL_BlitSurface(ts, nullptr, g_fb, &dst);
                SDL_FreeSurface(ts);
            }
        }
        ty += skip;
    }
    SDL_SetClipRect(g_fb, nullptr);
    return nullptr;
}

void FullUpdate(void)  { ink_update(0, 0, g_sw, g_sh); }
void SoftUpdate(void)  { ink_update(0, 0, g_sw, g_sh); }
void PartialUpdate(int x, int y, int w, int h) { ink_update(x, y, w, h); }

int GetCurrentTask(void) { return 0; }

// Thread-safe : appelée depuis le thread de synchronisation
void SendEventTo(int task, int type, int par1, int par2)
{
    (void)task;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_queue.push_back({type, par1, par2});
    }
    SDL_Event wake;
    SDL_zero(wake);
    wake.type = g_wake_event;
    SDL_PushEvent(&wake);
}

void OpenKeyboard(const char* title, char* buffer, int maxlen, int flags,
                  iv_keyboardhandler hproc)
{
    g_kb.active   = true;
    g_kb.title    = title ? title : "";
    g_kb.text     = buffer ? buffer : "";
    g_kb.target   = buffer;
    g_kb.maxlen   = maxlen > 0 ? maxlen : 256;
    g_kb.password = (flags & KBD_PASSWORD) != 0;
    g_kb.cb       = hproc;
    SDL_StartTextInput();
    present();
}

void SetPanelType(int type) { g_panel_type = type; }
int  PanelHeight(void) { return g_panel_type == PANEL_DISABLED ? 0 : g_panel_h; }

void SetHardTimer(const char* name, iv_timerproc tproc, int ms)
{
    for (auto& t : g_timers)
        if (t.name == name) { t.proc = tproc; t.due = SDL_GetTicks() + ms; return; }
    g_timers.push_back({ name, tproc, SDL_GetTicks() + (Uint32)ms });
}

void ClearTimer(iv_timerproc tproc)
{
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(),
                   [&](const Timer& t) { return t.proc == tproc; }), g_timers.end());
}

int QueryNetwork(void) { return g_wifi ? (NET_WIFI | NET_CONNECTED) : NET_WIFI; }

int NetConnect(const char* name)
{
    (void)name;
    fprintf(stderr, "[emu] NetConnect : connexion au Wi-Fi...\n");
    SDL_Delay(1000);
    set_wifi(true);
    return NET_OK;
}

int NetMgrPing(void)
{
    ++g_wifi_pings;
    fprintf(stderr, "[emu] NetMgrPing (%d)\n", g_wifi_pings);
    return 0;
}

} // extern "C"
