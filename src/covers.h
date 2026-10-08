#pragma once
/**
 * covers.h — Couvertures des livres (option « Affichage Vignette »)
 *
 *   • Les couvertures sont demandées au serveur déjà réduites à la taille
 *     affichée (PNG), puis gardées sur la liseuse dans COVER_DIR : une
 *     couverture n'est téléchargée qu'une fois (le nom du fichier contient
 *     l'ImageTag Jellyfin, qui change quand la couverture change).
 *   • Le téléchargement se fait dans un thread à part, uniquement pour les
 *     livres de la page affichée. Quand le lot est terminé, MSG_COVER_READY
 *     demande au thread principal de redessiner la liste.
 *   • Les images décodées sont gardées en mémoire (au plus COVER_MEM_MAX).
 */

#include "inkview_compat.h"
#include "config.h"
#include "jellyfin_api.h"

#include <pthread.h>
#include <sys/stat.h>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>

extern void log_write(const char* fmt, ...);

#define COVER_DIR      USERDATA "/jellysync/covers"
#define COVER_MEM_MAX  120

struct CoverJob {
    std::string id, tag, path;
    int w, h;
};

static pthread_mutex_t      g_cover_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t       g_cover_cond  = PTHREAD_COND_INITIALIZER;
static bool                 g_cover_thread_started = false;
static std::deque<CoverJob> g_cover_queue;
static std::set<std::string> g_cover_queued;   // chemins en file ou en cours
static std::set<std::string> g_cover_failed;   // échecs (pas de nouvel essai avant Actualiser)
static JellyfinClient       g_cover_client;

// Thread principal uniquement
static std::map<std::string, ibitmap*> g_cover_bitmaps;

static std::string cover_path(const std::string& id, const std::string& tag)
{
    std::string safe_tag;
    for (char c : tag) if (isalnum((unsigned char)c)) safe_tag += c;
    return std::string(COVER_DIR) + "/" + id + "_" + safe_tag + ".png";
}

static bool cover_file_ok(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

static void* cover_thread_func(void*)
{
    while (true) {
        pthread_mutex_lock(&g_cover_mutex);
        while (g_cover_queue.empty())
            pthread_cond_wait(&g_cover_cond, &g_cover_mutex);
        CoverJob job = g_cover_queue.front();
        g_cover_queue.pop_front();
        JellyfinClient client = g_cover_client;
        pthread_mutex_unlock(&g_cover_mutex);

        JFResult r = jf_download_cover(client, job.id, job.tag, job.w, job.h, job.path.c_str());
        if (r != JF_OK) log_write("  couverture indisponible (code %d)\n", (int)r);

        pthread_mutex_lock(&g_cover_mutex);
        g_cover_queued.erase(job.path);
        if (r != JF_OK) g_cover_failed.insert(job.path);
        bool batch_done = g_cover_queue.empty();
        pthread_mutex_unlock(&g_cover_mutex);

        // Un seul rafraîchissement par lot, pour ménager l'écran e-ink
        if (batch_done) SendEvent(GetCurrentTask(), EVT_CUSTOM, MSG_COVER_READY, 0);
    }
    return nullptr;
}

static void cover_mkdirs()
{
    mkdir(USERDATA, 0755);
    mkdir(USERDATA "/jellysync", 0755);
    mkdir(COVER_DIR, 0755);
}

// Serveur et jeton à utiliser (appelé une fois le catalogue chargé)
static void covers_set_server(const char* url, const char* token)
{
    pthread_mutex_lock(&g_cover_mutex);
    g_cover_client.base_url = url;
    if (!g_cover_client.base_url.empty() && g_cover_client.base_url.back() == '/')
        g_cover_client.base_url.pop_back();
    g_cover_client.token = token;
    g_cover_failed.clear();
    pthread_mutex_unlock(&g_cover_mutex);
}

// Nouvelle page affichée : les couvertures pas encore commencées des pages
// précédentes ne servent plus.
static void covers_new_page()
{
    pthread_mutex_lock(&g_cover_mutex);
    for (auto& j : g_cover_queue) g_cover_queued.erase(j.path);
    g_cover_queue.clear();
    pthread_mutex_unlock(&g_cover_mutex);
}

static void covers_free_bitmaps()
{
    for (auto& kv : g_cover_bitmaps) if (kv.second) free(kv.second);
    g_cover_bitmaps.clear();
}

// Couverture prête à dessiner (w×h au plus), ou nullptr si elle n'existe pas
// ou n'est pas encore téléchargée ; dans ce cas elle est mise en file.
static ibitmap* cover_get(const BookEntry& b, int w, int h)
{
    if (b.cover_tag.empty()) return nullptr;
    std::string path = cover_path(b.jf_id, b.cover_tag);

    auto it = g_cover_bitmaps.find(path);
    if (it != g_cover_bitmaps.end()) return it->second;

    if (cover_file_ok(path)) {
        if (g_cover_bitmaps.size() >= COVER_MEM_MAX) covers_free_bitmaps();
        ibitmap* bmp = LoadPNGStretch(path.c_str(), w, h, 1, 1);
        if (!bmp) {
            log_write("Couverture illisible, supprimée : %s\n", path.c_str());
            remove(path.c_str());
        }
        g_cover_bitmaps[path] = bmp;
        return bmp;
    }

    pthread_mutex_lock(&g_cover_mutex);
    bool skip = g_cover_client.base_url.empty() || g_cover_queued.count(path) ||
                g_cover_failed.count(path);
    if (!skip) {
        if (!g_cover_thread_started) {
            cover_mkdirs();
            pthread_t t;
            if (pthread_create(&t, nullptr, cover_thread_func, nullptr) == 0) {
                pthread_detach(t);
                g_cover_thread_started = true;
            }
        }
        g_cover_queue.push_back({ b.jf_id, b.cover_tag, path, w, h });
        g_cover_queued.insert(path);
        pthread_cond_signal(&g_cover_cond);
    }
    pthread_mutex_unlock(&g_cover_mutex);
    return nullptr;
}
