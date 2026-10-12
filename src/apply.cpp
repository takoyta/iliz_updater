#include "common.h"
#include <tlhelp32.h>

#define OLD_SUFFIX L".iliz_old"
#define MAX_HOSTS 16

BOOL g_hostWasRunning;
static HANDLE s_pidHandle;
static BOOL s_selfMoved;  // host passed with --pid; held open so the pid cannot be reused under us

static void WorkPath(wchar_t *out, const wchar_t *name)
{
    JoinPath(out, g_cfg.workDir, name, UPD_PATH);
}

static BOOL FileId(const wchar_t *p, BY_HANDLE_FILE_INFORMATION *fi)
{
    HANDLE f = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    BOOL ok = GetFileInformationByHandle(f, fi);
    CloseHandle(f);
    return ok;
}

// Compare by file identity, not by string: subst/junction/RAM-disk paths resolve differently.
static BOOL ProcIsHost(HANDLE h)
{
    wchar_t img[UPD_PATH];
    DWORD n = UPD_PATH;
    if (!QueryFullProcessImageNameW(h, 0, img, &n)) {
        Log("cannot query process image (%lu)", GetLastError());
        return FALSE;
    }
    if (!_wcsicmp(img, g_cfg.hostPath))
        return TRUE;
    // Some drivers (ImDisk, some VHD/RAM disks) report "\Device\X\..." instead of a drive path.
    wchar_t dev[UPD_PATH + 16];
    if (!_wcsnicmp(img, L"\\Device\\", 8))
        swprintf_s(dev, UPD_PATH + 16, L"\\\\?\\GLOBALROOT%ls", img);
    else
        CopyW(dev, img, UPD_PATH);
    BY_HANDLE_FILE_INFORMATION a, b;
    return FileId(dev, &a) && FileId(g_cfg.hostPath, &b) && a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

void AttachHostPid(DWORD pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
        Log("pid %lu already gone", pid);
        return;
    }
    if (!ProcIsHost(h)) {
        Log("pid %lu is not %ls, ignored", pid, g_cfg.hostExe);
        CloseHandle(h);
        return;
    }
    s_pidHandle = h;
}

// All running instances of the host exe (same full path), plus the --pid one.
static int OpenHosts(HANDLE *out)
{
    int n = 0;
    if (s_pidHandle && WaitForSingleObject(s_pidHandle, 0) == WAIT_TIMEOUT)
        out[n++] = s_pidHandle;
    DWORD skip = s_pidHandle ? GetProcessId(s_pidHandle) : 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return n;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok && n < MAX_HOSTS; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == skip || _wcsicmp(pe.szExeFile, wcsrchr(g_cfg.hostPath, L'\\') + 1))
            continue;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE,
                               pe.th32ProcessID);
        if (!h)
            continue;
        if (ProcIsHost(h))
            out[n++] = h;
        else
            CloseHandle(h);
    }
    CloseHandle(snap);
    return n;
}

static void CloseHosts(HANDLE *h, int n)
{
    for (int i = 0; i < n; ++i)
        if (h[i] != s_pidHandle)
            CloseHandle(h[i]);
}

BOOL HostIsRunning()
{
    HANDLE h[MAX_HOSTS];
    int n = OpenHosts(h);
    CloseHosts(h, n);
    return n > 0;
}

static BOOL CALLBACK CloseWnd(HWND w, LPARAM pid)
{
    DWORD p = 0;
    GetWindowThreadProcessId(w, &p);
    if (p == (DWORD)pid)
        PostMessageW(w, WM_CLOSE, 0, 0);
    return TRUE;
}

static BOOL WaitAll(HANDLE *h, int n, DWORD ms)
{
    return n == 0 || WaitForMultipleObjects((DWORD)n, h, TRUE, ms) != WAIT_TIMEOUT;
}

// Ask the host to quit: named event (see iliz_updater_client.h) + WM_CLOSE to its top-level windows.
static int StopHost(BOOL *wasRunning)
{
    HANDLE h[MAX_HOSTS];
    int n = OpenHosts(h);
    *wasRunning = n > 0;
    if (!n)
        return UPD_OK;
    Log("waiting for %d host process(es)", n);
    int rc = UPD_OK;
    if (!WaitAll(h, n, 1500)) {
        if (g_cfg.closeHost >= 1) {
            for (int i = 0; i < n; ++i) {
                DWORD pid = GetProcessId(h[i]);
                wchar_t name[64];
                swprintf_s(name, 64, L"Local\\iliz_updater_quit.%lu", pid);
                HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
                if (ev) {
                    SetEvent(ev);
                    CloseHandle(ev);
                }
                EnumWindows(CloseWnd, (LPARAM)pid);
            }
        }
        if (!WaitAll(h, n, (DWORD)g_cfg.closeTimeoutSec * 1000)) {
            if (g_cfg.closeHost == 2) {
                Log("host did not exit, terminating");
                for (int i = 0; i < n; ++i)
                    TerminateProcess(h[i], 1);
                WaitAll(h, n, 5000);
            }
            if (!WaitAll(h, n, 0)) {
                SetErr("%ls is still running", g_cfg.hostExe);
                rc = UPD_ERR_HOST_BUSY;
            }
        }
    }
    CloseHosts(h, n);
    Sleep(300);  // let the loader release mapped DLLs
    return rc;
}

static void PendingDelete(const wchar_t *path)
{
    wchar_t list[UPD_PATH];
    WorkPath(list, L"pending_delete.txt");
    char u[UPD_PATH * 3];
    W2U(path, u, sizeof(u));
    AppendLine(list, u);
}

// Rename a locked file to "<name>.iliz_old" (or "<name>.N.iliz_old" if that one is busy too).
static BOOL MoveAside(const wchar_t *p)
{
    for (int i = 0; i < 10; ++i) {
        wchar_t old[UPD_PATH];
        if (i)
            swprintf_s(old, UPD_PATH, L"%ls.%d" OLD_SUFFIX, p, i);
        else
            swprintf_s(old, UPD_PATH, L"%ls" OLD_SUFFIX, p);
        SetFileAttributesW(old, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(old);
        if (MoveFileExW(p, old, MOVEFILE_REPLACE_EXISTING)) {
            PendingDelete(old);
            return TRUE;
        }
    }
    return FALSE;
}

// Copy src over dst. A locked dst (running exe, loaded dll) is renamed aside first.
static BOOL PutFile(const wchar_t *src, const wchar_t *dst)
{
    // Our own running image cannot be overwritten, only renamed. Once moved aside, the
    // file at selfPath is an ordinary file again (matters for rollback).
    BOOL self = !_wcsicmp(dst, g_cfg.selfPath) && !s_selfMoved;
    for (int i = 0; !self && i < 15; ++i) {
        if (CopyFileW(src, dst, FALSE))
            return TRUE;
        DWORD e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION)
            break;
        Sleep(200);
    }
    if (FileExists(dst)) {
        if (!MoveAside(dst)) {
            Log("cannot move aside %ls (%lu)", dst, GetLastError());
            return FALSE;
        }
        if (self)
            s_selfMoved = TRUE;
    }
    if (CopyFileW(src, dst, FALSE))
        return TRUE;
    Log("copy to %ls failed (%lu)", dst, GetLastError());
    return FALSE;
}

static BOOL RemoveFile(const wchar_t *p)
{
    SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(p) || GetLastError() == ERROR_FILE_NOT_FOUND)
        return TRUE;
    return MoveAside(p);
}

// Journal lines: "R <rel>" replaced (backup exists), "N <rel>" newly added.
static void Rollback(const wchar_t *journal)
{
    DWORD len = 0;
    char *t = ReadAll(journal, &len);
    if (!t)
        return;
    Log("rolling back");
    wchar_t backup[UPD_PATH];
    WorkPath(backup, L"backup");
    // walk lines backwards
    char *end = t + len;
    while (end > t) {
        char *e = end;
        while (e > t && (e[-1] == '\r' || e[-1] == '\n'))
            --e;
        char *s = e;
        while (s > t && s[-1] != '\n')
            --s;
        end = s;
        if (e - s < 3)
            continue;
        char line[UPD_PATH * 3];
        size_t n = (size_t)(e - s);
        if (n >= sizeof(line))
            continue;
        memcpy(line, s, n);
        line[n] = 0;
        wchar_t rel[UPD_PATH], dst[UPD_PATH], bak[UPD_PATH];
        U2W(line + 2, rel, UPD_PATH);
        JoinPath(dst, g_cfg.appDir, rel, UPD_PATH);
        if (line[0] == 'R') {
            JoinPath(bak, backup, rel, UPD_PATH);
            if (!PutFile(bak, dst))
                Log("rollback: cannot restore %s", line + 2);
        } else if (line[0] == 'N') {
            RemoveFile(dst);
        }
    }
    free(t);
    DeleteFileW(journal);
}

void Recover()
{
    wchar_t j[UPD_PATH];
    WorkPath(j, L"journal.txt");
    if (FileExists(j)) {
        Log("found unfinished update journal");
        Rollback(j);
        wchar_t b[UPD_PATH];
        WorkPath(b, L"backup");
        DeleteTree(b);
    }
}

void CleanupOld()
{
    wchar_t list[UPD_PATH];
    WorkPath(list, L"pending_delete.txt");
    DWORD len = 0;
    char *t = ReadAll(list, &len);
    if (!t)
        return;
    DeleteFileW(list);
    for (char *line = strtok(t, "\r\n"); line; line = strtok(0, "\r\n")) {
        wchar_t p[UPD_PATH];
        U2W(line, p, UPD_PATH);
        size_t n = wcslen(p), sn = wcslen(OLD_SUFFIX);
        if (n <= sn || _wcsicmp(p + n - sn, OLD_SUFFIX))
            continue;  // only ever delete our own *.iliz_old files
        SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(p) && GetLastError() != ERROR_FILE_NOT_FOUND)
            AppendLine(list, line);
    }
    free(t);
}

struct FileList {
    wchar_t (*rel)[MAX_PATH];
    int n, cap;
};

static void Collect(const wchar_t *root, const wchar_t *sub, FileList *fl)
{
    wchar_t dir[UPD_PATH], mask[UPD_PATH];
    JoinPath(dir, root, sub, UPD_PATH);
    JoinPath(mask, dir, L"*", UPD_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(mask, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        wchar_t rel[MAX_PATH];
        if (sub[0])
            JoinPath(rel, sub, fd.cFileName, MAX_PATH);
        else
            CopyW(rel, fd.cFileName, MAX_PATH);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Collect(root, rel, fl);
            continue;
        }
        if (fl->n == fl->cap) {
            int nc = fl->cap ? fl->cap * 2 : 64;
            void *np = realloc(fl->rel, sizeof(*fl->rel) * nc);
            if (!np)
                break;
            fl->rel = (wchar_t(*)[MAX_PATH])np;
            fl->cap = nc;
        }
        CopyW(fl->rel[fl->n++], rel, MAX_PATH);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static BOOL CanWrite()
{
    if (!MakeDirs(g_cfg.workDir))
        return FALSE;
    wchar_t probe[UPD_PATH];
    JoinPath(probe, g_cfg.appDir, L".iliz_updater_probe", UPD_PATH);
    HANDLE f = CreateFileW(probe, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, 0);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    CloseHandle(f);
    return TRUE;
}

// Install update\ready into the app folder. installed receives the version.
int ApplyReady(char *installed, int cap)
{
    wchar_t readyPath[UPD_PATH];
    WorkPath(readyPath, L"ready");
    DWORD len = 0;
    char *t = ReadAll(readyPath, &len);
    if (!t) {
        installed[0] = 0;
        return UPD_OK;  // nothing prepared
    }
    char *ver = strtok(t, "\r\n");
    char *root8 = strtok(0, "\r\n");
    CopyA(installed, ver ? ver : "", cap);
    wchar_t rootRel[MAX_PATH], root[UPD_PATH];
    U2W(root8 ? root8 : "temp", rootRel, MAX_PATH);
    free(t);
    JoinPath(root, g_cfg.workDir, rootRel, UPD_PATH);
    if (!DirExists(root)) {
        DeleteFileW(readyPath);
        SetErr("prepared package is missing");
        return UPD_ERR_PACKAGE;
    }
    if (!CanWrite()) {
        SetErr("no write access to the app folder");
        return UPD_ERR_ACCESS;
    }
    BOOL wasRunning = FALSE;
    int rc = StopHost(&wasRunning);
    if (rc != UPD_OK)
        return rc;

    FileList fl = {0, 0, 0};
    Collect(root, L"", &fl);
    wchar_t journal[UPD_PATH], backup[UPD_PATH];
    WorkPath(journal, L"journal.txt");
    WorkPath(backup, L"backup");
    DeleteTree(backup);
    DeleteFileW(journal);
    Log("applying %s: %d files", installed, fl.n);
    rc = UPD_OK;
    int copied = 0;
    for (int i = 0; i < fl.n && rc == UPD_OK; ++i) {
        const wchar_t *rel = fl.rel[i];
        wchar_t src[UPD_PATH], dst[UPD_PATH], bak[UPD_PATH];
        JoinPath(src, root, rel, UPD_PATH);
        JoinPath(dst, g_cfg.appDir, rel, UPD_PATH);
        BOOL exists = FileExists(dst);
        if (exists && IsPreserved(rel)) {
            Log("keep %ls", rel);
            continue;
        }
        char line[UPD_PATH * 3];
        line[0] = exists ? 'R' : 'N';
        line[1] = ' ';
        W2U(rel, line + 2, sizeof(line) - 2);
        if (exists) {
            JoinPath(bak, backup, rel, UPD_PATH);
            if (!MakeParentDirs(bak) || !CopyFileW(dst, bak, FALSE)) {
                SetErr("cannot back up %ls (%lu)", rel, GetLastError());
                rc = UPD_ERR_APPLY;
                break;
            }
        } else if (!MakeParentDirs(dst)) {
            SetErr("cannot create folder for %ls", rel);
            rc = UPD_ERR_APPLY;
            break;
        }
        AppendLine(journal, line);
        if (!PutFile(src, dst)) {
            SetErr("cannot replace %ls (%lu)", rel, GetLastError());
            rc = UPD_ERR_APPLY;
            break;
        }
        ++copied;
    }
    free(fl.rel);
    if (rc != UPD_OK) {
        Rollback(journal);
    } else {
        DeleteFileW(journal);
        Log("applied %d files", copied);
    }
    DeleteTree(backup);
    DeleteFileW(readyPath);
    wchar_t temp[UPD_PATH];
    WorkPath(temp, L"temp");
    DeleteTree(temp);
    CleanupOld();
    g_hostWasRunning = wasRunning;
    return rc == UPD_OK ? UPD_UPDATED : rc;
}

BOOL LaunchHost()
{
    wchar_t cmd[UPD_PATH + 600];
    swprintf_s(cmd, sizeof(cmd) / sizeof(cmd[0]), L"\"%ls\" %ls", g_cfg.hostPath, g_cfg.restartArgs);
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(g_cfg.hostPath, cmd, 0, 0, FALSE, 0, 0, g_cfg.appDir, &si, &pi)) {
        Log("cannot start host (%lu)", GetLastError());
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log("host started");
    return TRUE;
}
