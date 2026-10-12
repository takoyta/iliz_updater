#include "common.h"
#include <commctrl.h>

int Check(Release *rel, BOOL force);
void MaybeRestart();

enum { S_CHECK, S_NONE, S_AVAIL, S_DOWNLOAD, S_APPLY, S_DONE, S_ERROR, S_CANCELLED };
enum { ID_INSTALL = 100, ID_RETRY = 101 };

enum {
    T_TITLE, T_CHECKING, T_NONE, T_NONE_C, T_NOREL, T_AVAIL, T_AVAIL_C, T_NOTES, T_NOTES_HIDE, T_INSTALL, T_LATER,
    T_DOWNLOADING, T_DL_C, T_APPLYING, T_APPLYING_C, T_DONE, T_DONE_C, T_ERROR, T_RETRY, T_BUSY_C, T_COUNT
};

static const wchar_t *kEn[T_COUNT] = {
    L"%ls update",
    L"Checking for updates\u2026",
    L"You have the latest version",
    L"%ls %hs is up to date.",
    L"No releases have been published yet.",
    L"Version %hs is available",
    L"Installed: %hs\nNew: %hs%ls",
    L"What's new",
    L"Hide what's new",
    L"Download and install",
    L"Later",
    L"Downloading update\u2026",
    L"%.1f of %.1f MB",
    L"Installing\u2026",
    L"%ls will be closed and restarted.",
    L"Update installed",
    L"%ls has been updated to version %hs.",
    L"Update failed",
    L"Retry",
    L"Close %ls and press Retry.",
};

static const wchar_t *kRu[T_COUNT] = {
    L"%ls \u2014 обновление",
    L"Проверка обновлений\u2026",
    L"У вас последняя версия",
    L"%ls %hs \u2014 актуальная версия.",
    L"Релизы ещё не опубликованы.",
    L"Доступна версия %hs",
    L"Установлена: %hs\nНовая: %hs%ls",
    L"Что нового",
    L"Скрыть",
    L"Скачать и установить",
    L"Позже",
    L"Загрузка обновления\u2026",
    L"%.1f из %.1f МБ",
    L"Установка\u2026",
    L"%ls будет закрыт и перезапущен.",
    L"Обновление установлено",
    L"%ls обновлён до версии %hs.",
    L"Не удалось обновить",
    L"Повторить",
    L"Закройте %ls и нажмите \u00abПовторить\u00bb.",
};

static const wchar_t *T(int i)
{
    return (g_cfg.lang == 1 ? kRu : kEn)[i];
}

static volatile LONG s_state = S_CHECK;
static LONG s_shown = -1;
static BOOL s_closing, s_hadUpdate;
static int s_rc;
static Release s_rel;
static TASKDIALOGCONFIG s_tc;
static TASKDIALOG_BUTTON s_btn[2];
static wchar_t s_title[200], s_main[300], s_content[1200], s_notes[6000];

static DWORD WINAPI CheckThread(void *)
{
    int rc = Check(&s_rel, FALSE);
    s_rc = rc;
    if (rc == UPD_UPDATE_AVAILABLE && !s_rel.zipUrl[0]) {
        SetErr("release %s has no asset matching the configured pattern", s_rel.version);
        s_rc = UPD_ERR_SOURCE;
    }
    InterlockedExchange(&s_state, s_rc == UPD_UPDATE_AVAILABLE ? S_AVAIL : (s_rc == UPD_OK ? S_NONE : S_ERROR));
    return 0;
}

static DWORD WINAPI InstallThread(void *)
{
    g_prog.cancel = 0;
    InterlockedExchange64(&g_prog.done, 0);
    int rc = PrepareUpdate(&s_rel);
    if (rc == UPD_CANCELLED) {
        s_rc = rc;
        InterlockedExchange(&s_state, S_CANCELLED);
        return 0;
    }
    if (rc == UPD_OK) {
        InterlockedExchange(&s_state, S_APPLY);
        char ver[64];
        rc = ApplyReady(ver, sizeof(ver));
        if (rc == UPD_UPDATED) {
            CopyA(g_current, ver, sizeof(g_current));
            MaybeRestart();
        }
    }
    s_rc = rc;
    InterlockedExchange(&s_state, rc == UPD_UPDATED ? S_DONE : S_ERROR);
    return 0;
}

static void Start(LPTHREAD_START_ROUTINE fn)
{
    HANDLE h = CreateThread(0, 0, fn, 0, 0, 0);
    if (h)
        CloseHandle(h);
}

static HRESULT CALLBACK Cb(HWND h, UINT n, WPARAM w, LPARAM, LONG_PTR);

static void Build(int st)
{
    memset(&s_tc, 0, sizeof(s_tc));
    s_tc.cbSize = sizeof(s_tc);
    s_tc.hInstance = GetModuleHandleW(0);
    s_tc.pfCallback = Cb;
    s_tc.dwFlags = TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
    s_tc.pszWindowTitle = s_title;
    s_tc.pszMainInstruction = s_main;
    s_tc.pszContent = s_content;
    s_tc.cxWidth = 260;
    s_main[0] = s_content[0] = 0;
    const wchar_t *app = g_cfg.appName;
    switch (st) {
    case S_CHECK:
        CopyW(s_main, T(T_CHECKING), 300);
        s_tc.dwFlags |= TDF_SHOW_MARQUEE_PROGRESS_BAR;
        s_tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        break;
    case S_NONE:
        if (s_rel.version[0]) {
            CopyW(s_main, T(T_NONE), 300);
            swprintf_s(s_content, 1200, T(T_NONE_C), app, g_current);
        } else {
            CopyW(s_main, T(T_NONE), 300);
            CopyW(s_content, T(T_NOREL), 1200);
        }
        s_tc.pszMainIcon = TD_INFORMATION_ICON;
        s_tc.dwCommonButtons = TDCBF_OK_BUTTON;
        break;
    case S_AVAIL: {
        swprintf_s(s_main, 300, T(T_AVAIL), s_rel.version);
        wchar_t size[64] = L"";
        if (s_rel.size > 0)
            swprintf_s(size, 64, g_cfg.lang == 1 ? L"\nРазмер: %.1f МБ" : L"\nSize: %.1f MB",
                       s_rel.size / 1048576.0);
        swprintf_s(s_content, 1200, T(T_AVAIL_C), g_current[0] ? g_current : "?", s_rel.version, size);
        if (s_rel.notes && s_rel.notes[0]) {
            U2W(s_rel.notes, s_notes, 6000);
            if (wcslen(s_notes) > 4000)
                wcscpy_s(s_notes + 3990, 10, L"\n\u2026");
            s_tc.pszExpandedInformation = s_notes;
            s_tc.pszCollapsedControlText = T(T_NOTES);
            s_tc.pszExpandedControlText = T(T_NOTES_HIDE);
        }
        s_btn[0].nButtonID = ID_INSTALL;
        s_btn[0].pszButtonText = T(T_INSTALL);
        s_btn[1].nButtonID = IDCANCEL;
        s_btn[1].pszButtonText = T(T_LATER);
        s_tc.pButtons = s_btn;
        s_tc.cButtons = 2;
        s_tc.nDefaultButton = ID_INSTALL;
        s_tc.pszMainIcon = TD_INFORMATION_ICON;
        break;
    }
    case S_DOWNLOAD:
        CopyW(s_main, T(T_DOWNLOADING), 300);
        s_tc.dwFlags |= TDF_SHOW_PROGRESS_BAR;
        s_tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        CopyW(s_content, L" ", 1200);
        break;
    case S_APPLY:
        CopyW(s_main, T(T_APPLYING), 300);
        if (HostIsRunning())
            swprintf_s(s_content, 1200, T(T_APPLYING_C), app);
        s_tc.dwFlags = TDF_CALLBACK_TIMER | TDF_SHOW_MARQUEE_PROGRESS_BAR;
        s_tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        break;
    case S_DONE:
        CopyW(s_main, T(T_DONE), 300);
        swprintf_s(s_content, 1200, T(T_DONE_C), app, g_current);
        s_tc.pszMainIcon = TD_INFORMATION_ICON;
        s_tc.dwCommonButtons = TDCBF_OK_BUTTON;
        break;
    case S_ERROR:
        CopyW(s_main, T(T_ERROR), 300);
        if (s_rc == UPD_ERR_HOST_BUSY)
            swprintf_s(s_content, 1200, T(T_BUSY_C), app);
        else
            swprintf_s(s_content, 1200, L"%hs", g_err);
        s_tc.pszMainIcon = TD_ERROR_ICON;
        if (s_hadUpdate) {
            s_btn[0].nButtonID = ID_RETRY;
            s_btn[0].pszButtonText = T(T_RETRY);
            s_tc.pButtons = s_btn;
            s_tc.cButtons = 1;
        }
        s_tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        break;
    }
}

static HRESULT CALLBACK Cb(HWND h, UINT n, WPARAM w, LPARAM, LONG_PTR)
{
    switch (n) {
    case TDN_CREATED:
    case TDN_NAVIGATED:
        if (s_shown == S_CHECK || s_shown == S_APPLY)
            SendMessageW(h, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        if (s_shown == S_DOWNLOAD)
            SendMessageW(h, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
        if (s_shown == S_APPLY)
            SendMessageW(h, TDM_ENABLE_BUTTON, IDCLOSE, FALSE);
        break;
    case TDN_TIMER: {
        LONG st = s_state;
        if (st == S_CANCELLED) {
            s_closing = TRUE;
            SendMessageW(h, TDM_CLICK_BUTTON, IDCANCEL, 0);
            break;
        }
        if (st != s_shown) {
            s_shown = st;
            Build(st);
            SendMessageW(h, TDM_NAVIGATE_PAGE, 0, (LPARAM)&s_tc);
        } else if (st == S_DOWNLOAD) {
            double done = (double)g_prog.done, total = (double)g_prog.total;
            int pos = total > 0 ? (int)(done * 1000 / total) : 0;
            SendMessageW(h, TDM_SET_PROGRESS_BAR_POS, pos > 1000 ? 1000 : pos, 0);
            wchar_t t[120];
            swprintf_s(t, 120, T(T_DL_C), done / 1048576.0, total / 1048576.0);
            SendMessageW(h, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)t);
        }
        break;
    }
    case TDN_BUTTON_CLICKED:
        if (w == ID_INSTALL || w == ID_RETRY) {
            s_hadUpdate = TRUE;
            InterlockedExchange(&s_state, S_DOWNLOAD);
            Start(InstallThread);
            return S_FALSE;
        }
        if (s_closing)
            return S_OK;
        if (s_shown == S_APPLY)
            return S_FALSE;  // never interrupt copying
        if (s_shown == S_DOWNLOAD && w == IDCANCEL) {
            InterlockedExchange(&g_prog.cancel, 1);
            return S_FALSE;
        }
        if (s_shown == S_AVAIL && w == IDCANCEL)
            s_rc = UPD_CANCELLED;
        if (s_shown == S_CHECK)
            s_rc = UPD_CANCELLED;
        return S_OK;
    }
    return S_OK;
}

int RunInteractive(BOOL)
{
    swprintf_s(s_title, 200, T(T_TITLE), g_cfg.appName);
    s_shown = S_CHECK;
    Build(S_CHECK);
    Start(CheckThread);
    int btn = 0;
    if (FAILED(TaskDialogIndirect(&s_tc, &btn, 0, 0)))
        return UPD_ERR_CONFIG;
    LONG st = s_state;
    if (st == S_DONE)
        return UPD_UPDATED;
    if (st == S_NONE)
        return UPD_OK;
    return s_rc;
}
