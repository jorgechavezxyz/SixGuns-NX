#pragma once

/* Everything the port needs lives under this folder on the SD card:
 *   libAndroid.so        - from the APK's lib/arm64-v8a/
 *   assets/              - the APK's assets/ folder (AAssetManager reads here)
 *   data/                - main OBB extracted, then patch OBB extracted over it
 *   sdcard/, internal/   - writable dirs the game sees as /sdcard and /data/data/<pkg>
 *   prefs.txt, log.txt   - created at runtime */
#define PORT_ROOT    "sdmc:/switch/sixguns"
#define SO_PATH      PORT_ROOT "/libAndroid.so"
#define APK_PATH     PORT_ROOT "/base.apk"  /* the real game APK (installer's assets/app.png) */
#define ASSETS_ROOT  PORT_ROOT "/assets"
#define DATA_ROOT    PORT_ROOT "/data"
#define OBB_ROOT     PORT_ROOT "/obb"
#define MAIN_OBB     "main.29431." PACKAGE_NAME ".obb"
#define PATCH_OBB    "patch.29523." PACKAGE_NAME ".obb"
#define SAVE_ROOT    PORT_ROOT "/save"
#define CACHE_ROOT   PORT_ROOT "/cache"
#define PREFS_PATH   PORT_ROOT "/prefs.txt"
#define LOG_PATH     PORT_ROOT "/log.txt"

#define PACKAGE_NAME "com.gameloft.android.ANMP.GloftSXHM"

#define SCREEN_W 1280
#define SCREEN_H 720

/* Memory reserved for the engine's own heap is just newlib malloc; the
 * game needs a full-RAM launch (title override), not applet mode. */
