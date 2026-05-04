#pragma once
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <set>

// ─── Constantes de messages (EVT_CUSTOM par1) ────────────────────────────────
#define MSG_SYNC_PROGRESS       0x01
#define MSG_SYNC_DONE           0x02
#define MSG_SYNC_ERROR          0x03
#define MSG_CATALOG_READY       0x04
#define MSG_CATALOG_ERROR       0x05
#define MSG_DOWNLOAD_PROGRESS   0x06
#define MSG_DOWNLOAD_DONE       0x07
#define MSG_DOWNLOAD_ERROR      0x08

// ─── Écrans ───────────────────────────────────────────────────────────────────
enum Screen {
    SCREEN_SETUP,
    SCREEN_MAIN,
    SCREEN_SYNCING,
};

// ─── Mode du thread de fond ───────────────────────────────────────────────────
enum BgMode {
    BG_CATALOG,
    BG_SYNC,
    BG_DOWNLOAD_ONE,
    BG_DOWNLOAD_FOLDER,    // télécharger tous les nouveaux d'un dossier
    BG_DOWNLOAD_SELECTED,  // télécharger les livres sélectionnés
};

// ─── Statut d'un livre ────────────────────────────────────────────────────────
enum BookStatus {
    BOOK_SYNCED,
    BOOK_NEW,
    BOOK_UPDATED,
    BOOK_LOCAL_ONLY,
    BOOK_DOWNLOADING,
    BOOK_ERROR,
};

// ─── Entrée catalogue ─────────────────────────────────────────────────────────
struct BookEntry {
    std::string jf_id;
    std::string name;
    std::string filename;
    std::string remote_path;
    std::string folder;         // Dossier parent (extrait du chemin serveur)
    long long   remote_size;
    long long   local_size;
    BookStatus  status;
};

// ─── Configuration persistée ─────────────────────────────────────────────────
struct AppConfig {
    char server_url[256];
    char username[128];
    char password[128];
    char api_key[256];
    char library_id[128];
    int  delete_local;
    char books_dir[512];
    int  lang;            // 0 = Français, 1 = English
};

// ─── État applicatif (non persisté) ──────────────────────────────────────────
struct AppState {
    Screen   screen;
    BgMode   bg_mode;
    bool     syncing;
    bool     catalog_loaded;
    int      progress;
    char     status_msg[256];
    char     error_msg[256];
    char     token[256];
    char     user_id[128];

    // Catalogue
    std::vector<BookEntry> catalog;
    int  new_count;
    int  total_remote;

    // Compteurs sync
    int  downloaded;
    int  skipped;
    int  deleted;

    // Navigation
    int  list_scroll;
    int  list_visible;
    int  filter;            // 0=Tous 1=Nouveaux 2=Présents 3=Local

    // Dossier sélectionné ("" = vue tous dossiers)
    std::string selected_folder;

    // Téléchargement individuel
    int  download_book_idx;
    int  download_progress;

    // Sélection multiple
    std::set<std::string> selected_ids;
    std::string download_folder_name;

    // Progression fine (téléchargement en cours)
    long long   dl_bytes_now;   // octets reçus pour le livre en cours
    long long   dl_bytes_total; // taille attendue (0 si inconnue)
    std::string dl_book_name;   // nom du livre en cours de téléchargement

    // Statuts connexion (mis à jour après Refresh)
    bool        wifi_connected;
    bool        server_connected;
    int         local_book_count;
};

// ─── Implémentation config ────────────────────────────────────────────────────
static void config_load(const char* path, AppConfig* cfg)
{
    memset(cfg, 0, sizeof(AppConfig));
    cfg->delete_local = 0;
    snprintf(cfg->books_dir, sizeof(cfg->books_dir), FLASHDIR "/books/Jellyfin");

    FILE* f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = line;
        const char* val = eq + 1;

        if      (strcmp(key, "server_url")  == 0) strncpy(cfg->server_url,  val, 255);
        else if (strcmp(key, "username")    == 0) strncpy(cfg->username,    val, 127);
        else if (strcmp(key, "password")    == 0) strncpy(cfg->password,    val, 127);
        else if (strcmp(key, "api_key")     == 0) strncpy(cfg->api_key,     val, 255);
        else if (strcmp(key, "library_id")  == 0) strncpy(cfg->library_id,  val, 127);
        else if (strcmp(key, "delete_local")== 0) cfg->delete_local = atoi(val);
        else if (strcmp(key, "books_dir")   == 0) strncpy(cfg->books_dir,   val, 511);
        else if (strcmp(key, "lang")        == 0) cfg->lang = atoi(val);
    }
    fclose(f);
}

static void config_save(const char* path, const AppConfig* cfg)
{
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "server_url=%s\n",   cfg->server_url);
    fprintf(f, "username=%s\n",     cfg->username);
    fprintf(f, "password=%s\n",     cfg->password);
    fprintf(f, "api_key=%s\n",      cfg->api_key);
    fprintf(f, "library_id=%s\n",   cfg->library_id);
    fprintf(f, "delete_local=%d\n", cfg->delete_local);
    fprintf(f, "books_dir=%s\n",    cfg->books_dir);
    fprintf(f, "lang=%d\n",         cfg->lang);
    fclose(f);
}
