#include "common.h"
#include <shlwapi.h>
#include "cJSON.h"

static void Str(cJSON *o, const char *key, wchar_t *out, int cap, const wchar_t *def)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring)
        U2W(v->valuestring, out, cap);
    else
        CopyW(out, def, cap);
}

static int Int(cJSON *o, const char *key, int def)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static BOOL Bool(cJSON *o, const char *key, BOOL def)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static void FullPath(const wchar_t *base, const wchar_t *rel, wchar_t *out)
{
    wchar_t tmp[UPD_PATH];
    if (PathIsRelativeW(rel))
        JoinPath(tmp, base, rel, UPD_PATH);
    else
        CopyW(tmp, rel, UPD_PATH);
    if (!GetFullPathNameW(tmp, UPD_PATH, out, 0))
        CopyW(out, tmp, UPD_PATH);
    size_t n = wcslen(out);
    if (n > 3 && out[n - 1] == L'\\')
        out[n - 1] = 0;
}

int LoadConfig(const wchar_t *path)
{
    Config *c = &g_cfg;
    CopyW(c->configPath, path, UPD_PATH);
    DWORD len = 0;
    char *text = ReadAll(path, &len);
    if (!text) {
        char p[UPD_PATH];
        W2U(path, p, sizeof(p));
        SetErr("cannot read config %s", p);
        return UPD_ERR_CONFIG;
    }
    const char *body = text;
    if (len >= 3 && (BYTE)text[0] == 0xEF && (BYTE)text[1] == 0xBB && (BYTE)text[2] == 0xBF)
        body += 3;
    cJSON *root = cJSON_Parse(body);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        SetErr("config is not valid JSON");
        return UPD_ERR_CONFIG;
    }

    wchar_t tmp[UPD_PATH];
    Str(root, "app_dir", tmp, UPD_PATH, L".");
    FullPath(c->selfDir, tmp, c->appDir);
    Str(root, "work_dir", tmp, UPD_PATH, L"update");
    FullPath(c->appDir, tmp, c->workDir);
    Str(root, "host_exe", c->hostExe, MAX_PATH, L"");
    Str(root, "app_name", c->appName, 128, c->hostExe);
    if (c->hostExe[0])
        FullPath(c->appDir, c->hostExe, c->hostPath);

    cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    Str(ver, "source", tmp, 32, L"exe");
    c->verSource = !_wcsicmp(tmp, L"file") ? 1 : (!_wcsicmp(tmp, L"fixed") ? 2 : 0);
    Str(ver, "value", c->verValue, UPD_PATH, L"");
    if (c->verSource == 1)
        FullPath(c->appDir, c->verValue[0] ? c->verValue : L"version.txt", c->verValue);

    cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "source");
    Str(src, "type", tmp, 32, L"github");
    c->srcType = !_wcsicmp(tmp, L"json") ? 1 : 0;
    Str(src, "repo", c->repo, 256, L"");
    Str(src, "api", c->api, 256, L"https://api.github.com");
    Str(src, "asset", c->asset, 128, L"*.zip");
    Str(src, "sums_asset", c->sumsAsset, 128, L"SHA256SUMS");
    c->prerelease = Bool(src, "prerelease", FALSE);
    Str(src, "url", c->url, UPD_URL, L"");

    c->requireHash = Bool(root, "require_hash", TRUE);
    c->preserveCount = 0;
    cJSON *pr = cJSON_GetObjectItemCaseSensitive(root, "preserve");
    cJSON *it;
    cJSON_ArrayForEach(it, pr)
    {
        if (cJSON_IsString(it) && c->preserveCount < UPD_MAX_PRESERVE) {
            wchar_t *d = c->preserve[c->preserveCount++];
            U2W(it->valuestring, d, MAX_PATH);
            for (wchar_t *s = d; *s; ++s)
                if (*s == L'/')
                    *s = L'\\';
        }
    }
    c->restart = Bool(root, "restart", TRUE);
    Str(root, "restart_args", c->restartArgs, 512, L"");
    Str(root, "close_host", tmp, 32, L"graceful");
    c->closeHost = !_wcsicmp(tmp, L"none") ? 0 : (!_wcsicmp(tmp, L"kill") ? 2 : 1);
    c->closeTimeoutSec = Int(root, "close_timeout_sec", 15);
    c->checkIntervalHours = Int(root, "check_interval_hours", 24);
    Str(root, "language", tmp, 32, L"auto");
    if (!_wcsicmp(tmp, L"ru"))
        c->lang = 1;
    else if (!_wcsicmp(tmp, L"en"))
        c->lang = 0;
    else
        c->lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    c->log = Bool(root, "log", TRUE);
    cJSON_Delete(root);

    if (!c->hostExe[0]) {
        SetErr("config: host_exe is required");
        return UPD_ERR_CONFIG;
    }
    if (c->srcType == 0 && !c->repo[0]) {
        SetErr("config: source.repo is required for github source");
        return UPD_ERR_CONFIG;
    }
    if (c->srcType == 1 && !c->url[0]) {
        SetErr("config: source.url is required for json source");
        return UPD_ERR_CONFIG;
    }
    return UPD_OK;
}

// rel is a path relative to the app folder, backslash separated.
BOOL IsPreserved(const wchar_t *rel)
{
    for (int i = 0; i < g_cfg.preserveCount; ++i) {
        const wchar_t *pat = g_cfg.preserve[i];
        size_t n = wcslen(pat);
        if (n && pat[n - 1] == L'\\') {  // "data/" = whole folder
            if (!_wcsnicmp(rel, pat, n))
                return TRUE;
            continue;
        }
        if (PathMatchSpecW(rel, pat))
            return TRUE;
    }
    return FALSE;
}
