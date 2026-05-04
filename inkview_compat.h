/**
 * Compatibility header for PocketBook SDK API differences
 * Maps older API calls to current SDK versions
 */

#ifndef INKVIEW_COMPAT_H
#define INKVIEW_COMPAT_H

#include <inkview.h>

// ─── KEY CONSTANTS ────────────────────────────────────────────────────────
// Map non-prefixed key constants to IV_KEY_* versions
#ifndef KEY_POWER
#define KEY_POWER   IV_KEY_POWER
#endif

#ifndef KEY_BACK
#define KEY_BACK    IV_KEY_BACK
#endif

#ifndef KEY_PREV
#define KEY_PREV    IV_KEY_PREV
#endif

#ifndef KEY_NEXT
#define KEY_NEXT    IV_KEY_NEXT
#endif

// ─── EVENT CONSTANTS ─────────────────────────────────────────────────────
// Custom events for internal messaging - use high values to avoid SDK conflicts
#ifndef EVT_CUSTOM
#define EVT_CUSTOM  1000  // Custom app event base
#endif

#ifndef EVT_BOOKLIST_UPDATED
#define EVT_BOOKLIST_UPDATED  1001
#endif

// ─── FONT CONSTANTS ──────────────────────────────────────────────────────
// PocketBook SDK uses enum values directly
#ifndef FONT_REGULAR
#define FONT_REGULAR 0
#endif

// FONT_BOLD is already defined in the SDK as enum value

// ─── COMPATIBILITY WRAPPERS ──────────────────────────────────────────────
// SendEvent compatibility - the old API used SendEvent(task_id, ...) but new SDK
// has SendEvent(handler_func, ...) and SendEventTo(task_id, ...)
// We need to replace calls to SendEvent(GetCurrentTask(), ...) with SendEventTo(GetCurrentTask(), ...)
inline void SendEvent_Compat(int task, int type, int par1, int par2) {
    SendEventTo(task, type, par1, par2);
}

// Replace SendEvent calls that use GetCurrentTask()
#ifdef SendEvent
#undef SendEvent
#endif
#define SendEvent(task, type, par1, par2) SendEvent_Compat((task), (type), (par1), (par2))

// SendGlobalEvent compatibility - not directly available, use broadcast approach
#ifndef SendGlobalEvent
#define SendGlobalEvent(type, par1, par2) \
    do { \
        int task = GetCurrentTask(); \
        if (task >= 0) SendEventTo(task, (type), (par1), (par2)); \
    } while(0)
#endif

#endif /* INKVIEW_COMPAT_H */