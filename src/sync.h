#pragma once
/**
 * sync.h — Catalogue et téléchargements JellySync
 *
 * Modes :
 *   BG_CATALOG           → auth + catalogue + comparaison avec la liseuse
 *   BG_DOWNLOAD_ONE      → téléchargement d'un seul livre
 *   BG_DOWNLOAD_FOLDER   → téléchargement des nouveaux livres d'un dossier
 *   BG_DOWNLOAD_SELECTED → téléchargement des livres sélectionnés
 *
 * Rien n'est jamais supprimé de la liseuse : on ne télécharge que ce
 * que l'utilisateur demande.
 */

#include "inkview_compat.h"

#include "config.h"
#include "jellyfin_api.h"

#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

extern void log_write(const char* fmt, ...);

enum SyncResult {
    SYNC_OK          = 0,
    SYNC_ERR_AUTH    = 1,
    SYNC_ERR_NETWORK = 2,
    SYNC_ERR_IO      = 3,
    SYNC_ERR_NO_LIB  = 4,
};

struct SyncContext {
    AppConfig* config;
    AppState*  state;
    BgMode     mode;
};

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string get_extension(const std::string& path)
{
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return ".epub";
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    return ext;
}

static std::string sanitize_filename(const std::string& name)
{
    std::string out;
    for (char c : name) {
        if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')
            out += '_';
        else
            out += c;
    }
    return out;
}

// Retourne le nom du dossier immédiat contenant le fichier sur le serveur.
// Ex: "/media/books/Romans/livre.epub" → "Romans"
static std::string extract_folder(const std::string& remote_path)
{
    if (remote_path.empty()) return "";
    std::string p = remote_path;
    for (auto& c : p) if (c == '\\') c = '/';
    size_t last = p.rfind('/');
    if (last == std::string::npos) return "";
    std::string dir = p.substr(0, last);
    size_t prev = dir.rfind('/');
    if (prev == std::string::npos) return dir;
    return dir.substr(prev + 1);
}

static void send_progress(AppState* state, int pct, const char* msg)
{
    state->progress = pct;
    if (msg) snprintf(state->status_msg, sizeof(state->status_msg), "%s", msg);
    SendEvent(GetCurrentTask(), EVT_CUSTOM, MSG_SYNC_PROGRESS, pct);
    usleep(40000);
}

// Fichiers déjà sur la liseuse, indexés par chemin relatif à books_dir :
// "livre.epub" (racine) ou "Dossier/livre.epub" (un niveau de sous-dossier,
// là où les téléchargements sont rangés).
static void list_local_files_in(const std::string& dir, const std::string& prefix,
                                std::map<std::string, long long>& files, int depth)
{
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string full = dir + "/" + e->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0)
                list_local_files_in(full, prefix + e->d_name + "/", files, depth - 1);
        } else {
            files[prefix + e->d_name] = st.st_size;
        }
    }
    closedir(d);
}

static std::map<std::string, long long> list_local_files(const char* dir)
{
    std::map<std::string, long long> files;
    list_local_files_in(dir, "", files, 1);
    return files;
}

static std::string local_path_for(const AppConfig* cfg, const BookEntry& e)
{
    std::string dir = cfg->books_dir;
    if (!e.folder.empty()) dir += "/" + e.folder;
    return dir + "/" + e.filename;
}

// ─── Authentification réutilisable ────────────────────────────────────────────
static JFResult sync_authenticate_once(AppConfig* cfg, JellyfinClient& client)
{
    if (cfg->auth_mode == 1) {
        log_write("Authentification par clé API sur %s\n", cfg->server_url);
        return jf_set_api_key(client, cfg->server_url, cfg->api_key);
    }
    log_write("Authentification de « %s » sur %s\n", cfg->username, cfg->server_url);
    return jf_authenticate(client, cfg->server_url, cfg->username, cfg->password);
}

static JFResult sync_authenticate(AppConfig* cfg, AppState* state, JellyfinClient& client)
{
    JFResult r = sync_authenticate_once(cfg, client);
    // Wi-Fi tombé entre-temps : on le relance et on réessaie
    for (int attempt = 0; r == JF_ERR_NETWORK && attempt < 2; ++attempt) {
        log_write("Serveur injoignable, vérification du Wi-Fi\n");
        net_ensure("connexion au serveur");
        sleep(2);
        r = sync_authenticate_once(cfg, client);
    }
    if (r == JF_OK) {
        strncpy(state->token,   client.token.c_str(),   sizeof(state->token)-1);
        strncpy(state->user_id, client.user_id.c_str(), sizeof(state->user_id)-1);
        state->wifi_connected   = true;
        state->server_connected = true;
        log_write("Connecté au serveur\n");
    } else {
        state->server_connected = false;
        log_write("Échec de connexion (code %d)\n", (int)r);
    }
    return r;
}

static const char* stx(const AppConfig* cfg, const char* fr, const char* en)
{
    return cfg->lang == 1 ? en : fr;
}

// ─── Téléchargement d'une liste de livres ─────────────────────────────────────
static int download_entries(AppConfig* cfg, AppState* state, const std::vector<int>& to_dl)
{
    send_progress(state, 2, "Connexion...");
    JellyfinClient client;
    JFResult r = sync_authenticate(cfg, state, client);
    if (r == JF_ERR_AUTH) {
        snprintf(state->error_msg, sizeof(state->error_msg), "%s",
                 stx(cfg, "Identifiants refusés par le serveur.", "The server rejected the credentials."));
        return SYNC_ERR_AUTH;
    }
    if (r != JF_OK) {
        snprintf(state->error_msg, sizeof(state->error_msg),
                 stx(cfg, "Impossible de joindre %s", "Cannot reach %s"), cfg->server_url);
        return SYNC_ERR_NETWORK;
    }

    int total_dl = (int)to_dl.size();
    state->downloaded = 0;
    state->dl_errors  = 0;
    state->dl_total   = total_dl;
    log_write("%d livre(s) à télécharger\n", total_dl);

    if (total_dl == 0) {
        snprintf(state->status_msg, sizeof(state->status_msg), "Rien à télécharger.");
        return SYNC_OK;
    }

    for (int di = 0; di < total_dl; ++di) {
        BookEntry& entry = state->catalog[to_dl[di]];
        state->dl_index = di + 1;
        int base_pct = (int)(100.0 * di / total_dl);

        std::string dest = local_path_for(cfg, entry);
        if (!entry.folder.empty()) {
            std::string dir = std::string(cfg->books_dir) + "/" + entry.folder;
            if (mkdir(dir.c_str(), 0755) == 0) log_write("Dossier créé : %s\n", dir.c_str());
        }
        entry.status = BOOK_DOWNLOADING;

        JFBook book;
        book.id        = entry.jf_id;
        book.name      = entry.name;
        book.path      = entry.remote_path;
        book.file_size = entry.remote_size;

        state->dl_book_name   = entry.name;
        state->dl_bytes_now   = 0;
        state->dl_bytes_total = entry.remote_size;
        log_write("[%d/%d] %s (%lld octets annoncés)\n", di + 1, total_dl,
                  entry.name.c_str(), entry.remote_size);
        send_progress(state, base_pct, nullptr);

        int last_logged = -1;
        r = jf_download_book(client, book, dest.c_str(),
            [&](double done, double total) {
                double effective = (total > 0) ? total
                                 : (book.file_size > 0 ? (double)book.file_size : 0);
                state->dl_bytes_now   = (long long)done;
                state->dl_bytes_total = (long long)effective;
                int book_pct = (effective > 0) ? (int)(done / effective * 100) : 0;
                int pct = base_pct + (int)(100.0 / total_dl * book_pct / 100);
                if (book_pct / 10 != last_logged && done > 0) {
                    last_logged = book_pct / 10;
                    log_write("  %.0f / %.0f octets (%d %%)\n", done, effective, book_pct);
                }
                if (book_pct != state->download_progress) {
                    state->download_progress = book_pct;
                    SendEvent(GetCurrentTask(), EVT_CUSTOM, MSG_DOWNLOAD_PROGRESS, pct);
                }
            });

        if (r == JF_OK) {
            entry.status = BOOK_SYNCED;
            struct stat st;
            if (stat(dest.c_str(), &st) == 0) entry.local_size = st.st_size;
            state->downloaded++;
            state->local_book_count++;
            if (state->new_count > 0) state->new_count--;
            log_write("[%d/%d] terminé, %lld octets sur la liseuse\n",
                      di + 1, total_dl, entry.local_size);
        } else {
            entry.status = BOOK_ERROR;
            state->dl_errors++;
            log_write("[%d/%d] ÉCHEC (code %d)\n", di + 1, total_dl, (int)r);
        }
        state->selected_ids.erase(entry.jf_id);
    }
    state->dl_book_name.clear();

    SendGlobalEvent(EVT_BOOKLIST_UPDATED, 0, 0);
    if (state->dl_errors > 0)
        snprintf(state->error_msg, sizeof(state->error_msg),
                 stx(cfg, "%d téléchargement(s) en échec sur %d.", "%d of %d downloads failed."), state->dl_errors, total_dl);
    snprintf(state->status_msg, sizeof(state->status_msg),
             "%d livre(s) téléchargé(s)", state->downloaded);
    log_write("Terminé : %d réussi(s), %d échec(s)\n", state->downloaded, state->dl_errors);
    send_progress(state, 100, nullptr);
    return state->dl_errors == total_dl ? SYNC_ERR_NETWORK : SYNC_OK;
}

// ─── Chargement du catalogue ──────────────────────────────────────────────────
static int load_catalog(AppConfig* cfg, AppState* state)
{
    send_progress(state, 3, "Connexion au serveur...");

    JellyfinClient client;
    JFResult r = sync_authenticate(cfg, state, client);
    if (r == JF_ERR_AUTH) {
        snprintf(state->error_msg, sizeof(state->error_msg), "%s",
                 stx(cfg, "Identifiants refusés. Vérifiez les réglages.", "Credentials rejected. Check Settings."));
        return SYNC_ERR_AUTH;
    }
    if (r != JF_OK) {
        snprintf(state->error_msg, sizeof(state->error_msg),
                 stx(cfg, "Impossible de joindre %s", "Cannot reach %s"), cfg->server_url);
        return SYNC_ERR_NETWORK;
    }

    send_progress(state, 15, "Recherche de la bibliothèque de livres...");

    std::string lib_id = cfg->library_id;
    if (lib_id.empty()) {
        std::vector<JFLibrary> libs;
        r = jf_get_libraries(client, libs);
        if (r != JF_OK) {
            snprintf(state->error_msg, sizeof(state->error_msg), "%s",
                     stx(cfg, "Impossible de lister les bibliothèques.", "Cannot list the libraries."));
            return SYNC_ERR_NETWORK;
        }
        for (auto& lib : libs)
            log_write("Bibliothèque : %s (%s)\n", lib.name.c_str(), lib.collection_type.c_str());

        for (auto& lib : libs)
            if (lib.collection_type == "books") { lib_id = lib.id; break; }

        if (lib_id.empty()) {
            for (auto& lib : libs) {
                std::string n = lib.name;
                for (auto& c : n) c = (char)tolower((unsigned char)c);
                if (n.find("livre")!=std::string::npos ||
                    n.find("book") !=std::string::npos ||
                    n.find("epub") !=std::string::npos) {
                    lib_id = lib.id; break;
                }
            }
        }

        if (lib_id.empty()) {
            snprintf(state->error_msg, sizeof(state->error_msg), "%s",
                     stx(cfg, "Aucune bibliothèque de livres trouvée sur le serveur.", "No book library found on the server."));
            return SYNC_ERR_NO_LIB;
        }
        log_write("Bibliothèque retenue : %s\n", lib_id.c_str());
        strncpy(cfg->library_id, lib_id.c_str(), sizeof(cfg->library_id)-1);
        config_save(CONFIG_FILE, cfg);
    }

    send_progress(state, 30, "Liste des livres...");

    std::vector<JFBook> remote_books;
    r = jf_get_books(client, lib_id, remote_books);
    if (r != JF_OK) {
        snprintf(state->error_msg, sizeof(state->error_msg),
                 stx(cfg, "Impossible de lister les livres (%d).", "Cannot list the books (%d)."), (int)r);
        return SYNC_ERR_NETWORK;
    }
    state->total_remote = (int)remote_books.size();
    log_write("%d livre(s) sur le serveur\n", state->total_remote);

    send_progress(state, 70, "Comparaison avec la liseuse...");
    mkdir(cfg->books_dir, 0755);
    auto local_files = list_local_files(cfg->books_dir);
    log_write("%d fichier(s) déjà dans %s\n", (int)local_files.size(), cfg->books_dir);

    state->catalog.clear();
    state->new_count        = 0;
    state->local_book_count = 0;

    for (auto& book : remote_books) {
        BookEntry entry;
        entry.jf_id       = book.id;
        entry.name        = book.name;
        entry.remote_path = book.path;
        entry.remote_size = book.file_size;
        entry.cover_tag   = book.image_tag;
        entry.folder      = extract_folder(book.path);
        entry.filename    = sanitize_filename(book.name) + get_extension(book.path);

        std::string rel = entry.folder.empty() ? entry.filename
                                               : entry.folder + "/" + entry.filename;
        auto it = local_files.find(rel);
        if (it == local_files.end()) it = local_files.find(entry.filename);

        if (it == local_files.end()) {
            entry.local_size = 0;
            entry.status     = BOOK_NEW;
            state->new_count++;
        } else {
            entry.local_size = it->second;
            if (entry.remote_size > 0 && entry.local_size != entry.remote_size) {
                entry.status = BOOK_UPDATED;
                state->new_count++;
                log_write("Mis à jour sur le serveur : %s (%lld → %lld octets)\n",
                          rel.c_str(), entry.local_size, entry.remote_size);
            } else {
                entry.status = BOOK_SYNCED;
                state->local_book_count++;
            }
        }
        state->catalog.push_back(entry);
    }

    // Tri : par dossier, nouveaux en premier, puis alphabétique
    std::sort(state->catalog.begin(), state->catalog.end(),
        [](const BookEntry& a, const BookEntry& b) {
            if (a.folder != b.folder) return a.folder < b.folder;
            int pa = (a.status==BOOK_NEW||a.status==BOOK_UPDATED) ? 0 : 1;
            int pb = (b.status==BOOK_NEW||b.status==BOOK_UPDATED) ? 0 : 1;
            if (pa != pb) return pa < pb;
            return a.name < b.name;
        });

    state->catalog_loaded    = true;
    state->download_book_idx = -1;
    state->selected_ids.clear();

    snprintf(state->status_msg, sizeof(state->status_msg),
             "%d livre(s), %d nouveau(x) ou mis à jour",
             state->total_remote, state->new_count);
    log_write("Catalogue prêt : %d nouveau(x), %d sur la liseuse\n",
              state->new_count, state->local_book_count);
    send_progress(state, 100, nullptr);
    return SYNC_OK;
}

// ─── Point d'entrée du thread de fond ─────────────────────────────────────────
static int sync_run(SyncContext* ctx)
{
    AppConfig* cfg   = ctx->config;
    AppState*  state = ctx->state;

    // Wi-Fi connecté (reconnexion si besoin) et sans mise en veille
    net_ensure(ctx->mode == BG_CATALOG ? "chargement de la bibliothèque" : "téléchargement");
    wifi_keepalive();

    if (ctx->mode == BG_CATALOG)
        return load_catalog(cfg, state);

    std::vector<int> to_dl;
    for (int i = 0; i < (int)state->catalog.size(); ++i) {
        auto& e = state->catalog[i];
        bool wanted = false;
        if (ctx->mode == BG_DOWNLOAD_ONE)
            wanted = (i == state->download_book_idx);
        else if (e.status != BOOK_NEW && e.status != BOOK_UPDATED && e.status != BOOK_ERROR)
            wanted = false;
        else if (ctx->mode == BG_DOWNLOAD_FOLDER)
            wanted = (e.folder == state->download_folder_name);
        else
            wanted = state->selected_ids.count(e.jf_id) > 0;
        if (wanted) to_dl.push_back(i);
    }
    return download_entries(cfg, state, to_dl);
}
