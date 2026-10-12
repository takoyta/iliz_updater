#include "common.h"
#include <shlwapi.h>
#include "cJSON.h"

static void Trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || (BYTE)*p == 0xEF || (BYTE)*p == 0xBB ||
           (BYTE)*p == 0xBF)
        ++p;
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

static void NormVersion(const char *in, char *out, int cap)
{
    if (*in == 'v' || *in == 'V')
        ++in;
    CopyA(out, in, cap);
    Trim(out);
}

BOOL ReadCurrentVersion(char *out, int cap)
{
    out[0] = 0;
    if (g_cfg.verSource == 2) {
        W2U(g_cfg.verValue, out, cap);
    } else if (g_cfg.verSource == 1) {
        DWORD len;
        char *t = ReadAll(g_cfg.verValue, &len);
        if (t) {
            char *nl = strpbrk(t, "\r\n");
            if (nl)
                *nl = 0;
            CopyA(out, t, cap);
            free(t);
        }
    } else {
        DWORD dummy = 0, sz = GetFileVersionInfoSizeW(g_cfg.hostPath, &dummy);
        void *vi = sz ? malloc(sz) : 0;
        if (vi && GetFileVersionInfoW(g_cfg.hostPath, 0, sz, vi)) {
            struct {
                WORD lang, cp;
            } *tr = 0;
            UINT n = 0;
            if (VerQueryValueW(vi, L"\\VarFileInfo\\Translation", (void **)&tr, &n) && n >= 4) {
                wchar_t key[64];
                swprintf_s(key, 64, L"\\StringFileInfo\\%04x%04x\\ProductVersion", tr->lang, tr->cp);
                wchar_t *s = 0;
                if (VerQueryValueW(vi, key, (void **)&s, &n) && n && s)
                    W2U(s, out, cap);
            }
            if (!out[0]) {
                VS_FIXEDFILEINFO *fi = 0;
                if (VerQueryValueW(vi, L"\\", (void **)&fi, &n) && fi)
                    sprintf_s(out, cap, "%u.%u.%u.%u", HIWORD(fi->dwProductVersionMS), LOWORD(fi->dwProductVersionMS),
                              HIWORD(fi->dwProductVersionLS), LOWORD(fi->dwProductVersionLS));
            }
        }
        free(vi);
    }
    char tmp[64];
    NormVersion(out, tmp, sizeof(tmp));
    CopyA(out, tmp, cap);
    return out[0] != 0;
}

void FreeRelease(Release *rel)
{
    free(rel->notes);
    memset(rel, 0, sizeof(*rel));
}

static const char *JStr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static void SetNotes(Release *rel, const char *s)
{
    free(rel->notes);
    rel->notes = _strdup(s ? s : "");
}

static int ParseGithub(cJSON *r, Release *rel)
{
    CopyA(rel->tag, JStr(r, "tag_name"), sizeof(rel->tag));
    NormVersion(rel->tag, rel->version, sizeof(rel->version));
    CopyA(rel->published, JStr(r, "published_at"), sizeof(rel->published));
    CopyA(rel->page, JStr(r, "html_url"), sizeof(rel->page));
    SetNotes(rel, JStr(r, "body"));
    cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(r, "assets"))
    {
        wchar_t name[256], url[UPD_URL];
        U2W(JStr(a, "name"), name, 256);
        U2W(JStr(a, "browser_download_url"), url, UPD_URL);
        if (!rel->sumsUrl[0] && g_cfg.sumsAsset[0] && !_wcsicmp(name, g_cfg.sumsAsset)) {
            CopyW(rel->sumsUrl, url, UPD_URL);
        } else if (!rel->zipUrl[0] && PathMatchSpecW(name, g_cfg.asset)) {
            CopyW(rel->zipUrl, url, UPD_URL);
            CopyW(rel->zipName, name, 256);
            cJSON *sz = cJSON_GetObjectItemCaseSensitive(a, "size");
            rel->size = cJSON_IsNumber(sz) ? (long long)sz->valuedouble : 0;
            const char *dg = JStr(a, "digest");  // GitHub: "sha256:<hex>"
            if (!_strnicmp(dg, "sha256:", 7) && strlen(dg + 7) == 64)
                CopyA(rel->sha256, dg + 7, sizeof(rel->sha256));
        }
    }
    if (!rel->version[0]) {
        SetErr("release has no tag_name");
        return UPD_ERR_SOURCE;
    }
    return UPD_OK;
}

// Resolve a possibly relative package url against the latest.json location.
static void ResolveUrl(const wchar_t *base, const wchar_t *ref, wchar_t *out)
{
    if (wcsstr(ref, L"://") || (ref[0] && ref[1] == L':') || (ref[0] == L'\\' && ref[1] == L'\\')) {
        CopyW(out, ref, UPD_URL);
        return;
    }
    CopyW(out, base, UPD_URL);
    wchar_t *q = wcschr(out, L'?');
    if (q)
        *q = 0;
    wchar_t *s1 = wcsrchr(out, L'/'), *s2 = wcsrchr(out, L'\\');
    wchar_t *s = s1 > s2 ? s1 : s2;
    if (s)
        s[1] = 0;
    wcsncat_s(out, UPD_URL, ref, _TRUNCATE);
}

static int ParseJson(cJSON *r, Release *rel)
{
    NormVersion(JStr(r, "version"), rel->version, sizeof(rel->version));
    CopyA(rel->tag, JStr(r, "version"), sizeof(rel->tag));
    CopyA(rel->published, JStr(r, "published"), sizeof(rel->published));
    CopyA(rel->page, JStr(r, "page"), sizeof(rel->page));
    SetNotes(rel, JStr(r, "notes"));
    const char *sha = JStr(r, "sha256");
    if (strlen(sha) == 64)
        CopyA(rel->sha256, sha, sizeof(rel->sha256));
    cJSON *sz = cJSON_GetObjectItemCaseSensitive(r, "size");
    rel->size = cJSON_IsNumber(sz) ? (long long)sz->valuedouble : 0;
    wchar_t ref[UPD_URL];
    U2W(JStr(r, "url"), ref, UPD_URL);
    if (ref[0]) {
        ResolveUrl(g_cfg.url, ref, rel->zipUrl);
        const wchar_t *n1 = wcsrchr(rel->zipUrl, L'/'), *n2 = wcsrchr(rel->zipUrl, L'\\');
        const wchar_t *n = n1 > n2 ? n1 : n2;
        CopyW(rel->zipName, n ? n + 1 : rel->zipUrl, 256);
    }
    U2W(JStr(r, "sha256_url"), ref, UPD_URL);
    if (ref[0])
        ResolveUrl(g_cfg.url, ref, rel->sumsUrl);
    if (!rel->version[0]) {
        SetErr("latest.json has no version");
        return UPD_ERR_SOURCE;
    }
    return UPD_OK;
}

// Returns UPD_OK with rel filled, UPD_ERR_* on failure, or UPD_OK with empty version when no release exists.
int FetchRelease(Release *rel)
{
    memset(rel, 0, sizeof(*rel));
    wchar_t url[UPD_URL];
    BOOL gh = g_cfg.srcType == 0;
    if (gh)
        swprintf_s(url, UPD_URL, g_cfg.prerelease ? L"%ls/repos/%ls/releases?per_page=20" : L"%ls/repos/%ls/releases/latest",
                   g_cfg.api, g_cfg.repo);
    else
        CopyW(url, g_cfg.url, UPD_URL);
    char *body = 0;
    DWORD len = 0;
    int status = 0;
    int rc = NetGetMem(url, gh, &body, &len, &status);
    if (rc != UPD_OK)
        return rc;
    if (status == 404) {
        free(body);
        Log("no release published (404)");
        return UPD_OK;
    }
    if (status != 200) {
        char snippet[160] = "";
        if (body) {
            CopyA(snippet, body, sizeof(snippet));
            for (char *p = snippet; *p; ++p)
                if (*p == '\r' || *p == '\n')
                    *p = ' ';
        }
        free(body);
        SetErr("update source answered HTTP %d %s", status, snippet);
        return status == 403 || status == 429 || status >= 500 ? UPD_ERR_NETWORK : UPD_ERR_SOURCE;
    }
    const char *p = body;
    if (len >= 3 && (BYTE)p[0] == 0xEF && (BYTE)p[1] == 0xBB && (BYTE)p[2] == 0xBF)
        p += 3;
    cJSON *root = cJSON_Parse(p);
    free(body);
    if (!root) {
        SetErr("update source returned invalid JSON");
        return UPD_ERR_SOURCE;
    }
    if (gh && cJSON_IsArray(root)) {
        cJSON *r, *pick = 0;
        cJSON_ArrayForEach(r, root)
        {
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "draft"))) {
                pick = r;
                break;
            }
        }
        rc = pick ? ParseGithub(pick, rel) : UPD_OK;
    } else if (cJSON_IsObject(root)) {
        rc = gh ? ParseGithub(root, rel) : ParseJson(root, rel);
    } else {
        SetErr("unexpected JSON from update source");
        rc = UPD_ERR_SOURCE;
    }
    cJSON_Delete(root);
    Log("latest: %s", rel->version[0] ? rel->version : "(none)");
    return rc;
}
