#pragma once

#define UPD_VER_MAJOR 1
#define UPD_VER_MINOR 0
#define UPD_VER_PATCH 0

#define UPD_VERSION_A   "1.0.0"
#define UPD_NAME_A      "iliz updater"
#define UPD_COMPANY_A   "iliz"
#define UPD_DESC_A      "Universal updater for iliz apps"
#define UPD_INTERNAL_A  "iliz_updater"
#define UPD_EXE_A       "iliz_updater.exe"
#define UPD_COPYRIGHT_A "Copyright (C) 2026 iliz"

#define UPD_WIDEN2(s) L##s
#define UPD_WIDEN(s)  UPD_WIDEN2(s)
#define UPD_VERSION   UPD_WIDEN(UPD_VERSION_A)
