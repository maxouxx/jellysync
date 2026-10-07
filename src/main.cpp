/**
 * JellySync — Client Jellyfin pour Vivlio InkPad 3 / PocketBook
 *
 * Parcourt la bibliothèque de livres d'un serveur Jellyfin et télécharge
 * à la demande les livres choisis (un livre, une sélection ou un dossier).
 * Rien n'est jamais supprimé de la liseuse.
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
#include <deque>
#include <string>
#include <vector>

// ─── Journal ──────────────────────────────────────────────────────────────────
// Écrit dans /mnt/ext1/jellysync.log et garde les dernières lignes en mémoire
// pour le panneau « Journal détaillé » affiché pendant les téléchargements.
static FILE*                   g_log_file = NULL;
static pthread_mutex_t         g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::deque<std::string> g_log_lines;
static std::string             g_log_pending;
static unsigned                g_log_seq = 0;
static const size_t            LOG_KEEP  = 64;

static void log_init()
{
    g_log_file = fopen(FLASHDIR "/jellysync.log", "a");
    if (!g_log_file) g_log_file = fopen("/tmp/jellysync.log", "a");
    if (g_log_file) {
        time_t now = time(NULL);
        fprintf(g_log_file, "\n--- JellySync démarré le %s", ctime(&now));
        fflush(g_log_file);
    }
}

static void log_write(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) { fputs(buf, g_log_file); fflush(g_log_file); }
    for (const char* p = buf; *p; ++p) {
        if (*p != '\n') { g_log_pending += *p; continue; }
        time_t now = time(NULL);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M:%S ", localtime(&now));
        g_log_lines.push_back(ts + g_log_pending);
        if (g_log_lines.size() > LOG_KEEP) g_log_lines.pop_front();
        g_log_pending.clear();
        g_log_seq++;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

static unsigned log_tail(std::vector<std::string>& out, int max_lines)
{
    pthread_mutex_lock(&g_log_mutex);
    out.clear();
    int n = std::min((int)g_log_lines.size(), max_lines);
    for (int i = (int)g_log_lines.size() - n; i < (int)g_log_lines.size(); ++i)
        out.push_back(g_log_lines[i]);
    unsigned seq = g_log_seq;
    pthread_mutex_unlock(&g_log_mutex);
    return seq;
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
    curl_global_init(CURL_GLOBAL_ALL);

    mkdir(FLASHDIR "/books", 0755);
    mkdir(BOOKS_DIR, 0755);

    config_load(CONFIG_FILE, &g_config);
    log_write("JellySync %s, serveur : %s\n", APP_VERSION,
              g_config.server_url[0] ? g_config.server_url : "(aucun)");

    // Placement new : AppState contient des objets STL
    new (&g_state) AppState();
    g_state.screen            = g_config.server_url[0] ? SCREEN_MAIN : SCREEN_SETUP;
    g_state.download_book_idx = -1;

    InkViewMain(main_handler);

    cleanup_fonts();
    curl_global_cleanup();
    log_close();
    return 0;
}

// ─── Gestionnaire d'événements ────────────────────────────────────────────────
static int main_handler(int event, int par1, int par2)
{
    switch (event)
    {
        case EVT_INIT:
            ui_init(&g_config, &g_state);
            if (g_state.screen == SCREEN_SETUP) g_cfg_backup = g_config;
            ui_draw(&g_config, &g_state);
            if (g_config.server_url[0])
                start_background(BG_CATALOG);
            return 1;

        case EVT_SHOW:
        case EVT_REPAINT:
            ui_draw(&g_config, &g_state);
            return 1;

        case EVT_POINTERUP:
            ui_handle_tap(par1, par2, &g_config, &g_state,
                []{ start_background(BG_CATALOG); },
                [](int idx){ start_download_one(idx); },
                [](const std::string& f){ start_download_folder(f); },
                []{ start_download_selected(); });
            return 1;

        case EVT_KEYPRESS:
            if (par1 == KEY_BACK || par1 == KEY_POWER) {
                if (!ui_back(&g_config, &g_state) && !g_state.syncing)
                    CloseApp();
            }
            else if (par1 == KEY_PREV) ui_page(&g_config, &g_state, -1);
            else if (par1 == KEY_NEXT) ui_page(&g_config, &g_state, +1);
            return 1;

        case EVT_CUSTOM:
            if (par1 == MSG_SYNC_PROGRESS || par1 == MSG_DOWNLOAD_PROGRESS) {
                g_state.progress = par2;
                ui_progress(&g_config, &g_state);
            }
            else if (par1 == MSG_CATALOG_READY || par1 == MSG_SYNC_DONE ||
                     par1 == MSG_DOWNLOAD_DONE || par1 == MSG_CATALOG_ERROR ||
                     par1 == MSG_SYNC_ERROR    || par1 == MSG_DOWNLOAD_ERROR) {
                pthread_join(g_thread, nullptr);
                g_state.syncing  = false;
                g_state.progress = 100;
                if (par1 == MSG_CATALOG_READY || par1 == MSG_CATALOG_ERROR) {
                    g_state.in_folder   = false;
                    g_state.list_scroll = 0;
                }
                ui_draw(&g_config, &g_state);
            }
            return 1;
    }
    return 0;
}

// ─── Thread de fond ───────────────────────────────────────────────────────────
static void begin_background(BgMode mode, const char* msg)
{
    g_state.syncing        = true;
    g_state.bg_mode        = mode;
    g_state.progress       = 0;
    g_state.error_msg[0]   = 0;
    g_state.dl_index       = 0;
    g_state.dl_total       = 0;
    g_state.dl_bytes_now   = 0;
    g_state.dl_bytes_total = 0;
    g_state.downloaded     = 0;
    g_state.dl_errors      = 0;
    snprintf(g_state.status_msg, sizeof(g_state.status_msg), "%s", msg);
    ui_draw(&g_config, &g_state);
    pthread_create(&g_thread, nullptr, bg_thread_func, (void*)(intptr_t)mode);
}

static void start_background(BgMode mode)
{
    if (g_state.syncing) return;
    log_write("Chargement de la bibliothèque\n");
    begin_background(mode, "Connexion...");
}

static void start_download_one(int catalog_idx)
{
    if (g_state.syncing) return;
    if (catalog_idx < 0 || catalog_idx >= (int)g_state.catalog.size()) return;
    g_state.download_book_idx = catalog_idx;
    log_write("Demande : télécharger « %s »\n", g_state.catalog[catalog_idx].name.c_str());
    begin_background(BG_DOWNLOAD_ONE, "Préparation...");
}

static void start_download_folder(const std::string& folder)
{
    if (g_state.syncing) return;
    g_state.download_folder_name = folder;
    log_write("Demande : nouveaux livres du dossier « %s »\n", folder.c_str());
    begin_background(BG_DOWNLOAD_FOLDER, "Préparation...");
}

static void start_download_selected()
{
    if (g_state.syncing || g_state.selected_ids.empty()) return;
    log_write("Demande : %d livre(s) sélectionné(s)\n", (int)g_state.selected_ids.size());
    begin_background(BG_DOWNLOAD_SELECTED, "Préparation...");
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
        default: // BG_DOWNLOAD_FOLDER, BG_DOWNLOAD_SELECTED
            ok_msg  = MSG_SYNC_DONE;
            err_msg = MSG_SYNC_ERROR;
            break;
    }

    SendEvent(GetCurrentTask(), EVT_CUSTOM,
              result == SYNC_OK ? ok_msg : err_msg, 0);
    return nullptr;
}
