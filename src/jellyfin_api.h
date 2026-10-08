#pragma once
/**
 * jellyfin_api.h — Client REST Jellyfin via libcurl
 * inkview.h doit être inclus AVANT ce fichier (depuis main.cpp)
 */

#include <string>
#include <vector>
#include <functional>
#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include "cJSON.h"

// ─── Structures ───────────────────────────────────────────────────────────────

struct JFLibrary {
    std::string id;
    std::string name;
    std::string collection_type;
};

struct JFBook {
    std::string id;
    std::string name;
    std::string path;
    long long   file_size;
};

enum JFResult {
    JF_OK           = 0,
    JF_ERR_NETWORK  = 1,
    JF_ERR_AUTH     = 2,
    JF_ERR_JSON     = 3,
    JF_ERR_IO       = 4,
    JF_ERR_HTTP     = 5,
};

struct JellyfinClient {
    std::string base_url;
    std::string token;
    std::string user_id;
    bool        use_api_key;
};

// ─── API ──────────────────────────────────────────────────────────────────────

JFResult jf_authenticate(JellyfinClient& client,
                          const char* url,
                          const char* username,
                          const char* password);

JFResult jf_set_api_key(JellyfinClient& client,
                         const char* url,
                         const char* api_key);

JFResult jf_get_libraries(const JellyfinClient& client,
                            std::vector<JFLibrary>& out);

JFResult jf_get_books(const JellyfinClient& client,
                       const std::string& library_id,
                       std::vector<JFBook>& out);

JFResult jf_download_book(const JellyfinClient& client,
                            const JFBook& book,
                            const char* dest_path,
                            std::function<void(double,double)> progress_cb);

// ─── Implémentation (incluse une seule fois depuis main.cpp) ──────────────────
#ifdef JELLYFIN_API_IMPL

struct MemBuffer { std::string data; };

static size_t _write_mem(void* ptr, size_t size, size_t nmemb, void* ud)
{
    ((MemBuffer*)ud)->data.append((char*)ptr, size * nmemb);
    return size * nmemb;
}

static size_t _write_file(void* ptr, size_t size, size_t nmemb, void* ud)
{
    return fwrite(ptr, size, nmemb, (FILE*)ud);
}

struct _ProgressData { std::function<void(double,double)> cb; };

static int _progress_cb(void* ud, curl_off_t dltotal, curl_off_t dlnow,
                         curl_off_t, curl_off_t)
{
    auto* p = (_ProgressData*)ud;
    if (p->cb) p->cb((double)dlnow, (double)dltotal);
    return 0;
}

static std::string _auth_header(const JellyfinClient& c)
{
    std::string h = "Authorization: MediaBrowser "
                    "Client=\"JellySync\","
                    "Device=\"VivlioInkpad3\","
                    "DeviceId=\"jellysync-001\","
                    "Version=\"1.0.0\"";
    if (!c.token.empty()) h += ",Token=\"" + c.token + "\"";
    return h;
}

// ─── Wi-Fi ────────────────────────────────────────────────────────────────────
// Vérifie que le Wi-Fi est connecté et le reconnecte sinon. Fourni par
// main.cpp (InkView) ; peut être appelé depuis le thread de fond.
extern bool net_ensure(const char* why);

// Désactive l'économie d'énergie de la puce Wi-Fi, qui coupe la liaison
// pendant les longs transferts.
static void wifi_keepalive()
{
    system("iwconfig wlan0 power off >/dev/null 2>&1");
}

// Keepalive TCP + délais : une liaison morte (Wi-Fi coupé) est détectée en
// 45 s au lieu d'attendre la fin du délai global.
static void _curl_set_keepalive(CURL* curl)
{
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE,   1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,    30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL,   10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,  15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,  45L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,        1L);
}

extern void log_write(const char* fmt, ...);

static JFResult _do_get_once(const JellyfinClient& c, const std::string& ep,
                              MemBuffer& buf, long* code_out)
{
    CURL* curl = curl_easy_init();
    if (!curl) return JF_ERR_NETWORK;
    std::string url = c.base_url + ep;
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, _auth_header(c).c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  _write_mem);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    _curl_set_keepalive(curl);
    log_write("GET %s\n", ep.c_str());
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    double secs = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &secs);
    if (code_out) *code_out = code;
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        log_write("  erreur réseau : %s\n", curl_easy_strerror(res));
        return JF_ERR_NETWORK;
    }
    log_write("  HTTP %ld, %zu octets, %.2f s\n", code, buf.data.size(), secs);
    if (code == 401)     return JF_ERR_AUTH;
    if (code >= 400)     return JF_ERR_HTTP;
    return JF_OK;
}

// GET avec une nouvelle tentative après reconnexion du Wi-Fi
static JFResult _do_get(const JellyfinClient& c, const std::string& ep,
                         MemBuffer& buf, long* code_out = nullptr)
{
    JFResult r = _do_get_once(c, ep, buf, code_out);
    if (r == JF_ERR_NETWORK) {
        net_ensure("requête au serveur");
        sleep(2);
        buf.data.clear();
        r = _do_get_once(c, ep, buf, code_out);
    }
    return r;
}

JFResult jf_authenticate(JellyfinClient& client,
                          const char* url, const char* user, const char* pass)
{
    client.base_url    = url;
    client.use_api_key = false;
    if (!client.base_url.empty() && client.base_url.back() == '/')
        client.base_url.pop_back();

    CURL* curl = curl_easy_init();
    if (!curl) return JF_ERR_NETWORK;

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "Username", user);
    cJSON_AddStringToObject(body, "Pw",       pass);
    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    MemBuffer buf;
    std::string ep = client.base_url + "/Users/AuthenticateByName";
    std::string ah = "Authorization: MediaBrowser "
                     "Client=\"JellySync\","
                     "Device=\"VivlioInkpad3\","
                     "DeviceId=\"jellysync-001\","
                     "Version=\"1.0.0\"";
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ah.c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            ep.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body_str);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  _write_mem);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    _curl_set_keepalive(curl);

    log_write("POST /Users/AuthenticateByName (utilisateur « %s »)\n", user);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    free(body_str);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log_write("  erreur réseau : %s\n", curl_easy_strerror(res));
        return JF_ERR_NETWORK;
    }
    log_write("  HTTP %ld\n", code);
    if (code == 401)     return JF_ERR_AUTH;
    if (code >= 400)     return JF_ERR_HTTP;

    cJSON* root = cJSON_Parse(buf.data.c_str());
    if (!root) return JF_ERR_JSON;
    cJSON* tok = cJSON_GetObjectItem(root, "AccessToken");
    cJSON* usr = cJSON_GetObjectItem(root, "User");
    cJSON* uid = usr ? cJSON_GetObjectItem(usr, "Id") : nullptr;
    if (!tok || !uid) { cJSON_Delete(root); return JF_ERR_JSON; }
    client.token   = cJSON_GetStringValue(tok);
    client.user_id = cJSON_GetStringValue(uid);
    cJSON_Delete(root);
    return JF_OK;
}

JFResult jf_set_api_key(JellyfinClient& client, const char* url, const char* key)
{
    client.base_url    = url;
    client.token       = key;
    client.use_api_key = true;
    if (!client.base_url.empty() && client.base_url.back() == '/')
        client.base_url.pop_back();
    MemBuffer buf;
    JFResult r = _do_get(client, "/Users/Me", buf);
    if (r != JF_OK) return r;
    cJSON* root = cJSON_Parse(buf.data.c_str());
    if (!root) return JF_ERR_JSON;
    cJSON* id = cJSON_GetObjectItem(root, "Id");
    if (id) client.user_id = cJSON_GetStringValue(id);
    cJSON_Delete(root);
    return JF_OK;
}

JFResult jf_get_libraries(const JellyfinClient& c, std::vector<JFLibrary>& out)
{
    MemBuffer buf;
    JFResult r = _do_get(c, "/Users/" + c.user_id + "/Views", buf);
    if (r != JF_OK) return r;
    cJSON* root  = cJSON_Parse(buf.data.c_str());
    if (!root) return JF_ERR_JSON;
    cJSON* items = cJSON_GetObjectItem(root, "Items");
    cJSON* item  = nullptr;
    cJSON_ArrayForEach(item, items) {
        JFLibrary lib;
        cJSON* j;
        if ((j = cJSON_GetObjectItem(item, "Id")))             lib.id              = cJSON_GetStringValue(j);
        if ((j = cJSON_GetObjectItem(item, "Name")))           lib.name            = cJSON_GetStringValue(j);
        if ((j = cJSON_GetObjectItem(item, "CollectionType"))) lib.collection_type = cJSON_GetStringValue(j);
        out.push_back(lib);
    }
    cJSON_Delete(root);
    return JF_OK;
}

JFResult jf_get_books(const JellyfinClient& c,
                       const std::string& lib_id, std::vector<JFBook>& out)
{
    const int PAGE = 500;
    int start = 0;

    while (true) {
        char ep[512];
        snprintf(ep, sizeof(ep),
            "/Items?parentId=%s&includeItemTypes=Book&recursive=true"
            "&fields=Path,MediaSources,Size&limit=%d&startIndex=%d",
            lib_id.c_str(), PAGE, start);

        MemBuffer buf;
        JFResult r = _do_get(c, ep, buf);
        if (r != JF_OK) return r;

        cJSON* root = cJSON_Parse(buf.data.c_str());
        if (!root) return JF_ERR_JSON;

        cJSON* total_j = cJSON_GetObjectItem(root, "TotalRecordCount");
        int total = total_j ? (int)cJSON_GetNumberValue(total_j) : 0;

        cJSON* items = cJSON_GetObjectItem(root, "Items");
        cJSON* item  = nullptr;
        int count = 0;
        cJSON_ArrayForEach(item, items) {
            JFBook book;
            cJSON* j;
            if ((j = cJSON_GetObjectItem(item, "Id")))   book.id   = cJSON_GetStringValue(j);
            if ((j = cJSON_GetObjectItem(item, "Name"))) book.name = cJSON_GetStringValue(j);
            if ((j = cJSON_GetObjectItem(item, "Path"))) book.path = cJSON_GetStringValue(j);
            cJSON* ms = cJSON_GetObjectItem(item, "MediaSources");
            if (ms && cJSON_IsArray(ms) && cJSON_GetArraySize(ms) > 0) {
                cJSON* sz = cJSON_GetObjectItem(cJSON_GetArrayItem(ms, 0), "Size");
                if (sz) book.file_size = (long long)cJSON_GetNumberValue(sz);
            }
            out.push_back(book);
            count++;
        }
        cJSON_Delete(root);

        start += count;
        if (count == 0 || start >= total) break;
    }
    return JF_OK;
}

// Une tentative de téléchargement vers dest + ".part". Si un fichier partiel
// existe déjà (tentative précédente coupée par le Wi-Fi), on reprend là où il
// s'est arrêté grâce à une requête Range.
static JFResult _download_once(const JellyfinClient& c, const JFBook& book,
                                const std::string& part,
                                std::function<void(double,double)> prog,
                                long long* got)
{
    long long have = 0;
    struct stat st;
    if (stat(part.c_str(), &st) == 0) have = st.st_size;

    FILE* f = fopen(part.c_str(), have > 0 ? "ab" : "wb");
    if (!f) return JF_ERR_IO;
    CURL* curl = curl_easy_init();
    if (!curl) { fclose(f); return JF_ERR_NETWORK; }
    std::string url = c.base_url + "/Items/" + book.id + "/Download";
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, _auth_header(c).c_str());
    // La progression affichée compte aussi ce qui était déjà reçu
    _ProgressData pd{ [&](double now, double total) {
        if (prog) prog(now + have, total > 0 ? total + have : 0);
    } };
    curl_easy_setopt(curl, CURLOPT_URL,              url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,       hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    _write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        f);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, _progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &pd);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,      1L);
    if (have > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)have);
        log_write("GET /Items/%s/Download (reprise à %lld octets)\n", book.id.c_str(), have);
    } else {
        log_write("GET /Items/%s/Download\n", book.id.c_str());
    }
    _curl_set_keepalive(curl);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    double secs = 0, speed = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &secs);
    curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD, &speed);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    fclose(f);
    if (stat(part.c_str(), &st) == 0) *got = st.st_size;

    if (res == CURLE_RANGE_ERROR || code == 416) {
        // Le serveur refuse la reprise : on repartira de zéro
        log_write("  reprise refusée par le serveur, on recommence au début\n");
        remove(part.c_str());
        *got = 0;
        return JF_ERR_NETWORK;
    }
    if (res == CURLE_HTTP_RETURNED_ERROR) {
        log_write("  refusé par le serveur : HTTP %ld\n", code);
        remove(part.c_str());
        return code == 401 ? JF_ERR_AUTH : JF_ERR_HTTP;
    }
    if (res != CURLE_OK) {
        // Fichier partiel conservé pour reprendre à la prochaine tentative
        log_write("  erreur réseau : %s (%lld octets reçus)\n", curl_easy_strerror(res), *got);
        return JF_ERR_NETWORK;
    }
    log_write("  HTTP %ld en %.1f s (%.0f Ko/s)\n", code, secs, speed / 1024.0);
    return JF_OK;
}

JFResult jf_download_book(const JellyfinClient& c, const JFBook& book,
                            const char* dest,
                            std::function<void(double,double)> prog)
{
    std::string part = std::string(dest) + ".part";
    wifi_keepalive();

    // Une coupure Wi-Fi ne fait pas échouer le livre : on reconnecte puis on
    // reprend. On n'abandonne qu'après plusieurs tentatives sans progrès.
    const int MAX_FAILS = 4;
    int fails = 0;
    long long last_got = -1;
    while (true) {
        long long got = 0;
        JFResult r = _download_once(c, book, part, prog, &got);
        if (r == JF_OK) {
            remove(dest);
            if (rename(part.c_str(), dest) != 0) {
                log_write("  impossible de renommer %s\n", part.c_str());
                return JF_ERR_IO;
            }
            log_write("  écrit : %s\n", dest);
            return JF_OK;
        }
        if (r != JF_ERR_NETWORK) return r;

        if (got > last_got) fails = 0;   // ça avançait : la tentative ne compte pas
        last_got = got;
        if (++fails >= MAX_FAILS) break;

        log_write("  tentative échouée (%d/%d), vérification du Wi-Fi\n", fails, MAX_FAILS);
        sleep(2);
        if (!net_ensure("reprise du téléchargement")) sleep(5);
        wifi_keepalive();
    }
    log_write("  abandon après %d tentatives sans progrès\n", MAX_FAILS);
    return JF_ERR_NETWORK;
}

#endif // JELLYFIN_API_IMPL
