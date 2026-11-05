#include_next "sys_config.h"

#pragma once

// CFG_SUPPORT_BLE = 0
#ifdef CFG_SUPPORT_BLE
#undef CFG_SUPPORT_BLE
#endif
#define CFG_SUPPORT_BLE 0
// CFG_IEEE80211W = 0
#ifdef CFG_IEEE80211W
#undef CFG_IEEE80211W
#endif
#define CFG_IEEE80211W 0
// CFG_WPA3 = 0
#ifdef CFG_WPA3
#undef CFG_WPA3
#endif
#define CFG_WPA3 0
