/* Android launcher (libmain.so, loaded by SDL's activity in the Type X2 app).
 *
 * The app (launcher/, Tauri) carries no game code: the launcher finds the game
 * folder (launch intent from the app's Play, ES-DE or another frontend, else
 * <app files>/game), makes sure it is readable (asking for all-files access
 * when it lives in shared storage), identifies the game from game.exe's PE
 * header and runs its pack, files/packs/x2-<size>-<entry>.so (installed by the
 * app), through its x2_game_main(). */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "game_exe.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "x2", __VA_ARGS__)

/* ---- stderr -> logcat (tag "x2") ------------------------------------------- */
static int log_pipe[2];
static void *log_forward(void *arg) {
    (void)arg;
    char buf[2048];
    size_t len = 0;
    for (;;) {
        ssize_t r = read(log_pipe[0], buf + len, sizeof buf - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', len - (size_t)(start - buf)))) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, "x2", start);
            start = nl + 1;
        }
        len -= (size_t)(start - buf);
        memmove(buf, start, len);
        if (len == sizeof buf - 1) {
            buf[len] = 0;
            __android_log_write(ANDROID_LOG_INFO, "x2", buf);
            len = 0;
        }
    }
    return NULL;
}
static void android_logging(void) {
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (pipe(log_pipe)) return;
    dup2(log_pipe[1], 2);
    pthread_t t;
    pthread_create(&t, NULL, log_forward, NULL);
    pthread_detach(t);
}

/* ---- JNI helpers --------------------------------------------------------- */
static JNIEnv *jni(void) { return (JNIEnv *)SDL_GetAndroidJNIEnv(); }
static void jni_clear(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

/* Game folder from the launch intent: a string extra "ROM"/"path"/"game" (ES-DE's
 * %EXTRA_ROM%=%ROM%), or a file:// / plain-path data URI. */
static bool intent_path(char *out, size_t n) {
    JNIEnv *env = jni();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return false;
    bool ok = false;
    jclass ac = (*env)->GetObjectClass(env, act);
    jobject intent = (*env)->CallObjectMethod(env, act, (*env)->GetMethodID(env, ac, "getIntent", "()Landroid/content/Intent;"));
    if (intent) {
        jclass ic = (*env)->GetObjectClass(env, intent);
        jmethodID extra = (*env)->GetMethodID(env, ic, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
        const char *keys[] = {"ROM", "path", "game"};
        for (int i = 0; i < 3 && !ok; i++) {
            jstring k = (*env)->NewStringUTF(env, keys[i]);
            jstring v = (jstring)(*env)->CallObjectMethod(env, intent, extra, k);
            if (v) {
                const char *s = (*env)->GetStringUTFChars(env, v, NULL);
                snprintf(out, n, "%s", s);
                ok = out[0] != 0;
                (*env)->ReleaseStringUTFChars(env, v, s);
            }
        }
        if (!ok) {
            jobject uri = (*env)->CallObjectMethod(env, intent, (*env)->GetMethodID(env, ic, "getDataString", "()Ljava/lang/String;"));
            if (uri) {
                const char *s = (*env)->GetStringUTFChars(env, (jstring)uri, NULL);
                const char *p = !strncmp(s, "file://", 7) ? s + 7 : s[0] == '/' ? s : NULL;
                if (p) {
                    snprintf(out, n, "%s", p);
                    ok = true;
                } else {
                    LOG("intent data %s is not a file path (use %%EXTRA_ROM%%=%%ROM%% in ES-DE)", s);
                }
                (*env)->ReleaseStringUTFChars(env, (jstring)uri, s);
            }
        }
    }
    jni_clear(env);
    return ok;
}

static bool all_files_access(void) {
    JNIEnv *env = jni();
    jclass e = (*env)->FindClass(env, "android/os/Environment");
    jmethodID m = e ? (*env)->GetStaticMethodID(env, e, "isExternalStorageManager", "()Z") : NULL;
    bool ok = m && (*env)->CallStaticBooleanMethod(env, e, m);
    jni_clear(env);
    return ok;
}

/* Settings > "All files access" for this app */
static void request_all_files_access(void) {
    JNIEnv *env = jni();
    jobject act = (jobject)SDL_GetAndroidActivity();
    jclass ac = (*env)->GetObjectClass(env, act);
    jstring pkg = (jstring)(*env)->CallObjectMethod(env, act, (*env)->GetMethodID(env, ac, "getPackageName", "()Ljava/lang/String;"));
    const char *p = (*env)->GetStringUTFChars(env, pkg, NULL);
    char uri_s[256];
    snprintf(uri_s, sizeof uri_s, "package:%s", p);
    (*env)->ReleaseStringUTFChars(env, pkg, p);
    jclass uc = (*env)->FindClass(env, "android/net/Uri");
    jobject uri = (*env)->CallStaticObjectMethod(env, uc, (*env)->GetStaticMethodID(env, uc, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
                                                 (*env)->NewStringUTF(env, uri_s));
    jclass ic = (*env)->FindClass(env, "android/content/Intent");
    jobject intent = (*env)->NewObject(env, ic, (*env)->GetMethodID(env, ic, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
                                       (*env)->NewStringUTF(env, "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), uri);
    (*env)->CallVoidMethod(env, act, (*env)->GetMethodID(env, ac, "startActivity", "(Landroid/content/Intent;)V"), intent);
    jni_clear(env);
}

static void fail(const char *title, const char *msg) {
    LOG("%s: %s", title, msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
}

/* ---- game identification ------------------------------------------------ */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* SizeOfImage and ImageBase + AddressOfEntryPoint, as x2recomp and the desktop launcher record them */
static bool pe_identity(FILE *f, uint32_t *size, uint32_t *entry) {
    uint8_t h[1024];
    size_t n = fread(h, 1, sizeof h, f);
    if (n < 64 || h[0] != 'M' || h[1] != 'Z') return false;
    uint32_t pe = rd32(h + 0x3c);
    if (pe + 0x58 > n || memcmp(h + pe, "PE\0\0", 4)) return false;
    const uint8_t *opt = h + pe + 24;
    *entry = rd32(opt + 28) + rd32(opt + 16);
    *size = rd32(opt + 56);
    return true;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    android_logging();
    static char dir[1024];
    bool from_intent = intent_path(dir, sizeof dir);
    if (!from_intent) {
        const char *base = SDL_GetAndroidExternalStoragePath();
        snprintf(dir, sizeof dir, "%s/game", base ? base : "/sdcard");
    } else {
        struct stat st; /* a file (e.g. game.exe) selects its folder */
        if (stat(dir, &st) == 0 && !S_ISDIR(st.st_mode)) {
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0;
        }
    }
    LOG("game folder: %s%s", dir, from_intent ? " (from launch intent)" : "");

    char exe[1100];
    x2_game_exe(dir, exe, sizeof exe);
    FILE *f = fopen(exe, "rb");
    if (!f && errno == EACCES && !all_files_access()) {
        /* shared storage (e.g. ES-DE's ROMs folder) needs "All files access" */
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Type X2",
                                 "To read games from your ROMs folder, allow \"All files access\" for this app on the next "
                                 "screen, then come back.",
                                 NULL);
        request_all_files_access();
        for (int i = 0; i < 600 && !all_files_access(); i++) SDL_Delay(500);
        x2_game_exe(dir, exe, sizeof exe); /* readable now: launcher.exe may be the one */
        f = fopen(exe, "rb");
    }
    if (!f) {
        char msg[1400];
        snprintf(msg, sizeof msg, "Cannot open %s (%s).\n\nPut the game folder there, or launch it from ES-DE.", exe, strerror(errno));
        fail("Type X2: game not found", msg);
        return 1;
    }
    uint32_t size = 0, entry = 0;
    bool pe = pe_identity(f, &size, &entry);
    fclose(f);
    if (!pe) {
        char msg[1400];
        snprintf(msg, sizeof msg, "%s is not a Windows executable.", exe);
        fail("Type X2: unsupported game", msg);
        return 1;
    }
    /* the game's pack, installed by the app into its own storage (Android loads code only from
     * there or from the APK) under the game's identity */
    static char pack[1200];
    const char *internal = SDL_GetAndroidInternalStoragePath();
    snprintf(pack, sizeof pack, "%s/packs/x2-%08x-%08x.so", internal ? internal : ".", size, entry);
    if (access(pack, R_OK) != 0) {
        char msg[1400];
        snprintf(msg, sizeof msg,
                 "No game pack for %s (size %08x, entry %08x).\n\nExport the game's Android pack in the Type X "
                 "Recomp launcher on your computer, then send it to this device.",
                 exe, size, entry);
        fail("Type X2: game not installed", msg);
        return 1;
    }
    LOG("game: %s", pack);
    void *lib = dlopen(pack, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fail("Type X2: cannot load game", dlerror());
        return 1;
    }
    int (*game_main)(int, char **) = (int (*)(int, char **))dlsym(lib, "x2_game_main");
    if (!game_main) {
        fail("Type X2: cannot load game", "x2_game_main not found");
        return 1;
    }
    char *gargv[] = {pack, dir, NULL};
    return game_main(2, gargv);
}
