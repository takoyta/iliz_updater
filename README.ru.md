# iliz updater

Маленький универсальный апдейтер для портативных Win32-программ. Кладёте `iliz_updater.exe` и
`iliz_updater.json` рядом с программой. Он проверяет новый релиз, качает zip, сверяет SHA-256,
распаковывает, ждёт или закрывает программу и заменяет **все** файлы (exe, DLL, звуки…). При ошибке
он откатывает изменения, а после установки перезапускает программу.

- Чистый Win32/C++17, статический CRT, без установщика и лишних DLL (~250 КБ).
- WinHTTP (HTTPS), BCrypt (SHA-256), miniz (распаковка), cJSON.
- Два режима: **обычный** (TaskDialog: текущая и новая версия, «Что нового», прогресс) и
  **тихий** (без окон, для программы-хозяина или планировщика).
- **API через командную строку**: коды выхода и JSON в stdout, а также header-only помощник
  `include/iliz_updater_client.h`.
- Ничего не захардкожено, все настройки в конфиге.

English: [README.md](README.md). Там же полная таблица ключей конфига.

## Сборка

`build.bat`. Нужны VS 2022 Build Tools (путь меняется переменной `VSINSTALL`). Результат лежит в `bin\iliz_updater.exe`.

## Что лежит рядом с программой

```
my_app\
  my_app.exe
  iliz_updater.exe
  iliz_updater.json      конфиг (имя = имя exe + .json)
  update\                рабочая папка
    temp\                распакованный пакет
    ready                маркер: только после загрузки + проверки + распаковки
    backup\ journal.txt  на время установки, для отката
    pending_delete.txt   занятые файлы переименованы в *.iliz_old и удаляются при следующем запуске
    last_check updater.log
```

## Конфиг

Пример для iliz_pc_remote:

```json
{
  "app_name": "iliz pc remote",
  "host_exe": "iliz_pc_remote.exe",
  "version": { "source": "exe" },
  "source": { "type": "github", "repo": "takoyta/iliz_pc_remote",
              "asset": "iliz_pc_remote-*.zip", "sums_asset": "SHA256SUMS", "prerelease": false },
  "require_hash": true,
  "preserve": ["config.json", "*.log"],
  "restart": true, "restart_args": "",
  "close_host": "graceful", "close_timeout_sec": 15,
  "check_interval_hours": 24, "work_dir": "update", "language": "auto", "log": true
}
```

- `version.source`: `exe` (ProductVersion из VERSIONINFO программы), `file` (первая строка файла) или `fixed`.
- `source.type`: `github` (Releases API, ассет ищется по маске `asset`, хеш берётся из `SHA256SUMS` или из `digest` ассета)
  или `json` (свой `latest.json` по `source.url`: http(s), `file:///` или путь к сетевой папке).
- `preserve`: файлы, которые кладутся, только если их ещё нет (пользовательский config, данные). `dir/` означает всю папку.
- `close_host`: `none` (только ждать), `graceful` (событие + `WM_CLOSE`) или `kill` (потом завершить принудительно).

Формат `latest.json`: `version`, `url` (можно относительный), `sha256` или `sha256_url`, `size`, `published`, `page`, `notes`.

## Пакет обновления

Это обычный zip релиза со всем набором файлов. Копируется всё, кроме уже существующих файлов из `preserve`.
В zip должен быть `host_exe`, а также желательно `iliz_updater.exe` и `iliz_updater.json`: так апдейтер
обновляет сам себя. Запущенный exe переименовывается в `.iliz_old` и удаляется при следующем запуске.

Установка идёт так: апдейтер ждёт или закрывает программу (по `--pid` и по самому exe-файлу, поэтому
повторное использование pid не страшно), делает бэкап каждого заменяемого файла, копирует с повторами,
переименовывает занятые файлы и ведёт журнал. При ошибке всё возвращается из бэкапа, а если апдейтер
упал посреди установки, откат выполнится при следующем запуске.

Сейчас целостность проверяется по SHA-256. Место для проверки подписи (Ed25519/RSA) уже размечено
в `package.cpp`, см. `TODO(signature)`.

## Командная строка (API)

```
iliz_updater.exe                         обычный режим: проверить, спросить, скачать, установить
iliz_updater.exe --check --json [--if-due]
iliz_updater.exe --update --silent --pid <n> [--restart|--no-restart]
iliz_updater.exe --download-only         скачать и подготовить (update\ready)
iliz_updater.exe --apply --silent --pid <n>
прочее: --force --current-version <v> --config <путь> --version --help
```

| Код | Значение |
|---|---|
| 0 | Актуальная версия / релизов нет / нечего применять |
| 1 | Есть обновление (`--check`) / скачано и готово (`--download-only`) |
| 2 | Обновление установлено |
| 3 | Отменено пользователем |
| 4 | Пропущено (`--if-due`, интервал ещё не прошёл) |
| 10 | Неверные аргументы |
| 11 | Ошибка конфига |
| 12 | Сеть (включая лимит GitHub 403/429) |
| 13 | Источник: плохой JSON или нет ассета по маске |
| 14 | Проверка: нет хеша или хеш не совпал |
| 15 | Пакет: битый zip, опасные пути или нет `host_exe` |
| 16 | Программа не закрылась |
| 17 | Нет прав на запись в папку |
| 18 | Ошибка установки, выполнен откат |
| 19 | Уже запущен другой апдейтер для этой программы |

`--json` выводит одну строку JSON с полями `status` (`up_to_date`, `update_available`, `no_release`, `ready`,
`updated`, `nothing_to_apply`, `skipped`, `error`), `exit_code`, `current`, `latest`, `tag`, `notes`, `url`,
`size`, `sha256`, `page`, `published`, а при ошибке ещё `error` и `message`.

## Как подключить к своей программе

```cpp
#include "iliz_updater_client.h"
IlizUpdaterWatchQuit(hwnd, WM_CLOSE);                     // апдейтер сможет попросить нас выйти
IlizUpdaterCheckAsync(hwnd, WM_APP_UPDATE, L"--if-due");  // результат придёт сообщением
// WM_APP_UPDATE: lParam = IlizUpdateInfo* (latest, status, json), освободить IlizUpdaterFree
IlizUpdaterLaunch(FALSE);                                 // «Обновить»: окно; TRUE = тихо
```

Когда пакет готов, апдейтер выставляет событие `Local\iliz_updater_quit.<pid>` и шлёт `WM_CLOSE`.
Затем он ждёт выхода программы, ставит обновление и перезапускает её. Можно сделать и как в Telegram:
`--download-only` в фоне, а при выходе из программы `--apply --silent --pid <свой pid> --no-restart`.

Заголовок вызывает только kernel32/user32 (`HeapAlloc`, `wsprintfW`, `lstr*`) и не использует CRT,
поэтому подходит и для сборок без CRT (`/NODEFAULTLIB`, своя точка входа). Результат освобождать
только через `IlizUpdaterFree`: память берётся из кучи процесса, а не через `malloc`. Заголовок лучше
скопировать в репозиторий программы, чтобы она собиралась сама по себе.

При `version.source: exe` (по умолчанию) у exe программы **обязательно** должен быть ресурс
VERSIONINFO с `ProductVersion` (файл `.rc`, собранный `rc` и прилинкованный). Без него текущая версия
пустая, и любой релиз считается новым. Версию лучше держать в одном define и брать её и в коде, и в `.rc`.

## Чеклист релиза

1. Положить в zip `iliz_updater.exe` и `iliz_updater.json` проекта.
2. Загрузить в релиз GitHub zip **и** `SHA256SUMS`.
3. Тег = версия (`0.1.9` или `v0.1.9`), та же, что в ProductVersion exe.

4. Скрипт упаковки может быть совсем простым (пример: `release.bat` в
   [iliz_resmon](https://github.com/takoyta/iliz_resmon)): собрать, скопировать файлы во временную
   папку, упаковать `Compress-Archive` в `<app>-<ver>.zip`, записать `SHA256SUMS` строкой
   `(Get-FileHash -Algorithm SHA256 zip).Hash.ToLower() + "  <имя zip>"` и выполнить
   `gh release create v<ver> <zip> SHA256SUMS`.

## Лицензия

MIT, см. [LICENSE](LICENSE). miniz и cJSON (MIT) лежат в `third_party`.
