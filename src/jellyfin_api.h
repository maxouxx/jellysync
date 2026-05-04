#pragma once
/**
 * jellyfin_api.h — Client REST Jellyfin via libcurl
 * inkview.h doit être inclus AVANT ce fichier (depuis main.cpp)
 */

#include <string>
#include <vector>
#include <functional>
#include <curl/curl.h>
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

// Disable WiFi power management to prevent disconnects during transfers.
static void wifi_keepalive()
{
    system("iwconfig wlan0 power off 2>/dev/null");
}

// Apply TCP keepalive settings to a curl handle so long-idle connections
// don't get dropped by the router/NAT.
static void _curl_set_keepalive(CURL* curl)
{
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE,  1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE,   30L);  // first probe after 30s idle
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL,  10L);  // probe every 10s thereafter
}

static JFResult _do_get(const JellyfinClient& c, const std::string& ep,
                         MemBuffer& buf, long* code_out = nullptr)
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
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code_out) *code_out = code;
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return JF_ERR_NETWORK;
    if (code == 401)     return JF_ERR_AUTH;
    if (code >= 400)     return JF_ERR_HTTP;
    return JF_OK;
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    free(body_str);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return JF_ERR_NETWORK;
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

// Single download attempt — returns CURLE_OK / curl error code via res_out.
static JFResult _download_once(const JellyfinClient& c, const JFBook& book,
                                const char* dest,
                                std::function<void(double,double)> prog)
{
    FILE* f = fopen(dest, "wb");
    if (!f) return JF_ERR_IO;
    CURL* curl = curl_easy_init();
    if (!curl) { fclose(f); return JF_ERR_NETWORK; }
    std::string url = c.base_url + "/Items/" + book.id + "/Download";
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, _auth_header(c).c_str());
    _ProgressData pd{ prog };
    curl_easy_setopt(curl, CURLOPT_URL,              url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,       hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    _write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        f);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, _progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &pd);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          600L);  // 10 min for large books
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   0L);
    _curl_set_keepalive(curl);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    fclose(f);
    if (res != CURLE_OK || code >= 400) { remove(dest); return JF_ERR_NETWORK; }
    return JF_OK;
}

JFResult jf_download_book(const JellyfinClient& c, const JFBook& book,
                            const char* dest,
                            std::function<void(double,double)> prog)
{
    // Re-disable WiFi power saving before every download attempt.
    wifi_keepalive();

    const int MAX_RETRIES = 3;
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        JFResult r = _download_once(c, book, dest, prog);
        if (r == JF_OK) return JF_OK;

        if (attempt < MAX_RETRIES) {
            // Wait, then wake WiFi back up before retrying.
            sleep(3);
            wifi_keepalive();
        }
    }
    return JF_ERR_NETWORK;
}

#endif // JELLYFIN_API_IMPL
