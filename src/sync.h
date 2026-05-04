#pragma once
/**
 * sync.h — Moteur de synchronisation JellySync v3
 *
 * Modes :
 *   BG_CATALOG      → auth + catalogue + comparaison locale (pas de DL)
 *   BG_SYNC         → catalogue + téléchargement de tous les nouveaux/modifiés
 *   BG_DOWNLOAD_ONE      → téléchargement d'un seul livre
 *   BG_DOWNLOAD_FOLDER   → téléchargement de tous les nouveaux d'un dossier
 *   BG_DOWNLOAD_SELECTED → téléchargement des livres sélectionnés
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

static std::map<std::string, long long> list_local_files(const char* dir)
{
    std::map<std::string, long long> files;
    DIR* d = opendir(dir);
    if (!d) return files;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string full = std::string(dir) + "/" + e->d_name;
        struct stat st;
        long long sz = 0;
        if (stat(full.c_str(), &st) == 0) sz = st.st_size;
        files[e->d_name] = sz;
    }
    closedir(d);
    return files;
}

// ─── Expose pour main.cpp (scroll touches physiques) ─────────────────────────
// Compte les éléments de la liste aplatie (en-têtes de dossiers + livres)
// en tenant compte du filtre et du dossier sélectionné.
static int get_filtered_count(const AppState* state)
{
    // Si un dossier est sélectionné, on compte seulement ses livres
    if (!state->selected_folder.empty()) {
        int count = 0;
        for (auto& b : state->catalog) {
            if (b.folder != state->selected_folder) continue;
            switch (state->filter) {
                case 0: count++; break;
                case 1: if (b.status==BOOK_NEW||b.status==BOOK_UPDATED) count++; break;
                case 2: if (b.status==BOOK_SYNCED||b.status==BOOK_DOWNLOADING) count++; break;
                case 3: if (b.status==BOOK_LOCAL_ONLY) count++; break;
            }
        }
        return count;
    }

    // Vue dossiers : on compte les en-têtes de dossier (1 par dossier distinct)
    // + les livres sans dossier
    std::set<std::string> folders;
    int no_folder = 0;
    for (auto& b : state->catalog) {
        bool match = false;
        switch (state->filter) {
            case 0: match = true; break;
            case 1: match = (b.status==BOOK_NEW||b.status==BOOK_UPDATED); break;
            case 2: match = (b.status==BOOK_SYNCED||b.status==BOOK_DOWNLOADING); break;
            case 3: match = (b.status==BOOK_LOCAL_ONLY); break;
        }
        if (!match) continue;
        if (b.folder.empty()) no_folder++;
        else folders.insert(b.folder);
    }
    return (int)folders.size() + no_folder;
}

// ─── Authentification réutilisable ────────────────────────────────────────────
static JFResult sync_authenticate(AppConfig* cfg, AppState* state, JellyfinClient& client)
{
    JFResult r;
    if (cfg->api_key[0])
        r = jf_set_api_key(client, cfg->server_url, cfg->api_key);
    else
        r = jf_authenticate(client, cfg->server_url, cfg->username, cfg->password);

    if (r == JF_OK) {
        strncpy(state->token,   client.token.c_str(),   sizeof(state->token)-1);
        strncpy(state->user_id, client.user_id.c_str(), sizeof(state->user_id)-1);
        state->wifi_connected   = true;
        state->server_connected = true;
    }
    return r;
}

// ─── Moteur principal ─────────────────────────────────────────────────────────
static int sync_run(SyncContext* ctx)
{
    AppConfig* cfg   = ctx->config;
    AppState*  state = ctx->state;
    BgMode     mode  = ctx->mode;

    // Disable WiFi power management before any network activity.
    wifi_keepalive();

    // ── Mode téléchargement individuel ────────────────────────────────────────
    if (mode == BG_DOWNLOAD_ONE) {
        int idx = state->download_book_idx;
        if (idx < 0 || idx >= (int)state->catalog.size())
            return SYNC_ERR_IO;

        BookEntry& entry = state->catalog[idx];
        send_progress(state, 5, ("Connexion pour : " + entry.name.substr(0,40)).c_str());

        JellyfinClient client;
        JFResult r = sync_authenticate(cfg, state, client);
        if (r == JF_ERR_AUTH) {
            snprintf(state->error_msg, sizeof(state->error_msg),
                     "Erreur d'authentification.");
            return SYNC_ERR_AUTH;
        }
        if (r != JF_OK) {
            snprintf(state->error_msg, sizeof(state->error_msg),
                     "Impossible de joindre le serveur.");
            return SYNC_ERR_NETWORK;
        }

        send_progress(state, 20, ("Téléchargement : " + entry.name.substr(0,40)).c_str());

        std::string dest_dir = std::string(cfg->books_dir);
        if (!entry.folder.empty()) {
            dest_dir += "/" + entry.folder;
            mkdir(dest_dir.c_str(), 0755);
        }
        std::string dest = dest_dir + "/" + entry.filename;
        entry.status = BOOK_DOWNLOADING;

        JFBook book;
        book.id        = entry.jf_id;
        book.name      = entry.name;
        book.path      = entry.remote_path;
        book.file_size = entry.remote_size;

        state->dl_book_name  = entry.name;
        state->dl_bytes_now  = 0;
        state->dl_bytes_total= entry.remote_size;
        log_write("[DL_ONE] Début : %s → %s (taille: %lld)\n",
                  entry.name.c_str(), dest.c_str(), entry.remote_size);

        r = jf_download_book(client, book, dest.c_str(),
            [&](double done, double total) {
                double effective = (total > 0) ? total
                                 : (book.file_size > 0 ? (double)book.file_size : 0);
                int pct = (effective > 0) ? (int)(20.0 + 78.0 * done / effective) : 50;
                state->dl_bytes_now  = (long long)done;
                state->dl_bytes_total= (long long)effective;
                if (pct != state->download_progress) {
                    state->download_progress = pct;
                    log_write("[DL_ONE] %.0f / %.0f octets — %d%%\n", done, effective, pct);
                    SendEvent(GetCurrentTask(), EVT_CUSTOM, MSG_DOWNLOAD_PROGRESS, pct);
                }
            });

        state->dl_book_name.clear();
        if (r == JF_OK) {
            entry.status = BOOK_SYNCED;
            struct stat st;
            if (stat(dest.c_str(), &st) == 0) entry.local_size = st.st_size;
            state->downloaded++;
            log_write("[DL_ONE] OK — %lld octets sur disque\n", entry.local_size);
            SendGlobalEvent(EVT_BOOKLIST_UPDATED, 0, 0);
            snprintf(state->status_msg, sizeof(state->status_msg),
                     "Téléchargé : %s", entry.name.substr(0,40).c_str());
        } else {
            entry.status = BOOK_ERROR;
            log_write("[DL_ONE] ERREUR (code=%d)\n", (int)r);
            snprintf(state->error_msg, sizeof(state->error_msg),
                     "Échec téléchargement : %s", entry.name.substr(0,40).c_str());
        }
        send_progress(state, 100, state->status_msg);
        return (r == JF_OK) ? SYNC_OK : SYNC_ERR_NETWORK;
    }

    // ── Mode téléchargement dossier ou sélection ──────────────────────────────
    if (mode == BG_DOWNLOAD_FOLDER || mode == BG_DOWNLOAD_SELECTED) {
        send_progress(state, 5, "Connexion...");
        JellyfinClient client;
        JFResult r = sync_authenticate(cfg, state, client);
        if (r == JF_ERR_AUTH) {
            snprintf(state->error_msg, sizeof(state->error_msg),
                     "Erreur d'authentification.");
            return SYNC_ERR_AUTH;
        }
        if (r != JF_OK) {
            snprintf(state->error_msg, sizeof(state->error_msg),
                     "Impossible de joindre le serveur.");
            return SYNC_ERR_NETWORK;
        }

        std::vector<int> to_dl;
        for (int i = 0; i < (int)state->catalog.size(); ++i) {
            auto& e = state->catalog[i];
            if (e.status != BOOK_NEW && e.status != BOOK_UPDATED &&
                e.status != BOOK_ERROR) continue;
            if (mode == BG_DOWNLOAD_FOLDER) {
                if (e.folder == state->download_folder_name) to_dl.push_back(i);
            } else {
                if (state->selected_ids.count(e.jf_id)) to_dl.push_back(i);
            }
        }

        int total_dl = (int)to_dl.size();
        state->downloaded = 0;
        char msg[256];

        if (total_dl == 0) {
            snprintf(state->status_msg, sizeof(state->status_msg),
                     "Rien à télécharger.");
            return SYNC_OK;
        }

        for (int di = 0; di < total_dl; ++di) {
            auto& entry = state->catalog[to_dl[di]];
            int base_pct = 10 + (int)(85.0 * di / total_dl);
            snprintf(msg, sizeof(msg), "(%d/%d) %s",
                     di + 1, total_dl, entry.name.substr(0, 45).c_str());
            send_progress(state, base_pct, msg);

            std::string dest_dir2 = std::string(cfg->books_dir);
            if (!entry.folder.empty()) {
                dest_dir2 += "/" + entry.folder;
                mkdir(dest_dir2.c_str(), 0755);
            }
            std::string dest = dest_dir2 + "/" + entry.filename;
            entry.status = BOOK_DOWNLOADING;

            JFBook book;
            book.id        = entry.jf_id;
            book.name      = entry.name;
            book.path      = entry.remote_path;
            book.file_size = entry.remote_size;

            state->dl_book_name  = entry.name;
            state->dl_bytes_now  = 0;
            state->dl_bytes_total= entry.remote_size;
            log_write("[DL %d/%d] Début : %s → %s (taille: %lld)\n",
                      di + 1, total_dl, entry.name.c_str(),
                      dest.c_str(), entry.remote_size);

            r = jf_download_book(client, book, dest.c_str(),
                [&](double done, double total) {
                    double effective = (total > 0) ? total
                                     : (book.file_size > 0 ? (double)book.file_size : 0);
                    int pct = base_pct;
                    if (effective > 0)
                        pct = base_pct + (int)(85.0 / total_dl * done / effective);
                    int book_pct = (effective > 0) ? (int)(done / effective * 100) : 50;
                    state->dl_bytes_now  = (long long)done;
                    state->dl_bytes_total= (long long)effective;
                    if (book_pct != state->download_progress) {
                        state->download_progress = book_pct;
                        log_write("[DL %d/%d] %.0f / %.0f octets — global %d%%\n",
                                  di + 1, total_dl, done, effective, pct);
                        SendEvent(GetCurrentTask(), EVT_CUSTOM,
                                  MSG_DOWNLOAD_PROGRESS, pct);
                    }
                });

            if (r == JF_OK) {
                entry.status = BOOK_SYNCED;
                struct stat st;
                if (stat(dest.c_str(), &st) == 0) entry.local_size = st.st_size;
                state->downloaded++;
                log_write("[DL %d/%d] OK — %lld octets sur disque\n",
                          di + 1, total_dl, entry.local_size);
            } else {
                entry.status = BOOK_ERROR;
                log_write("[DL %d/%d] ERREUR (code curl=%d)\n", di + 1, total_dl, (int)r);
            }
        }
        state->dl_book_name.clear();

        if (mode == BG_DOWNLOAD_SELECTED)
            state->selected_ids.clear();

        SendGlobalEvent(EVT_BOOKLIST_UPDATED, 0, 0);
        snprintf(state->status_msg, sizeof(state->status_msg),
                 "%d livre(s) téléchargé(s)", state->downloaded);
        send_progress(state, 100, state->status_msg);
        return SYNC_OK;
    }

    // ── Mode catalogue / sync complète ────────────────────────────────────────
    wifi_keepalive();
    send_progress(state, 3, "Connexion au serveur...");

    JellyfinClient client;
    JFResult r = sync_authenticate(cfg, state, client);

    if (r == JF_ERR_AUTH) {
        snprintf(state->error_msg, sizeof(state->error_msg),
                 "Erreur d'authentification. Vérifiez identifiants / clé API.");
        return SYNC_ERR_AUTH;
    }
    if (r != JF_OK) {
        snprintf(state->error_msg, sizeof(state->error_msg),
                 "Impossible de joindre %s", cfg->server_url);
        return SYNC_ERR_NETWORK;
    }

    send_progress(state, 12, "Récupération des bibliothèques...");

    // ── Sélection bibliothèque ────────────────────────────────────────────────
    std::string lib_id = cfg->library_id;

    if (lib_id.empty()) {
        std::vector<JFLibrary> libs;
        r = jf_get_libraries(client, libs);
        if (r != JF_OK) {
            snprintf(state->error_msg, sizeof(state->error_msg),
                     "Impossible de lister les bibliothèques.");
            return SYNC_ERR_NETWORK;
        }

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
            snprintf(state->error_msg, sizeof(state->error_msg),
                     "Aucune bibliothèque 'Livres' trouvée.\n"
                     "Configurez l'ID manuellement dans les paramètres.");
            return SYNC_ERR_NO_LIB;
        }

        strncpy(cfg->library_id, lib_id.c_str(), sizeof(cfg->library_id)-1);
        config_save(CONFIG_FILE, cfg);
    }

    send_progress(state, 22, "Liste des livres...");

    // ── Liste distante ────────────────────────────────────────────────────────
    std::vector<JFBook> remote_books;
    r = jf_get_books(client, lib_id, remote_books);
    if (r != JF_OK) {
        snprintf(state->error_msg, sizeof(state->error_msg),
                 "Impossible de lister les livres (%d).", (int)r);
        return SYNC_ERR_NETWORK;
    }

    state->total_remote = (int)remote_books.size();
    char msg[256];
    snprintf(msg, sizeof(msg), "%d livre(s) sur le serveur", state->total_remote);
    send_progress(state, 35, msg);

    // ── Liste locale ──────────────────────────────────────────────────────────
    mkdir(cfg->books_dir, 0755);
    auto local_files = list_local_files(cfg->books_dir);

    // ── Construction du catalogue ─────────────────────────────────────────────
    state->catalog.clear();
    state->new_count   = 0;
    state->downloaded  = 0;
    state->skipped     = 0;
    state->deleted     = 0;

    std::set<std::string> expected;

    for (auto& book : remote_books) {
        BookEntry entry;
        entry.jf_id       = book.id;
        entry.name        = book.name;
        entry.remote_path = book.path;
        entry.remote_size = book.file_size;
        entry.folder      = extract_folder(book.path);

        std::string ext  = get_extension(book.path);
        entry.filename   = sanitize_filename(book.name) + ext;
        expected.insert(entry.filename);

        auto it = local_files.find(entry.filename);
        if (it == local_files.end()) {
            entry.local_size = 0;
            entry.status     = BOOK_NEW;
            state->new_count++;
        } else {
            entry.local_size = it->second;
            if (entry.remote_size > 0 && entry.local_size != entry.remote_size) {
                entry.status = BOOK_UPDATED;
                state->new_count++;
            } else {
                entry.status = BOOK_SYNCED;
            }
        }

        state->catalog.push_back(entry);
    }

    // Livres locaux uniquement
    for (auto& local : local_files) {
        if (!expected.count(local.first)) {
            BookEntry entry;
            entry.jf_id      = "";
            entry.name       = local.first;
            entry.filename   = local.first;
            entry.local_size = local.second;
            entry.remote_size= 0;
            entry.folder     = "";
            entry.status     = BOOK_LOCAL_ONLY;
            state->catalog.push_back(entry);
        }
    }

    // Tri : par dossier, puis nouveaux en premier, puis alphabétique
    std::sort(state->catalog.begin(), state->catalog.end(),
        [](const BookEntry& a, const BookEntry& b) {
            if (a.folder != b.folder) return a.folder < b.folder;
            int pa = (a.status==BOOK_NEW||a.status==BOOK_UPDATED) ? 0 : 1;
            int pb = (b.status==BOOK_NEW||b.status==BOOK_UPDATED) ? 0 : 1;
            if (pa != pb) return pa < pb;
            return a.name < b.name;
        });

    state->catalog_loaded = true;
    state->selected_folder = "";
    state->download_book_idx = -1;

    // Count locally present books
    state->local_book_count = 0;
    for (auto& e : state->catalog)
        if (e.status == BOOK_SYNCED || e.status == BOOK_LOCAL_ONLY)
            state->local_book_count++;

    send_progress(state, 50, "Catalogue chargé");

    // ── Mode catalogue uniquement ─────────────────────────────────────────────
    if (mode == BG_CATALOG) {
        snprintf(state->status_msg, sizeof(state->status_msg),
                 "%d livre(s)  ·  %d nouveau(x)/mis à jour",
                 state->total_remote, state->new_count);
        return SYNC_OK;
    }

    // ── Téléchargement (mode BG_SYNC) ─────────────────────────────────────────
    int total = (int)state->catalog.size();
    int done  = 0;

    for (auto& entry : state->catalog) {
        if (entry.status != BOOK_NEW && entry.status != BOOK_UPDATED) {
            done++;
            continue;
        }

        int pct = 50 + (int)(45.0 * done / total);
        snprintf(msg, sizeof(msg), "(%d/%d) %s",
                 state->downloaded + 1, state->new_count,
                 entry.name.substr(0, 45).c_str());
        send_progress(state, pct, msg);

        std::string dest_dir3 = std::string(cfg->books_dir);
        if (!entry.folder.empty()) {
            dest_dir3 += "/" + entry.folder;
            mkdir(dest_dir3.c_str(), 0755);
        }
        std::string dest = dest_dir3 + "/" + entry.filename;
        entry.status = BOOK_DOWNLOADING;

        JFBook book;
        book.id        = entry.jf_id;
        book.name      = entry.name;
        book.path      = entry.remote_path;
        book.file_size = entry.remote_size;

        r = jf_download_book(client, book, dest.c_str(), nullptr);

        if (r == JF_OK) {
            entry.status = BOOK_SYNCED;
            struct stat st;
            if (stat(dest.c_str(), &st) == 0) entry.local_size = st.st_size;
            state->downloaded++;
        } else {
            entry.status = BOOK_ERROR;
        }
        done++;
    }

    // ── Suppression optionnelle ───────────────────────────────────────────────
    if (cfg->delete_local) {
        for (auto& entry : state->catalog) {
            if (entry.status == BOOK_LOCAL_ONLY) {
                std::string path = std::string(cfg->books_dir) + "/" + entry.filename;
                remove(path.c_str());
                state->deleted++;
            }
        }
        state->catalog.erase(
            std::remove_if(state->catalog.begin(), state->catalog.end(),
                [](const BookEntry& e){ return e.status == BOOK_LOCAL_ONLY; }),
            state->catalog.end());
    }

    SendGlobalEvent(EVT_BOOKLIST_UPDATED, 0, 0);

    snprintf(state->status_msg, sizeof(state->status_msg),
             "Sync terminée : %d téléchargé(s), %d déjà présent(s)%s",
             state->downloaded, state->skipped,
             cfg->delete_local ?
                 (std::string(", ") + std::to_string(state->deleted) + " supprimé(s)").c_str()
                 : "");

    send_progress(state, 100, state->status_msg);
    return SYNC_OK;
}
