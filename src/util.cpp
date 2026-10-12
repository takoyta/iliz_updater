#include "common.h"
#include <bcrypt.h>

Config g_cfg;
Progress g_prog;
char g_err[512];
char g_current[64];

static wchar_t s_logPath[UPD_PATH];

void SetErr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g_err, sizeof(g_err), _TRUNCATE, fmt, ap);
    va_end(ap);
    Log("error: %s", g_err);
}

void LogOpen()
{
    s_logPath[0] = 0;
    if (!g_cfg.log || !MakeDirs(g_cfg.workDir))
        return;
    JoinPath(s_logPath, g_cfg.workDir, L"updater.log", UPD_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(s_logPath, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 256 * 1024)
        DeleteFileW(s_logPath);
}

void Log(const char *fmt, ...)
{
    if (!s_logPath[0])
        return;
    char line[1200];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d [%lu] ", t.wYear, t.wMonth,
                        t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    if (n < 0)
        n = 0;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    AppendLine(s_logPath, line);
}

void U2W(const char *s, wchar_t *out, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap))
        out[0] = 0;
    out[cap - 1] = 0;
}

void W2U(const wchar_t *s, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, s ? s : L"", -1, out, cap, 0, 0))
        out[0] = 0;
    out[cap - 1] = 0;
}

void CopyW(wchar_t *dst, const wchar_t *src, int cap)
{
    wcsncpy_s(dst, cap, src ? src : L"", _TRUNCATE);
}

void CopyA(char *dst, const char *src, int cap)
{
    strncpy_s(dst, cap, src ? src : "", _TRUNCATE);
}

void JoinPath(wchar_t *out, const wchar_t *a, const wchar_t *b, int cap)
{
    wchar_t tmp[UPD_PATH];
    CopyW(tmp, a, UPD_PATH);
    size_t n = wcslen(tmp);
    if (n && tmp[n - 1] != L'\\' && tmp[n - 1] != L'/' && b[0])
        wcsncat_s(tmp, UPD_PATH, L"\\", _TRUNCATE);
    wcsncat_s(tmp, UPD_PATH, b, _TRUNCATE);
    CopyW(out, tmp, cap);
}

BOOL FileExists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

BOOL DirExists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

BOOL MakeDirs(const wchar_t *p)
{
    if (DirExists(p))
        return TRUE;
    wchar_t tmp[UPD_PATH];
    CopyW(tmp, p, UPD_PATH);
    for (wchar_t *s = tmp + 3; *s; ++s) {
        if (*s == L'\\' || *s == L'/') {
            wchar_t c = *s;
            *s = 0;
            CreateDirectoryW(tmp, 0);
            *s = c;
        }
    }
    CreateDirectoryW(tmp, 0);
    return DirExists(p);
}

BOOL MakeParentDirs(const wchar_t *file)
{
    wchar_t dir[UPD_PATH];
    CopyW(dir, file, UPD_PATH);
    wchar_t *s = wcsrchr(dir, L'\\');
    if (!s)
        return TRUE;
    *s = 0;
    return MakeDirs(dir);
}

void DeleteTree(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES)
        return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p);
        return;
    }
    wchar_t mask[UPD_PATH];
    JoinPath(mask, p, L"*", UPD_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(mask, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            wchar_t sub[UPD_PATH];
            JoinPath(sub, p, fd.cFileName, UPD_PATH);
            DeleteTree(sub);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(p);
}

char *ReadAll(const wchar_t *p, DWORD *len)
{
    *len = 0;
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER sz;
    char *buf = 0;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart < 512LL * 1024 * 1024) {
        buf = (char *)malloc((size_t)sz.QuadPart + 1);
        DWORD got = 0;
        if (buf && ReadFile(f, buf, (DWORD)sz.QuadPart, &got, 0) && got == (DWORD)sz.QuadPart) {
            buf[got] = 0;
            *len = got;
        } else {
            free(buf);
            buf = 0;
        }
    }
    CloseHandle(f);
    return buf;
}

BOOL WriteAll(const wchar_t *p, const void *data, DWORD len)
{
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data, len, &w, 0) && w == len && FlushFileBuffers(f);
    CloseHandle(f);
    return ok;
}

BOOL AppendLine(const wchar_t *p, const char *line)
{
    HANDLE f = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD w = 0;
    BOOL ok = WriteFile(f, line, (DWORD)strlen(line), &w, 0) && WriteFile(f, "\r\n", 2, &w, 0);
    FlushFileBuffers(f);
    CloseHandle(f);
    return ok;
}

BOOL Sha256File(const wchar_t *p, char hex[65])
{
    hex[0] = 0;
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    BCRYPT_ALG_HANDLE alg = 0;
    BCRYPT_HASH_HANDLE h = 0;
    BOOL ok = FALSE;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, 0) == 0 &&
        BCryptCreateHash(alg, &h, 0, 0, 0, 0, 0) == 0) {
        static BYTE buf[64 * 1024];
        DWORD got;
        ok = TRUE;
        while (ReadFile(f, buf, sizeof(buf), &got, 0) && got) {
            if (BCryptHashData(h, buf, got, 0) != 0) {
                ok = FALSE;
                break;
            }
        }
        BYTE dig[32];
        if (ok && BCryptFinishHash(h, dig, 32, 0) == 0) {
            for (int i = 0; i < 32; ++i)
                sprintf_s(hex + i * 2, 3, "%02x", dig[i]);
        } else {
            ok = FALSE;
        }
    }
    if (h)
        BCryptDestroyHash(h);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

// "v1.2.10" > "1.2.9"; "1.2.0" == "1.2"; "1.3.0-beta" < "1.3.0".
int CompareVersions(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V')
        ++a;
    if (*b == 'v' || *b == 'V')
        ++b;
    for (;;) {
        BOOL ea = !*a || *a == '-' || *a == '+';
        BOOL eb = !*b || *b == '-' || *b == '+';
        if (ea && eb)
            break;
        unsigned long long x = 0, y = 0;
        while (*a >= '0' && *a <= '9')
            x = x * 10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9')
            y = y * 10 + (*b++ - '0');
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.')
            ++a;
        if (*b == '.')
            ++b;
        if (!ea && *a && *a != '-' && *a != '+' && (*a < '0' || *a > '9'))
            ++a;
        if (!eb && *b && *b != '-' && *b != '+' && (*b < '0' || *b > '9'))
            ++b;
    }
    BOOL pa = *a == '-', pb = *b == '-';
    if (pa != pb)
        return pa ? -1 : 1;
    if (pa) {
        int c = strcmp(a, b);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    return 0;
}
