/*
 * iliz_updater_client.h - drop-in, header-only helper for apps that ship iliz_updater.exe.
 * MIT License, Copyright (C) 2026 iliz. C and C++, Win32 only, no dependencies.
 *
 *   // 1) Background check, result posted to your window:
 *   IlizUpdaterCheckAsync(hwnd, WM_APP_UPDATE);
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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

/* <folder of this exe>\iliz_updater.exe */
static __inline BOOL IlizUpdaterPath(wchar_t *out, DWORD cap)
{
    DWORD n = GetModuleFileNameW(NULL, out, cap);
    if (!n || n >= cap)
        return FALSE;
    wchar_t *s = wcsrchr(out, L'\\');
    if (!s || (DWORD)(s - out) + 18 >= cap)
        return FALSE;
    wcscpy_s(s + 1, cap - (DWORD)(s + 1 - out), L"iliz_updater.exe");
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

/* Run the updater hidden with args, capture stdout (malloc'd, caller frees). */
static __inline int IlizUpdaterRun(const wchar_t *args, char **out, DWORD timeoutMs)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 512];
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char *buf = NULL;
    DWORD len = 0, cap = 0, code = ILIZ_UPD_ERR_NOT_FOUND;
    if (out)
        *out = NULL;
    if (!IlizUpdaterPath(exe, MAX_PATH))
        return ILIZ_UPD_ERR_NOT_FOUND;
    swprintf_s(cmd, MAX_PATH + 512, L"\"%ls\" %ls", exe, args ? args : L"");
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return ILIZ_UPD_ERR_NOT_FOUND;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof(si));
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
        char tmp[4096];
        DWORD got = 0;
        if (!ReadFile(rd, tmp, sizeof(tmp), &got, NULL) || !got)
            break;
        if (len + got + 1 > cap) {
            DWORD nc = cap ? cap * 2 : 8192;
            char *nb;
            while (nc < len + got + 1)
                nc *= 2;
            nb = (char *)realloc(buf, nc);
            if (!nb)
                break;
            buf = nb;
            cap = nc;
        }
        memcpy(buf + len, tmp, got);
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
        free(buf);
    return (int)code;
}

/* Minimal reader for top-level "key":"string" values of the updater's flat JSON. */
static __inline BOOL IlizUpdaterJsonStr(const char *json, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p;
    size_t n = 0;
    out[0] = 0;
    if (!json || strlen(key) > 56)
        return FALSE;
    sprintf_s(pat, sizeof(pat), "\"%s\":\"", key);
    p = strstr(json, pat);
    if (!p)
        return FALSE;
    p += strlen(pat);
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
    IlizUpdateInfo *u = (IlizUpdateInfo *)calloc(1, sizeof(IlizUpdateInfo));
    if (!u)
        return NULL;
    swprintf_s(args, 256, L"--check --json %ls", extraArgs ? extraArgs : L"");
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
        free(u->json);
        free(u);
    }
}

typedef struct { HWND hwnd; UINT msg; wchar_t args[128]; } IlizUpdaterAsyncCtx_;

static __inline DWORD WINAPI IlizUpdaterCheckThread_(void *p)
{
    IlizUpdaterAsyncCtx_ *c = (IlizUpdaterAsyncCtx_ *)p;
    IlizUpdateInfo *u = IlizUpdaterCheck(c->args, 0);
    if (!u || !PostMessageW(c->hwnd, c->msg, (WPARAM)(u ? u->exitCode : ILIZ_UPD_ERR_NOT_FOUND), (LPARAM)u))
        IlizUpdaterFree(u);
    free(c);
    return 0;
}

/* Background check; posts msg with wParam = exit code, lParam = IlizUpdateInfo* (free it). */
static __inline BOOL IlizUpdaterCheckAsync(HWND hwnd, UINT msg, const wchar_t *extraArgs)
{
    HANDLE h;
    IlizUpdaterAsyncCtx_ *c = (IlizUpdaterAsyncCtx_ *)calloc(1, sizeof(*c));
    if (!c)
        return FALSE;
    c->hwnd = hwnd;
    c->msg = msg;
    wcsncpy_s(c->args, 128, extraArgs ? extraArgs : L"", _TRUNCATE);
    h = CreateThread(NULL, 0, IlizUpdaterCheckThread_, c, 0, NULL);
    if (!h) {
        free(c);
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
    swprintf_s(cmd, MAX_PATH + 128, L"\"%ls\" --update %ls--pid %lu", exe, silent ? L"--silent " : L"",
               GetCurrentProcessId());
    memset(&si, 0, sizeof(si));
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
    free(c);
    return 0;
}

/* The updater sets event Local\iliz_updater_quit.<pid> when it needs this app to exit.
   Call once at startup; msg is posted to hwnd (use your normal "quit" path). */
static __inline BOOL IlizUpdaterWatchQuit(HWND hwnd, UINT msg)
{
    wchar_t name[64];
    HANDLE h;
    IlizUpdaterQuitCtx_ *c = (IlizUpdaterQuitCtx_ *)calloc(1, sizeof(*c));
    if (!c)
        return FALSE;
    swprintf_s(name, 64, L"Local\\iliz_updater_quit.%lu", GetCurrentProcessId());
    c->hwnd = hwnd;
    c->msg = msg;
    c->ev = CreateEventW(NULL, TRUE, FALSE, name);
    if (!c->ev) {
        free(c);
        return FALSE;
    }
    h = CreateThread(NULL, 0, IlizUpdaterQuitThread_, c, 0, NULL);
    if (!h) {
        CloseHandle(c->ev);
        free(c);
        return FALSE;
    }
    CloseHandle(h);
    return TRUE;
}

#ifdef __cplusplus
}
#endif
