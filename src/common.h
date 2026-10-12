#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "appinfo.h"

#define UPD_PATH 1024
#define UPD_URL  2048
#define UPD_MAX_PRESERVE 64

// Process exit codes: the public CLI contract (see README.md).
enum {
    UPD_OK               = 0,   // up to date / nothing to do
    UPD_UPDATE_AVAILABLE = 1,   // --check / --download-only: newer version exists
    UPD_UPDATED          = 2,   // update was installed
    UPD_CANCELLED        = 3,   // user cancelled
    UPD_SKIPPED          = 4,   // --if-due and the check interval has not passed
    UPD_ERR_ARGS         = 10,
    UPD_ERR_CONFIG       = 11,
    UPD_ERR_NETWORK      = 12,
    UPD_ERR_SOURCE       = 13,  // bad/unsupported release data, no matching asset
    UPD_ERR_VERIFY       = 14,  // checksum missing or mismatch
    UPD_ERR_PACKAGE      = 15,  // broken zip / unpack failed
    UPD_ERR_HOST_BUSY    = 16,  // host app did not exit
    UPD_ERR_ACCESS       = 17,  // app folder is not writable
    UPD_ERR_APPLY        = 18,  // copying failed, rolled back
    UPD_ERR_BUSY         = 19,  // another updater instance is running
};

struct Config {
    wchar_t selfPath[UPD_PATH];
    wchar_t selfDir[UPD_PATH];
    wchar_t configPath[UPD_PATH];
    wchar_t appDir[UPD_PATH];
    wchar_t workDir[UPD_PATH];
    wchar_t appName[128];
    wchar_t hostExe[MAX_PATH];
    wchar_t hostPath[UPD_PATH];
    int verSource;                 // 0 exe, 1 file, 2 fixed
    wchar_t verValue[UPD_PATH];
    int srcType;                   // 0 github, 1 json
    wchar_t repo[256];
    wchar_t api[256];
    wchar_t asset[128];
    wchar_t sumsAsset[128];
    BOOL prerelease;
    wchar_t url[UPD_URL];
    BOOL requireHash;
    wchar_t preserve[UPD_MAX_PRESERVE][MAX_PATH];
    int preserveCount;
    BOOL restart;
    wchar_t restartArgs[512];
    int closeHost;                 // 0 none, 1 graceful, 2 kill
    int closeTimeoutSec;
    int checkIntervalHours;
    int lang;                      // 0 en, 1 ru
    BOOL log;
};

struct Release {
    char version[64];
    char tag[64];
    char published[40];
    char page[512];
    wchar_t zipUrl[UPD_URL];
    wchar_t zipName[256];
    wchar_t sumsUrl[UPD_URL];
    char sha256[65];
    long long size;
    char *notes;
};

struct Progress {
    volatile LONG64 done;
    volatile LONG64 total;
    volatile LONG cancel;
};

extern Config g_cfg;
extern Progress g_prog;
extern char g_err[512];
extern char g_current[64];
extern BOOL g_hostWasRunning;

// util.cpp
void SetErr(const char *fmt, ...);
void Log(const char *fmt, ...);
void LogOpen();
void U2W(const char *s, wchar_t *out, int cap);
void W2U(const wchar_t *s, char *out, int cap);
void CopyW(wchar_t *dst, const wchar_t *src, int cap);
void CopyA(char *dst, const char *src, int cap);
void JoinPath(wchar_t *out, const wchar_t *a, const wchar_t *b, int cap);
BOOL FileExists(const wchar_t *p);
BOOL DirExists(const wchar_t *p);
BOOL MakeDirs(const wchar_t *p);
BOOL MakeParentDirs(const wchar_t *file);
void DeleteTree(const wchar_t *p);
char *ReadAll(const wchar_t *p, DWORD *len);
BOOL WriteAll(const wchar_t *p, const void *data, DWORD len);
BOOL AppendLine(const wchar_t *p, const char *line);
BOOL Sha256File(const wchar_t *p, char hex[65]);
int CompareVersions(const char *a, const char *b);

// config.cpp
int LoadConfig(const wchar_t *path);
BOOL IsPreserved(const wchar_t *rel);

// net.cpp
int NetGetMem(const wchar_t *url, BOOL githubApi, char **out, DWORD *len, int *status);
int NetDownload(const wchar_t *url, const wchar_t *dest, int *status);

// release.cpp
BOOL ReadCurrentVersion(char *out, int cap);
int FetchRelease(Release *rel);
void FreeRelease(Release *rel);

// package.cpp
int PrepareUpdate(Release *rel);
BOOL ReadyVersion(char *ver, int cap);

// apply.cpp
void AttachHostPid(DWORD pid);
BOOL HostIsRunning();
int ApplyReady(char *installed, int cap);
void Recover();
void CleanupOld();
BOOL LaunchHost();

// ui.cpp
int RunInteractive(BOOL checkOnly);
