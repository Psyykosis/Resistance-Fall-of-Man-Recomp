// Resistance: Fall of Man PC port: mods folder (RFOM_MODS).
//
// RFOM_MODS=1 (launcher: PC OPTIONS > Mods folder) reads the "mods" folder next to the
// executable (RFOM_MODS=<folder> names another one). Each sub-folder is one mod, laid out like
// the game's PS3_GAME folder:
//
//     mods\MyMod\USRDIR\<the game file to replace>
//
// A file there is opened in place of the game's own copy, by every file layer of the runtime
// (cellFs, sys_fs: the g_vfs_override_hook in runtime/syscalls/sys_fs.c). Mods are applied in
// alphabetical order of their folder names; when two replace the same file the later one wins.
// A folder whose name starts with '_' or '-' is skipped (disabled). Only whole files can be
// replaced (no texture replacement). The overrides are listed in the log at start-up.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <mutex>

extern "C" void (*volatile g_vfs_override_hook)(char* path, size_t cap);   // runtime/syscalls/sys_fs.c

namespace {

std::map<std::string, std::string> s_index;   // "usrdir/x/y.bin" (lower case, '/') -> host path

std::string lower_slash(std::string s)
{
    for (auto& c : s) c = c == '\\' ? '/' : (char)tolower((unsigned char)c);
    return s;
}

void scan(const std::string& dir, const std::string& rel, std::map<std::string, std::string>& out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string full = dir + "\\" + fd.cFileName;
        const std::string r = rel.empty() ? std::string(fd.cFileName) : rel + "/" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scan(full, r, out);
        else out[lower_slash(r)] = full;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void build_index(const std::string& root)
{
    std::vector<std::string> mods;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.' || fd.cFileName[0] == '_' || fd.cFileName[0] == '-') continue;
            mods.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(mods.begin(), mods.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    for (const auto& m : mods) {
        std::map<std::string, std::string> files;
        scan(root + "\\" + m, "", files);
        fprintf(stderr, "[mods] %s: %zu file(s)\n", m.c_str(), files.size());
        for (auto& [k, v] : files) {
            if (s_index.count(k)) fprintf(stderr, "[mods]   %s overrides the same file from an earlier mod\n", k.c_str());
            s_index[k] = v;
        }
    }
    for (const auto& [k, v] : s_index) fprintf(stderr, "[mods] override: %s -> %s\n", k.c_str(), v.c_str());
    fprintf(stderr, "[mods] %zu mod(s), %zu file override(s) from %s\n", mods.size(), s_index.size(), root.c_str());
}

// The part of a resolved game path a mod replaces: from "PS3_GAME/" on (without it), else from "USRDIR/".
const char* game_rel(const char* p)
{
    for (const char* q = p; *q; q++) {
        if ((q == p || q[-1] == '/' || q[-1] == '\\') && !_strnicmp(q, "PS3_GAME", 8) && (q[8] == '/' || q[8] == '\\'))
            return q + 9;
    }
    for (const char* q = p; *q; q++) {
        if ((q == p || q[-1] == '/' || q[-1] == '\\') && !_strnicmp(q, "USRDIR", 6) && (q[6] == '/' || q[6] == '\\'))
            return q;
    }
    return nullptr;
}

void override_hook(char* path, size_t cap)
{
    if (s_index.empty()) return;
    const char* rel = game_rel(path);
    if (!rel) return;
    std::string key = lower_slash(rel);
    // collapse "a//b" and "./"
    std::string k2; k2.reserve(key.size());
    for (size_t i = 0; i < key.size(); i++) {
        if (key[i] == '/' && !k2.empty() && k2.back() == '/') continue;
        if (key[i] == '.' && (i + 1 < key.size() && key[i + 1] == '/') && (k2.empty() || k2.back() == '/')) { i++; continue; }
        k2 += key[i];
    }
    const auto it = s_index.find(k2);
    if (it == s_index.end() || it->second.size() + 1 > cap) return;
    static std::mutex mtx;
    static std::map<std::string, int> said;
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (!said[k2]++) fprintf(stderr, "[mods] using %s\n", it->second.c_str());
    }
    snprintf(path, cap, "%s", it->second.c_str());
}

std::string exe_dir()
{
    char p[MAX_PATH] = "";
    GetModuleFileNameA(NULL, p, MAX_PATH);
    if (char* s = strrchr(p, '\\')) *s = 0;
    return p;
}

struct ModsInit {
    ModsInit()
    {
        const char* e = getenv("RFOM_MODS");
        if (!e || !*e || *e == '0' || !_stricmp(e, "off")) return;
        std::string root;
        if (!strcmp(e, "1") || !_stricmp(e, "on")) {
            // next to the exe, or its parent (build\ResistanceFoM.exe / build_game\ResistanceFoMGame.exe with mods\ in the port folder)
            const std::string d = exe_dir();
            root = d + "\\mods";
            if (GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES) root = d + "\\..\\mods";
            if (GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES) root = "mods";
        } else root = e;
        if (GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "[mods] RFOM_MODS=%s: folder %s not found, no mods\n", e, root.c_str());
            return;
        }
        build_index(root);
        if (!s_index.empty()) g_vfs_override_hook = override_hook;
    }
} s_mods_init;

} // namespace
