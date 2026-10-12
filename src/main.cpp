#include "common.h"
#include <shellapi.h>
#include "cJSON.h"

BOOL g_forceRestart, g_noRestart;

static void Out(const char *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    BOOL own = FALSE;
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            return;
        h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        own = TRUE;
        if (h == INVALID_HANDLE_VALUE)
            return;
    }
    DWORD w;
    if (GetFileType(h) == FILE_TYPE_CHAR) {
        // Console: write UTF-16 so non-ASCII text survives any code page.
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);
        wchar_t *ws = (wchar_t *)malloc(n * sizeof(wchar_t));
        if (ws) {
            MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, n);
            WriteConsoleW(h, ws, (DWORD)n - 1, &w, 0);
            WriteConsoleW(h, L"\r\n", 2, &w, 0);
            free(ws);
        }
    } else {
        WriteFile(h, s, (DWORD)strlen(s), &w, 0);
        WriteFile(h, "\r\n", 2, &w, 0);
    }
    if (own)
        CloseHandle(h);
}

static const char *ErrName(int rc)
{
    switch (rc) {
    case UPD_ERR_ARGS: return "args";
    case UPD_ERR_CONFIG: return "config";
    case UPD_ERR_NETWORK: return "network";
    case UPD_ERR_SOURCE: return "source";
    case UPD_ERR_VERIFY: return "verify";
    case UPD_ERR_PACKAGE: return "package";
    case UPD_ERR_HOST_BUSY: return "host_busy";
    case UPD_ERR_ACCESS: return "access";
    case UPD_ERR_APPLY: return "apply";
    case UPD_ERR_BUSY: return "busy";
    }
    return "unknown";
}

static BOOL s_json;

// Machine-readable result on stdout (with --json), or a short text line.
static int Emit(const char *status, int rc, const Release *rel)
{
    if (s_json) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", status);
        cJSON_AddNumberToObject(o, "exit_code", rc);
        cJSON_AddStringToObject(o, "current", g_current);
        if (rel && rel->version[0]) {
            cJSON_AddStringToObject(o, "latest", rel->version);
            cJSON_AddStringToObject(o, "tag", rel->tag);
            if (rel->published[0])
                cJSON_AddStringToObject(o, "published", rel->published);
            if (rel->page[0])
                cJSON_AddStringToObject(o, "page", rel->page);
            if (rel->zipUrl[0]) {
                char u[UPD_URL * 3];
                W2U(rel->zipUrl, u, sizeof(u));
                cJSON_AddStringToObject(o, "url", u);
                cJSON_AddNumberToObject(o, "size", (double)rel->size);
            }
            if (rel->sha256[0])
                cJSON_AddStringToObject(o, "sha256", rel->sha256);
            cJSON_AddStringToObject(o, "notes", rel->notes ? rel->notes : "");
        } else {
            cJSON_AddNullToObject(o, "latest");
        }
        if (rc >= UPD_ERR_ARGS) {
            cJSON_AddStringToObject(o, "error", ErrName(rc));
            cJSON_AddStringToObject(o, "message", g_err);
        }
        cJSON_AddStringToObject(o, "updater_version", UPD_VERSION_A);
        char *s = cJSON_PrintUnformatted(o);
        if (s) {
            Out(s);
            cJSON_free(s);
        }
        cJSON_Delete(o);
    } else {
        char line[800];
        if (rc >= UPD_ERR_ARGS)
            sprintf_s(line, sizeof(line), "error %s: %s", ErrName(rc), g_err);
        else
            sprintf_s(line, sizeof(line), "%s %s -> %s", status, g_current, rel && rel->version[0] ? rel->version : "-");
        Out(line);
    }
    Log("result %s rc=%d", status, rc);
    return rc;
}

static void CheckStamp(BOOL write, ULONGLONG *last)
{
    wchar_t p[UPD_PATH];
    JoinPath(p, g_cfg.workDir, L"last_check", UPD_PATH);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (write) {
        char s[32];
        sprintf_s(s, sizeof(s), "%llu", now);
        if (MakeDirs(g_cfg.workDir))
            WriteAll(p, s, (DWORD)strlen(s));
        return;
    }
    DWORD len;
    char *t = ReadAll(p, &len);
    *last = t ? _strtoui64(t, 0, 10) : 0;
    free(t);
    if (*last > now)
        *last = 0;
    *last = (now - *last) / 10000000ULL;  // seconds since last check
}

static BOOL Due()
{
    ULONGLONG ago = 0;
    CheckStamp(FALSE, &ago);
    return ago >= (ULONGLONG)g_cfg.checkIntervalHours * 3600;
}

// 0 = up to date / none, 1 = newer available, else error code.
int Check(Release *rel, BOOL force)
{
    int rc = FetchRelease(rel);
    if (rc != UPD_OK)
        return rc;
    CheckStamp(TRUE, 0);
    if (!rel->version[0])
        return UPD_OK;
    if (force || !g_current[0] || CompareVersions(rel->version, g_current) > 0)
        return UPD_UPDATE_AVAILABLE;
    return UPD_OK;
}

void MaybeRestart()
{
    if (!g_noRestart && (g_forceRestart || (g_cfg.restart && g_hostWasRunning)))
        LaunchHost();
}

static HANDLE TakeLock()
{
    wchar_t name[UPD_PATH + 32], dir[UPD_PATH];
    CopyW(dir, g_cfg.appDir, UPD_PATH);
    CharLowerW(dir);
    DWORD h = 5381;
    for (wchar_t *s = dir; *s; ++s)
        h = h * 33 + *s;
    swprintf_s(name, UPD_PATH + 32, L"Local\\iliz_updater.%08lx", h);
    HANDLE m = CreateMutexW(0, TRUE, name);
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return 0;
    }
    return m;
}

static const char *kUsage =
    "iliz_updater " UPD_VERSION_A "\n"
    "  (no args)                  interactive: check, ask, download and install\n"
    "  --check [--json] [--if-due]  check only; exit 0 up to date, 1 update available\n"
    "  --update [--silent]        check + download + install (2 = updated)\n"
    "  --download-only            check + download + unpack, install later with --apply\n"
    "  --apply [--silent]         install an already downloaded update\n"
    "  --pid <n>                  host process to wait for / close before installing\n"
    "  --restart | --no-restart   start the host after installing (default: from config)\n"
    "  --current-version <v>      override the installed version\n"
    "  --force                    treat the latest release as newer\n"
    "  --config <path>            config file (default: iliz_updater.json next to exe)\n"
    "  --json                     machine-readable result on stdout\n"
    "  --version                  print updater version";

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL check = FALSE, update = FALSE, apply = FALSE, dlOnly = FALSE, silent = FALSE, ifDue = FALSE, force = FALSE;
    DWORD pid = 0;
    const wchar_t *cfgPath = 0, *curVer = 0;
    for (int i = 1; i < argc; ++i) {
        const wchar_t *a = argv[i];
        BOOL more = i + 1 < argc;
        if (!wcscmp(a, L"--check")) check = TRUE;
        else if (!wcscmp(a, L"--update")) update = TRUE;
        else if (!wcscmp(a, L"--apply")) apply = TRUE;
        else if (!wcscmp(a, L"--download-only")) dlOnly = TRUE;
        else if (!wcscmp(a, L"--silent")) silent = TRUE;
        else if (!wcscmp(a, L"--json")) s_json = TRUE;
        else if (!wcscmp(a, L"--if-due")) ifDue = TRUE;
        else if (!wcscmp(a, L"--force")) force = TRUE;
        else if (!wcscmp(a, L"--restart")) g_forceRestart = TRUE;
        else if (!wcscmp(a, L"--no-restart")) g_noRestart = TRUE;
        else if (!wcscmp(a, L"--pid") && more) pid = (DWORD)_wtoi(argv[++i]);
        else if (!wcscmp(a, L"--config") && more) cfgPath = argv[++i];
        else if (!wcscmp(a, L"--current-version") && more) curVer = argv[++i];
        else if (!wcscmp(a, L"--version")) { Out(UPD_VERSION_A); return 0; }
        else if (!wcscmp(a, L"--help") || !wcscmp(a, L"-h") || !wcscmp(a, L"/?")) { Out(kUsage); return 0; }
        else {
            char u[256];
            W2U(a, u, sizeof(u));
            SetErr("unknown argument %s", u);
            return Emit("error", UPD_ERR_ARGS, 0);
        }
    }
    if (dlOnly)
        silent = TRUE;
    if (check || s_json)
        silent = silent || check;

    Config *c = &g_cfg;
    GetModuleFileNameW(0, c->selfPath, UPD_PATH);
    CopyW(c->selfDir, c->selfPath, UPD_PATH);
    *wcsrchr(c->selfDir, L'\\') = 0;
    wchar_t defCfg[UPD_PATH];
    CopyW(defCfg, c->selfPath, UPD_PATH);
    wchar_t *dot = wcsrchr(defCfg, L'.');
    if (dot && dot > wcsrchr(defCfg, L'\\'))
        *dot = 0;
    wcsncat_s(defCfg, UPD_PATH, L".json", _TRUNCATE);
    wchar_t cfgFull[UPD_PATH];
    if (cfgPath)
        GetFullPathNameW(cfgPath, UPD_PATH, cfgFull, 0);
    else
        CopyW(cfgFull, defCfg, UPD_PATH);

    int rc = LoadConfig(cfgFull);
    if (rc != UPD_OK) {
        if (!silent) {
            wchar_t m[600];
            swprintf_s(m, 600, L"%hs", g_err);
            MessageBoxW(0, m, L"iliz updater", MB_ICONERROR);
        }
        return Emit("error", rc, 0);
    }
    LogOpen();
    {
        char a[1024];
        W2U(GetCommandLineW(), a, sizeof(a));
        Log("start %s", a);
    }
    if (curVer)
        W2U(curVer, g_current, sizeof(g_current));
    else
        ReadCurrentVersion(g_current, sizeof(g_current));
    if (pid)
        AttachHostPid(pid);

    if (check && !update && !apply && !dlOnly) {
        HANDLE lk = TakeLock();
        if (lk) {
            CleanupOld();
            CloseHandle(lk);
        }
        if (ifDue && !Due())
            return Emit("skipped", UPD_SKIPPED, 0);
        Release rel;
        rc = Check(&rel, force);
        const char *st = rc == UPD_UPDATE_AVAILABLE ? "update_available"
                         : rc == UPD_OK              ? (rel.version[0] ? "up_to_date" : "no_release")
                                                     : "error";
        if (rc == UPD_UPDATE_AVAILABLE && !rel.zipUrl[0]) {
            SetErr("release %s has no asset matching the configured pattern", rel.version);
            rc = UPD_ERR_SOURCE;
            st = "error";
        }
        Emit(st, rc, &rel);
        FreeRelease(&rel);
        return rc;
    }

    HANDLE lock = TakeLock();
    if (!lock) {
        SetErr("another updater is already running for this app");
        return Emit("error", UPD_ERR_BUSY, 0);
    }
    Recover();
    CleanupOld();

    if (!silent && !apply)
        return RunInteractive(FALSE);

    if (apply) {
        char ver[64];
        rc = ApplyReady(ver, sizeof(ver));
        if (rc == UPD_UPDATED) {
            CopyA(g_current, ver, sizeof(g_current));
            MaybeRestart();
        }
        if (rc == UPD_OK && !ver[0])
            return Emit("nothing_to_apply", UPD_OK, 0);
        return Emit(rc == UPD_UPDATED ? "updated" : "error", rc, 0);
    }

    if (ifDue && !Due())
        return Emit("skipped", UPD_SKIPPED, 0);
    Release rel;
    rc = Check(&rel, force);
    if (rc == UPD_UPDATE_AVAILABLE) {
        rc = PrepareUpdate(&rel);
        if (rc == UPD_OK && dlOnly) {
            rc = Emit("ready", UPD_UPDATE_AVAILABLE, &rel);
        } else if (rc == UPD_OK) {
            char ver[64];
            rc = ApplyReady(ver, sizeof(ver));
            if (rc == UPD_UPDATED) {
                CopyA(g_current, ver, sizeof(g_current));
                MaybeRestart();
                Emit("updated", rc, &rel);
            } else {
                Emit("error", rc, &rel);
            }
        } else {
            Emit("error", rc, &rel);
        }
    } else {
        Emit(rc == UPD_OK ? (rel.version[0] ? "up_to_date" : "no_release") : "error", rc, &rel);
    }
    FreeRelease(&rel);
    ReleaseMutex(lock);
    return rc;
}
