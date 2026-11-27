// games/common/game_entry.cpp - the process entry of every executable.
//
// Reconnects stdio on Windows, selects the game among the games the
// executable links (--disc's, game_build.h, or the launcher's pick,
// game_pick.h), makes its final install, and runs its entry. SDL3's
// SDL_main.h re-#defines main -> SDL_main and supplies the platform entry
// shim; it must be included by the TU defining main().

#include <SDL3/SDL_main.h>

#include "openpete_disc_build.h"
#include "game_build.h"
#include "game_pick.h"
#include "launcher_assets.h"
#include "openpete_bugreport_replay.h"
#include "openpete_launcher.h"
#include "openpete_paths.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>

/** @brief Bind one C stdio stream to the OS standard handle it belongs to, unbuffered.
 *
 * A distribution Windows build links the GUI subsystem (-mwindows), so its
 * CRT streams start unconnected; launched from a terminal, behind a
 * `> log` redirect, or through WSL2 interop (which passes inherited pipe
 * handles, not a console), the streams are bound here to whatever the
 * standard handle refers to (console, pipe or file). A stream already wired
 * to that handle (a console-subsystem build, or any CRT that inherited its
 * handles, which is every WSL-interop launch) is left connected:
 * _open_osfhandle would take a second CRT reference to the same HANDLE and
 * _dup2 would close the destination fd first, closing the one handle under
 * both references, after which every write to the stream is dropped.
 * @param which The standard handle (STD_OUTPUT_HANDLE, STD_ERROR_HANDLE,
 *        STD_INPUT_HANDLE).
 * @param stream The C stream to bind (stdout, stderr, stdin). */
static void reconnect_std(DWORD which, FILE* stream) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    int target = _fileno(stream);

    if (target >= 0 && (HANDLE)_get_osfhandle(target) == h) {
        setvbuf(stream, NULL, _IONBF, 0);
        return;
    }

    int fd = _open_osfhandle((intptr_t)h, _O_TEXT);
    if (fd == -1) return;
    if (fd != target) { _dup2(fd, target); _close(fd); }
    setvbuf(stream, NULL, _IONBF, 0);
}

/** @brief Attach the process to its parent console and rebind stdout, stderr and stdin to the OS handles.
 *
 * An AttachConsole failure (the parent has no console, or stdio is a pipe or
 * file) is ignored; each stream is then bound by reconnect_std to whatever
 * its standard handle refers to. */
static void attach_parent_console(void) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    reconnect_std(STD_OUTPUT_HANDLE, stdout);
    reconnect_std(STD_ERROR_HANDLE,  stderr);
    reconnect_std(STD_INPUT_HANDLE,  stdin);
}
#else
/** @brief Do nothing: outside Windows the C stdio streams are already connected. */
static void attach_parent_console(void) {}
#endif

/** @brief Harness: the disc and content hash OPENPETE_ASSET_DISCS names for a game.
 *
 * The variable holds `game=sha256=path` items separated by ';'.
 * @param id The game id.
 * @param sha Receives the content hash.
 * @param path Receives the disc path.
 * @return true when the variable names the game. */
static bool asset_disc_of(const char* id, std::string& sha, std::string& path) {
    const char* e = std::getenv("OPENPETE_ASSET_DISCS");
    if (!e) return false;
    const std::string all(e);
    size_t at = 0;
    while (at <= all.size()) {
        const size_t end = std::min(all.find(';', at), all.size());
        const std::string item = all.substr(at, end - at);
        const size_t a = item.find('='), b = a == std::string::npos ? a : item.find('=', a + 1);
        if (b != std::string::npos && item.compare(0, a, id) == 0 && a == std::strlen(id)) {
            sha = item.substr(a + 1, b - a - 1);
            path = item.substr(b + 1);
            return true;
        }
        at = end + 1;
    }
    return false;
}

/** @brief Harness: install every other linked game before the target when OPENPETE_INSTALL_OTHERS_FIRST is set, as a launcher that ran other games' jobs has.
 *
 * With the value `extract` each other game's asset extraction job also runs
 * after its install, inline in the `ingest` role, on the disc
 * OPENPETE_ASSET_DISCS names for it (a game it names no disc for fails the
 * run's log line and runs no job); OPENPETE_ASSET_DUMP names a directory the
 * results are written to.
 * @param target The game the final install makes the running game. */
static void install_others_first(const openpete_game_build_t* target) {
    const char* e = std::getenv("OPENPETE_INSTALL_OTHERS_FIRST");
    if (!e || !*e || *e == '0') return;
    const bool extract = std::strcmp(e, "extract") == 0;
    const char* dump = std::getenv("OPENPETE_ASSET_DUMP");
    for (size_t i = 0; i < openpete_game_build_count; i++) {
        const openpete_game_build_t* b = openpete_game_builds[i];
        if (b == target) continue;
        openpete_game_build_install(b);
        if (!extract) continue;
        std::string sha, path;
        if (!asset_disc_of(b->id, sha, path)) {
            std::fprintf(stderr, "openpete: OPENPETE_ASSET_DISCS names no disc of %s\n", b->id);
            continue;
        }
        const int rc = game_assets_run_inline(b, path.c_str(), sha.c_str(), dump && *dump ? dump : nullptr);
        std::fprintf(stderr, "openpete: asset extraction of %s: %s\n", b->id,
                     rc == 0 ? "ok" : rc == -2 ? "stopped" : "failed");
    }
    std::fprintf(stderr, "openpete: installed the %zu other linked games before %s "
                         "(OPENPETE_INSTALL_OTHERS_FIRST)\n", openpete_game_build_count - 1, target->id);
}

/** @brief Harness: write every installed global to the file OPENPETE_INSTALL_DUMP names, after the final install. */
static void install_dump() {
    const char* path = std::getenv("OPENPETE_INSTALL_DUMP");
    if (!path || !*path) return;
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    openpete_game_install_dump(f);
    std::fclose(f);
}

/** @brief Select and install the game this process runs, then run its entry.
 *
 * stdio is reconnected first, before any output and whichever game runs.
 * A --bug-report-replay command line is first rebuilt from the bundle
 * (openpete_bugreport_replay_prepare); a refused bundle exits 64. A run
 * whose write paths leave the OPENPETE_SANDBOX root for a real per-user
 * location exits OPENPETE_EXIT_SANDBOX_GUARD (openpete_paths_sandbox_guard).
 * The game is the one --disc names, else the launcher's pick over every
 * linked game's library entries (game_pick.h), which installs each game
 * whose jobs it runs; the final install here makes the picked game the
 * running game before its entry. The return to the launcher is armed first
 * (openpete_launcher_return_arm) and withdrawn when the entry's status is
 * not OPENPETE_EXIT_OK.
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The game entry's exit status, or 1 when the executable links no game. */
int main(int argc, char** argv) {
    attach_parent_console();
    /* First, before any atexit registration: the return to the launcher
     * relaunches from the last atexit handler to run. */
    openpete_launcher_return_arm(argc, argv);
    if (openpete_game_build_count == 0) {
        std::fprintf(stderr, "openpete: this executable links no game\n");
        return 1;
    }
    /* The sandbox root is latched from the environment before a replay
     * replaces it, so the guard is enforced exactly when the caller set
     * OPENPETE_SANDBOX. A bug-report replay rebuilds the command line from
     * the bundle before the game is chosen from it. */
    /* Every linked game's known disc builds, before the launcher's library
     * (openpete_game_pick) or the boot classifies a disc of any of them. */
    for (size_t i = 0; i < openpete_game_build_count; i++) {
        const openpete_game_runtime_t* rt = openpete_game_builds[i]->runtime;
        if (rt) openpete_disc_build_add_rows(rt->disc_builds);
    }
    (void)openpete_paths_sandbox();
    if (const int rrc = openpete_bugreport_replay_prepare(&argc, &argv)) return rrc;
    if (const int src = openpete_paths_sandbox_guard(argc, argv)) return src;
    const openpete_game_build_t* b = openpete_game_build_named(argc, argv);
    if (!b) {
        int rc = 0;
        b = openpete_game_pick(argc, argv, &rc);
        if (!b) return rc;
    }
    install_others_first(b);
    openpete_game_build_install(b);
    install_dump();
    const int rc = openpete_game_main(argc, argv);
    openpete_launcher_return_settle(rc);
    return rc;
}
