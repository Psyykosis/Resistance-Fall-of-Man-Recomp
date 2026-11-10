/* rfom_pcport.inc -- PC-port conveniences that run at process start (included
 * by main.cpp; both executables).
 *
 * Save location (v200)
 *   Saves and trophies live in  %USERPROFILE%\Saved Games\Resistance Fall of Man\
 *   (dev_hdd0\home\00000001\savedata\BCUS98107_* and trophies\) instead of the
 *   game folder, so reinstalling / updating the port can't touch them.
 *   - First start: an existing <game>\gamedata\dev_hdd0\home and
 *     <game>\gamedata\trophies are COPIED there (the old copy is left alone
 *     as a backup).
 *   - RFOM_SAVE_DIR=portable keeps the old place (<game>\gamedata);
 *     RFOM_SAVE_DIR=<folder> uses that folder.
 *   - An explicit PS3_GAMEDATA_ROOT wins over all of this.
 *   The runtime reads PS3_GAMEDATA_ROOT (cellSaveData.c, sceNpTrophy.c); the
 *   game's own data under gamedata\dev_hdd0\game stays where it is. */
#pragma once
#ifdef _WIN32
#include <shlobj.h>
#include <knownfolders.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static int rfom_dir_has_files(const wchar_t* dir)
{
    wchar_t pat[MAX_PATH * 2];
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int any = 0;
    do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) { any = 1; break; } } while (FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

/* copy a tree; files already at the destination are kept */
static int rfom_copy_tree(const wchar_t* src, const wchar_t* dst)
{
    SHCreateDirectoryExW(NULL, dst, NULL);
    wchar_t pat[MAX_PATH * 2];
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", src);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t s[MAX_PATH * 2], d[MAX_PATH * 2];
        _snwprintf(s, MAX_PATH * 2, L"%ls\\%ls", src, fd.cFileName);
        _snwprintf(d, MAX_PATH * 2, L"%ls\\%ls", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) n += rfom_copy_tree(s, d);
        else if (CopyFileW(s, d, TRUE)) n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* a wide path the narrow (ANSI) file APIs of the runtime can open: as is when
 * it converts losslessly, else its 8.3 short form */
static int rfom_narrow_path(const wchar_t* w, char* out, int cap)
{
    BOOL lossy = FALSE;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, cap, NULL, &lossy) > 0 && !lossy) return 1;
    wchar_t sh[MAX_PATH * 2];
    if (!GetShortPathNameW(w, sh, MAX_PATH * 2)) return 0;
    lossy = FALSE;
    return WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, sh, -1, out, cap, NULL, &lossy) > 0 && !lossy;
}

static void rfom_save_location_init(void)
{
    const char* pre = getenv("PS3_GAMEDATA_ROOT");
    if (pre && *pre) { printf("[saves] folder: %s (PS3_GAMEDATA_ROOT)\n", pre); return; }
    const char* sel = getenv("RFOM_SAVE_DIR");
    if (sel && !_stricmp(sel, "portable")) { printf("[saves] folder: <game folder>\\gamedata (RFOM_SAVE_DIR=portable)\n"); return; }

    wchar_t root[MAX_PATH * 2] = L"";
    if (sel && *sel) MultiByteToWideChar(CP_ACP, 0, sel, -1, root, MAX_PATH * 2);
    else {
        PWSTR sg = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, NULL, &sg)) && sg)
            _snwprintf(root, MAX_PATH * 2, L"%ls\\Resistance Fall of Man", sg);
        if (sg) CoTaskMemFree(sg);
    }
    if (!root[0]) { printf("[saves] no Saved Games folder -- keeping saves in <game folder>\\gamedata\n"); return; }
    SHCreateDirectoryExW(NULL, root, NULL);

    /* one-time copy of saves made before v200 */
    wchar_t nsave[MAX_PATH * 2];
    _snwprintf(nsave, MAX_PATH * 2, L"%ls\\dev_hdd0\\home\\00000001\\savedata", root);
    if (!rfom_dir_has_files(nsave) && rfom_dir_has_files(L"gamedata\\dev_hdd0\\home\\00000001\\savedata")) {
        wchar_t nh[MAX_PATH * 2], nt[MAX_PATH * 2];
        _snwprintf(nh, MAX_PATH * 2, L"%ls\\dev_hdd0\\home", root);
        _snwprintf(nt, MAX_PATH * 2, L"%ls\\trophies", root);
        const int a = rfom_copy_tree(L"gamedata\\dev_hdd0\\home", nh);
        const int b = GetFileAttributesW(L"gamedata\\trophies") != INVALID_FILE_ATTRIBUTES ? rfom_copy_tree(L"gamedata\\trophies", nt) : 0;
        printf("[saves] copied %d save file(s) and %d trophy file(s) from <game folder>\\gamedata (left there as a backup)\n", a, b);
    }

    char nroot[MAX_PATH * 2];
    if (!rfom_narrow_path(root, nroot, (int)sizeof nroot)) {
        printf("[saves] the Saved Games path can't be opened by the runtime -- keeping saves in <game folder>\\gamedata\n");
        return;
    }
    _putenv_s("PS3_GAMEDATA_ROOT", nroot);   /* the game process inherits it */
    printf("[saves] folder: %ls\\dev_hdd0\\home\\00000001\\savedata\n", root);
}

/* RFOM_PRESET=auto|low|medium|high|ultra: fill every graphics / frame-rate
 * variable that is not set already from the hardware rules the launcher uses
 * (project/rfom_autoconfig.h). Runs before the renderer starts. */
#include "rfom_autoconfig.h"
static void rfom_preset_init(void)
{
    const char* p = getenv("RFOM_PRESET");
    if (!p || !*p) return;
    char sum[640];
    rfac::apply_preset_env(p, sum, sizeof sum);
    printf("[preset] %s\n", sum);
}
#else
static void rfom_save_location_init(void) {}
static void rfom_preset_init(void) {}
#endif
