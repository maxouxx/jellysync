/**
 * JellySync v3 — Synchroniseur Jellyfin pour Vivlio Inkpad 3
 *
 * Nouveautés v3 :
 *   • Affichage par dossiers (groupement automatique depuis le chemin serveur)
 *   • Téléchargement individuel d'un livre (bouton "↓ DL" sur chaque ligne)
 *   • Interface adaptive : toutes les dimensions sont calculées depuis
 *     ScreenWidth() / ScreenHeight() au moment du dessin
 */

#include "inkview_compat.h"
#include <inkview.h>
#include <curl/curl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <algorithm>
#include <cstdarg>
#include <ctime>

// ─── Logging ──────────────────────────────────────────────────────────────────
static FILE* g_log_file = NULL;

static void log_init()
{
    g_log_file = fopen("/mnt/ext1/jellysync.log", "a");
    if (!g_log_file) {
        g_log_file = fopen("/tmp/jellysync.log", "a");
    }
    if (g_log_file) {
        time_t now = time(NULL);
        fprintf(g_log_file, "\n--- JellySync started at %s ---\n", ctime(&now));
        fflush(g_log_file);
    }
}

static void log_write(const char* fmt, ...)
{
    if (!g_log_file) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);
    fflush(g_log_file);
}

static void log_close()
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

// ── cJSON : implémentation incluse UNE SEULE FOIS dans cJSON.cpp ──────────────
#include "cJSON.h"

#include "config.h"
#include "jellyfin_api.h"
#include "sync.h"
#include "ui.h"

// ─── État global ──────────────────────────────────────────────────────────────
static AppConfig  g_config;
static AppState   g_state;
static pthread_t  g_thread;

// ─── Prototypes ───────────────────────────────────────────────────────────────
static int  main_handler(int event, int par1, int par2);
static void start_background(BgMode mode);
static void start_download_one(int catalog_idx);
static void start_download_folder(const std::string& folder);
static void start_download_selected();
static void* bg_thread_func(void* arg);

// ─── Point d'entrée ──────────────────────────────────────────────────────────
int main(void)
{
    log_init();
    log_write("main() started\n");
    
    curl_global_init(CURL_GLOBAL_ALL);
    log_write("curl_global_init done\n");
    
    mkdir(FLASHDIR "/books", 0755);
    mkdir(BOOKS_DIR, 0755);
    log_write("directories created\n");

    config_load(CONFIG_FILE, &g_config);
    log_write("config loaded: server_url='%s'\n", g_config.server_url);

    // Use placement new to properly construct C++ STL objects in AppState
    new (&g_state) AppState();
    log_write("AppState constructed with placement new\n");
    
    g_state.screen           = g_config.server_url[0] ? SCREEN_MAIN : SCREEN_SETUP;
    g_state.catalog_loaded   = false;
    g_state.filter           = 0;
    g_state.download_book_idx = -1;
    g_state.download_progress = 0;
    g_state.dl_bytes_now      = 0;
    g_state.dl_bytes_total    = 0;
    g_state.wifi_connected    = false;
    g_state.server_connected  = false;
    g_state.local_book_count  = 0;
    log_write("state initialized, screen=%d\n", g_state.screen);

    InkViewMain(main_handler);
    log_write("InkViewMain returned\n");

    cleanup_fonts();
    curl_global_cleanup();
    log_close();
    return 0;
}

// ─── Gestionnaire d'événements ────────────────────────────────────────────────
static int main_handler(int event, int par1, int par2)
{
    log_write("main_handler: event=%d, par1=%d, par2=%d\n", event, par1, par2);
    
    switch (event)
    {
        case EVT_INIT:
            log_write("EVT_INIT: calling ui_init\n");
            ui_init(&g_config, &g_state);
            log_write("EVT_INIT: ui_init done, calling ui_draw\n");
            ui_draw(&g_config, &g_state);
            log_write("EVT_INIT: ui_draw done\n");
            if (g_config.server_url[0]) {
                log_write("EVT_INIT: starting background catalog\n");
                start_background(BG_CATALOG);
            }
            log_write("EVT_INIT: completed\n");
            return 1;

        case EVT_SHOW:
        case EVT_REPAINT:
            log_write("EVT_SHOW/REPAINT: calling ui_draw\n");
            ui_draw(&g_config, &g_state);
            log_write("EVT_SHOW/REPAINT: ui_draw done\n");
            return 1;

        case EVT_POINTERUP:
            ui_handle_tap(par1, par2, &g_config, &g_state,
                []{ start_background(BG_CATALOG); },
                []{ start_background(BG_SYNC);    },
                []{ CloseApp();                   },
                [](int idx){ start_download_one(idx); },
                [](const std::string& f){ start_download_folder(f); },
                []{ start_download_selected(); });
            return 1;

        case EVT_KEYPRESS:
            if (par1 == KEY_POWER || par1 == KEY_BACK) {
                if (!g_state.syncing) {
                    // Retour à la vue dossiers si dans un dossier
                    if (!g_state.selected_folder.empty()) {
                        g_state.selected_folder = "";
                        g_state.list_scroll     = 0;
                        ui_draw(&g_config, &g_state);
                    } else {
                        CloseApp();
                    }
                }
            }
            if (par1 == KEY_PREV) {
                if (g_state.list_scroll > 0) {
                    g_state.list_scroll -= std::max(1, g_state.list_visible);
                    if (g_state.list_scroll < 0) g_state.list_scroll = 0;
                    ui_draw(&g_config, &g_state);
                }
            }
            if (par1 == KEY_NEXT) {
                int total = get_filtered_count(&g_state);
                int max_s = std::max(0, total - g_state.list_visible);
                if (g_state.list_scroll < max_s) {
                    g_state.list_scroll += std::max(1, g_state.list_visible);
                    if (g_state.list_scroll > max_s) g_state.list_scroll = max_s;
                    ui_draw(&g_config, &g_state);
                }
            }
            return 1;

        case EVT_CUSTOM:
            if (par1 == MSG_SYNC_PROGRESS || par1 == MSG_DOWNLOAD_PROGRESS) {
                g_state.progress         = par2;
                g_state.download_progress = par2;
                ui_draw_partial(&g_config, &g_state);
            }
            else if (par1 == MSG_CATALOG_READY || par1 == MSG_SYNC_DONE ||
                     par1 == MSG_DOWNLOAD_DONE) {
                g_state.syncing  = false;
                g_state.progress = 100;
                if (par1 == MSG_DOWNLOAD_DONE || par1 == MSG_SYNC_DONE) {
                    g_state.selected_folder = "";
                    g_state.list_scroll     = 0;
                }
                pthread_join(g_thread, nullptr);
                ui_draw(&g_config, &g_state);
            }
            else if (par1 == MSG_CATALOG_ERROR || par1 == MSG_SYNC_ERROR ||
                     par1 == MSG_DOWNLOAD_ERROR) {
                g_state.syncing = false;
                pthread_join(g_thread, nullptr);
                ui_draw(&g_config, &g_state);
            }
            return 1;
    }
    return 0;
}

// ─── Thread de fond ───────────────────────────────────────────────────────────
static void start_background(BgMode mode)
{
    log_write("start_background: mode=%d\n", mode);
    
    if (g_state.syncing) {
        log_write("start_background: already syncing, ignoring\n");
        return;
    }
    config_save(CONFIG_FILE, &g_config);
    log_write("start_background: config saved\n");
    
    g_state.syncing      = true;
    g_state.bg_mode      = mode;
    g_state.progress     = 0;
    g_state.error_msg[0] = 0;
    snprintf(g_state.status_msg, sizeof(g_state.status_msg),
             mode == BG_CATALOG ? "Chargement..." : "Connexion...");
    log_write("start_background: state updated, status='%s'\n", g_state.status_msg);
    
    ui_draw(&g_config, &g_state);
    log_write("start_background: ui_draw done\n");
    
    pthread_create(&g_thread, nullptr, bg_thread_func, (void*)(intptr_t)mode);
    log_write("start_background: thread created\n");
}

static void start_download_one(int catalog_idx)
{
    if (g_state.syncing) return;
    if (catalog_idx < 0 || catalog_idx >= (int)g_state.catalog.size()) return;

    g_state.syncing           = true;
    g_state.bg_mode           = BG_DOWNLOAD_ONE;
    g_state.progress          = 0;
    g_state.download_progress = 0;
    g_state.download_book_idx = catalog_idx;
    g_state.error_msg[0]      = 0;

    const std::string& name = g_state.catalog[catalog_idx].name;
    snprintf(g_state.status_msg, sizeof(g_state.status_msg),
             "Préparation : %s", name.substr(0, 40).c_str());
    ui_draw(&g_config, &g_state);
    pthread_create(&g_thread, nullptr, bg_thread_func, (void*)(intptr_t)BG_DOWNLOAD_ONE);
}

static void start_download_folder(const std::string& folder)
{
    if (g_state.syncing) return;
    g_state.download_folder_name = folder;
    g_state.syncing      = true;
    g_state.bg_mode      = BG_DOWNLOAD_FOLDER;
    g_state.progress     = 0;
    g_state.error_msg[0] = 0;
    snprintf(g_state.status_msg, sizeof(g_state.status_msg),
             "Préparation : %s", folder.substr(0, 40).c_str());
    config_save(CONFIG_FILE, &g_config);
    ui_draw(&g_config, &g_state);
    pthread_create(&g_thread, nullptr, bg_thread_func, (void*)(intptr_t)BG_DOWNLOAD_FOLDER);
}

static void start_download_selected()
{
    if (g_state.syncing || g_state.selected_ids.empty()) return;
    g_state.syncing      = true;
    g_state.bg_mode      = BG_DOWNLOAD_SELECTED;
    g_state.progress     = 0;
    g_state.error_msg[0] = 0;
    snprintf(g_state.status_msg, sizeof(g_state.status_msg),
             "Téléchargement de %d livre(s)...", (int)g_state.selected_ids.size());
    config_save(CONFIG_FILE, &g_config);
    ui_draw(&g_config, &g_state);
    pthread_create(&g_thread, nullptr, bg_thread_func, (void*)(intptr_t)BG_DOWNLOAD_SELECTED);
}

static void* bg_thread_func(void* arg)
{
    BgMode mode = (BgMode)(intptr_t)arg;
    SyncContext ctx{ &g_config, &g_state, mode };
    int result = sync_run(&ctx);

    int ok_msg, err_msg;
    switch (mode) {
        case BG_CATALOG:
            ok_msg  = MSG_CATALOG_READY;
            err_msg = MSG_CATALOG_ERROR;
            break;
        case BG_DOWNLOAD_ONE:
            ok_msg  = MSG_DOWNLOAD_DONE;
            err_msg = MSG_DOWNLOAD_ERROR;
            break;
        case BG_DOWNLOAD_FOLDER:
        case BG_DOWNLOAD_SELECTED:
            ok_msg  = MSG_SYNC_DONE;
            err_msg = MSG_SYNC_ERROR;
            break;
        default: // BG_SYNC
            ok_msg  = MSG_SYNC_DONE;
            err_msg = MSG_SYNC_ERROR;
            break;
    }

    SendEvent(GetCurrentTask(), EVT_CUSTOM,
              result == SYNC_OK ? ok_msg : err_msg, 0);
    return nullptr;
}
