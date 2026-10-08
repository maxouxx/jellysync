#pragma once
/**
 * ui.h — Interface e-ink JellySync
 *
 * Principes (voir la maquette validée « Refonte JellySync ») :
 *   • Toutes les dimensions sont données en pixels de l'InkPad 3 (1404×1872)
 *     et mises à l'échelle par U() selon la largeur réelle de l'écran.
 *   • Cibles tactiles de 112 px minimum (9,5 mm), lignes de 156 px.
 *   • Noir et blanc uniquement pour les aplats : pas de fonds gris tramés.
 *   • Listes paginées (Précédent / Suivant, touches physiques), pas de défilement.
 *   • Rafraîchissement complet seulement quand l'écran change ; sinon partiel,
 *     limité à la zone modifiée.
 *   • Pendant un téléchargement, les 20 % du bas affichent le journal détaillé.
 */

#include <inkview.h>
#include "inkview_compat.h"
#include <cstring>
#include <cstdio>
#include <functional>
#include <vector>
#include <set>
#include <map>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include "config.h"

extern void log_write(const char* fmt, ...);
// Dernières lignes du journal (définie dans main.cpp) ; renvoie le compteur
// de lignes écrites depuis le démarrage.
static unsigned log_tail(std::vector<std::string>& out, int max_lines);

// ─── Palette ──────────────────────────────────────────────────────────────────
#define C_BLACK      0x000000
#define C_WHITE      0xFFFFFF
#define C_GRAY_DARK  0x555555   // texte secondaire
#define C_GRAY_MID   0x888888   // éléments désactivés
#define C_GRAY_LITE  0xBBBBBB   // filets entre les lignes

// ─── Dimensions (pixels InkPad 3, mis à l'échelle) ───────────────────────────
#define SW  (ScreenWidth())
#define SH  (ScreenHeight())
static inline int U(int px) { return px * SW / 1404; }

#define MG        U(48)    // marge latérale
#define HDR_H     U(144)   // en-tête
#define BTN_H     U(112)   // hauteur de bouton = cible tactile minimale
#define GAP       U(24)
#define BANNER_H  U(132)
#define TABS_H    U(100)
#define ROW_H     U(156)
#define FOOT_H    U(136)
#define LINE      std::max(2, U(4))   // trait épais (bordures)
#define HAIR      std::max(1, U(2))   // filet

#define F_TITLE   U(56)
#define F_BANNER  U(42)
#define F_BTN     U(34)
#define F_ROW     U(42)
#define F_DETAIL  U(31)
#define F_PILL    U(25)
#define F_SECTION U(27)
#define F_LOG     U(25)

static bool check_screen_dims()
{
    if (SW <= 0 || SH <= 0) {
        log_write("ERREUR : dimensions d'écran invalides (%d×%d)\n", SW, SH);
        return false;
    }
    return true;
}

// ─── Zones tactiles ───────────────────────────────────────────────────────────
struct TouchZone { int x, y, w, h, id; };

#define ZONE_BTN_REFRESH       1
#define ZONE_BTN_SETTINGS      3
#define ZONE_BTN_DL_SELECTED   4
#define ZONE_BTN_CLEAR_SEL     5
#define ZONE_BTN_RECONNECT     7
#define ZONE_BTN_DL_FOLDER     6
#define ZONE_FILTER_BASE      10   // + 0..2
#define ZONE_PAGE_PREV        20
#define ZONE_PAGE_NEXT        21
#define ZONE_BTN_BACK         22
#define ZONE_URL              30
#define ZONE_USER             31
#define ZONE_PASS             32
#define ZONE_APIKEY           33
#define ZONE_AUTH_CRED        34
#define ZONE_SAVE             35
#define ZONE_BACK_SETUP       36
#define ZONE_LANG_FR          37
#define ZONE_LANG_EN          38
#define ZONE_AUTH_KEY         39
#define ZONE_FOLDER_BASE    1000   // + index dans g_folders
#define ZONE_ROW_SELECT     2000   // + ligne visible
#define ZONE_ROW_DL         3000   // + ligne visible

#define MAX_ZONES 200
static TouchZone g_zones[MAX_ZONES];
static int       g_zone_count = 0;

static void zones_clear() { g_zone_count = 0; }
static void zones_add(int x, int y, int w, int h, int id)
{
    if (g_zone_count >= MAX_ZONES) return;
    g_zones[g_zone_count++] = { x, y, w, h, id };
}
static int zones_hit(int px, int py)
{
    for (int i = g_zone_count - 1; i >= 0; --i) {
        auto& z = g_zones[i];
        if (px >= z.x && px < z.x + z.w && py >= z.y && py < z.y + z.h)
            return z.id;
    }
    return -1;
}

// ─── État propre à l'interface ───────────────────────────────────────────────
static AppConfig* g_ui_cfg   = nullptr;
static AppState*  g_ui_state = nullptr;
static AppConfig  g_cfg_backup;          // pour « Annuler » dans les réglages

struct FolderInfo {
    std::string key;       // nom réel ("" = livres sans dossier)
    int total, n_new, n_local;
};
static std::vector<FolderInfo> g_folders;        // dossiers affichés
static std::vector<int>        g_row_catalog_idx; // ligne visible → index catalogue
static int g_list_total = 0;                     // éléments de la liste courante

// Zones redessinées pendant un téléchargement
struct Rect { int x, y, w, h; };
static Rect g_progress_rect = { 0, 0, 0, 0 };
static Rect g_log_rect      = { 0, 0, 0, 0 };

static const char* tr(const char* fr, const char* en)
{
    return (g_ui_cfg && g_ui_cfg->lang == 1) ? en : fr;
}

static std::string folder_label(const std::string& key)
{
    return key.empty() ? tr("Sans dossier", "No folder") : key;
}

struct UIKeyboardReq { char* target; int max_len; };
static UIKeyboardReq g_kb_req;

// ─── Polices (ouvertes une fois, gardées en cache) ───────────────────────────

static std::map<int, ifont*> g_font_cache;

static ifont* font(int size, bool bold = false)
{
    if (size <= 0) size = 24;
    int key = size * 2 + (bold ? 1 : 0);
    auto it = g_font_cache.find(key);
    if (it != g_font_cache.end()) return it->second;

    const char* names[] = {
        iv_get_default_font(bold ? FONT_BOLD : FONT_STD),
        bold ? "LiberationSans-Bold" : "LiberationSans",
        bold ? "DejaVuSans-Bold" : "DejaVuSans",
        nullptr
    };
    ifont* f = nullptr;
    for (int i = 0; names[i] && !f; ++i)
        f = OpenFont(names[i], size, 1);
    if (!f) log_write("Police introuvable (taille %d)\n", size);
    g_font_cache[key] = f;
    return f;
}

static void cleanup_fonts()
{
    for (auto& kv : g_font_cache)
        if (kv.second) CloseFont(kv.second);
    g_font_cache.clear();
}

static void use_font(int size, bool bold, int color)
{
    ifont* f = font(size, bold);
    if (f) SetFont(f, color);
}

// ─── Texte ────────────────────────────────────────────────────────────────────

static int utf8_len(const char* s)
{
    int n = 0;
    for (; *s; ++s) if ((*s & 0xC0) != 0x80) ++n;
    return n;
}

// Largeur estimée (pas de mesure de texte dans toutes les versions du SDK)
static int text_w_est(const char* s, int size, bool bold)
{
    return utf8_len(s) * size * (bold ? 64 : 56) / 100;
}

// Texte sur une seule ligne, centré verticalement sur cy, tronqué par « … »
static void text_line(int x, int cy, int w, const char* s,
                      int size, bool bold, int color, int align = ALIGN_LEFT)
{
    if (!s || !*s || w <= 0) return;
    int h = size * 14 / 10;
    use_font(size, bold, color);
    DrawTextRect(x, cy - h / 2, w, h, s, align | VALIGN_MIDDLE | DOTS);
}

static std::string fmt_size(long long bytes)
{
    char buf[32];
    bool en = g_ui_cfg && g_ui_cfg->lang == 1;
    if (bytes <= 0)                 snprintf(buf, sizeof(buf), "—");
    else if (bytes < 1024 * 1024)   snprintf(buf, sizeof(buf), en ? "%lld KB" : "%lld Ko", bytes / 1024);
    else                            snprintf(buf, sizeof(buf), en ? "%.1f MB" : "%.1f Mo", bytes / 1048576.0);
    if (!en) for (char* p = buf; *p; ++p) if (*p == '.') *p = ',';
    return buf;
}

static std::string fmt_ext(const std::string& filename)
{
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return "";
    std::string e = filename.substr(dot + 1);
    for (auto& c : e) c = (char)toupper((unsigned char)c);
    return e;
}

// ─── Formes ───────────────────────────────────────────────────────────────────

static void frame(int x, int y, int w, int h, int t, int color)
{
    FillArea(x, y, w, t, color);
    FillArea(x, y + h - t, w, t, color);
    FillArea(x, y, t, h, color);
    FillArea(x + w - t, y, t, h, color);
}

static void thick_line(int x1, int y1, int x2, int y2, int t, int color)
{
    bool flat = std::abs(x2 - x1) > std::abs(y2 - y1);
    for (int i = -t / 2; i <= t / 2; ++i) {
        if (flat) DrawLine(x1, y1 + i, x2, y2 + i, color);
        else      DrawLine(x1 + i, y1, x2 + i, y2, color);
    }
}

// Icônes tracées au trait (les glyphes ✓ ⬇ manquent dans certaines polices)
static void icon_chevron(int cx, int cy, int s, bool right, int color)
{
    int d = right ? 1 : -1, t = std::max(3, s / 7);
    thick_line(cx - d * s / 4, cy - s / 2, cx + d * s / 4, cy, t, color);
    thick_line(cx + d * s / 4, cy, cx - d * s / 4, cy + s / 2, t, color);
}

static void icon_check(int cx, int cy, int s, int color)
{
    int t = std::max(3, s / 7);
    thick_line(cx - s / 2, cy, cx - s / 6, cy + s / 3, t, color);
    thick_line(cx - s / 6, cy + s / 3, cx + s / 2, cy - s / 3, t, color);
}

static void icon_download(int cx, int cy, int s, int color)
{
    int t = std::max(3, s / 8);
    FillArea(cx - t / 2, cy - s / 2, t, s * 2 / 3, color);
    thick_line(cx - s / 3, cy - s / 12, cx, cy + s / 4, t, color);
    thick_line(cx, cy + s / 4, cx + s / 3, cy - s / 12, t, color);
    FillArea(cx - s / 2, cy + s / 2 - t, s, t, color);
}

static void icon_folder(int x, int cy, int w, int h, int color)
{
    int y = cy - h / 2;
    FillArea(x, y, w * 2 / 5, h / 5, color);          // onglet
    frame(x, y + h / 6, w, h - h / 6, LINE, color);
    FillArea(x, y + h / 6, w, LINE * 2, color);
}

enum BtnStyle { BTN_OUTLINE, BTN_FILL, BTN_DISABLED, BTN_ON_DARK, BTN_FILL_ON_DARK };

static void button(int x, int y, int w, int h, const char* label, BtnStyle st)
{
    int bg = C_WHITE, fg = C_BLACK, border = C_BLACK;
    switch (st) {
        case BTN_FILL:         bg = C_BLACK; fg = C_WHITE; border = C_BLACK; break;
        case BTN_DISABLED:     fg = C_GRAY_MID; border = C_GRAY_LITE;        break;
        case BTN_ON_DARK:      bg = C_BLACK; fg = C_WHITE; border = C_WHITE; break;
        case BTN_FILL_ON_DARK: bg = C_WHITE; fg = C_BLACK; border = C_WHITE; break;
        default: break;
    }
    FillArea(x, y, w, h, bg);
    frame(x, y, w, h, LINE, border);
    if (label) text_line(x + U(12), y + h / 2, w - U(24), label, F_BTN, true, fg, ALIGN_CENTER);
}

static int button_w(const char* label)
{
    return std::max(BTN_H, text_w_est(label, F_BTN, true) + U(64));
}

// Étiquette d'état (NOUVEAU, MIS À JOUR…) ; renvoie sa largeur
static int pill(int x, int cy, const char* label, bool filled)
{
    int h = U(40), pad = U(12);
    int w = text_w_est(label, F_PILL, true) + 2 * pad;
    if (filled) FillArea(x, cy - h / 2, w, h, C_BLACK);
    else        frame(x, cy - h / 2, w, h, HAIR + 1, C_BLACK);
    text_line(x, cy, w, label, F_PILL, true, filled ? C_WHITE : C_BLACK, ALIGN_CENTER);
    return w;
}

// ─── Rafraîchissement e-ink ───────────────────────────────────────────────────
// Au-delà de 6 mises à jour partielles, un rafraîchissement complet efface les
// traces (ghosting).
static int g_partial_count = 0;

static void flush_full()
{
    FullUpdate();
    g_partial_count = 0;
}

static void flush_partial(int x, int y, int w, int h)
{
    if (++g_partial_count >= 6) { flush_full(); return; }
    PartialUpdate(x, y, w, h);
}

// ─── Blocs communs ────────────────────────────────────────────────────────────

static void draw_header(const char* title, bool back, bool refresh, bool settings)
{
    int sw = SW, mg = MG;
    int by = (HDR_H - BTN_H) / 2;
    int x  = mg;

    if (back) {
        button(x, by, BTN_H, BTN_H, nullptr, BTN_OUTLINE);
        icon_chevron(x + BTN_H / 2, by + BTN_H / 2, BTN_H * 2 / 5, false, C_BLACK);
        zones_add(x, by, BTN_H, BTN_H, ZONE_BTN_BACK);
        x += BTN_H + GAP;
    }

    int right = sw - mg;
    if (settings) {
        const char* l = tr("Réglages", "Settings");
        int w = button_w(l);
        right -= w;
        button(right, by, w, BTN_H, l, BTN_OUTLINE);
        zones_add(right, by, w, BTN_H, ZONE_BTN_SETTINGS);
        right -= GAP;
    }
    if (refresh) {
        const char* l = tr("Actualiser", "Refresh");
        int w = button_w(l);
        right -= w;
        button(right, by, w, BTN_H, l, BTN_OUTLINE);
        zones_add(right, by, w, BTN_H, ZONE_BTN_REFRESH);
        right -= GAP;
    }

    text_line(x, HDR_H / 2, right - x, title, F_TITLE, true, C_BLACK);
    FillArea(mg, HDR_H - LINE, sw - 2 * mg, LINE, C_BLACK);
}

// Bandeau d'état sous l'en-tête ; action optionnelle à droite
static void draw_banner(const char* title, const char* detail,
                        const char* action, int action_zone, bool error = false)
{
    int sw = SW, mg = MG;
    int y = HDR_H + GAP, w = sw - 2 * mg, h = BANNER_H;
    frame(mg, y, w, h, error ? LINE * 2 : LINE, C_BLACK);

    int text_w = w - 2 * U(28);
    if (action) {
        int bw = button_w(action), bh = h - 2 * U(20);
        int bx = mg + w - U(20) - bw;
        button(bx, y + U(20), bw, bh, action, BTN_FILL);
        zones_add(bx, y + U(10), bw, h - U(20), action_zone);
        text_w = bx - mg - U(28) - GAP;
    }
    text_line(mg + U(28), y + h * 36 / 100, text_w, title, F_BANNER, true, C_BLACK);
    text_line(mg + U(28), y + h * 72 / 100, text_w, detail, F_DETAIL, false, C_GRAY_DARK);
}

static int tabs_top() { return HDR_H + GAP + BANNER_H + GAP; }
static int list_top() { return tabs_top() + TABS_H + U(12); }
static int list_bottom() { return SH - FOOT_H; }

static bool book_matches_filter(const BookEntry& b, int filter)
{
    switch (filter) {
        case 1: return b.status == BOOK_NEW || b.status == BOOK_UPDATED || b.status == BOOK_ERROR;
        case 2: return b.status == BOOK_SYNCED || b.status == BOOK_DOWNLOADING;
        default: return true;
    }
}

static void draw_tabs(AppState* state)
{
    int sw = SW, mg = MG, y = tabs_top();
    int w = sw - 2 * mg, tw = w / 3;
    const char* labels[] = { tr("Tous", "All"), tr("Nouveaux", "New"),
                             tr("Sur la liseuse", "On device") };
    int counts[3] = { 0, 0, 0 };
    for (auto& b : state->catalog) {
        if (state->in_folder && b.folder != state->selected_folder) continue;
        for (int i = 0; i < 3; ++i) if (book_matches_filter(b, i)) counts[i]++;
    }

    for (int i = 0; i < 3; ++i) {
        int x = mg + i * tw, cw = (i == 2) ? w - 2 * tw : tw;
        bool on = state->filter == i;
        FillArea(x, y, cw, TABS_H, on ? C_BLACK : C_WHITE);
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%s (%d)", labels[i], counts[i]);
        text_line(x + U(8), y + TABS_H / 2, cw - U(16), lbl, F_BTN, on, on ? C_WHITE : C_BLACK, ALIGN_CENTER);
        if (i > 0) FillArea(x, y, LINE, TABS_H, C_BLACK);
        zones_add(x, y, cw, TABS_H, ZONE_FILTER_BASE + i);
    }
    frame(mg, y, w, TABS_H, LINE, C_BLACK);
}

static int list_visible() { return std::max(1, (list_bottom() - list_top()) / ROW_H); }

static void clamp_page(AppState* state, int total)
{
    int vis = list_visible();
    state->list_visible = vis;
    int last = total > 0 ? ((total - 1) / vis) * vis : 0;
    if (state->list_scroll > last) state->list_scroll = last;
    if (state->list_scroll < 0)    state->list_scroll = 0;
    state->list_scroll -= state->list_scroll % vis;
}

static void draw_pager(AppState* state)
{
    int sw = SW, mg = MG, y = list_bottom();
    FillArea(mg, y, sw - 2 * mg, LINE, C_BLACK);

    int vis   = list_visible();
    int pages = std::max(1, (g_list_total + vis - 1) / vis);
    int page  = state->list_scroll / vis + 1;
    int by    = y + (FOOT_H - BTN_H) / 2 + LINE / 2;

    const char* prev = tr("‹ Précédent", "‹ Previous");
    const char* next = tr("Suivant ›", "Next ›");
    int pw = button_w(prev), nw = button_w(next);
    bool has_prev = page > 1, has_next = page < pages;

    button(mg, by, pw, BTN_H, prev, has_prev ? BTN_OUTLINE : BTN_DISABLED);
    button(sw - mg - nw, by, nw, BTN_H, next, has_next ? BTN_OUTLINE : BTN_DISABLED);
    if (has_prev) zones_add(mg, by, pw, BTN_H, ZONE_PAGE_PREV);
    if (has_next) zones_add(sw - mg - nw, by, nw, BTN_H, ZONE_PAGE_NEXT);

    char lbl[48];
    snprintf(lbl, sizeof(lbl), tr("Page %d sur %d", "Page %d of %d"), page, pages);
    text_line(mg + pw, by + BTN_H / 2, sw - 2 * mg - pw - nw, lbl, F_BTN, false, C_BLACK, ALIGN_CENTER);
}

static void draw_selection_bar(AppState* state)
{
    int sw = SW, mg = MG, y = list_bottom();
    FillArea(0, y, sw, SH - y, C_BLACK);

    int n = 0;
    long long bytes = 0;
    for (auto& b : state->catalog)
        if (state->selected_ids.count(b.jf_id)) { n++; bytes += b.remote_size; }

    int by = y + (FOOT_H - BTN_H) / 2;
    const char* dl  = tr("Télécharger", "Download");
    const char* can = tr("Annuler", "Cancel");
    int dw = button_w(dl), cw = button_w(can);
    int dx = sw - mg - dw, cx = dx - GAP - cw;
    button(dx, by, dw, BTN_H, dl,  BTN_FILL_ON_DARK);
    button(cx, by, cw, BTN_H, can, BTN_ON_DARK);
    zones_add(dx, by, dw, BTN_H, ZONE_BTN_DL_SELECTED);
    zones_add(cx, by, cw, BTN_H, ZONE_BTN_CLEAR_SEL);

    char lbl[96];
    if (g_ui_cfg && g_ui_cfg->lang == 1)
        snprintf(lbl, sizeof(lbl), "%d selected · %s", n, fmt_size(bytes).c_str());
    else
        snprintf(lbl, sizeof(lbl), "%d sélectionné%s · %s", n, n > 1 ? "s" : "", fmt_size(bytes).c_str());
    text_line(mg, by + BTN_H / 2, cx - mg - GAP, lbl, F_BANNER * 9 / 10, true, C_WHITE);
}

static void draw_empty(const char* msg)
{
    text_line(MG, list_top() + (list_bottom() - list_top()) / 3, SW - 2 * MG,
              msg, F_ROW, false, C_GRAY_DARK, ALIGN_CENTER);
}

// ─── Écran 1 : les dossiers ───────────────────────────────────────────────────

static void collect_folders(AppState* state)
{
    g_folders.clear();
    std::map<std::string, size_t> idx;
    for (auto& b : state->catalog) {
        auto it = idx.find(b.folder);
        if (it == idx.end()) {
            it = idx.emplace(b.folder, g_folders.size()).first;
            g_folders.push_back({ b.folder, 0, 0, 0 });
        }
        FolderInfo& f = g_folders[it->second];
        f.total++;
        if (b.status == BOOK_NEW || b.status == BOOK_UPDATED || b.status == BOOK_ERROR) f.n_new++;
        if (b.status == BOOK_SYNCED) f.n_local++;
    }
    // Ne garder que les dossiers qui ont au moins un livre dans le filtre
    std::vector<FolderInfo> kept;
    for (auto& f : g_folders) {
        int n = (state->filter == 0) ? f.total : (state->filter == 1) ? f.n_new : f.n_local;
        if (n > 0) kept.push_back(f);
    }
    // « Sans dossier » en dernier
    std::stable_sort(kept.begin(), kept.end(), [](const FolderInfo& a, const FolderInfo& b) {
        return !a.key.empty() && b.key.empty();
    });
    g_folders.swap(kept);
}

static void draw_screen_folders(AppConfig* cfg, AppState* state)
{
    int sw = SW, mg = MG;
    ClearScreen();
    zones_clear();
    g_row_catalog_idx.clear();

    draw_header("JellySync", false, true, true);

    char title[128], detail[256];
    bool err = state->error_msg[0] != 0;
    if (!cfg->server_url[0]) {
        snprintf(title,  sizeof(title),  "%s", tr("Aucun serveur configuré", "No server configured"));
        snprintf(detail, sizeof(detail), "%s", tr("Ouvrez les Réglages pour saisir l'adresse.",
                                                  "Open Settings to enter the address."));
    } else if (err) {
        snprintf(title,  sizeof(title),  "%s", tr("Problème de connexion", "Connection problem"));
        snprintf(detail, sizeof(detail), "%s", state->error_msg);
    } else if (!state->catalog_loaded) {
        snprintf(title,  sizeof(title),  "%s", tr("Bibliothèque non chargée", "Library not loaded"));
        snprintf(detail, sizeof(detail), "%s", tr("Appuyez sur Actualiser pour la charger.",
                                                  "Press Refresh to load it."));
    } else {
        int n = state->new_count;
        const char* pl = n > 1 ? "s" : "";
        if (n == 0)
            snprintf(title, sizeof(title), "%s", tr("Bibliothèque à jour", "Library up to date"));
        else if (cfg->lang == 1)
            snprintf(title, sizeof(title), "%d new book%s on the server", n, pl);
        else
            snprintf(title, sizeof(title), "%d nouveau%s livre%s sur le serveur", n, n > 1 ? "x" : "", pl);
        snprintf(detail, sizeof(detail), tr("%d livres sur le serveur · %d sur la liseuse",
                                            "%d books on the server · %d on device"),
                 state->total_remote, state->local_book_count);
    }
    // Serveur injoignable : proposer de relancer le Wi-Fi
    bool offline = err && !state->server_connected && cfg->server_url[0];
    draw_banner(title, detail, offline ? tr("Reconnecter le Wi-Fi", "Reconnect Wi-Fi") : nullptr,
                ZONE_BTN_RECONNECT, err);

    if (!state->catalog_loaded) {
        g_list_total = 0;
        state->list_scroll = 0;
        FillArea(mg, list_bottom(), sw - 2 * mg, LINE, C_BLACK);
        return;
    }

    draw_tabs(state);
    collect_folders(state);
    g_list_total = (int)g_folders.size();
    clamp_page(state, g_list_total);

    int top = list_top(), w = sw - 2 * mg;
    if (g_folders.empty())
        draw_empty(tr("Aucun dossier dans cette catégorie.", "No folder in this category."));

    for (int i = 0; i < list_visible() && state->list_scroll + i < g_list_total; ++i) {
        int fi = state->list_scroll + i;
        const FolderInfo& f = g_folders[fi];
        int ry = top + i * ROW_H, cy = ry + ROW_H / 2;

        icon_folder(mg + U(4), cy, U(84), U(64), C_BLACK);
        int tx = mg + U(120), tw = w - U(120) - BTN_H;

        text_line(tx, ry + ROW_H * 36 / 100, tw, folder_label(f.key).c_str(), F_ROW, f.n_new > 0, C_BLACK);

        int my = ry + ROW_H * 70 / 100, mx = tx;
        char meta[96];
        if (f.n_new > 0) {
            char p[48];
            if (cfg->lang == 1) snprintf(p, sizeof(p), "%d NEW", f.n_new);
            else                snprintf(p, sizeof(p), "%d NOUVEAU%s", f.n_new, f.n_new > 1 ? "X" : "");
            mx += pill(mx, my, p, true) + U(16);
        }
        snprintf(meta, sizeof(meta), tr("%d livre%s · %d sur la liseuse", "%d book%s · %d on device"),
                 f.total, f.total > 1 ? "s" : "", f.n_local);
        text_line(mx, my, tx + tw - mx, meta, F_DETAIL, false, C_GRAY_DARK);

        icon_chevron(mg + w - BTN_H / 2, cy, U(44), true, C_BLACK);
        FillArea(mg, ry + ROW_H - HAIR, w, HAIR, C_GRAY_LITE);
        zones_add(mg, ry, w, ROW_H, ZONE_FOLDER_BASE + fi);
    }

    draw_pager(state);
}

// ─── Écran 2 : les livres d'un dossier ───────────────────────────────────────

static bool is_selectable(const BookEntry& b)
{
    return b.status == BOOK_NEW || b.status == BOOK_UPDATED || b.status == BOOK_ERROR;
}

static void draw_screen_books(AppConfig* cfg, AppState* state)
{
    int sw = SW, mg = MG;
    ClearScreen();
    zones_clear();
    g_row_catalog_idx.clear();

    std::string label = folder_label(state->selected_folder);
    draw_header(label.c_str(), true, false, true);

    int f_total = 0, f_new = 0, f_local = 0;
    for (auto& b : state->catalog) {
        if (b.folder != state->selected_folder) continue;
        f_total++;
        if (is_selectable(b)) f_new++;
        if (b.status == BOOK_SYNCED) f_local++;
    }

    char title[128], detail[256];
    bool err = state->error_msg[0] != 0;
    if (err) {
        snprintf(title, sizeof(title), "%s", tr("Attention", "Warning"));
        snprintf(detail, sizeof(detail), "%s", state->error_msg);
    } else {
        if (f_new > 0 && cfg->lang == 1) {
            snprintf(title, sizeof(title), "%d new in this folder", f_new);
        } else if (f_new > 0) {
            snprintf(title, sizeof(title), "%d nouveau%s dans ce dossier", f_new, f_new > 1 ? "x" : "");
        } else {
            snprintf(title, sizeof(title), "%s", tr("Dossier à jour", "Folder up to date"));
        }
        snprintf(detail, sizeof(detail), tr("%d livre%s · %d sur la liseuse", "%d book%s · %d on device"),
                 f_total, f_total > 1 ? "s" : "", f_local);
    }
    draw_banner(title, detail, f_new > 0 ? tr("Tout télécharger", "Download all") : nullptr,
                ZONE_BTN_DL_FOLDER, err);
    draw_tabs(state);

    std::vector<int> rows;
    for (int i = 0; i < (int)state->catalog.size(); ++i) {
        const BookEntry& b = state->catalog[i];
        if (b.folder == state->selected_folder && book_matches_filter(b, state->filter))
            rows.push_back(i);
    }
    g_list_total = (int)rows.size();
    clamp_page(state, g_list_total);

    int top = list_top(), w = sw - 2 * mg;
    int chk = U(76);
    if (rows.empty())
        draw_empty(tr("Aucun livre dans cette catégorie.", "No book in this category."));

    for (int i = 0; i < list_visible() && state->list_scroll + i < g_list_total; ++i) {
        int cat_idx = rows[state->list_scroll + i];
        const BookEntry& b = state->catalog[cat_idx];
        g_row_catalog_idx.push_back(cat_idx);
        int ry = top + i * ROW_H, cy = ry + ROW_H / 2;

        bool sel_ok   = is_selectable(b);
        bool selected = sel_ok && state->selected_ids.count(b.jf_id) > 0;

        // Case à cocher
        if (sel_ok) {
            int cx = mg, cyy = cy - chk / 2;
            FillArea(cx, cyy, chk, chk, selected ? C_BLACK : C_WHITE);
            frame(cx, cyy, chk, chk, LINE, C_BLACK);
            if (selected) icon_check(cx + chk / 2, cy, chk * 3 / 5, C_WHITE);
        }

        int dl_x = mg + w - BTN_H;
        int tx = mg + chk + U(32), tw = dl_x - GAP - tx;

        bool strong = sel_ok || b.status == BOOK_DOWNLOADING;
        text_line(tx, ry + ROW_H * 36 / 100, tw, b.name.c_str(), F_ROW, strong,
                  strong ? C_BLACK : C_GRAY_DARK);

        // Ligne de détail : état + format + taille
        int my = ry + ROW_H * 70 / 100, mx = tx;
        std::string ext = fmt_ext(b.filename);
        std::string meta;
        switch (b.status) {
            case BOOK_NEW:         mx += pill(mx, my, tr("NOUVEAU", "NEW"), true) + U(16); break;
            case BOOK_UPDATED:     mx += pill(mx, my, tr("MIS À JOUR", "UPDATED"), false) + U(16); break;
            case BOOK_ERROR:       mx += pill(mx, my, tr("ÉCHEC", "FAILED"), true) + U(16); break;
            case BOOK_DOWNLOADING: mx += pill(mx, my, tr("EN COURS", "DOWNLOADING"), false) + U(16); break;
            case BOOK_SYNCED:      meta = std::string(tr("Sur la liseuse", "On device")) + " · "; break;
        }
        meta += ext;
        long long size = (b.status == BOOK_SYNCED) ? b.local_size : b.remote_size;
        if (size > 0) meta += (ext.empty() ? "" : " · ") + fmt_size(size);
        text_line(mx, my, tx + tw - mx, meta.c_str(), F_DETAIL, false, C_GRAY_DARK);

        // Action à droite
        if (sel_ok) {
            int by = cy - BTN_H / 2;
            button(dl_x, by, BTN_H, BTN_H, nullptr, BTN_OUTLINE);
            icon_download(dl_x + BTN_H / 2, cy, BTN_H * 2 / 5, C_BLACK);
            zones_add(dl_x - U(8), ry, BTN_H + U(8), ROW_H, ZONE_ROW_DL + i);
            zones_add(mg, ry, dl_x - U(8) - mg, ROW_H, ZONE_ROW_SELECT + i);
        } else if (b.status == BOOK_SYNCED) {
            icon_check(dl_x + BTN_H / 2, cy, U(44), C_GRAY_DARK);
        }

        FillArea(mg, ry + ROW_H - HAIR, w, HAIR, C_GRAY_LITE);
    }

    if (!state->selected_ids.empty()) draw_selection_bar(state);
    else                              draw_pager(state);
}

// ─── Écran 3 : téléchargement en cours + journal ─────────────────────────────

static void draw_log_panel(int y, int h)
{
    int sw = SW, mg = MG;
    g_log_rect = { 0, y, sw, h };
    FillArea(0, y, sw, h, C_WHITE);
    FillArea(0, y, sw, LINE, C_BLACK);

    int hy = y + LINE + U(30);
    text_line(mg, hy, sw - 2 * mg, tr("JOURNAL DÉTAILLÉ", "DETAILED LOG"), F_SECTION, true, C_GRAY_DARK);

    int line_h = F_LOG * 13 / 10;
    int top = hy + U(30);
    int n = std::max(1, (y + h - U(12) - top) / line_h);
    std::vector<std::string> lines;
    log_tail(lines, n);
    int ly = top + (n - (int)lines.size()) * line_h;   // lignes récentes en bas
    for (auto& l : lines) {
        text_line(mg, ly + line_h / 2, sw - 2 * mg, l.c_str(), F_LOG, false, C_BLACK);
        ly += line_h;
    }
}

static int progress_pct(AppState* state)
{
    if (state->bg_mode != BG_CATALOG && state->dl_bytes_total > 0)
        return (int)std::min<long long>(100, state->dl_bytes_now * 100 / state->dl_bytes_total);
    return std::max(0, std::min(100, state->progress));
}

static void draw_screen_syncing(AppConfig* cfg, AppState* state)
{
    int sw = SW, sh = SH, mg = MG;
    int px = U(96), pw = sw - 2 * px;
    ClearScreen();
    zones_clear();

    const char* label =
        (state->bg_mode == BG_CATALOG)           ? tr("CHARGEMENT DE LA BIBLIOTHÈQUE", "LOADING LIBRARY") :
        (state->bg_mode == BG_DOWNLOAD_FOLDER)   ? tr("TÉLÉCHARGEMENT DU DOSSIER", "DOWNLOADING FOLDER") :
        (state->bg_mode == BG_DOWNLOAD_SELECTED) ? tr("TÉLÉCHARGEMENT DE LA SÉLECTION", "DOWNLOADING SELECTION") :
                                                   tr("TÉLÉCHARGEMENT", "DOWNLOADING");
    int log_y = sh * 4 / 5;
    int y = U(220);
    text_line(px, y, pw, label, F_SECTION, true, C_GRAY_DARK);

    // Zone redessinée à chaque palier de progression
    int ry = y + U(50);
    g_progress_rect = { 0, ry, sw, log_y - ry };

    char step[160];
    bool dl = state->bg_mode != BG_CATALOG;
    if (state->wifi_reconnecting)
        snprintf(step, sizeof(step), "%s", tr("Wi-Fi perdu, reconnexion en cours...",
                                              "Wi-Fi lost, reconnecting..."));
    else if (dl && state->dl_total > 0)
        snprintf(step, sizeof(step), tr("Livre %d sur %d", "Book %d of %d"),
                 std::max(1, state->dl_index), state->dl_total);
    else
        snprintf(step, sizeof(step), "%s", state->status_msg);
    text_line(px, ry + U(40), pw, step, F_BANNER, false, C_BLACK);

    if (dl && !state->dl_book_name.empty()) {
        use_font(U(60), true, C_BLACK);
        DrawTextRect(px, ry + U(100), pw, U(160), state->dl_book_name.c_str(),
                     ALIGN_LEFT | VALIGN_TOP | DOTS);
    }

    int pct = progress_pct(state);
    int by = ry + U(300), bh = U(56);
    frame(px, by, pw, bh, LINE, C_BLACK);
    int fill = (pw - 2 * LINE) * pct / 100;
    if (fill > 0) FillArea(px + LINE, by + LINE, fill, bh - 2 * LINE, C_BLACK);

    char nums[96], pct_s[16];
    snprintf(pct_s, sizeof(pct_s), "%d %%", pct);
    if (dl && state->dl_bytes_total > 0)
        snprintf(nums, sizeof(nums), tr("%s sur %s", "%s of %s"),
                 fmt_size(state->dl_bytes_now).c_str(), fmt_size(state->dl_bytes_total).c_str());
    else if (dl && state->dl_bytes_now > 0)
        snprintf(nums, sizeof(nums), tr("%s reçus", "%s received"), fmt_size(state->dl_bytes_now).c_str());
    else
        nums[0] = 0;
    text_line(px, by + bh + U(50), pw * 2 / 3, nums, F_DETAIL * 11 / 10, false, C_BLACK);
    text_line(px + pw / 3, by + bh + U(50), pw * 2 / 3, pct_s, F_DETAIL * 11 / 10, true, C_BLACK, ALIGN_RIGHT);

    if (dl && state->dl_total > 0) {
        int ty = by + bh + U(130);
        FillArea(px, ty, pw, HAIR, C_GRAY_LITE);
        int done = state->downloaded, errs = state->dl_errors;
        int left = std::max(0, state->dl_total - done - errs);
        int vals[3] = { done, left, errs };
        const char* lbls[3] = { tr("terminés", "done"), tr("restants", "remaining"), tr("échecs", "failed") };
        int cw = pw / 3;
        for (int i = 0; i < 3; ++i) {
            char v[16];
            snprintf(v, sizeof(v), "%d", vals[i]);
            text_line(px + i * cw, ty + U(70), cw, v, U(68), true, C_BLACK);
            text_line(px + i * cw, ty + U(140), cw, lbls[i], F_DETAIL, false, C_GRAY_DARK);
        }
    }

    draw_log_panel(log_y, sh - log_y);
}

// ─── Écran 4 : réglages ───────────────────────────────────────────────────────

static int draw_section(int y, const char* label)
{
    text_line(MG, y + U(30), SW - 2 * MG, label, F_SECTION, true, C_GRAY_DARK);
    return y + U(56);
}

static int draw_field(int y, const char* label, const char* value, bool password, int zone)
{
    int sw = SW, mg = MG, w = sw - 2 * mg, h = U(112);
    text_line(mg, y + U(22), w, label, F_DETAIL, false, C_BLACK);
    y += U(48);
    frame(mg, y, w, h, LINE, C_BLACK);

    char disp[300];
    int color = C_BLACK;
    if (!value[0]) {
        snprintf(disp, sizeof(disp), "%s", tr("Toucher pour saisir", "Tap to enter"));
        color = C_GRAY_MID;
    } else if (password) {
        int n = std::min((int)strlen(value), 24);
        for (int i = 0; i < n; ++i) disp[i] = '*';
        disp[n] = 0;
    } else {
        snprintf(disp, sizeof(disp), "%s", value);
    }
    text_line(mg + U(28), y + h / 2, w - U(56), disp, U(38), false, color);
    zones_add(mg, y - U(48), w, h + U(48), zone);
    return y + h + U(20);
}

static int draw_segmented(int y, const char* a, const char* b, int active, int zone_a, int zone_b)
{
    int sw = SW, mg = MG, w = sw - 2 * mg, h = BTN_H, hw = w / 2;
    for (int i = 0; i < 2; ++i) {
        int x = mg + i * hw, cw = i ? w - hw : hw;
        bool on = active == i;
        FillArea(x, y, cw, h, on ? C_BLACK : C_WHITE);
        text_line(x, y + h / 2, cw, i ? b : a, F_BTN, on, on ? C_WHITE : C_BLACK, ALIGN_CENTER);
        zones_add(x, y, cw, h, i ? zone_b : zone_a);
    }
    FillArea(mg + hw, y, LINE, h, C_BLACK);
    frame(mg, y, w, h, LINE, C_BLACK);
    return y + h + U(20);
}

static void draw_screen_setup(AppConfig* cfg, AppState* state)
{
    int sw = SW, sh = SH, mg = MG;
    ClearScreen();
    zones_clear();

    draw_header(tr("Réglages", "Settings"), true, false, false);

    int y = HDR_H + U(20);
    y = draw_section(y, tr("SERVEUR", "SERVER"));
    y = draw_field(y, tr("Adresse du serveur Jellyfin", "Jellyfin server address"),
                   cfg->server_url, false, ZONE_URL);

    y = draw_section(y + U(10), tr("CONNEXION", "SIGN-IN"));
    y = draw_segmented(y, tr("Identifiants", "Username"), tr("Clé API", "API key"),
                       cfg->auth_mode == 1 ? 1 : 0, ZONE_AUTH_CRED, ZONE_AUTH_KEY);
    if (cfg->auth_mode == 1) {
        y = draw_field(y, tr("Clé API", "API key"), cfg->api_key, true, ZONE_APIKEY);
    } else {
        y = draw_field(y, tr("Nom d'utilisateur", "Username"), cfg->username, false, ZONE_USER);
        y = draw_field(y, tr("Mot de passe", "Password"), cfg->password, true, ZONE_PASS);
    }

    y = draw_section(y + U(10), tr("LANGUE", "LANGUAGE"));
    y = draw_segmented(y, "Français", "English", cfg->lang == 1 ? 1 : 0, ZONE_LANG_FR, ZONE_LANG_EN);

    int bh = U(124), bw = (sw - 2 * mg - GAP) / 2, by = sh - mg - bh;
    button(mg, by, bw, bh, tr("Annuler", "Cancel"), BTN_OUTLINE);
    button(mg + bw + GAP, by, bw, bh, tr("Enregistrer", "Save"), BTN_FILL);
    zones_add(mg, by, bw, bh, ZONE_BACK_SETUP);
    zones_add(mg + bw + GAP, by, bw, bh, ZONE_SAVE);
}

// ─── API publique ─────────────────────────────────────────────────────────────

static void ui_init(AppConfig* cfg, AppState* state)
{
    g_ui_cfg   = cfg;
    g_ui_state = state;
    state->in_folder = false;
    state->selected_folder.clear();
    state->download_book_idx = -1;
    state->download_progress = 0;
}

static void draw_current(AppConfig* cfg, AppState* state)
{
    if (state->syncing)                     draw_screen_syncing(cfg, state);
    else if (state->screen == SCREEN_SETUP) draw_screen_setup(cfg, state);
    else if (state->in_folder)              draw_screen_books(cfg, state);
    else                                    draw_screen_folders(cfg, state);
}

// Changement d'écran : rafraîchissement complet
static void ui_draw(AppConfig* cfg, AppState* state)
{
    if (!check_screen_dims()) return;
    draw_current(cfg, state);
    flush_full();
}

// Même écran, contenu modifié entre y0 et y1 : rafraîchissement partiel
static void ui_draw_region(AppConfig* cfg, AppState* state, int y0, int y1)
{
    if (!check_screen_dims()) return;
    draw_current(cfg, state);
    flush_partial(0, y0, SW, y1 - y0);
}

// Progression d'un téléchargement : ne redessine que la zone de progression
// (paliers de 5 %, changement de livre) et le journal (au plus 1 fois/s).
static void ui_progress(AppConfig* cfg, AppState* state)
{
    static int      last_bucket = -1, last_index = -1;
    static unsigned last_log    = 0;
    static time_t   last_log_t  = 0;
    if (!state->syncing || state->screen == SCREEN_SETUP) return;

    std::vector<std::string> dummy;
    unsigned seq = log_tail(dummy, 0);
    int bucket = progress_pct(state) / 5;
    time_t now = time(nullptr);

    bool prog = bucket != last_bucket || state->dl_index != last_index;
    bool logs = seq != last_log && now != last_log_t;
    if (!prog && !logs) return;

    draw_screen_syncing(cfg, state);
    if (prog) {
        last_bucket = bucket;
        last_index  = state->dl_index;
        PartialUpdate(g_progress_rect.x, g_progress_rect.y, g_progress_rect.w, g_progress_rect.h);
    }
    if (logs) {
        last_log   = seq;
        last_log_t = now;
        PartialUpdate(g_log_rect.x, g_log_rect.y, g_log_rect.w, g_log_rect.h);
    }
}

// Page précédente (-1) ou suivante (+1) ; aussi appelée par les touches physiques
static void ui_page(AppConfig* cfg, AppState* state, int dir)
{
    if (state->syncing || state->screen != SCREEN_MAIN) return;
    int vis = list_visible();
    int ns  = state->list_scroll + dir * vis;
    if (ns < 0 || ns >= g_list_total) return;
    state->list_scroll = ns;
    ui_draw_region(cfg, state, list_top(), SH);
}

// Touche Retour : renvoie false si l'application doit se fermer
static bool ui_back(AppConfig* cfg, AppState* state)
{
    if (state->syncing) return true;
    if (state->screen == SCREEN_SETUP) {
        *cfg = g_cfg_backup;
        state->screen = SCREEN_MAIN;
        ui_draw(cfg, state);
        return true;
    }
    if (!state->selected_ids.empty()) {
        state->selected_ids.clear();
        ui_draw_region(cfg, state, list_top(), SH);
        return true;
    }
    if (state->in_folder) {
        state->in_folder = false;
        state->list_scroll = 0;
        state->error_msg[0] = 0;
        ui_draw(cfg, state);
        return true;
    }
    return false;
}

// ─── Clavier natif PocketBook ─────────────────────────────────────────────────

static void keyboard_cb(char* text)
{
    if (text && g_kb_req.target) {
        strncpy(g_kb_req.target, text, g_kb_req.max_len - 1);
        g_kb_req.target[g_kb_req.max_len - 1] = 0;
    }
    ui_draw(g_ui_cfg, g_ui_state);
}

static void open_keyboard(char* target, int max_len, const char* title, bool password = false)
{
    g_kb_req = { target, max_len };
    OpenKeyboard(title, target, max_len, password ? KBD_PASSWORD : KBD_NORMAL, keyboard_cb);
}

// ─── Gestion des taps ────────────────────────────────────────────────────────

static void ui_handle_tap(int px, int py,
                          AppConfig* cfg, AppState* state,
                          std::function<void()> on_refresh,
                          std::function<void()> on_reconnect,
                          std::function<void(int)> on_download_one,
                          std::function<void(const std::string&)> on_download_folder,
                          std::function<void()> on_download_selected)
{
    if (state->syncing) return;
    int zone = zones_hit(px, py);
    if (zone < 0) return;

    // Ouvrir un dossier
    if (zone >= ZONE_FOLDER_BASE && zone < ZONE_FOLDER_BASE + 1000) {
        int fi = zone - ZONE_FOLDER_BASE;
        if (fi < (int)g_folders.size()) {
            state->in_folder       = true;
            state->selected_folder = g_folders[fi].key;
            state->list_scroll     = 0;
            state->error_msg[0]    = 0;
            ui_draw(cfg, state);
        }
        return;
    }

    // Cocher / décocher un livre (toute la ligne sauf le bouton de droite)
    if (zone >= ZONE_ROW_SELECT && zone < ZONE_ROW_SELECT + 1000) {
        int row = zone - ZONE_ROW_SELECT;
        if (row < (int)g_row_catalog_idx.size()) {
            const BookEntry& b = state->catalog[g_row_catalog_idx[row]];
            if (is_selectable(b)) {
                if (state->selected_ids.count(b.jf_id)) state->selected_ids.erase(b.jf_id);
                else                                     state->selected_ids.insert(b.jf_id);
                // La ligne et la barre du bas suffisent
                int ry = list_top() + row * ROW_H;
                draw_current(cfg, state);
                flush_partial(0, ry, SW, ROW_H);
                flush_partial(0, list_bottom(), SW, SH - list_bottom());
            }
        }
        return;
    }

    // Télécharger un livre tout de suite
    if (zone >= ZONE_ROW_DL && zone < ZONE_ROW_DL + 1000) {
        int row = zone - ZONE_ROW_DL;
        if (row < (int)g_row_catalog_idx.size())
            on_download_one(g_row_catalog_idx[row]);
        return;
    }

    if (zone >= ZONE_FILTER_BASE && zone < ZONE_FILTER_BASE + 3) {
        int f = zone - ZONE_FILTER_BASE;
        if (f != state->filter) {
            state->filter = f;
            state->list_scroll = 0;
            ui_draw_region(cfg, state, tabs_top(), SH);
        }
        return;
    }

    switch (zone) {
        case ZONE_BTN_BACK:
            ui_back(cfg, state);
            break;
        case ZONE_BTN_REFRESH:
            state->error_msg[0] = 0;
            on_refresh();
            break;
        case ZONE_BTN_RECONNECT:
            on_reconnect();
            break;
        case ZONE_BTN_DL_FOLDER:
            state->error_msg[0] = 0;
            on_download_folder(state->selected_folder);
            break;
        case ZONE_BTN_DL_SELECTED:
            state->error_msg[0] = 0;
            on_download_selected();
            break;
        case ZONE_BTN_CLEAR_SEL:
            state->selected_ids.clear();
            ui_draw_region(cfg, state, list_top(), SH);
            break;
        case ZONE_BTN_SETTINGS:
            g_cfg_backup  = *cfg;
            state->screen = SCREEN_SETUP;
            ui_draw(cfg, state);
            break;
        case ZONE_PAGE_PREV: ui_page(cfg, state, -1); break;
        case ZONE_PAGE_NEXT: ui_page(cfg, state, +1); break;

        // ── Réglages ──────────────────────────────────────────────────────
        case ZONE_URL:
            open_keyboard(cfg->server_url, sizeof(cfg->server_url),
                          tr("Adresse Jellyfin (ex : http://192.168.1.10:8096)",
                             "Jellyfin address (e.g. http://192.168.1.10:8096)"));
            break;
        case ZONE_USER:
            open_keyboard(cfg->username, sizeof(cfg->username), tr("Nom d'utilisateur", "Username"));
            break;
        case ZONE_PASS:
            open_keyboard(cfg->password, sizeof(cfg->password), tr("Mot de passe", "Password"), true);
            break;
        case ZONE_APIKEY:
            open_keyboard(cfg->api_key, sizeof(cfg->api_key), tr("Clé API", "API key"), true);
            break;
        case ZONE_AUTH_CRED:
        case ZONE_AUTH_KEY: {
            int m = (zone == ZONE_AUTH_KEY) ? 1 : 0;
            if (cfg->auth_mode != m) { cfg->auth_mode = m; ui_draw_region(cfg, state, HDR_H, SH); }
            break;
        }
        case ZONE_LANG_FR:
        case ZONE_LANG_EN: {
            int l = (zone == ZONE_LANG_EN) ? 1 : 0;
            if (cfg->lang != l) { cfg->lang = l; ui_draw(cfg, state); }
            break;
        }
        case ZONE_SAVE: {
            bool server_changed = strcmp(cfg->server_url, g_cfg_backup.server_url) != 0;
            if (server_changed) cfg->library_id[0] = 0;
            config_save(CONFIG_FILE, cfg);
            state->screen = SCREEN_MAIN;
            state->in_folder = false;
            state->error_msg[0] = 0;
            if (cfg->server_url[0]) on_refresh();
            else                    ui_draw(cfg, state);
            break;
        }
        case ZONE_BACK_SETUP:
            *cfg = g_cfg_backup;
            state->screen = SCREEN_MAIN;
            ui_draw(cfg, state);
            break;
    }
}
