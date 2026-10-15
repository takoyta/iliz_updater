# iliz updater

A small universal updater for portable Win32 apps. Drop `iliz_updater.exe` and `iliz_updater.json`
next to your app, and it checks for a new release, downloads the release zip, verifies SHA-256,
unpacks it, waits for or closes the app, replaces **all** files (exe, DLLs, sounds, ...), rolls back
on failure, and restarts the app.

- Plain Win32 C++17, static CRT, no installer, no extra DLLs (~250 KB).
- WinHTTP (HTTPS), BCrypt (SHA-256), [miniz](https://github.com/richgel999/miniz) (unzip),
  [cJSON](https://github.com/DaveGamble/cJSON).
- Two modes: **interactive** (TaskDialog: current vs new version, release notes, progress) and
  **silent** (no UI, for the host app or a scheduler).
- A **CLI API** with exit codes and JSON on stdout, plus a header-only helper
  [`include/iliz_updater_client.h`](include/iliz_updater_client.h).
- Nothing project-specific is hardcoded: everything is in the config.

Russian: [README.ru.md](README.ru.md).

## Build

```bat
build.bat
```

Needs VS 2022 Build Tools (override the path with `VSINSTALL`). Output: `bin\iliz_updater.exe`.

## Layout next to the app

```
my_app\
  my_app.exe
  iliz_updater.exe
  iliz_updater.json        <- config (name = exe name + .json)
  ...other app files...
  update\                  <- work folder, created on demand
    package.zip            (while downloading)
    temp\                  unpacked package
    ready                  marker: written only after download + verify + unpack
    backup\  journal.txt   (while installing; used for rollback)
    pending_delete.txt     locked files renamed to *.iliz_old, deleted on the next run
    last_check  updater.log
```

## Config: `iliz_updater.json`

```json
{
  "app_name": "iliz pc remote",
  "host_exe": "iliz_pc_remote.exe",
  "version": { "source": "exe" },
  "source": {
    "type": "github",
    "repo": "takoyta/iliz_pc_remote",
    "asset": "iliz_pc_remote-*.zip",
    "sums_asset": "SHA256SUMS",
    "prerelease": false
  },
  "require_hash": true,
  "preserve": ["config.json", "*.log"],
  "restart": true,
  "restart_args": "",
  "close_host": "graceful",
  "close_timeout_sec": 15,
  "check_interval_hours": 24,
  "work_dir": "update",
  "language": "auto",
  "log": true
}
```

| Key | Default | Meaning |
|---|---|---|
| `app_name` | `host_exe` | Name shown in the dialog. |
| `host_exe` | required | App exe, relative to `app_dir`. |
| `app_dir` | `.` | App folder, relative to the updater exe. |
| `version.source` | `exe` | `exe`: ProductVersion from the host exe VERSIONINFO. `file`: first line of `version.value` (default `version.txt`). `fixed`: `version.value` itself. `--current-version` overrides all. |
| `source.type` | `github` | `github` or `json`. |
| `source.repo` | | `owner/name` (github). |
| `source.api` | `https://api.github.com` | For GitHub Enterprise. |
| `source.asset` | `*.zip` | Wildcard for the package asset name. |
| `source.sums_asset` | `SHA256SUMS` | Asset with `sha256sum`-style lines. If missing, the GitHub asset `digest` is used. |
| `source.prerelease` | `false` | Also take pre-releases (newest non-draft). |
| `source.url` | | `json` source: URL of `latest.json` (http(s), `file:///`, or a local/UNC path). |
| `require_hash` | `true` | Refuse packages without a known SHA-256. |
| `preserve` | `[]` | Files installed only if missing (user config, data). Wildcards; `dir/` = whole folder. |
| `restart` | `true` | Start the app again after installing if it was running. |
| `restart_args` | `""` | Command line for the restart. |
| `close_host` | `graceful` | `none`: only wait. `graceful`: signal quit event + `WM_CLOSE`. `kill`: graceful, then terminate. |
| `close_timeout_sec` | `15` | How long to wait for the app to exit. |
| `check_interval_hours` | `24` | Used by `--if-due`. |
| `work_dir` | `update` | Work folder, relative to `app_dir`. |
| `language` | `auto` | `en`, `ru`, or `auto` (Windows UI language). |
| `log` | `true` | Write `update\updater.log`. |

### Generic source: `latest.json`

```json
{
  "version": "1.4.0",
  "url": "my_app-1.4.0.zip",
  "sha256": "<64 hex>",
  "sha256_url": "SHA256SUMS",
  "size": 1234567,
  "published": "2026-10-12T12:00:00Z",
  "page": "https://example.com/releases/1.4.0",
  "notes": "- New sounds"
}
```

`url` and `sha256_url` may be relative to `latest.json`. Give either `sha256` or `sha256_url`.
This works on any static hosting (GitHub Pages, S3, a LAN share).

## Package

The package is the normal release zip with the full app file set. Files may sit at the zip root or
in one top-level folder. Every file in it is copied into the app folder, except `preserve` files that
already exist. It must contain `host_exe`, and it should contain `iliz_updater.exe` and
`iliz_updater.json`, so the updater updates itself (the running exe is renamed to
`iliz_updater.exe.iliz_old` and removed on the next run).

Install steps: wait for or close the app (by `--pid` and by matching the exe file, so pid reuse
is not an issue), back up each replaced file, copy with retries, rename locked files aside, and
journal every step. On any error, everything is restored from the backup. If the updater itself
crashes mid-way, the journal is rolled back on the next start.

The integrity check today is SHA-256 from the release. There is a placeholder in
`package.cpp` (`TODO(signature)`) for an Ed25519/RSA signature check, if packages ever come
from an untrusted mirror.

## CLI (the API)

```
iliz_updater.exe                       interactive: check, ask, download, install
iliz_updater.exe --check --json        check only
iliz_updater.exe --check --json --if-due   check only if check_interval_hours passed
iliz_updater.exe --update --silent --pid <n> [--restart|--no-restart]
iliz_updater.exe --update --pid <n>    interactive, bound to the calling app
iliz_updater.exe --download-only       download + verify + unpack (writes update\ready)
iliz_updater.exe --apply --silent --pid <n>   install what --download-only prepared
other: --force  --current-version <v>  --config <path>  --json  --version  --help
```

Exit codes:

| Code | Meaning |
|---|---|
| 0 | Up to date, no release, or nothing to apply |
| 1 | Update available (`--check`), or downloaded and ready (`--download-only`) |
| 2 | Update installed |
| 3 | Cancelled by the user |
| 4 | Skipped: `--if-due` and the interval has not passed |
| 10 | Bad arguments |
| 11 | Config error |
| 12 | Network error (including GitHub rate limit 403/429) |
| 13 | Source error: bad JSON, or no asset matches `asset` |
| 14 | Verify error: no checksum, or checksum mismatch |
| 15 | Package error: broken zip, unsafe paths, `host_exe` missing |
| 16 | App did not exit in time |
| 17 | No write access to the app folder |
| 18 | Install failed, rolled back |
| 19 | Another updater instance is running for this app |

With `--json`, a single line of JSON goes to stdout:

```json
{"status":"update_available","exit_code":1,"current":"0.1.8","latest":"0.1.9","tag":"v0.1.9",
 "published":"2026-10-20T10:00:00Z","page":"https://github.com/.../releases/tag/v0.1.9",
 "url":"https://github.com/.../iliz_pc_remote-0.1.9.zip","size":612345,"sha256":"...",
 "notes":"...","updater_version":"1.0.0"}
```

`status` is one of `up_to_date`, `update_available`, `no_release`, `ready`, `updated`,
`nothing_to_apply`, `skipped`, or `error`. On errors you also get `"error":"network"` and the
like, plus `"message"`.

## Host integration (C/C++)

```cpp
#include "iliz_updater_client.h"
#define WM_APP_UPDATE (WM_APP + 7)

// at startup
IlizUpdaterWatchQuit(hwnd, WM_CLOSE);               // updater may ask us to quit
IlizUpdaterCheckAsync(hwnd, WM_APP_UPDATE, L"--if-due");

// in WndProc
case WM_APP_UPDATE: {
    IlizUpdateInfo *u = (IlizUpdateInfo *)lParam;
    if (u->exitCode == ILIZ_UPD_UPDATE_AVAILABLE)
        ShowTrayBalloon(u->latest);                  // u->json has notes, url, ...
    IlizUpdaterFree(u);
    break;
}
// menu "Update now":
IlizUpdaterLaunch(FALSE);   // interactive dialog; TRUE = silent
```

`IlizUpdaterLaunch` passes `--pid <self>`. When the package is ready, the updater sets the event
`Local\iliz_updater_quit.<pid>` (that is how `IlizUpdaterWatchQuit` gets notified) and posts
`WM_CLOSE` to the app's top-level windows. It then waits `close_timeout_sec`, installs, and
restarts the app. Alternatively, the app can simply exit right after starting the updater.
For a "download now, install on exit" flow like Telegram's, run `--download-only` in the
background, and on exit run `--apply --silent --pid <self> --no-restart`.

The header calls only kernel32/user32 (`HeapAlloc`, `wsprintfW`, `lstr*`), no C runtime, so it
works in CRT-free builds (`/NODEFAULTLIB`, own entry point). Free results only with
`IlizUpdaterFree` (memory comes from the process heap, not `malloc`). Copy the header into the host
repo so the project builds on its own.

With `version.source: exe` (the default) the host exe **must** have a VERSIONINFO resource with
`ProductVersion` set (an `.rc` file compiled with `rc` and linked in). Without it the current version
is empty and every release looks new. Keep one version define and use it in both the code and the
`.rc`.

Other languages: run the exe hidden, read the exit code, and parse the stdout JSON.

## Release checklist for a host project

1. Ship `iliz_updater.exe` + the project's `iliz_updater.json` inside the release zip.
2. Upload the zip **and** `SHA256SUMS` (`<sha256>  <zip name>`) to the GitHub release.
3. Tag the release with the version (`0.1.9` or `v0.1.9`), and make it match the exe's
   ProductVersion.

4. A pack script can be this small (see `release.bat` in
   [iliz_resmon](https://github.com/takoyta/iliz_resmon)): build, copy the files into a stage folder,
   `Compress-Archive` it to `<app>-<ver>.zip`, and write `SHA256SUMS` with
   `(Get-FileHash -Algorithm SHA256 zip).Hash.ToLower() + "  <zip name>"`, then
   `gh release create v<ver> <zip> SHA256SUMS`.

## License

MIT, see [LICENSE](LICENSE). miniz (MIT) and cJSON (MIT) are in `third_party`.
