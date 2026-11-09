// Starts Resident Evil: The Umbrella Chronicles on Meta Quest (the HOTD4 VR start-up, games/hotd4/src/android_main.c, for this game).
//
// SDL's Java side calls SDL_main. The game's disc files are not in the APK: they are copied to the headset, and looked for in turn at
//   <app's own folder>/game/DATA/sys/main.dol        (/sdcard/Android/data/<package>/files; a diagnostic run's private folder)
//   /sdcard/UmbrellaChronicles/game/DATA/sys/main.dol (needs all-files access)
// The port's own files (data/shaders, data/pipeline_keys.seed) are APK assets, unpacked into the app's own folder on the first start and
// after an update (data.files lists them; data.stamp names the build). That folder then stands in for the executable's (UC_EXE_PATH):
// config.json, logs and saves live there.
//
// Everything the game prints goes to logcat (tag "uc") and to uc.log in the app's folder. env.txt there, one NAME=value per line, sets
// environment options without rebuilding; args.txt, one argument per line, adds command-line options.
#ifdef UC_PGO_GENERATE
extern "C" void __llvm_profile_set_filename(const char*);   // compiler-rt's profile runtime, linked by -fprofile-instr-generate
extern "C" int __llvm_profile_write_file(void);
#endif
#include <SDL3/SDL.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "xr_android.h"

int uc_game_main(int argc, char** argv);   // uc_main.cpp's main, renamed for Android

namespace {
FILE* g_log = nullptr;

void* log_pump(void* arg) {
    FILE* in = fdopen(int(intptr_t(arg)), "r");
    char line[2048];
    while (in && std::fgets(line, sizeof line, in)) {
        __android_log_write(ANDROID_LOG_INFO, "uc", line);
        if (g_log) { std::fputs(line, g_log); std::fflush(g_log); }
    }
    return nullptr;
}
void log_to_logcat() {
    int fd[2];
    if (pipe(fd)) return;
    setvbuf(stderr, nullptr, _IOLBF, 0);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dup2(fd[1], 2);
    dup2(fd[1], 1);
    pthread_t t;
    pthread_create(&t, nullptr, log_pump, reinterpret_cast<void*>(intptr_t(fd[0])));
    pthread_detach(t);
}
void load_env(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        line[std::strcspn(line, "\r\n")] = 0;
        char* eq = std::strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        std::fprintf(stderr, "[android] %s=%s\n", line, eq + 1);
    }
    std::fclose(f);
}
std::vector<std::string> load_args(const std::string& path) {
    std::vector<std::string> args;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] != '#') args.push_back(line);
    }
    return args;
}

// ---- the port's files inside the APK ----
AAssetManager* asset_manager() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return nullptr;
    jclass cls = env->GetObjectClass(activity);
    jmethodID get = env->GetMethodID(cls, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject assets = get ? env->CallObjectMethod(activity, get) : nullptr;
    AAssetManager* manager = assets ? AAssetManager_fromJava(env, assets) : nullptr;
    // The Java AssetManager outlives this: it belongs to the activity.
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return manager;
}
std::string read_asset(AAssetManager* manager, const char* name) {
    AAsset* asset = AAssetManager_open(manager, name, AASSET_MODE_BUFFER);
    if (!asset) return {};
    std::string text(static_cast<const char*>(AAsset_getBuffer(asset)), size_t(AAsset_getLength(asset)));
    AAsset_close(asset);
    return text;
}
void make_parents(const std::string& path) {
    for (size_t at = path.find('/', 1); at != std::string::npos; at = path.find('/', at + 1)) mkdir(path.substr(0, at).c_str(), 0755);
}
void unpack_data(const std::string& own) {
    AAssetManager* manager = asset_manager();
    if (!manager) { std::fprintf(stderr, "[android] no asset manager: the port's data is not unpacked\n"); return; }
    const std::string id = read_asset(manager, "data.stamp");
    const std::string stamp = own + "/data/.stamp";
    {
        std::ifstream have(stamp);
        std::string old((std::istreambuf_iterator<char>(have)), std::istreambuf_iterator<char>());
        if (!id.empty() && old == id) return;
    }
    const std::string list = read_asset(manager, "data.files");
    unsigned files = 0, failed = 0;
    for (size_t at = 0; at < list.size();) {
        size_t end = list.find('\n', at);
        if (end == std::string::npos) end = list.size();
        const std::string name = list.substr(at, end - at);
        at = end + 1;
        if (name.empty()) continue;
        AAsset* asset = AAssetManager_open(manager, name.c_str(), AASSET_MODE_STREAMING);
        const std::string target = own + "/" + name;
        make_parents(target);
        FILE* out = asset ? std::fopen((target + ".part").c_str(), "wb") : nullptr;
        bool ok = out != nullptr;
        char buffer[1 << 16];
        for (int n; ok && (n = AAsset_read(asset, buffer, sizeof buffer)) > 0;) ok = std::fwrite(buffer, 1, size_t(n), out) == size_t(n);
        if (out && std::fclose(out) != 0) ok = false;
        if (asset) AAsset_close(asset);
        if (ok && std::rename((target + ".part").c_str(), target.c_str()) == 0) ++files;
        else { ++failed; std::fprintf(stderr, "[android] cannot unpack %s\n", name.c_str()); }
    }
    if (!failed) { std::ofstream(stamp) << id; }
    std::fprintf(stderr, "[android] unpacked %u files, %u failed\n", files, failed);
}

struct Run { std::vector<std::string> args; int rc = 1; };
void* run_game(void* arg) {
    auto& run = *static_cast<Run*>(arg);
    std::vector<char*> argv;
    for (auto& a : run.args) argv.push_back(a.data());
    argv.push_back(nullptr);
    run.rc = uc_game_main(int(run.args.size()), argv.data());
    return nullptr;
}
}  // namespace

extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char** argv) {
    const char* diagnostic = nullptr;
    if (argc == 3 && std::strcmp(argv[1], "--private-game") == 0) diagnostic = argv[2];
    const char* external = SDL_GetAndroidExternalStoragePath();
    const std::string own = diagnostic ? diagnostic : external ? external : "";
    if (!own.empty()) g_log = std::fopen((own + "/uc.log").c_str(), "w");
    log_to_logcat();
    if (own.empty()) { std::fprintf(stderr, "[android] no app folder\n"); return 1; }
    unpack_data(own);
    const std::string bases[2] = {own, diagnostic ? std::string() : std::string("/sdcard/UmbrellaChronicles")};
    std::string base;
    for (const auto& b : bases)
        if (!b.empty() && access((b + "/game/DATA/sys/main.dol").c_str(), R_OK) == 0) { base = b; break; }
    if (base.empty()) {
        std::fprintf(stderr, "[android] no game found: copy the game folder (game/DATA/...) to %s or %s (and allow the app all-files access)\n",
                     own.c_str(), bases[1].c_str());
        return 1;
    }
    std::fprintf(stderr, "[android] game %s/game, app folder %s\n", base.c_str(), own.c_str());
    if (chdir(own.c_str()) != 0) { std::fprintf(stderr, "[android] cannot enter %s: %s\n", own.c_str(), std::strerror(errno)); return 1; }
    load_env(own + "/env.txt");
    setenv("UC_EXE_PATH", (own + "/uc").c_str(), 1);   // config.json, data/, logs/ beside it (exe_path.h)

    Run run;
    run.args = {"uc", "--game", base + "/game"};
    for (auto& a : load_args(own + "/args.txt")) run.args.push_back(a);
    for (auto& a : run.args) std::fprintf(stderr, "[android] arg %s\n", a.c_str());
    // Every guest call is a host call: the game's main thread gets the same deep stack as HOTD4's.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, run_game, &run) != 0) { std::fprintf(stderr, "[android] cannot start the game thread\n"); return 1; }
    pthread_join(thread, nullptr);
#ifdef UC_OPENXR
    if (uc::complete_android_xr_restart()) return run.rc;
#endif
#ifdef UC_PGO_GENERATE
    // An instrumented build (scripts/build-quest.sh with UC_QUEST_PGO=generate): the app's process outlives the game, so its exit-time
    // profile write would never run; write it here, into the app folder (scripts/quest_pgo.sh pulls it).
    {
        __llvm_profile_set_filename((own + "/uc.profraw").c_str());
        std::fprintf(stderr, "[android] PGO profile %s: %d\n", (own + "/uc.profraw").c_str(), __llvm_profile_write_file());
    }
#endif
    std::fprintf(stderr, "[android] game exited %d\n", run.rc);
    return run.rc;
}
