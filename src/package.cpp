#include "common.h"
#include "miniz.h"

static void WorkPath(wchar_t *out, const wchar_t *name)
{
    JoinPath(out, g_cfg.workDir, name, UPD_PATH);
}

// update\ready: line 1 = version, line 2 = package root relative to the work folder.
BOOL ReadyVersion(char *ver, int cap)
{
    ver[0] = 0;
    wchar_t p[UPD_PATH];
    WorkPath(p, L"ready");
    DWORD len;
    char *t = ReadAll(p, &len);
    if (!t)
        return FALSE;
    char *nl = strpbrk(t, "\r\n");
    if (nl)
        *nl = 0;
    CopyA(ver, t, cap);
    free(t);
    return ver[0] != 0;
}

static BOOL FindSum(const char *text, const wchar_t *name, char out[65])
{
    char want[256];
    W2U(name, want, sizeof(want));
    const char *line = text;
    while (line && *line) {
        const char *end = strpbrk(line, "\r\n");
        size_t n = end ? (size_t)(end - line) : strlen(line);
        // "<64 hex> <space|*><name>"  (sha256sum / certutil-style)
        if (n > 66) {
            const char *f = line + 64;
            while (*f == ' ' || *f == '\t' || *f == '*')
                ++f;
            size_t fn = n - (size_t)(f - line);
            if (fn == strlen(want) && !_strnicmp(f, want, fn)) {
                memcpy(out, line, 64);
                out[64] = 0;
                return TRUE;
            }
        }
        line = end ? end + 1 : 0;
        while (line && (*line == '\r' || *line == '\n'))
            ++line;
    }
    return FALSE;
}

static BOOL SafeRel(wchar_t *rel)
{
    for (wchar_t *s = rel; *s; ++s)
        if (*s == L'/')
            *s = L'\\';
    if (!rel[0] || rel[0] == L'\\' || wcschr(rel, L':'))
        return FALSE;
    const wchar_t *p = rel;
    while (*p) {
        const wchar_t *e = wcschr(p, L'\\');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        if ((n == 2 && p[0] == L'.' && p[1] == L'.'))
            return FALSE;
        if (!e)
            break;
        p = e + 1;
    }
    return TRUE;
}

static size_t WriteCb(void *opaque, mz_uint64 ofs, const void *buf, size_t n)
{
    (void)ofs;
    DWORD w = 0;
    return WriteFile(*(HANDLE *)opaque, buf, (DWORD)n, &w, 0) ? w : 0;
}

static int Extract(const wchar_t *zip, const wchar_t *dest)
{
    DWORD len = 0;
    char *data = ReadAll(zip, &len);
    if (!data) {
        SetErr("cannot read downloaded package");
        return UPD_ERR_PACKAGE;
    }
    mz_zip_archive za;
    memset(&za, 0, sizeof(za));
    int rc = UPD_OK;
    if (!mz_zip_reader_init_mem(&za, data, len, 0)) {
        SetErr("package is not a valid zip");
        free(data);
        return UPD_ERR_PACKAGE;
    }
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && rc == UPD_OK; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) {
            rc = UPD_ERR_PACKAGE;
            break;
        }
        wchar_t rel[MAX_PATH], full[UPD_PATH];
        U2W(st.m_filename, rel, MAX_PATH);
        size_t rl = wcslen(rel);
        BOOL isDir = st.m_is_directory || (rl && (rel[rl - 1] == L'/' || rel[rl - 1] == L'\\'));
        if (isDir) {
            while (rl && (rel[rl - 1] == L'/' || rel[rl - 1] == L'\\'))
                rel[--rl] = 0;
            if (!rl)
                continue;
        }
        if (!SafeRel(rel)) {
            SetErr("unsafe path in package: %s", st.m_filename);
            rc = UPD_ERR_PACKAGE;
            break;
        }
        JoinPath(full, dest, rel, UPD_PATH);
        if (isDir) {
            MakeDirs(full);
            continue;
        }
        MakeParentDirs(full);
        HANDLE f = CreateFileW(full, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (f == INVALID_HANDLE_VALUE) {
            SetErr("cannot create %s", st.m_filename);
            rc = UPD_ERR_PACKAGE;
            break;
        }
        BOOL ok = mz_zip_reader_extract_to_callback(&za, i, WriteCb, &f, 0);
        CloseHandle(f);
        if (!ok) {
            SetErr("cannot unpack %s (%s)", st.m_filename, mz_zip_get_error_string(mz_zip_get_last_error(&za)));
            rc = UPD_ERR_PACKAGE;
        }
    }
    mz_zip_reader_end(&za);
    free(data);
    return rc;
}

// If the zip has a single top-level folder, use it as the package root.
static void PackageRoot(const wchar_t *temp, wchar_t *rootRel)
{
    CopyW(rootRel, L"temp", MAX_PATH);
    wchar_t mask[UPD_PATH];
    JoinPath(mask, temp, L"*", UPD_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(mask, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    int dirs = 0, files = 0;
    wchar_t only[MAX_PATH] = L"";
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++dirs;
            CopyW(only, fd.cFileName, MAX_PATH);
        } else {
            ++files;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (dirs == 1 && files == 0)
        JoinPath(rootRel, L"temp", only, MAX_PATH);
}

// Download, verify, unpack into update\temp and write update\ready. Idempotent.
int PrepareUpdate(Release *rel)
{
    char have[64];
    if (ReadyVersion(have, sizeof(have)) && !strcmp(have, rel->version)) {
        Log("package %s already prepared", have);
        return UPD_OK;
    }
    if (!rel->zipUrl[0]) {
        SetErr("release %s has no package matching the configured asset", rel->version);
        return UPD_ERR_SOURCE;
    }
    if (!MakeDirs(g_cfg.workDir)) {
        SetErr("cannot create work folder (no write access?)");
        return UPD_ERR_ACCESS;
    }
    wchar_t ready[UPD_PATH], temp[UPD_PATH], zip[UPD_PATH];
    WorkPath(ready, L"ready");
    WorkPath(temp, L"temp");
    WorkPath(zip, L"package.zip");
    DeleteFileW(ready);
    DeleteTree(temp);

    char sha[65];
    CopyA(sha, rel->sha256, sizeof(sha));
    if (!sha[0] && rel->sumsUrl[0]) {
        char *sums = 0;
        DWORD len = 0;
        int st = 0;
        int rc = NetGetMem(rel->sumsUrl, FALSE, &sums, &len, &st);
        if (rc != UPD_OK)
            return rc;
        if (st == 200 && sums)
            FindSum(sums, rel->zipName, sha);
        free(sums);
    }
    if (!sha[0] && g_cfg.requireHash) {
        SetErr("no SHA-256 for %ls (add a SHA256SUMS asset or sha256 field)", rel->zipName);
        return UPD_ERR_VERIFY;
    }

    InterlockedExchange64(&g_prog.total, rel->size);
    int status = 0;
    int rc = NetDownload(rel->zipUrl, zip, &status);
    if (rc != UPD_OK)
        return rc;
    if (status != 200) {
        SetErr("package download failed: HTTP %d", status);
        return UPD_ERR_NETWORK;
    }
    if (sha[0]) {
        char got[65];
        if (!Sha256File(zip, got) || _stricmp(got, sha)) {
            DeleteFileW(zip);
            SetErr("package checksum mismatch (expected %s, got %s)", sha, got);
            return UPD_ERR_VERIFY;
        }
        Log("sha256 ok %s", got);
    }
    // TODO(signature): verify an Ed25519/RSA signature of the zip here before unpacking.

    rc = Extract(zip, temp);
    DeleteFileW(zip);
    if (rc != UPD_OK) {
        DeleteTree(temp);
        return rc;
    }
    wchar_t rootRel[MAX_PATH], root[UPD_PATH], hostIn[UPD_PATH];
    PackageRoot(temp, rootRel);
    JoinPath(root, g_cfg.workDir, rootRel, UPD_PATH);
    JoinPath(hostIn, root, g_cfg.hostExe, UPD_PATH);
    if (!FileExists(hostIn)) {
        DeleteTree(temp);
        SetErr("package does not contain %ls", g_cfg.hostExe);
        return UPD_ERR_PACKAGE;
    }
    char line[400], r8[MAX_PATH];
    W2U(rootRel, r8, sizeof(r8));
    sprintf_s(line, sizeof(line), "%s\n%s\n", rel->version, r8);
    if (!WriteAll(ready, line, (DWORD)strlen(line))) {
        SetErr("cannot write ready marker");
        return UPD_ERR_ACCESS;
    }
    Log("package %s ready", rel->version);
    return UPD_OK;
}
