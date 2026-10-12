@echo off
setlocal
set ROOT=%~dp0
if not defined VSINSTALL set VSINSTALL=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "%ROOT%bin" mkdir "%ROOT%bin"
if not exist "%ROOT%bin\iliz_updater.json" copy /y "%ROOT%examples\iliz_updater.json" "%ROOT%bin\iliz_updater.json" >nul
cd /d "%ROOT%bin"

rc /nologo /I"%ROOT%src" /foiliz_updater.res "%ROOT%src\app.rc" || exit /b 1

cl /nologo /std:c++17 /utf-8 /MT /O1 /Os /GS- /Gy /GR- /EHs-c- /W3 /DUNICODE /D_UNICODE ^
   /D_CRT_SECURE_NO_WARNINGS /DCJSON_HIDE_SYMBOLS ^
   /DMINIZ_NO_STDIO /DMINIZ_NO_TIME /DMINIZ_NO_DEFLATE_APIS /DMINIZ_NO_ZLIB_APIS ^
   /I"%ROOT%src" /I"%ROOT%third_party\cjson" /I"%ROOT%third_party\miniz" /Fe:iliz_updater.exe ^
   "%ROOT%src\main.cpp" "%ROOT%src\ui.cpp" "%ROOT%src\config.cpp" "%ROOT%src\release.cpp" ^
   "%ROOT%src\package.cpp" "%ROOT%src\apply.cpp" "%ROOT%src\net.cpp" "%ROOT%src\util.cpp" ^
   "%ROOT%third_party\cjson\cJSON.c" "%ROOT%third_party\miniz\miniz.c" ^
   iliz_updater.res ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:NO ^
   kernel32.lib user32.lib shell32.lib comctl32.lib advapi32.lib ^
   winhttp.lib bcrypt.lib version.lib shlwapi.lib || exit /b 1

del /q *.obj *.res *.lib *.exp 2>nul
echo Built: %ROOT%bin\iliz_updater.exe
