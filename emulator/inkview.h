/**
 * inkview.h — Émulateur InkView pour PC (SDL2)
 *
 * Remplace le <inkview.h> du SDK PocketBook lors du build desktop.
 * Seule la partie de l'API utilisée par JellySync est simulée.
 * Le build liseuse (CMakeLists.txt racine) n'utilise jamais ce fichier.
 */

#ifndef INKVIEW_EMU_H
#define INKVIEW_EMU_H

// Le vrai inkview.h inclut ces en-têtes système ; le code en dépend
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Événements ───────────────────────────────────────────────────────────────
#define EVT_INIT          21
#define EVT_EXIT          22
#define EVT_SHOW          23
#define EVT_HIDE          24
#define EVT_KEYPRESS      25
#define EVT_KEYDOWN       EVT_KEYPRESS
#define EVT_KEYRELEASE    26
#define EVT_KEYUP         EVT_KEYRELEASE
#define EVT_KEYREPEAT     28
#define EVT_POINTERUP     29
#define EVT_POINTERDOWN   30
#define EVT_POINTERMOVE   31
#define EVT_REPAINT       91
#define EVT_NET_CONNECTED    256
#define EVT_NET_DISCONNECTED 257

// ─── Touches ──────────────────────────────────────────────────────────────────
#define IV_KEY_POWER      0x01
#define IV_KEY_BACK       0x1b
#define IV_KEY_PREV       0x18
#define IV_KEY_NEXT       0x19
#define IV_KEY_OK         0x0a
#define IV_KEY_MENU       0x17

// ─── Alignement texte (DrawTextRect) ──────────────────────────────────────────
#define ALIGN_LEFT        1
#define ALIGN_CENTER      2
#define ALIGN_RIGHT       4
#define ALIGN_FIT         8
#define VALIGN_TOP        16
#define VALIGN_MIDDLE     32
#define VALIGN_BOTTOM     64
#define DOTS              512

// ─── Clavier ──────────────────────────────────────────────────────────────────
#define KBD_NORMAL        0
#define KBD_PASSWORD      0x1000

// ─── Réseau ───────────────────────────────────────────────────────────────────
#define NET_WIFI          0x0002
#define NET_WIFIREADY     0x0200
#define NET_CONNECTED     0x0f00
#define NET_OK            0
#define NET_FAIL          -11

// ─── Barre d'état du système ─────────────────────────────────────────────────
#define PANEL_DISABLED      0
#define PANEL_ENABLED       (1 << 1)
#define PANEL_NO_FB_OFFSET  (1 << 3)

// ─── Polices ──────────────────────────────────────────────────────────────────
enum iv_fonttype { FONT_STD = 0, FONT_BOLD, FONT_ITALIC, FONT_BOLDITALIC, FONT_MONO };

typedef struct ifont_s {
    char* name;
    int   size;
    int   isbold;
    int   height;
    int   linespacing;
    int   baseline;
    void* fdata;       // TTF_Font* (Liberation Sans, police par défaut de la liseuse)
    void* fallback;    // TTF_Font* (DejaVu Sans, pour ✓ ↓ ─ absents de Liberation)
} ifont;

typedef int  (*iv_handler)(int type, int par1, int par2);
typedef void (*iv_keyboardhandler)(char* text);
typedef void (*iv_timerproc)(void);

// ─── API ──────────────────────────────────────────────────────────────────────
void InkViewMain(iv_handler h);
void CloseApp(void);

int  ScreenWidth(void);
int  ScreenHeight(void);

void ClearScreen(void);
void FillArea(int x, int y, int w, int h, int color);
void DrawLine(int x1, int y1, int x2, int y2, int color);
char* DrawTextRect(int x, int y, int w, int h, const char* s, int flags);

const char* iv_get_default_font(int fonttype);
ifont* OpenFont(const char* name, int size, int aa);
void   CloseFont(ifont* f);
void   SetFont(const ifont* font, int color);
ifont* GetFont(void);

void FullUpdate(void);
void SoftUpdate(void);
void PartialUpdate(int x, int y, int w, int h);

int  GetCurrentTask(void);
void SendEventTo(int task, int type, int par1, int par2);

void OpenKeyboard(const char* title, char* buffer, int maxlen, int flags,
                  iv_keyboardhandler hproc);

// Barre d'état : tant qu'elle est active, l'affichage est décalé de sa hauteur
// vers le bas (le bas de l'image réapparaît en haut), comme sur la liseuse.
void SetPanelType(int type);
int  PanelHeight(void);

void SetHardTimer(const char* name, iv_timerproc tproc, int ms);
void ClearTimer(iv_timerproc tproc);

// Wi-Fi simulé : F9 coupe / rétablit la connexion ; JELLYSYNC_WIFI=off au
// démarrage. NetConnect() le rétablit au bout d'une seconde.
int  QueryNetwork(void);
int  NetConnect(const char* name);
int  NetMgrPing(void);

#ifdef __cplusplus
}
#endif

#endif /* INKVIEW_EMU_H */
