#include "common.h"
#include <winhttp.h>

#define MAX_MEM_RESPONSE (8 * 1024 * 1024)

static BOOL IsHttp(const wchar_t *url)
{
    return !_wcsnicmp(url, L"https://", 8) || !_wcsnicmp(url, L"http://", 7);
}

// file:///C:/x or plain path -> local path (handy for LAN shares and testing).
static void LocalPath(const wchar_t *url, wchar_t *out)
{
    const wchar_t *p = url;
    if (!_wcsnicmp(p, L"file:///", 8))
        p += 8;
    else if (!_wcsnicmp(p, L"file://", 7))
        p += 5;  // file://server/share -> //server/share
    CopyW(out, p, UPD_PATH);
    for (wchar_t *s = out; *s; ++s)
        if (*s == L'/')
            *s = L'\\';
}

typedef BOOL (*Sink)(void *ctx, const BYTE *data, DWORD len);

static int Get(const wchar_t *url, BOOL githubApi, Sink sink, void *ctx, int *status)
{
    *status = 0;
    char u8[UPD_URL];
    W2U(url, u8, sizeof(u8));
    Log("GET %s", u8);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[UPD_URL];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = UPD_URL;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        SetErr("bad url %s", u8);
        return UPD_ERR_CONFIG;
    }
    if (!path[0])
        CopyW(path, L"/", UPD_URL);  // path includes ?query (lpszExtraInfo not split off)

    int rc = UPD_ERR_NETWORK;
    HINTERNET ses = WinHttpOpen(UPD_WIDEN(UPD_INTERNAL_A) L"/" UPD_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = 0, req = 0;
    if (ses) {
        DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protos |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        if (!WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos))) {
            protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
        }
        WinHttpSetTimeouts(ses, 10000, 15000, 30000, 60000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, L"GET", path, 0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    const wchar_t *hdr = githubApi ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                   : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (req && WinHttpSendRequest(req, hdr, (DWORD)-1L, 0, 0, 0, 0) && WinHttpReceiveResponse(req, 0)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &sz, WINHTTP_NO_HEADER_INDEX);
        *status = (int)code;
        wchar_t cl[32];
        sz = sizeof(cl);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &sz,
                                WINHTTP_NO_HEADER_INDEX))
            InterlockedExchange64(&g_prog.total, _wtoi64(cl));
        rc = UPD_OK;
        static BYTE buf[64 * 1024];
        for (;;) {
            if (g_prog.cancel) {
                rc = UPD_CANCELLED;
                break;
            }
            DWORD got = 0;
            if (!WinHttpReadData(req, buf, sizeof(buf), &got)) {
                rc = UPD_ERR_NETWORK;
                SetErr("network read failed (%lu)", GetLastError());
                break;
            }
            if (!got)
                break;
            InterlockedAdd64(&g_prog.done, got);
            if (!sink(ctx, buf, got)) {
                rc = UPD_ERR_NETWORK;
                break;
            }
        }
        Log("HTTP %d", *status);
    } else {
        SetErr("cannot connect to %s (error %lu)", u8, GetLastError());
    }
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    if (ses)
        WinHttpCloseHandle(ses);
    return rc;
}

struct MemBuf {
    char *p;
    DWORD len, cap;
};

static BOOL MemSink(void *ctx, const BYTE *data, DWORD len)
{
    MemBuf *m = (MemBuf *)ctx;
    if (m->len + len + 1 > MAX_MEM_RESPONSE) {
        SetErr("response too large");
        return FALSE;
    }
    if (m->len + len + 1 > m->cap) {
        DWORD nc = m->cap ? m->cap * 2 : 64 * 1024;
        while (nc < m->len + len + 1)
            nc *= 2;
        char *np = (char *)realloc(m->p, nc);
        if (!np)
            return FALSE;
        m->p = np;
        m->cap = nc;
    }
    memcpy(m->p + m->len, data, len);
    m->len += len;
    m->p[m->len] = 0;
    return TRUE;
}

static BOOL FileSink(void *ctx, const BYTE *data, DWORD len)
{
    DWORD w = 0;
    if (!WriteFile(*(HANDLE *)ctx, data, len, &w, 0) || w != len) {
        SetErr("cannot write download (disk full?)");
        return FALSE;
    }
    return TRUE;
}

int NetGetMem(const wchar_t *url, BOOL githubApi, char **out, DWORD *len, int *status)
{
    *out = 0;
    *len = 0;
    if (!IsHttp(url)) {
        wchar_t p[UPD_PATH];
        LocalPath(url, p);
        *out = ReadAll(p, len);
        *status = *out ? 200 : 404;
        return UPD_OK;
    }
    MemBuf m = {0, 0, 0};
    int rc = Get(url, githubApi, MemSink, &m, status);
    if (rc != UPD_OK) {
        free(m.p);
        return rc;
    }
    if (!m.p)
        m.p = (char *)calloc(1, 1);
    *out = m.p;
    *len = m.len;
    return UPD_OK;
}

int NetDownload(const wchar_t *url, const wchar_t *dest, int *status)
{
    InterlockedExchange64(&g_prog.done, 0);
    if (!IsHttp(url)) {
        wchar_t p[UPD_PATH];
        LocalPath(url, p);
        *status = CopyFileW(p, dest, FALSE) ? 200 : 404;
        if (*status == 200)
            InterlockedExchange64(&g_prog.done, g_prog.total);
        return UPD_OK;
    }
    HANDLE f = CreateFileW(dest, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) {
        SetErr("cannot create %ls", dest);
        return UPD_ERR_ACCESS;
    }
    int rc = Get(url, FALSE, FileSink, &f, status);
    CloseHandle(f);
    if (rc != UPD_OK || *status != 200)
        DeleteFileW(dest);
    return rc;
}
