/*
 * iliz_updater_client.h - drop-in, header-only helper for apps that ship iliz_updater.exe.
 * MIT License, Copyright (C) 2026 iliz. C and C++, Win32 only, no dependencies.
 *
 * Uses only kernel32/user32 (HeapAlloc, wsprintfW, lstr*): no C runtime calls, so it also works
 * in CRT-free builds (/NODEFAULTLIB, own entry point). Link kernel32.lib and user32.lib.
 *
 *   // 1) Background check, result posted to your window:
 *   IlizUpdaterCheckAsync(hwnd, WM_APP_UPDATE, L"--if-due");
 *   case WM_APP_UPDATE: {
 *       IlizUpdateInfo *u = (IlizUpdateInfo *)lParam;
 *       if (u->exitCode == ILIZ_UPD_UPDATE_AVAILABLE) ShowBadge(u->latest);
 *       IlizUpdaterFree(u);
 *   }
 *   // 2) Let the updater close us gracefully when it is ready to install:
 *   IlizUpdaterWatchQuit(hwnd, WM_CLOSE);          // or your own "exit app" message
 *   // 3) Install: interactive dialog, or silently in the background:
 *   IlizUpdaterLaunch(FALSE);                      // TRUE = silent
 */
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ILIZ_UPD_OK = 0,
    ILIZ_UPD_UPDATE_AVAILABLE = 1,
    ILIZ_UPD_UPDATED = 2,
    ILIZ_UPD_CANCELLED = 3,
    ILIZ_UPD_SKIPPED = 4,
    ILIZ_UPD_ERR_FIRST = 10,       /* 10..19: see README */
    ILIZ_UPD_ERR_NOT_FOUND = 100,  /* helper: updater exe missing / failed to start */
    ILIZ_UPD_ERR_TIMEOUT = 101     /* helper: updater did not finish in time */
};

typedef struct IlizUpdateInfo {
    int exitCode;
    char status[32];   /* up_to_date | update_available | no_release | skipped | error */
    char current[64];
    char latest[64];
    char error[32];
    char message[512];
    char *json;        /* full stdout (UTF-8), includes "notes", "url", "page"... */
} IlizUpdateInfo;

static __inline void *IlizUpdaterAlloc_(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static __inline void IlizUpdaterHeapFree_(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* <folder of this exe>\iliz_updater.exe; FALSE if it is not there */
static __inline BOOL IlizUpdaterPath(wchar_t *out, DWORD cap)
{
    DWORD n = GetModuleFileNameW(NULL, out, cap), s = n;
    if (!n || n >= cap)
        return FALSE;
    while (s && out[s - 1] != L'\\')
        --s;
    if (!s || s + 17 > cap)
        return FALSE;
    lstrcpyW(out + s, L"iliz_updater.exe");
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

/* Run the updater hidden with args, capture stdout (heap; free with IlizUpdaterFree or HeapFree). */
static __inline int IlizUpdaterRun(const wchar_t *args, char **out, DWORD timeoutMs)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 512];
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char *buf = NULL;
    DWORD len = 0, cap = 0, code = ILIZ_UPD_ERR_NOT_FOUND;
    if (out)
        *out = NULL;
    if (!IlizUpdaterPath(exe, MAX_PATH) || (args && lstrlenW(args) > 500))
        return ILIZ_UPD_ERR_NOT_FOUND;
    wsprintfW(cmd, L"\"%s\" %s", exe, args ? args : L"");
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return ILIZ_UPD_ERR_NOT_FOUND;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    SecureZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return ILIZ_UPD_ERR_NOT_FOUND;
    }
    CloseHandle(wr);
    for (;;) {
        char tmp[1024];        /* keep frames under 4 KB: no __chkstk in CRT-free builds */
        DWORD got = 0, i;
        if (!ReadFile(rd, tmp, sizeof(tmp), &got, NULL) || !got)
            break;
        if (len + got + 1 > cap) {
            DWORD nc = cap ? cap * 2 : 8192;
            char *nb;
            while (nc < len + got + 1)
                nc *= 2;
            nb = (char *)(buf ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, buf, nc) : IlizUpdaterAlloc_(nc));
            if (!nb)
                break;
            buf = nb;
            cap = nc;
        }
        for (i = 0; i < got; ++i)
            buf[len + i] = tmp[i];
        len += got;
        buf[len] = 0;
    }
    CloseHandle(rd);
    if (WaitForSingleObject(pi.hProcess, timeoutMs ? timeoutMs : INFINITE) == WAIT_TIMEOUT)
        code = ILIZ_UPD_ERR_TIMEOUT;
    else if (!GetExitCodeProcess(pi.hProcess, &code))
        code = ILIZ_UPD_ERR_NOT_FOUND;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (out)
        *out = buf;
    else
        IlizUpdaterHeapFree_(buf);
    return (int)code;
}

/* Minimal reader for top-level "key":"string" values of the updater's flat JSON. */
static __inline BOOL IlizUpdaterJsonStr(const char *json, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p;
    size_t n = 0;
    int k;
    out[0] = 0;
    if (!json || lstrlenA(key) > 56)
        return FALSE;
    wsprintfA(pat, "\"%s\":\"", key);
    k = lstrlenA(pat);
    for (p = json; *p; ++p) {               /* strstr without the CRT */
        int i = 0;
        while (i < k && p[i] == pat[i])
            ++i;
        if (i == k)
            break;
    }
    if (!*p)
        return FALSE;
    p += k;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            out[n++] = *p == 'n' ? '\n' : (*p == 't' ? '\t' : (*p == 'r' ? '\r' : *p));
            ++p;
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return TRUE;
}

/* Synchronous check. extraArgs e.g. L"--if-due". Returns heap info, free with IlizUpdaterFree. */
static __inline IlizUpdateInfo *IlizUpdaterCheck(const wchar_t *extraArgs, DWORD timeoutMs)
{
    wchar_t args[256];
    IlizUpdateInfo *u = (IlizUpdateInfo *)IlizUpdaterAlloc_(sizeof(IlizUpdateInfo));
    if (!u)
        return NULL;
    if (extraArgs && lstrlenW(extraArgs) > 200)
        extraArgs = NULL;
    wsprintfW(args, L"--check --json %s", extraArgs ? extraArgs : L"");
    u->exitCode = IlizUpdaterRun(args, &u->json, timeoutMs ? timeoutMs : 60000);
    IlizUpdaterJsonStr(u->json, "status", u->status, sizeof(u->status));
    IlizUpdaterJsonStr(u->json, "current", u->current, sizeof(u->current));
    IlizUpdaterJsonStr(u->json, "latest", u->latest, sizeof(u->latest));
    IlizUpdaterJsonStr(u->json, "error", u->error, sizeof(u->error));
    IlizUpdaterJsonStr(u->json, "message", u->message, sizeof(u->message));
    return u;
}

static __inline void IlizUpdaterFree(IlizUpdateInfo *u)
{
    if (u) {
        IlizUpdaterHeapFree_(u->json);
        IlizUpdaterHeapFree_(u);
    }
}

typedef struct { HWND hwnd; UINT msg; wchar_t args[128]; } IlizUpdaterAsyncCtx_;

static __inline DWORD WINAPI IlizUpdaterCheckThread_(void *p)
{
    IlizUpdaterAsyncCtx_ *c = (IlizUpdaterAsyncCtx_ *)p;
    IlizUpdateInfo *u = IlizUpdaterCheck(c->args, 0);
    if (!u || !PostMessageW(c->hwnd, c->msg, (WPARAM)u->exitCode, (LPARAM)u))
        IlizUpdaterFree(u);
    IlizUpdaterHeapFree_(c);
    return 0;
}

/* Background check; posts msg with wParam = exit code, lParam = IlizUpdateInfo* (free it). */
static __inline BOOL IlizUpdaterCheckAsync(HWND hwnd, UINT msg, const wchar_t *extraArgs)
{
    HANDLE h;
    IlizUpdaterAsyncCtx_ *c = (IlizUpdaterAsyncCtx_ *)IlizUpdaterAlloc_(sizeof(*c));
    if (!c)
        return FALSE;
    c->hwnd = hwnd;
    c->msg = msg;
    lstrcpynW(c->args, extraArgs ? extraArgs : L"", 128);
    h = CreateThread(NULL, 0, IlizUpdaterCheckThread_, c, 0, NULL);
    if (!h) {
        IlizUpdaterHeapFree_(c);
        return FALSE;
    }
    CloseHandle(h);
    return TRUE;
}

/* Start the updater detached, bound to this process (--pid). It closes us when ready. */
static __inline BOOL IlizUpdaterLaunch(BOOL silent)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 128];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (!IlizUpdaterPath(exe, MAX_PATH))
        return FALSE;
    wsprintfW(cmd, L"\"%s\" --update %s--pid %lu", exe, silent ? L"--silent " : L"", GetCurrentProcessId());
    SecureZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return FALSE;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

typedef struct { HWND hwnd; UINT msg; HANDLE ev; } IlizUpdaterQuitCtx_;

static __inline DWORD WINAPI IlizUpdaterQuitThread_(void *p)
{
    IlizUpdaterQuitCtx_ *c = (IlizUpdaterQuitCtx_ *)p;
    if (WaitForSingleObject(c->ev, INFINITE) == WAIT_OBJECT_0)
        PostMessageW(c->hwnd, c->msg, 0, 0);
    CloseHandle(c->ev);
    IlizUpdaterHeapFree_(c);
    return 0;
}

/* The updater sets event Local\iliz_updater_quit.<pid> when it needs this app to exit.
   Call once at startup; msg is posted to hwnd (use your normal "quit" path). */
static __inline BOOL IlizUpdaterWatchQuit(HWND hwnd, UINT msg)
{
    wchar_t name[64];
    HANDLE h;
    IlizUpdaterQuitCtx_ *c = (IlizUpdaterQuitCtx_ *)IlizUpdaterAlloc_(sizeof(*c));
    if (!c)
        return FALSE;
    wsprintfW(name, L"Local\\iliz_updater_quit.%lu", GetCurrentProcessId());
    c->hwnd = hwnd;
    c->msg = msg;
    c->ev = CreateEventW(NULL, TRUE, FALSE, name);
    if (!c->ev) {
        IlizUpdaterHeapFree_(c);
        return FALSE;
    }
    h = CreateThread(NULL, 0, IlizUpdaterQuitThread_, c, 0, NULL);
    if (!h) {
        CloseHandle(c->ev);
        IlizUpdaterHeapFree_(c);
        return FALSE;
    }
    CloseHandle(h);
    return TRUE;
}

#ifdef __cplusplus
}
#endif
