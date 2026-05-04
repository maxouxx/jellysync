#pragma once
/**
 * ui.h — Interface e-ink JellySync v3
 *
 * Nouveautés v3 :
 *   • Dimensions 100% relatives à ScreenWidth()/ScreenHeight() — s'adapte
 *     automatiquement à toute résolution (Inkpad 3 : 1404×1872)
 *   • Affichage par dossiers : vue dossiers → tap → vue livres du dossier
 *   • Bouton "Télécharger" sur chaque livre non présent (tap sur la ligne)
 *   • Bouton "Retour" pour revenir à la vue dossiers
 *   • Tailles de police, marges, hauteurs de ligne toutes calculées depuis SW/SH
 */

#include <inkview.h>
#include "inkview_compat.h"
#include <cstring>
#include <cstdio>
#include <functional>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include "config.h"

// ─── Logging ──────────────────────────────────────────────────────────────────
extern void log_write(const char* fmt, ...);

// ─── Palette ──────────────────────────────────────────────────────────────────
#define C_BLACK      0x000000
#define C_WHITE      0xFFFFFF
#define C_GRAY_DARK  0x333333
#define C_GRAY_MID   0x888888
#define C_GRAY_LITE  0xCCCCCC
#define C_GRAY_BG    0xEEEEEE

// ─── Dimensions relatives ─────────────────────────────────────────────────────
// Toutes les constantes sont des MACROS calculées depuis ScreenWidth/Height
// afin de s'adapter dynamiquement à la résolution réelle.
//
//  sw = ScreenWidth()   → 1404 sur Inkpad 3
//  sh = ScreenHeight()  → 1872 sur Inkpad 3
//
// Convention : SW_ = fraction de sw,  SH_ = fraction de sh
//
#define SW  (ScreenWidth())
#define SH  (ScreenHeight())

// Check for valid screen dimensions
static bool check_screen_dims()
{
    int sw = SW;
    int sh = SH;
    log_write("check_screen_dims: SW=%d, SH=%d\n", sw, sh);
    if (sw <= 0 || sh <= 0) {
        log_write("ERROR: Invalid screen dimensions!\n");
        return false;
    }
    return true;
}

// Marges latérales : ~2.5% de la largeur
#define MARGIN      (SW / 40)

// En-tête : ~6% de la hauteur
#define HEADER_H    (SH / 16)

// Barre de filtres : ~5% de la hauteur
#define FILTER_BAR_H (SH / 20)

// Pied de page : ~6% de la hauteur
#define FOOTER_H    (SH / 16)

// Hauteur d'une ligne livre : ~5.5% de la hauteur
#define ROW_H       (SH / 18)

// Hauteur d'un en-tête de dossier : ~4.5% de la hauteur
#define FOLDER_H    (SH / 22)

// Barre de scroll latérale : ~2% de la largeur
#define SCROLL_W    (SW / 50)

// Badge de statut (barre colorée gauche) : ~1% de la largeur
#define BADGE_W     (SW / 100)

// Taille police titre application
#define FONT_TITLE  (SW / 14)
// Taille police sous-titre / filtre
#define FONT_SUB    (SW / 26)
// Taille police ligne livre
#define FONT_ROW    (SW / 24)
// Taille police détail (taille fichier, status)
#define FONT_DETAIL (SW / 32)
// Taille police bouton
#define FONT_BTN    (SW / 26)
// Taille police en-tête de dossier
#define FONT_FOLDER (SW / 22)

// ─── Zones tactiles ───────────────────────────────────────────────────────────
struct TouchZone { int x, y, w, h, id; };

#define ZONE_BTN_REFRESH            1
#define ZONE_BTN_SYNC               2
#define ZONE_BTN_SETTINGS           3
#define ZONE_BTN_DOWNLOAD_SELECTED  4   // télécharger la sélection
#define ZONE_BTN_CLEAR_SELECTION    5   // annuler la sélection
#define ZONE_FILTER_ALL             10
#define ZONE_FILTER_NEW             11
#define ZONE_FILTER_SYNCED          12
#define ZONE_FILTER_LOCAL           13
#define ZONE_SCROLL_UP              20
#define ZONE_SCROLL_DOWN            21
#define ZONE_BTN_BACK               22
// Dossiers navigation : 50 + index (max 50)
#define ZONE_FOLDER_BASE            50
// Lignes livres (bouton DL) : 100 + index visuel (max 200)
#define ZONE_ROW_BASE               100
// Setup
#define ZONE_URL                    30
#define ZONE_USER                   31
#define ZONE_PASS                   32
#define ZONE_APIKEY                 33
#define ZONE_DELETE_TOG             34
#define ZONE_SAVE                   35
#define ZONE_BACK_SETUP             36
#define ZONE_LANG_TOG               37
// Bouton "Télécharger tout" par dossier : 300 + index (max 50)
#define ZONE_FOLDER_DOWNLOAD_BASE   300
// Cases à cocher livres : 400 + index visuel (max 200)
#define ZONE_CHECKBOX_BASE          400

static TouchZone g_zones[700];
static int       g_zone_count = 0;

static void zones_clear() { g_zone_count = 0; }
static void zones_add(int x, int y, int w, int h, int id)
{
    if (g_zone_count >= 300) return;
    g_zones[g_zone_count++] = { x, y, w, h, id };
}
static int zones_hit(int px, int py)
{
    for (int i = g_zone_count - 1; i >= 0; --i) {
        auto& z = g_zones[i];
        if (px >= z.x && px <= z.x + z.w && py >= z.y && py <= z.y + z.h)
            return z.id;
    }
    return -1;
}

// ─── Globals UI ───────────────────────────────────────────────────────────────
static AppConfig* g_ui_cfg   = nullptr;
static AppState*  g_ui_state = nullptr;

// ─── Traduction ───────────────────────────────────────────────────────────────
static const char* tr(const char* fr, const char* en)
{
    return (g_ui_cfg && g_ui_cfg->lang == 1) ? en : fr;
}

// Index de catalogue associé à chaque ligne visible (pour le tap → download)
static std::vector<int> g_row_catalog_idx;

struct UIKeyboardReq { char* target; int max_len; const char* title; bool password; };
static UIKeyboardReq g_kb_req;

// ─── Font management ──────────────────────────────────────────────────────────

static ifont* font(int size, bool bold = false)
{
    if (size <= 0) size = 24; // Fallback size
    
    log_write("font: requesting size=%d, bold=%d\n", size, bold);
    
    const char* font_names[] = {
        bold ? iv_get_default_font(FONT_BOLD) : iv_get_default_font(FONT_STD),
        "LiberationSans-Regular.ttf",
        "DejaVuSans.ttf", 
        "Arial.ttf",
        "sans.ttf",
        NULL
    };
    
    for (int i = 0; font_names[i]; ++i) {
        log_write("font: trying '%s'\n", font_names[i]);
        ifont* f = OpenFont(font_names[i], size, 1);
        if (f) {
            log_write("font: success with '%s'\n", font_names[i]);
            return f;
        }
    }
    
    // Last resort: try to get current font
    const ifont* current = GetFont();
    log_write("font: fallback to current font: %p\n", current);
    return (ifont*)current;
}

static void cleanup_fonts()
{
    // No-op since we're not caching fonts
}

// ─── Safe font functions ──────────────────────────────────────────────────────
static void safe_set_font(ifont* f, int color)
{
    log_write("safe_set_font: f=%p, color=%d\n", f, color);
    
    if (f) {
        SetFont(f, color);
        log_write("safe_set_font: SetFont success\n");
    } else {
        // Try to use system default font
        const ifont* default_font = GetFont();
        log_write("safe_set_font: current font=%p\n", default_font);
        if (default_font) {
            SetFont(default_font, color);
            log_write("safe_set_font: used current font\n");
        } else {
            // Last resort - try to load a basic font
            log_write("safe_set_font: trying fallback font\n");
            ifont* fallback = OpenFont("LiberationSans", 24, 1);
            if (fallback) {
                SetFont(fallback, color);
                CloseFont(fallback);
                log_write("safe_set_font: used fallback font\n");
            } else {
                log_write("safe_set_font: no font available - potential crash\n");
            }
            // If all fails, just continue - the system might have a default
        }
    }
}

// Use safe_set_font instead of SetFont for crash prevention
#define SetFont(f, c) safe_set_font((f), (c))

// ─── Helpers dessin ───────────────────────────────────────────────────────────

static void draw_rect_border(int x, int y, int w, int h, int color)
{
    DrawLine(x,     y,     x + w, y,     color);
    DrawLine(x,     y + h, x + w, y + h, color);
    DrawLine(x,     y,     x,     y + h, color);
    DrawLine(x + w, y,     x + w, y + h, color);
}

static void draw_button(int x, int y, int w, int h, const char* label, bool filled)
{
    int fs = FONT_BTN;
    if (filled) {
        FillArea(x, y, w, h, C_BLACK);
        SetFont(font(fs, true), C_WHITE);
    } else {
        FillArea(x, y, w, h, C_WHITE);
        draw_rect_border(x, y, w, h, C_BLACK);
        SetFont(font(fs, true), C_BLACK);
    }
    DrawTextRect(x, y, w, h, label, ALIGN_CENTER | VALIGN_MIDDLE);
}

static void draw_progress_bar(int x, int y, int w, int h, int pct)
{
    FillArea(x, y, w, h, C_GRAY_LITE);
    draw_rect_border(x, y, w, h, C_GRAY_DARK);
    int fill = (pct * (w - 2)) / 100;
    if (fill > 0) FillArea(x + 1, y + 1, fill, h - 2, C_BLACK);
}

static std::string fmt_size(long long bytes)
{
    char buf[32];
    if (bytes <= 0)              snprintf(buf, sizeof(buf), "—");
    else if (bytes < 1024)       snprintf(buf, sizeof(buf), "%lld o", bytes);
    else if (bytes < 1024*1024)  snprintf(buf, sizeof(buf), "%lld Ko", bytes / 1024);
    else snprintf(buf, sizeof(buf), "%.1f Mo", bytes / 1048576.0);
    return buf;
}

// ─── Filtrage ─────────────────────────────────────────────────────────────────

static bool book_matches_filter(const BookEntry& b, int filter)
{
    switch (filter) {
        case 0: return true;
        case 1: return (b.status == BOOK_NEW || b.status == BOOK_UPDATED);
        case 2: return (b.status == BOOK_SYNCED || b.status == BOOK_DOWNLOADING);
        case 3: return (b.status == BOOK_LOCAL_ONLY);
        default: return true;
    }
}

// ─── Badge de statut ──────────────────────────────────────────────────────────

static void draw_status_badge(int x, int y, int h, BookStatus status)
{
    int bw = BADGE_W;
    int icon_x = x + bw + (SW / 60);
    int icon_w = SW / 18;
    int icon_fs = SW / 28;

    switch (status) {
        case BOOK_NEW:
            FillArea(x, y + 4, bw, h - 8, C_BLACK);
            SetFont(font(icon_fs, true), C_BLACK);
            DrawTextRect(icon_x, y, icon_w, h, "N", ALIGN_CENTER | VALIGN_MIDDLE);
            break;
        case BOOK_UPDATED:
            FillArea(x, y + 4, bw, h - 8, C_GRAY_DARK);
            SetFont(font(icon_fs, true), C_GRAY_DARK);
            DrawTextRect(icon_x, y, icon_w, h, "M", ALIGN_CENTER | VALIGN_MIDDLE);
            break;
        case BOOK_SYNCED:
            FillArea(x, y + 4, bw, h - 8, C_GRAY_LITE);
            SetFont(font(icon_fs, false), C_GRAY_MID);
            DrawTextRect(icon_x, y, icon_w, h, "✓", ALIGN_CENTER | VALIGN_MIDDLE);
            break;
        case BOOK_LOCAL_ONLY:
            for (int yy = y + 4; yy < y + h - 4; yy += SW / 70)
                FillArea(x, yy, bw, SW / 140, C_GRAY_MID);
            SetFont(font(icon_fs, false), C_GRAY_MID);
            DrawTextRect(icon_x, y, icon_w, h, "?", ALIGN_CENTER | VALIGN_MIDDLE);
            break;
        case BOOK_DOWNLOADING:
            FillArea(x, y + 4, bw, h - 8, C_GRAY_DARK);
            SetFont(font(icon_fs, true), C_BLACK);
            DrawTextRect(icon_x, y, icon_w, h, "↓", ALIGN_CENTER | VALIGN_MIDDLE);
            break;
        case BOOK_ERROR:
            FillArea(x, y + 4, bw, h - 8, C_GRAY_DARK);
            SetFont(font(icon_fs, true), C_GRAY_DARK);
            DrawTextRect(icon_x, y, icon_w, h, "!", ALIGN_CENTER | VALIGN_MIDDLE);
            break;
    }
}

// ─── En-tête commun ───────────────────────────────────────────────────────────

static void draw_header(AppState* state, bool show_back)
{
    int sw = SW;
    int hh = HEADER_H;
    int mg = MARGIN;

    FillArea(0, 0, sw, hh, C_WHITE);

    // Bouton retour (si dans un dossier)
    if (show_back) {
        int back_w = sw / 8;
        draw_button(mg, hh / 4, back_w, hh / 2, tr("< Retour", "< Back"), false);
        zones_add(mg, hh / 4, back_w, hh / 2, ZONE_BTN_BACK);

        // Nom du dossier en titre
        SetFont(font(FONT_TITLE, true), C_BLACK);
        DrawTextRect(mg + back_w + mg, 0, sw / 2, hh,
                     state->selected_folder.c_str(), ALIGN_LEFT | VALIGN_MIDDLE);
    } else {
        // Titre application
        SetFont(font(FONT_TITLE, true), C_BLACK);
        DrawTextRect(mg, 0, sw / 3, hh, "JellySync", ALIGN_LEFT | VALIGN_MIDDLE);

        // Badge nouveautés
        if (state->catalog_loaded && state->new_count > 0) {
            char badge[48];
            snprintf(badge, sizeof(badge), "%d nouveau%s",
                     state->new_count, state->new_count > 1 ? "x" : "");
            int bw = sw / 7;
            int bx = sw / 3 + mg;
            int by = hh / 4;
            int bh = hh / 2;
            FillArea(bx, by, bw, bh, C_BLACK);
            SetFont(font(FONT_DETAIL, true), C_WHITE);
            DrawTextRect(bx, by, bw, bh, badge, ALIGN_CENTER | VALIGN_MIDDLE);
        }
    }

    // Boutons action (toujours à droite)
    int btn_h  = hh * 6 / 10;
    int btn_y  = hh * 2 / 10;
    int btn_w  = sw / 8;
    int gap    = mg / 2;

    int n_sel = (int)state->selected_ids.size();
    if (n_sel > 0) {
        // Mode sélection : remplace les boutons normaux
        int bx_annuler = sw - mg - btn_w;
        int bx_dl      = bx_annuler - btn_w - gap;
        char dl_lbl[32];
        snprintf(dl_lbl, sizeof(dl_lbl), "DL (%d)", n_sel);
        draw_button(bx_dl,      btn_y, btn_w, btn_h, dl_lbl,                    true);
        draw_button(bx_annuler, btn_y, btn_w, btn_h, tr("Annuler", "Cancel"), false);
        zones_add(bx_dl,      btn_y, btn_w, btn_h, ZONE_BTN_DOWNLOAD_SELECTED);
        zones_add(bx_annuler, btn_y, btn_w, btn_h, ZONE_BTN_CLEAR_SELECTION);
    } else {
        int btn_x2 = sw - mg - btn_w;
        int btn_x1 = btn_x2 - btn_w - gap;
        draw_button(btn_x1, btn_y, btn_w, btn_h, tr("Refresh",  "Refresh"),  false);
        draw_button(btn_x2, btn_y, btn_w, btn_h, tr("Reglages", "Settings"), false);
        zones_add(btn_x1, btn_y, btn_w, btn_h, ZONE_BTN_REFRESH);
        zones_add(btn_x2, btn_y, btn_w, btn_h, ZONE_BTN_SETTINGS);
    }

    // Ligne séparatrice
    DrawLine(mg, hh - 1, sw - mg, hh - 1, C_GRAY_MID);
}

// ─── Barre de filtres ─────────────────────────────────────────────────────────

static void draw_filter_bar(AppState* state)
{
    int sw  = SW;
    int mg  = MARGIN;
    int fy  = HEADER_H;
    int fh  = FILTER_BAR_H;
    int fw  = sw - 2 * mg;
    int fc  = fw / 4;

    const char* flabels[] = {
        tr("Tous", "All"), tr("Nouveaux", "New"),
        tr("Presents", "Present"), tr("Local", "Local")
    };
    int fzones[] = { ZONE_FILTER_ALL, ZONE_FILTER_NEW, ZONE_FILTER_SYNCED, ZONE_FILTER_LOCAL };

    int fcounts[4] = { 0, 0, 0, 0 };
    for (auto& b : state->catalog) {
        // Si vue dossier sélectionné, ne compter que ses livres
        if (!state->selected_folder.empty() && b.folder != state->selected_folder)
            continue;
        fcounts[0]++;
        if (b.status == BOOK_NEW || b.status == BOOK_UPDATED) fcounts[1]++;
        if (b.status == BOOK_SYNCED || b.status == BOOK_DOWNLOADING) fcounts[2]++;
        if (b.status == BOOK_LOCAL_ONLY) fcounts[3]++;
    }

    for (int i = 0; i < 4; ++i) {
        int fx = mg + i * fc;
        bool active = (state->filter == i);

        if (active) {
            FillArea(fx, fy, fc, fh, C_BLACK);
            SetFont(font(FONT_SUB, true), C_WHITE);
        } else {
            FillArea(fx, fy, fc, fh, C_GRAY_BG);
            SetFont(font(FONT_SUB, false), C_GRAY_DARK);
        }
        DrawLine(fx, fy, fx, fy + fh, C_GRAY_MID);

        char flbl[32];
        snprintf(flbl, sizeof(flbl), "%s (%d)", flabels[i], fcounts[i]);
        DrawTextRect(fx + 4, fy, fc - 8, fh, flbl, ALIGN_CENTER | VALIGN_MIDDLE);
        zones_add(fx, fy, fc, fh, fzones[i]);
    }
    DrawLine(mg, fy + fh, sw - mg, fy + fh, C_GRAY_MID);
}

// ─── Pied de page ─────────────────────────────────────────────────────────────

static void draw_footer(AppState* state)
{
    int sw = SW;
    int sh = SH;
    int mg = MARGIN;
    int fy = sh - FOOTER_H;

    DrawLine(mg, fy, sw - mg, fy, C_GRAY_MID);

    // Légende compacte
    SetFont(font(FONT_DETAIL, false), C_GRAY_MID);
    DrawTextRect(mg, fy + FOOTER_H / 6, sw - 2*mg, FOOTER_H / 3,
                 tr("N=Nouveau  M=Mis a jour  \xe2\x9c\x93=Present  ?=Local",
                    "N=New  M=Updated  \xe2\x9c\x93=Present  ?=Local only"),
                 ALIGN_LEFT);

    // Message état / erreur
    if (state->error_msg[0]) {
        SetFont(font(FONT_DETAIL, false), C_GRAY_DARK);
        DrawTextRect(mg, fy + FOOTER_H / 2, sw - 2*mg, FOOTER_H / 2,
                     state->error_msg, ALIGN_LEFT);
    } else if (state->status_msg[0] && state->catalog_loaded) {
        SetFont(font(FONT_DETAIL, false), C_GRAY_MID);
        DrawTextRect(mg, fy + FOOTER_H / 2, sw - 2*mg, FOOTER_H / 2,
                     state->status_msg, ALIGN_LEFT);
    }
}

// ─── Barre de scroll ──────────────────────────────────────────────────────────

static void draw_scrollbar(int list_top, int list_h, int list_bottom,
                           int total, int visible, int scroll)
{
    if (total <= visible) return;
    int sw = SW;
    int mg = MARGIN;
    int scw = SCROLL_W;
    int sx  = sw - mg - scw;
    int max_s = total - visible;

    FillArea(sx, list_top, scw, list_h, C_GRAY_BG);
    DrawLine(sx, list_top, sx, list_top + list_h, C_GRAY_LITE);

    int thumb_h = std::max(SW / 20, list_h * visible / total);
    int thumb_y = list_top + (max_s > 0 ? (list_h - thumb_h) * scroll / max_s : 0);
    FillArea(sx + 2, thumb_y, scw - 4, thumb_h, C_GRAY_DARK);

    // Flèches
    int arr_h = SW / 22;
    draw_button(sx, list_top,          scw, arr_h, "^", false);
    draw_button(sx, list_bottom-arr_h, scw, arr_h, "v", false);
    zones_add(sx, list_top,           scw, arr_h, ZONE_SCROLL_UP);
    zones_add(sx, list_bottom-arr_h,  scw, arr_h, ZONE_SCROLL_DOWN);
}

// ─── VUE DOSSIERS ─────────────────────────────────────────────────────────────

static void draw_screen_folders(AppConfig* cfg, AppState* state)
{
    int sw = SW;
    int sh = SH;
    int mg = MARGIN;

    ClearScreen();
    zones_clear();
    g_row_catalog_idx.clear();

    draw_header(state, false);
    draw_filter_bar(state);

    int list_top    = HEADER_H + FILTER_BAR_H + mg / 2;
    int list_bottom = sh - FOOTER_H;
    int list_h      = list_bottom - list_top;
    int list_w      = sw - 2 * mg - SCROLL_W - mg / 2;

    if (!state->catalog_loaded) {
        SetFont(font(FONT_SUB, false), C_GRAY_MID);
        DrawTextRect(mg, list_top + list_h / 3, sw - 2*mg, ROW_H,
                     tr("Appuyez sur Refresh pour charger la liste...",
                        "Press Refresh to load the library..."), ALIGN_CENTER);
        draw_footer(state);
        return;
    }

    // ── Panneau d'état (WiFi / Serveur / Local / Distant) ────────────────────
    {
        int info_h = SH / 18;
        int info_y = list_top;
        list_top  += info_h + mg / 4;
        list_h     = list_bottom - list_top;

        FillArea(mg, info_y, sw - 2*mg, info_h, C_GRAY_BG);
        draw_rect_border(mg, info_y, sw - 2*mg, info_h, C_GRAY_LITE);

        int col_w = (sw - 2*mg) / 4;
        SetFont(font(FONT_DETAIL, false), C_GRAY_DARK);

        // WiFi
        char buf_wifi[64];
        snprintf(buf_wifi, sizeof(buf_wifi), "%s: %s",
                 tr("Wifi", "WiFi"),
                 state->wifi_connected ? tr("Connecte", "Connected")
                                       : tr("Deconnecte", "Offline"));
        DrawTextRect(mg + col_w * 0 + mg/4, info_y, col_w - mg/2, info_h,
                     buf_wifi, ALIGN_LEFT | VALIGN_MIDDLE);

        // Serveur
        char buf_srv[64];
        snprintf(buf_srv, sizeof(buf_srv), "%s: %s",
                 tr("Serveur", "Server"),
                 state->server_connected ? tr("En ligne", "Online")
                                         : tr("Hors ligne", "Offline"));
        DrawTextRect(mg + col_w * 1 + mg/4, info_y, col_w - mg/2, info_h,
                     buf_srv, ALIGN_LEFT | VALIGN_MIDDLE);

        // Local
        char buf_local[64];
        snprintf(buf_local, sizeof(buf_local), "%s: %d",
                 tr("Local", "Local"), state->local_book_count);
        DrawTextRect(mg + col_w * 2 + mg/4, info_y, col_w - mg/2, info_h,
                     buf_local, ALIGN_LEFT | VALIGN_MIDDLE);

        // Distant
        char buf_remote[64];
        snprintf(buf_remote, sizeof(buf_remote), "%s: %d",
                 tr("Distant", "Remote"), state->total_remote);
        DrawTextRect(mg + col_w * 3 + mg/4, info_y, col_w - mg/2, info_h,
                     buf_remote, ALIGN_LEFT | VALIGN_MIDDLE);
    }

    // Collect dossiers et leurs stats
    struct FolderInfo {
        std::string name;
        int total, n_new, n_synced;
    };
    std::vector<FolderInfo> folders;
    std::map<std::string, size_t> folder_index;  // name -> index in folders

    // Livres sans dossier
    FolderInfo no_folder{ "(Sans dossier)", 0, 0, 0 };

    for (auto& b : state->catalog) {
        if (!book_matches_filter(b, state->filter)) continue;
        if (b.folder.empty()) {
            no_folder.total++;
            if (b.status == BOOK_NEW || b.status == BOOK_UPDATED) no_folder.n_new++;
            if (b.status == BOOK_SYNCED) no_folder.n_synced++;
        } else {
            // Get or create folder info
            size_t idx;
            if (folder_index.find(b.folder) == folder_index.end()) {
                idx = folders.size();
                folders.push_back({ b.folder, 0, 0, 0 });
                folder_index[b.folder] = idx;
            } else {
                idx = folder_index[b.folder];
            }
            
            folders[idx].total++;
            if (b.status == BOOK_NEW || b.status == BOOK_UPDATED) folders[idx].n_new++;
            if (b.status == BOOK_SYNCED) folders[idx].n_synced++;
        }
    }
    if (no_folder.total > 0) folders.push_back(no_folder);

    int n_folders = (int)folders.size();
    int fh        = FOLDER_H;
    int visible   = list_h / fh;
    state->list_visible = visible;

    int max_scroll = std::max(0, n_folders - visible);
    if (state->list_scroll > max_scroll) state->list_scroll = max_scroll;
    if (state->list_scroll < 0)         state->list_scroll = 0;

    if (n_folders == 0) {
        SetFont(font(FONT_SUB, false), C_GRAY_MID);
        DrawTextRect(mg, list_top + list_h / 3, sw - 2*mg, ROW_H,
                     "Aucun livre dans cette catégorie.", ALIGN_CENTER);
    } else {
        for (int i = 0; i < visible && (state->list_scroll + i) < n_folders; ++i) {
            auto& fi = folders[state->list_scroll + i];
            int ry = list_top + i * fh;

            // Fond alterné
            FillArea(mg, ry, list_w, fh,
                     (i % 2 == 0) ? C_WHITE : C_GRAY_BG);

            // Icone dossier : boite dessinée
            {
                int bx = mg + mg/2;
                int bh2 = fh * 5 / 8;
                int bw2 = SW / 16;
                int by2 = ry + (fh - bh2) / 2;
                FillArea(bx, by2, bw2, bh2, C_GRAY_LITE);
                draw_rect_border(bx, by2, bw2, bh2, C_GRAY_DARK);
            }

            // Nom du dossier
            SetFont(font(FONT_FOLDER, true), C_BLACK);
            DrawTextRect(mg + SW/10, ry, list_w / 2, fh,
                         fi.name.c_str(), ALIGN_LEFT | VALIGN_MIDDLE);

            // Bouton "⬇ Tout" (seulement si nouveaux à télécharger)
            int dl_btn_w = sw / 9;
            int dl_btn_h = fh * 6 / 10;
            int dl_btn_x = mg + list_w - dl_btn_w;
            int dl_btn_y = ry + (fh - dl_btn_h) / 2;
            int name_zone_w = list_w - dl_btn_w - mg / 2;

            if (fi.n_new > 0) {
                // Checkbox-style "☑ All" button (\xe2\x98\x91 = ☑)
                draw_button(dl_btn_x, dl_btn_y, dl_btn_w, dl_btn_h,
                            tr("\xe2\x98\x91 Tout", "\xe2\x98\x91 All"), true);
                zones_add(dl_btn_x, dl_btn_y, dl_btn_w, dl_btn_h,
                          ZONE_FOLDER_DOWNLOAD_BASE + (state->list_scroll + i));
            }

            // Stats
            char stats[64];
            if (fi.n_new > 0)
                snprintf(stats, sizeof(stats), "%d livre%s  •  %d nouveau%s",
                         fi.total, fi.total>1?"s":"",
                         fi.n_new, fi.n_new>1?"x":"");
            else
                snprintf(stats, sizeof(stats), "%d livre%s",
                         fi.total, fi.total>1?"s":"");

            int stats_w = name_zone_w / 2 - mg;
            int stats_x = mg + name_zone_w / 2;
            SetFont(font(FONT_DETAIL, false),
                    fi.n_new > 0 ? C_BLACK : C_GRAY_MID);
            DrawTextRect(stats_x, ry, stats_w, fh, stats, ALIGN_RIGHT | VALIGN_MIDDLE);

            // Badge nouveau à gauche
            if (fi.n_new > 0)
                FillArea(mg, ry + fh/4, BADGE_W, fh/2, C_BLACK);

            // Séparateur
            DrawLine(mg, ry + fh - 1, mg + list_w, ry + fh - 1, C_GRAY_LITE);

            int zone_id = ZONE_FOLDER_BASE + (state->list_scroll + i);
            zones_add(mg, ry, name_zone_w, fh, zone_id);
            g_row_catalog_idx.push_back(-(state->list_scroll + i + 1));
        }

        draw_scrollbar(list_top, list_h, list_bottom, n_folders, visible, state->list_scroll);
    }

    draw_footer(state);
}

// ─── VUE LIVRES D'UN DOSSIER ──────────────────────────────────────────────────

static void draw_screen_main(AppConfig* cfg, AppState* state)
{
    int sw = SW;
    int sh = SH;
    int mg = MARGIN;

    ClearScreen();
    zones_clear();
    g_row_catalog_idx.clear();

    bool in_folder = !state->selected_folder.empty();
    draw_header(state, in_folder);
    draw_filter_bar(state);

    int list_top    = HEADER_H + FILTER_BAR_H + mg / 2;
    int list_bottom = sh - FOOTER_H;
    int list_h      = list_bottom - list_top;
    int list_w      = sw - 2 * mg - SCROLL_W - mg / 2;

    // Bouton téléchargement : w environ 1/7 de la largeur liste
    int dl_btn_w = list_w / 7;

    if (!state->catalog_loaded) {
        SetFont(font(FONT_SUB, false), C_GRAY_MID);
        DrawTextRect(mg, list_top + list_h/3, sw - 2*mg, ROW_H,
                     "Appuyez sur Refresh pour charger la liste...", ALIGN_CENTER);
        draw_footer(state);
        return;
    }

    // Filtre catalogue
    std::vector<int> filtered_idx;
    for (int i = 0; i < (int)state->catalog.size(); ++i) {
        auto& b = state->catalog[i];
        if (in_folder && b.folder != state->selected_folder) continue;
        if (!in_folder && !b.folder.empty()) continue; // sans dossier seulement si pas de filtre
        if (!book_matches_filter(b, state->filter)) continue;
        filtered_idx.push_back(i);
    }

    // Si vue dossier sélectionné : prendre TOUS les livres du dossier filtrés
    if (in_folder) {
        filtered_idx.clear();
        for (int i = 0; i < (int)state->catalog.size(); ++i) {
            auto& b = state->catalog[i];
            if (b.folder != state->selected_folder) continue;
            if (!book_matches_filter(b, state->filter)) continue;
            filtered_idx.push_back(i);
        }
    }

    int total_f = (int)filtered_idx.size();
    int row_h   = ROW_H;
    int visible = list_h / row_h;
    state->list_visible = visible;

    int max_scroll = std::max(0, total_f - visible);
    if (state->list_scroll > max_scroll) state->list_scroll = max_scroll;
    if (state->list_scroll < 0)         state->list_scroll = 0;

    if (total_f == 0) {
        SetFont(font(FONT_SUB, false), C_GRAY_MID);
        DrawTextRect(mg, list_top + list_h/3, sw - 2*mg, row_h,
                     "Aucun livre dans cette catégorie.", ALIGN_CENTER);
    } else {
        for (int i = 0; i < visible && (state->list_scroll + i) < total_f; ++i) {
            int cat_idx = filtered_idx[state->list_scroll + i];
            const BookEntry* b = &state->catalog[cat_idx];
            int ry = list_top + i * row_h;

            // Case à cocher (books non synchronisés uniquement)
            int chk_w  = row_h * 2 / 3;
            int chk_h  = chk_w;
            int chk_x  = mg + (chk_w / 4);
            int chk_y  = ry + (row_h - chk_h) / 2;
            bool selectable = (b->status == BOOK_NEW || b->status == BOOK_UPDATED ||
                               b->status == BOOK_ERROR);
            bool selected   = selectable && state->selected_ids.count(b->jf_id) > 0;

            if (selectable) {
                FillArea(chk_x, chk_y, chk_w, chk_h, selected ? C_BLACK : C_WHITE);
                draw_rect_border(chk_x, chk_y, chk_w, chk_h, C_GRAY_DARK);
                if (selected) {
                    SetFont(font(FONT_DETAIL, true), C_WHITE);
                    DrawTextRect(chk_x, chk_y, chk_w, chk_h, "\xe2\x9c\x93",
                                 ALIGN_CENTER | VALIGN_MIDDLE);
                }
                zones_add(chk_x, chk_y, chk_w + chk_w/2, row_h,
                          ZONE_CHECKBOX_BASE + i);
            }

            int left_offset = chk_w + mg / 2;

            // Fond alterné
            FillArea(mg + left_offset, ry, list_w - left_offset, row_h,
                     selected ? C_GRAY_BG :
                     (i % 2 == 0) ? C_WHITE : C_GRAY_BG);

            // Séparateur bas
            DrawLine(mg, ry + row_h - 1, mg + list_w, ry + row_h - 1, C_GRAY_LITE);

            // Badge statut
            draw_status_badge(mg + left_offset, ry, row_h, b->status);

            int badge_area = BADGE_W + SW/18 + SW/60;
            int title_x    = mg + left_offset + badge_area;

            // Colonne droite : taille + bouton télécharger
            int right_w  = dl_btn_w + SW/20;
            int title_w  = list_w - left_offset - badge_area - right_w;

            // Titre
            bool is_new  = (b->status == BOOK_NEW || b->status == BOOK_UPDATED);
            SetFont(font(FONT_ROW, is_new), is_new ? C_BLACK : C_GRAY_DARK);
            DrawTextRect(title_x, ry, title_w, row_h,
                         b->name.c_str(), ALIGN_LEFT | VALIGN_MIDDLE);

            // Taille
            std::string sz;
            if (b->status == BOOK_SYNCED || b->status == BOOK_DOWNLOADING)
                sz = fmt_size(b->local_size);
            else if (b->remote_size > 0)
                sz = fmt_size(b->remote_size);

            if (!sz.empty()) {
                SetFont(font(FONT_DETAIL, false), C_GRAY_MID);
                DrawTextRect(title_x + title_w, ry, SW/20, row_h,
                             sz.c_str(), ALIGN_RIGHT | VALIGN_MIDDLE);
            }

            // Bouton Télécharger (seulement si livre pas encore présent)
            if (b->status == BOOK_NEW || b->status == BOOK_UPDATED ||
                b->status == BOOK_ERROR) {
                int dl_x = mg + list_w - dl_btn_w;
                int dl_y = ry + (row_h - row_h * 6/10) / 2;
                int dl_h = row_h * 6 / 10;
                draw_button(dl_x, dl_y, dl_btn_w, dl_h, "\xe2\xac\x87 DL", true);
                zones_add(dl_x, dl_y, dl_btn_w, dl_h, ZONE_ROW_BASE + i);
                g_row_catalog_idx.push_back(cat_idx);
            } else {
                g_row_catalog_idx.push_back(-1);
            }

            // Indicateur progression si en cours
            if (b->status == BOOK_DOWNLOADING &&
                state->download_book_idx == cat_idx) {
                int bar_x = title_x;
                int bar_y = ry + row_h - row_h/5 - 2;
                int bar_w = title_w + SW/20;
                int bar_h = row_h / 5;
                draw_progress_bar(bar_x, bar_y, bar_w, bar_h, state->download_progress);
            }
        }

        draw_scrollbar(list_top, list_h, list_bottom, total_f, visible, state->list_scroll);
    }

    draw_footer(state);
}

// ─── SCREEN_SYNCING ───────────────────────────────────────────────────────────

static void draw_screen_syncing(AppConfig* cfg, AppState* state)
{
    int sw = SW;
    int sh = SH;
    int mg = MARGIN;
    int cy = sh / 2;

    ClearScreen();

    const char* title =
        (state->bg_mode == BG_CATALOG)           ? tr("Chargement du catalogue...", "Loading library...") :
        (state->bg_mode == BG_DOWNLOAD_ONE)      ? tr("Telechargement...", "Downloading...") :
        (state->bg_mode == BG_DOWNLOAD_FOLDER)   ? tr("Telechargement du dossier...", "Downloading folder...") :
        (state->bg_mode == BG_DOWNLOAD_SELECTED) ? tr("Telechargement de la selection...", "Downloading selection...") :
                                                    tr("Synchronisation en cours...", "Syncing...");

    SetFont(font(SW / 16, true), C_BLACK);
    DrawTextRect(0, cy - sh/6, sw, sw/14, title, ALIGN_CENTER);

    int bw = sw - 4 * mg;
    int bx = 2 * mg;
    int by = cy - sw/28;
    int bh = sw / 36;
    draw_progress_bar(bx, by, bw, bh, state->progress);

    // Pourcentage + nom du livre en cours
    char pct_line[64];
    if (!state->dl_book_name.empty())
        snprintf(pct_line, sizeof(pct_line), "%d %%", state->progress);
    else
        snprintf(pct_line, sizeof(pct_line), "%d %%", state->progress);
    SetFont(font(SW / 18, true), C_BLACK);
    DrawTextRect(0, by + bh + mg, sw, sw/18, pct_line, ALIGN_CENTER);

    // Nom du livre en cours (ligne principale)
    if (!state->dl_book_name.empty()) {
        SetFont(font(FONT_SUB, true), C_BLACK);
        DrawTextRect(mg, by + bh + sw/18 + mg, sw - 2*mg, sw/16,
                     state->dl_book_name.c_str(), ALIGN_CENTER);
    }

    // Message d'état
    SetFont(font(FONT_DETAIL, false), C_GRAY_DARK);
    int msg_y = by + bh + sw/18 + mg + (state->dl_book_name.empty() ? 0 : sw/16 + mg/2);
    DrawTextRect(mg, msg_y, sw - 2*mg, sw/18,
                 state->status_msg, ALIGN_CENTER);

    // Taille téléchargée
    if (state->dl_bytes_now > 0) {
        char size_line[64];
        if (state->dl_bytes_total > 0)
            snprintf(size_line, sizeof(size_line), "%.1f Mo / %.1f Mo",
                     state->dl_bytes_now / 1048576.0,
                     state->dl_bytes_total / 1048576.0);
        else
            snprintf(size_line, sizeof(size_line), "%.1f Mo reçus...",
                     state->dl_bytes_now / 1048576.0);
        SetFont(font(FONT_DETAIL, false), C_GRAY_MID);
        DrawTextRect(mg, msg_y + sw/18 + mg/2, sw - 2*mg, sw/22,
                     size_line, ALIGN_CENTER);
    }

    // Compteurs globaux
    if ((state->bg_mode == BG_SYNC || state->bg_mode == BG_DOWNLOAD_FOLDER ||
         state->bg_mode == BG_DOWNLOAD_SELECTED) && state->downloaded + state->skipped > 0) {
        char cnt[128];
        snprintf(cnt, sizeof(cnt), "%d téléchargé(s)", state->downloaded);
        SetFont(font(FONT_DETAIL, false), C_GRAY_MID);
        DrawTextRect(mg, cy + sh/8, sw - 2*mg, sw/22, cnt, ALIGN_CENTER);
    }
}

// ─── SCREEN_SETUP ─────────────────────────────────────────────────────────────

static void draw_field(int x, int y, int w, int h,
                       const char* label, const char* value,
                       bool is_password = false)
{
    int sw = SW;
    int label_w = w / 3;
    SetFont(font(FONT_SUB, false), C_GRAY_DARK);
    DrawTextRect(x, y, label_w, h, label, ALIGN_LEFT | VALIGN_MIDDLE);

    int fx = x + label_w;
    int fw = w - label_w;
    int pad = h / 8;
    FillArea(fx, y + pad, fw, h - 2*pad, C_WHITE);
    draw_rect_border(fx, y + pad, fw, h - 2*pad, C_GRAY_MID);

    char disp[128];
    if (is_password && value[0]) { memset(disp, '*', strlen(value)); disp[strlen(value)] = 0; }
    else strncpy(disp, value[0] ? value : "Appuyer pour saisir...", 127);

    int color = (!value[0]) ? C_GRAY_MID : C_BLACK;
    SetFont(font(FONT_SUB, false), color);
    DrawTextRect(fx + pad*2, y + pad, fw - 4*pad, h - 2*pad, disp,
                 ALIGN_LEFT | VALIGN_MIDDLE);
}

static void draw_screen_setup(AppConfig* cfg, AppState* state)
{
    log_write("draw_screen_setup: start\n");
    
    int sw = SW;
    int sh = SH;
    int mg = MARGIN;
    log_write("draw_screen_setup: SW=%d, SH=%d, MARGIN=%d\n", sw, sh, mg);
    
    ClearScreen();
    log_write("draw_screen_setup: ClearScreen done\n");
    
    zones_clear();
    log_write("draw_screen_setup: zones_clear done\n");

    SetFont(font(FONT_TITLE, true), C_BLACK);
    log_write("draw_screen_setup: title font set\n");
    
    DrawTextRect(mg, mg, sw - 2*mg, HEADER_H, tr("Configuration", "Settings"), ALIGN_LEFT | VALIGN_MIDDLE);
    log_write("draw_screen_setup: title drawn\n");
    
    DrawLine(mg, mg + HEADER_H, sw - mg, mg + HEADER_H, C_GRAY_MID);
    log_write("draw_screen_setup: line drawn\n");

    int y  = mg + HEADER_H + mg;
    int fh = SH / 14;
    int fw = sw - 2 * mg;

    draw_field(mg, y, fw, fh, tr("Serveur", "Server"), cfg->server_url);
    zones_add(mg + fw/3, y, fw*2/3, fh, ZONE_URL);
    y += fh + mg;

    SetFont(font(FONT_DETAIL, false), C_GRAY_MID);
    DrawTextRect(mg, y, fw, fh/2, tr("─── Authentification ───", "─── Authentication ───"), ALIGN_LEFT);
    y += fh/2 + mg/2;

    draw_field(mg, y, fw, fh, tr("Utilisateur", "Username"), cfg->username);
    zones_add(mg + fw/3, y, fw*2/3, fh, ZONE_USER);
    y += fh + mg;

    draw_field(mg, y, fw, fh, tr("Mot de passe", "Password"), cfg->password, true);
    zones_add(mg + fw/3, y, fw*2/3, fh, ZONE_PASS);
    y += fh + mg;

    SetFont(font(FONT_DETAIL, false), C_GRAY_MID);
    DrawTextRect(mg, y, fw, fh/2, tr("─── OU cle API (prioritaire) ───", "─── OR API key (takes priority) ───"), ALIGN_LEFT);
    y += fh/2 + mg/2;

    draw_field(mg, y, fw, fh, tr("Cle API", "API Key"), cfg->api_key, true);
    zones_add(mg + fw/3, y, fw*2/3, fh, ZONE_APIKEY);
    y += fh + mg;

    // Toggle suppression locale
    int tog_h = fh * 3/4;
    char del_lbl[96];
    snprintf(del_lbl, sizeof(del_lbl), "[%s]  %s",
             cfg->delete_local ? "X" : "  ",
             tr("Supprimer les livres retires du serveur",
                "Delete books removed from server"));
    FillArea(mg, y, fw, tog_h, cfg->delete_local ? C_GRAY_BG : C_WHITE);
    draw_rect_border(mg, y, fw, tog_h, C_GRAY_MID);
    SetFont(font(FONT_SUB, false), C_BLACK);
    DrawTextRect(mg + mg/2, y, fw - mg, tog_h, del_lbl, ALIGN_LEFT | VALIGN_MIDDLE);
    zones_add(mg, y, fw, tog_h, ZONE_DELETE_TOG);
    y += tog_h + mg;

    // Toggle langue
    char lang_lbl[64];
    snprintf(lang_lbl, sizeof(lang_lbl), "Language: [%s]",
             cfg->lang == 1 ? "English" : "Francais");
    FillArea(mg, y, fw, tog_h, C_WHITE);
    draw_rect_border(mg, y, fw, tog_h, C_GRAY_MID);
    SetFont(font(FONT_SUB, false), C_BLACK);
    DrawTextRect(mg + mg/2, y, fw - mg, tog_h, lang_lbl, ALIGN_LEFT | VALIGN_MIDDLE);
    zones_add(mg, y, fw, tog_h, ZONE_LANG_TOG);
    y += tog_h + mg;

    int btn_w = (fw - mg) / 2;
    int btn_y = sh - mg - fh;
    draw_button(mg,            btn_y, btn_w, fh, tr("Annuler",     "Cancel"), false);
    draw_button(mg + btn_w + mg, btn_y, btn_w, fh, tr("Enregistrer", "Save"),   true);
    zones_add(mg,            btn_y, btn_w, fh, ZONE_BACK_SETUP);
    zones_add(mg + btn_w + mg, btn_y, btn_w, fh, ZONE_SAVE);
    
    log_write("draw_screen_setup: completed\n");
}

// ─── API publique ─────────────────────────────────────────────────────────────

static void ui_init(AppConfig* cfg, AppState* state)
{
    log_write("ui_init: start\n");
    g_ui_cfg   = cfg;
    g_ui_state = state;
    log_write("ui_init: globals set\n");
    
    state->selected_folder = "";
    log_write("ui_init: selected_folder cleared\n");
    
    state->download_book_idx = -1;
    state->download_progress = 0;
    log_write("ui_init: download state cleared\n");
}

static void ui_draw(AppConfig* cfg, AppState* state)
{
    extern void log_write(const char* fmt, ...);
    log_write("ui_draw: screen=%d, syncing=%d\n", state->screen, state->syncing);
    
    if (!check_screen_dims()) {
        log_write("ui_draw: invalid screen dims, skipping draw\n");
        return;
    }
    
    Screen scr = state->screen;
    if (state->syncing) scr = SCREEN_SYNCING;

    switch (scr) {
        case SCREEN_SETUP: 
            log_write("ui_draw: calling draw_screen_setup\n");
            draw_screen_setup(cfg, state); 
            log_write("ui_draw: draw_screen_setup done\n");
            break;
        case SCREEN_SYNCING: 
            log_write("ui_draw: calling draw_screen_syncing\n");
            draw_screen_syncing(cfg, state); 
            log_write("ui_draw: draw_screen_syncing done\n");
            break;
        case SCREEN_MAIN:
            // Si aucun dossier sélectionné : vue dossiers
            // Si dossier sélectionné : vue liste du dossier
            if (state->selected_folder.empty()) {
                log_write("ui_draw: calling draw_screen_folders\n");
                draw_screen_folders(cfg, state);
                log_write("ui_draw: draw_screen_folders done\n");
            } else {
                log_write("ui_draw: calling draw_screen_main\n");
                draw_screen_main(cfg, state);
                log_write("ui_draw: draw_screen_main done\n");
            }
            break;
    }

    log_write("ui_draw: calling FullUpdate\n");
    FullUpdate();
    log_write("ui_draw: FullUpdate done\n");
}

static void ui_draw_partial(AppConfig* cfg, AppState* state)
{
    if (state->syncing)
        draw_screen_syncing(cfg, state);
    else if (state->screen == SCREEN_MAIN) {
        if (state->selected_folder.empty())
            draw_screen_folders(cfg, state);
        else
            draw_screen_main(cfg, state);
    }
    PartialUpdate(0, 0, SW, SH);
}

// ─── Clavier natif PocketBook ─────────────────────────────────────────────────

static void keyboard_cb(char* text)
{
    if (!text || !g_kb_req.target) return;
    strncpy(g_kb_req.target, text, g_kb_req.max_len - 1);
    ui_draw(g_ui_cfg, g_ui_state);
}

static void open_keyboard(char* target, int max_len,
                           const char* title, bool password = false)
{
    g_kb_req = { target, max_len, title, password };
    OpenKeyboard(title, target, max_len,
                 password ? KBD_PASSWORD : KBD_NORMAL, keyboard_cb);
}

// ─── Gestion des taps ────────────────────────────────────────────────────────

static void ui_handle_tap(int px, int py,
                          AppConfig* cfg, AppState* state,
                          std::function<void()> on_refresh,
                          std::function<void()> on_sync,
                          std::function<void()> on_close,
                          std::function<void(int)> on_download_one,
                          std::function<void(const std::string&)> on_download_folder,
                          std::function<void()> on_download_selected)
{
    int zone = zones_hit(px, py);
    if (zone < 0) return;

    // ── Cases à cocher (toggle sélection) ─────────────────────────────────
    if (zone >= ZONE_CHECKBOX_BASE && zone < ZONE_CHECKBOX_BASE + 200) {
        int row_i = zone - ZONE_CHECKBOX_BASE;
        if (row_i < (int)g_row_catalog_idx.size()) {
            int cat_idx = g_row_catalog_idx[row_i];
            if (cat_idx >= 0) {
                auto& entry = state->catalog[cat_idx];
                if (entry.status == BOOK_NEW || entry.status == BOOK_UPDATED ||
                    entry.status == BOOK_ERROR) {
                    auto it = state->selected_ids.find(entry.jf_id);
                    if (it != state->selected_ids.end())
                        state->selected_ids.erase(it);
                    else
                        state->selected_ids.insert(entry.jf_id);
                    ui_draw(cfg, state);
                }
            }
        }
        return;
    }

    // ── Boutons téléchargement individuel (lignes) ─────────────────────────
    if (zone >= ZONE_ROW_BASE && zone < ZONE_ROW_BASE + 200) {
        int row_i = zone - ZONE_ROW_BASE;
        if (row_i < (int)g_row_catalog_idx.size()) {
            int cat_idx = g_row_catalog_idx[row_i];
            if (cat_idx >= 0)
                on_download_one(cat_idx);
        }
        return;
    }

    // ── Bouton "Télécharger tout" par dossier ──────────────────────────────
    if (zone >= ZONE_FOLDER_DOWNLOAD_BASE && zone < ZONE_FOLDER_DOWNLOAD_BASE + 50) {
        std::vector<std::string> folder_names;
        std::set<std::string> seen;
        bool has_no_folder = false;
        for (auto& b : state->catalog) {
            if (!book_matches_filter(b, state->filter)) continue;
            if (b.folder.empty()) has_no_folder = true;
            else if (!seen.count(b.folder)) { seen.insert(b.folder); folder_names.push_back(b.folder); }
        }
        if (has_no_folder) folder_names.push_back("(Sans dossier)");
        int fi = zone - ZONE_FOLDER_DOWNLOAD_BASE;
        if (fi < (int)folder_names.size())
            on_download_folder(folder_names[fi]);
        return;
    }

    // ── Tap sur un dossier (vue dossiers) ─────────────────────────────────
    if (zone >= ZONE_FOLDER_BASE && zone < ZONE_FOLDER_BASE + 50) {
        std::vector<std::string> folder_names;
        std::set<std::string> seen;
        bool has_no_folder = false;
        for (auto& b : state->catalog) {
            if (!book_matches_filter(b, state->filter)) continue;
            if (b.folder.empty()) has_no_folder = true;
            else if (!seen.count(b.folder)) { seen.insert(b.folder); folder_names.push_back(b.folder); }
        }
        if (has_no_folder) folder_names.push_back("(Sans dossier)");
        int fi = zone - ZONE_FOLDER_BASE;
        if (fi < (int)folder_names.size()) {
            state->selected_folder = folder_names[fi];
            state->list_scroll     = 0;
            ui_draw(cfg, state);
        }
        return;
    }

    switch (zone) {
        // ── Bouton retour ─────────────────────────────────────────────────
        case ZONE_BTN_BACK:
            state->selected_folder = "";
            state->list_scroll     = 0;
            ui_draw(cfg, state);
            break;

        // ── Boutons principaux ────────────────────────────────────────────
        case ZONE_BTN_REFRESH:
            state->error_msg[0] = 0;
            on_refresh();
            break;

        case ZONE_BTN_SYNC:
            state->error_msg[0] = 0;
            on_sync();
            break;

        case ZONE_BTN_DOWNLOAD_SELECTED:
            on_download_selected();
            break;

        case ZONE_BTN_CLEAR_SELECTION:
            state->selected_ids.clear();
            ui_draw(cfg, state);
            break;

        case ZONE_BTN_SETTINGS:
            state->screen = SCREEN_SETUP;
            ui_draw(cfg, state);
            break;

        // ── Filtres ───────────────────────────────────────────────────────
        case ZONE_FILTER_ALL:    state->filter = 0; state->list_scroll = 0; ui_draw(cfg, state); break;
        case ZONE_FILTER_NEW:    state->filter = 1; state->list_scroll = 0; ui_draw(cfg, state); break;
        case ZONE_FILTER_SYNCED: state->filter = 2; state->list_scroll = 0; ui_draw(cfg, state); break;
        case ZONE_FILTER_LOCAL:  state->filter = 3; state->list_scroll = 0; ui_draw(cfg, state); break;

        // ── Scroll ────────────────────────────────────────────────────────
        case ZONE_SCROLL_UP:
            if (state->list_scroll > 0) {
                state->list_scroll -= std::max(1, state->list_visible / 2);
                if (state->list_scroll < 0) state->list_scroll = 0;
                ui_draw(cfg, state);
            }
            break;

        case ZONE_SCROLL_DOWN: {
            int total = get_filtered_count(state);
            int max_s = std::max(0, total - state->list_visible);
            if (state->list_scroll < max_s) {
                state->list_scroll += std::max(1, state->list_visible / 2);
                if (state->list_scroll > max_s) state->list_scroll = max_s;
                ui_draw(cfg, state);
            }
            break;
        }

        // ── Setup ─────────────────────────────────────────────────────────
        case ZONE_URL:
            open_keyboard(cfg->server_url, sizeof(cfg->server_url),
                          "URL Jellyfin (ex: http://192.168.1.10:8096)");
            break;
        case ZONE_USER:
            open_keyboard(cfg->username, sizeof(cfg->username), "Nom d'utilisateur");
            break;
        case ZONE_PASS:
            open_keyboard(cfg->password, sizeof(cfg->password), "Mot de passe", true);
            break;
        case ZONE_APIKEY:
            open_keyboard(cfg->api_key, sizeof(cfg->api_key), "Clé API", true);
            break;
        case ZONE_DELETE_TOG:
            cfg->delete_local = !cfg->delete_local;
            ui_draw(cfg, state);
            break;
        case ZONE_LANG_TOG:
            cfg->lang = (cfg->lang == 0) ? 1 : 0;
            ui_draw(cfg, state);
            break;
        case ZONE_SAVE:
            config_save(CONFIG_FILE, cfg);
            state->screen = SCREEN_MAIN;
            ui_draw(cfg, state);
            break;
        case ZONE_BACK_SETUP:
            state->screen = SCREEN_MAIN;
            ui_draw(cfg, state);
            break;
    }
}
