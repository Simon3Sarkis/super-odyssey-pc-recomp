// Updates from GitHub releases (runtime/updater.h). Windows: WinHTTP for the API and the download, CNG for the
// SHA-256 GitHub lists for each release file, the system's tar.exe to unpack the zip. Other systems: not offered.
#include "updater.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "rt.h"
#include "settings.h"
#include "version.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static UpdStatus g_st = {.state = UPD_IDLE};
static char g_zip_url[1024], g_zip_sha[65];  // the release's Windows zip and its SHA-256 (hex; "" if GitHub gave none)
static volatile int g_busy;                  // a check or an install is running

const char *updater_repo(void) {
    const char *e = getenv("ODYSSEY_UPDATE_REPO");
    return e && *e ? e : APP_UPDATE_REPO;
}

void updater_status(UpdStatus *out) {
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    snprintf(out->current, sizeof out->current, "%s", APP_VERSION);
    out->can_install = g_zip_url[0] && g_st.latest[0] && g_st.state != UPD_LATEST;
    if (!updater_repo()[0]) out->state = UPD_UNCONFIGURED;
    pthread_mutex_unlock(&g_lock);
}

static void set_state(int s, const char *error) {
    pthread_mutex_lock(&g_lock);
    g_st.state = s;
    if (error) snprintf(g_st.error, sizeof g_st.error, "%s", error);
    pthread_mutex_unlock(&g_lock);
    if (error) LOG("[update] %s\n", error);
}

// ASCII, case-insensitive (no CRT locale calls on the updater's threads)
static bool same_ascii(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return false;
        if (!x) return true;
    }
}

// "v1.2.3" / "1.2" -> comparable parts; true when a is newer than b
__attribute__((noinline)) static void parse_version(const char *s, int v[3]) {
    // (by hand, indexed: sscanf and an inlined pointer walk both faulted here - clang -O2 folded the walk over the
    // constant into reads at address 0x10 - it was winuser.h's RT_VERSION, resource type 16: the macro is APP_VERSION now)
    v[0] = v[1] = v[2] = 0;
    size_t i = 0, n = strlen(s);
    while (i < n && (s[i] < '0' || s[i] > '9')) i++;
    for (int part = 0; part < 3 && i < n; part++) {
        while (i < n && s[i] >= '0' && s[i] <= '9') v[part] = v[part] * 10 + (s[i++] - '0');
        if (i >= n || s[i] != '.') break;
        i++;
    }
}
static bool newer(const char *a, const char *b) {
    int x[3], y[3];
    parse_version(a, x), parse_version(b, y);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

// The string value of "key" in [p, end) (JSON; the escapes a tag, URL or digest can hold: \/ and \"), "" if absent
static bool json_str(const char *p, const char *end, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    size_t pl = strlen(pat);
    out[0] = 0;
    for (const char *q = p; q + pl <= end; q++) {
        if (memcmp(q, pat, pl)) continue;
        q += pl;
        while (q < end && (*q == ' ' || *q == ':' || *q == '\n' || *q == '\r' || *q == '\t')) q++;
        if (q >= end || *q != '"') return false;  // (null or a number)
        size_t n = 0;
        for (q++; q < end && *q != '"'; q++) {
            if (*q == '\\' && q + 1 < end) q++;
            if (n + 1 < cap) out[n++] = *q;
        }
        out[n] = 0;
        return true;
    }
    return false;
}

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc((size_t)(n > 0 ? n : 1) * sizeof *w);
    if (w) { w[0] = 0; MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); }
    return w;
}

// The program's folder (where the exe is), with a trailing backslash
static bool program_dir(wchar_t *out, size_t cap) {
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (!n || n >= cap) return false;
    wchar_t *s = wcsrchr(out, L'\\');
    if (!s) return false;
    s[1] = 0;
    return true;
}

// GET url: the body to sink (or into *buf when sink is NULL), progress by bytes. HTTP status, or -1 (no connection).
typedef bool (*Sink)(void *ctx, const void *data, DWORD n);
static int http_get(const char *url, Sink sink, void *ctx, char **buf, size_t *len, void (*progress)(uint64_t, uint64_t)) {
    int status = -1;
    wchar_t *wurl = wide(url);
    URL_COMPONENTS uc = {.dwStructSize = sizeof uc, .dwHostNameLength = (DWORD)-1, .dwUrlPathLength = (DWORD)-1,
                         .dwExtraInfoLength = (DWORD)-1};
    HINTERNET s = NULL, c = NULL, r = NULL;
    wchar_t host[256], path[2048];
    if (!wurl || !WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS || uc.dwHostNameLength >= 256 ||
        uc.dwUrlPathLength + uc.dwExtraInfoLength >= 2048)
        goto out;
    wcsncpy(host, uc.lpszHostName, uc.dwHostNameLength), host[uc.dwHostNameLength] = 0;
    wcsncpy(path, uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength), path[uc.dwUrlPathLength + uc.dwExtraInfoLength] = 0;
    // (a plain client name: nothing about the player)
    s = WinHttpOpen(L"SuperOdysseyRecomp-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) goto out;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    c = WinHttpConnect(s, host, uc.nPort, 0);
    r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (!r) goto out;
    // (GitHub's release files redirect to its download host: followed, HTTPS only - WinHTTP's default policy)
    if (!WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28", (DWORD)-1, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, NULL))
        goto out;
    DWORD code = 0, sz = sizeof code;
    if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL)) goto out;
    status = (int)code;
    if (code != 200) goto out;
    uint64_t total = 0, got = 0;
    {
        wchar_t cl[32];
        DWORD cls = sizeof cl;
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, NULL, cl, &cls, NULL)) total = _wcstoui64(cl, NULL, 10);
    }
    size_t cap = 0;
    if (buf) *buf = NULL, *len = 0;
    for (;;) {
        char chunk[65536];
        DWORD n = 0;
        if (!WinHttpReadData(r, chunk, sizeof chunk, &n)) { status = -1; break; }
        if (!n) break;
        got += n;
        if (sink) {
            if (!sink(ctx, chunk, n)) { status = -2; break; }
        } else {
            if (*len + n + 1 > cap) {
                cap = (*len + n + 1) * 2;
                char *nb = realloc(*buf, cap);
                if (!nb) { status = -2; break; }
                *buf = nb;
            }
            memcpy(*buf + *len, chunk, n);
            *len += n;
            (*buf)[*len] = 0;
        }
        if (progress) progress(got, total);
        if (got > (uint64_t)2 << 30) { status = -2; break; }  // (no release is that big)
    }
out:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    free(wurl);
    return status;
}

// ---- check: GET /repos/<repo>/releases/latest (the newest release that isn't a draft or a pre-release)
static void *check_thread(void *arg) {
    (void)arg;
    char url[512], *body = NULL;
    size_t len = 0;
    if (getenv("ODYSSEY_UPDATE_TEST_ZIP")) {  // (test) a release v99.0.0 whose download is that local zip: no network
        pthread_mutex_lock(&g_lock);
        snprintf(g_st.latest, sizeof g_st.latest, "v99.0.0");
        snprintf(g_zip_url, sizeof g_zip_url, "(test)");
        g_zip_sha[0] = 0;
        pthread_mutex_unlock(&g_lock);
        set_state(UPD_AVAILABLE, NULL);
        g_busy = 0;
        if (getenv("ODYSSEY_UPDATE_TEST_AUTO")) updater_install();  // (test: install and restart by themselves)
        return NULL;
    }
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", updater_repo());
    LOG("[update] checking %s\n", url);
    int st = http_get(url, NULL, NULL, &body, &len, NULL);
    LOG("[update] GitHub answered %d (%zu bytes)\n", st, len);
    if (st != 200 || !body) {
        char e[160];
        if (st == 404) snprintf(e, sizeof e, "No release published yet.");
        else if (st == 403 || st == 429) snprintf(e, sizeof e, "GitHub is busy (rate limit). Try again later.");
        else if (st < 0) snprintf(e, sizeof e, "Couldn't reach GitHub. Check the internet connection.");
        else snprintf(e, sizeof e, "GitHub answered %d.", st);
        set_state(st == 404 ? UPD_LATEST : UPD_FAILED, e);
        free(body);
        g_busy = 0;
        return NULL;
    }
    char tag[64], page[512];
    json_str(body, body + len, "tag_name", tag, sizeof tag);
    json_str(body, body + len, "html_url", page, sizeof page);
    // the release's files: the one whose download ends in -windows.zip, and its digest (in the same file entry, before
    // its download URL: GitHub's order - "digest" comes with "size", "browser_download_url" last)
    char zip[1024] = "", sha[80] = "";
    const char *end = body + len;
    for (const char *p = strstr(body, "\"browser_download_url\""); p; p = strstr(p + 1, "\"browser_download_url\"")) {
        char u[1024];
        json_str(p, end, "browser_download_url", u, sizeof u);
        size_t ul = strlen(u);
        if (ul < 12 || !same_ascii(u + ul - 12, "-windows.zip")) continue;
        snprintf(zip, sizeof zip, "%s", u);
        // this file's entry: from the "url" of its own (".../releases/assets/<id>") on
        const char *start = p;
        for (const char *q = p; q > body; q--)
            if (!memcmp(q, "/releases/assets/", 17)) { start = q; break; }
        char d[96];
        if (json_str(start, p, "digest", d, sizeof d) && !strncmp(d, "sha256:", 7) && strlen(d + 7) == 64) snprintf(sha, sizeof sha, "%s", d + 7);
        break;
    }
    free(body);
    pthread_mutex_lock(&g_lock);
    snprintf(g_st.latest, sizeof g_st.latest, "%s", tag);
    snprintf(g_st.page, sizeof g_st.page, "%s", page);
    snprintf(g_zip_url, sizeof g_zip_url, "%s", zip);
    snprintf(g_zip_sha, sizeof g_zip_sha, "%s", sha);
    pthread_mutex_unlock(&g_lock);
    if (!tag[0]) set_state(UPD_FAILED, "The release list didn't make sense.");
    else if (!newer(tag, APP_VERSION)) set_state(UPD_LATEST, NULL);
    else if (!zip[0]) set_state(UPD_FAILED, "The newest release has no Windows download yet.");
    else set_state(UPD_AVAILABLE, NULL);
    LOG("[update] newest release %s (this is %s)%s\n", tag[0] ? tag : "?", APP_VERSION, zip[0] ? "" : ", no Windows zip");
    g_busy = 0;
    return NULL;
}

void updater_check(void) {
    if (!updater_repo()[0] || g_busy) return;
    g_busy = 1;
    set_state(UPD_CHECKING, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, check_thread, NULL)) { g_busy = 0; set_state(UPD_FAILED, "Couldn't start the check."); return; }
    pthread_detach(t);
}

void updater_auto_check(void) {
    if (g_settings.check_updates && !getenv("ODYSSEY_NO_UPDATE_CHECK")) updater_check();
}

// ---- install
typedef struct { HANDLE f; BCRYPT_HASH_HANDLE h; } Dl;
static bool dl_sink(void *ctx, const void *data, DWORD n) {
    Dl *d = ctx;
    DWORD w = 0;
    if (d->h) BCryptHashData(d->h, (PUCHAR)data, n, 0);
    return WriteFile(d->f, data, n, &w, NULL) && w == n;
}
static void dl_progress(uint64_t got, uint64_t total) {
    pthread_mutex_lock(&g_lock);
    g_st.progress = total ? (float)((double)got / (double)total) * 0.9f : 0;
    pthread_mutex_unlock(&g_lock);
}

static bool run_hidden(wchar_t *cmd) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
}

static void remove_tree_w(const wchar_t *dir) {
    wchar_t pat[MAX_PATH * 2];
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[MAX_PATH * 2];
            _snwprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree_w(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

// The new files over the program's: each one in use (the exe, its DLLs) or already there is renamed to <name>.old
// first (Windows lets a running program's files be renamed, not replaced), the list of those goes to old (removed at
// the next start: updater_startup). On a failure the renamed ones go back. Returns the number of files, -1 failed.
typedef struct { wchar_t (*moved)[MAX_PATH * 2]; int n, cap; FILE *old; } Apply;
static int apply_dir(const wchar_t *src, const wchar_t *dst, Apply *a) {
    CreateDirectoryW(dst, NULL);
    wchar_t pat[MAX_PATH * 2];
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int files = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t s[MAX_PATH * 2], d[MAX_PATH * 2];
        _snwprintf(s, MAX_PATH * 2, L"%ls\\%ls", src, fd.cFileName);
        _snwprintf(d, MAX_PATH * 2, L"%ls\\%ls", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int k = apply_dir(s, d, a);
            if (k < 0) { files = -1; break; }
            files += k;
            continue;
        }
        if (GetFileAttributesW(d) != INVALID_FILE_ATTRIBUTES) {
            wchar_t o[MAX_PATH * 2 + 8];
            _snwprintf(o, MAX_PATH * 2 + 8, L"%ls.old", d);
            SetFileAttributesW(o, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(o);  // (one left from an update before, if it isn't in use)
            if (!MoveFileExW(d, o, MOVEFILE_REPLACE_EXISTING)) { files = -1; break; }
            if (a->n == a->cap) {
                a->cap = a->cap ? a->cap * 2 : 64;
                void *m = realloc(a->moved, (size_t)a->cap * sizeof *a->moved);
                if (!m) { MoveFileExW(o, d, 0); files = -1; break; }
                a->moved = m;
            }
            wcscpy(a->moved[a->n++], d);
            char u[MAX_PATH * 6];
            WideCharToMultiByte(CP_UTF8, 0, o, -1, u, sizeof u, NULL, NULL);
            if (a->old) fprintf(a->old, "%s\n", u);
        }
        if (!CopyFileW(s, d, FALSE)) { files = -1; break; }
        files++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return files;
}

static void *install_thread(void *arg) {
    (void)arg;
    wchar_t dir[MAX_PATH], upd[MAX_PATH + 16], zip[MAX_PATH + 32], stage[MAX_PATH + 32];
    char url[1024], sha[65];
    pthread_mutex_lock(&g_lock);
    snprintf(url, sizeof url, "%s", g_zip_url);
    snprintf(sha, sizeof sha, "%s", g_zip_sha);
    g_st.progress = 0;
    pthread_mutex_unlock(&g_lock);
    if (!program_dir(dir, MAX_PATH)) { set_state(UPD_FAILED, "Couldn't find the program's folder."); g_busy = 0; return NULL; }
    _snwprintf(upd, MAX_PATH + 16, L"%lsupdate", dir);
    _snwprintf(zip, MAX_PATH + 32, L"%ls\\package.zip", upd);
    _snwprintf(stage, MAX_PATH + 32, L"%ls\\files", upd);
    remove_tree_w(upd);
    CreateDirectoryW(upd, NULL);
    CreateDirectoryW(stage, NULL);
    // download (and its SHA-256 on the way)
    Dl d = {.f = CreateFileW(zip, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)};
    if (d.f == INVALID_HANDLE_VALUE) { set_state(UPD_FAILED, "Couldn't write in the program's folder."); g_busy = 0; return NULL; }
    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) BCryptCreateHash(alg, &d.h, NULL, 0, NULL, 0, 0);
    int st;
    if (getenv("ODYSSEY_UPDATE_TEST_ZIP")) {  // (test) the local zip, through the same hashing and writing
        FILE *tf = fopen(getenv("ODYSSEY_UPDATE_TEST_ZIP"), "rb");
        st = tf ? 200 : -1;
        static char tbuf[65536];
        for (size_t k; tf && (k = fread(tbuf, 1, sizeof tbuf, tf)) > 0;)
            if (!dl_sink(&d, tbuf, (DWORD)k)) { st = -2; break; }
        if (tf) fclose(tf);
    } else {
        LOG("[update] downloading %s\n", url);
        st = http_get(url, dl_sink, &d, NULL, NULL, dl_progress);
    }
    CloseHandle(d.f);
    unsigned char hash[32];
    bool hashed = d.h && BCryptFinishHash(d.h, hash, 32, 0) == 0;
    if (d.h) BCryptDestroyHash(d.h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 200) {
        set_state(UPD_FAILED, st < 0 ? "The download stopped. Check the internet connection and try again." : "The download failed.");
        remove_tree_w(upd);
        g_busy = 0;
        return NULL;
    }
    if (sha[0]) {  // the file is exactly the one GitHub lists for the release
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
        if (!hashed || !same_ascii(hex, sha)) {
            set_state(UPD_FAILED, "The download was damaged (its checksum doesn't match). Try again.");
            remove_tree_w(upd);
            g_busy = 0;
            return NULL;
        }
        LOG("[update] SHA-256 matches the release's\n");
    }
    // unpack: Windows' own tar (Windows 10 1803 and later) reads zips
    pthread_mutex_lock(&g_lock);
    g_st.progress = 0.92f;
    pthread_mutex_unlock(&g_lock);
    wchar_t sys[MAX_PATH], cmd[MAX_PATH * 4];
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(cmd, MAX_PATH * 4, L"\"%ls\\tar.exe\" -xf \"%ls\" -C \"%ls\"", sys, zip, stage);
    if (!run_hidden(cmd)) { set_state(UPD_FAILED, "Couldn't unpack the download."); remove_tree_w(upd); g_busy = 0; return NULL; }
    // its one top folder ("Super Odyssey PC Recomp") holds the program; it must have the exe
    wchar_t top[MAX_PATH * 2] = L"", pat[MAX_PATH * 2];
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", stage);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                _snwprintf(top, MAX_PATH * 2, L"%ls\\%ls", stage, fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    wchar_t exe[MAX_PATH * 2 + 32];
    _snwprintf(exe, MAX_PATH * 2 + 32, L"%ls\\SuperOdysseyRecomp.exe", top);
    if (!top[0] || GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        set_state(UPD_FAILED, "The download isn't a Windows release of this program.");
        remove_tree_w(upd);
        g_busy = 0;
        return NULL;
    }
    // over the program's files
    wchar_t oldlist[MAX_PATH + 32], prog[MAX_PATH];
    _snwprintf(oldlist, MAX_PATH + 32, L"%lsupdate-old-files.txt", dir);
    wcscpy(prog, dir);
    prog[wcslen(prog) - 1] = 0;  // (no trailing backslash)
    Apply a = {.old = _wfopen(oldlist, L"a")};
    int n = apply_dir(top, prog, &a);
    if (n < 0) {  // put back what was renamed
        for (int i = a.n - 1; i >= 0; i--) {
            wchar_t o[MAX_PATH * 2 + 8];
            _snwprintf(o, MAX_PATH * 2 + 8, L"%ls.old", a.moved[i]);
            DeleteFileW(a.moved[i]);
            MoveFileExW(o, a.moved[i], MOVEFILE_REPLACE_EXISTING);
        }
    }
    if (a.old) fclose(a.old);
    free(a.moved);
    remove_tree_w(upd);
    if (n < 0) { set_state(UPD_FAILED, "Couldn't replace the program's files (is another copy running?). Nothing was changed."); g_busy = 0; return NULL; }
    LOG("[update] installed %d files\n", n);
    pthread_mutex_lock(&g_lock);
    g_st.progress = 1;
    pthread_mutex_unlock(&g_lock);
    set_state(UPD_READY, NULL);
    g_busy = 0;
    if (getenv("ODYSSEY_UPDATE_TEST_AUTO")) updater_restart();
    return NULL;
}

void updater_install(void) {
    UpdStatus s;
    updater_status(&s);
    if (s.state != UPD_AVAILABLE && s.state != UPD_FAILED) return;
    if (!g_zip_url[0] || g_busy) return;
    g_busy = 1;
    set_state(UPD_DOWNLOADING, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, install_thread, NULL)) { g_busy = 0; set_state(UPD_FAILED, "Couldn't start the download."); return; }
    pthread_detach(t);
}

// The new version, told to wait for this process to end (updater_startup), then this one ends normally (settings saved)
void updater_restart(void) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 8];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    _snwprintf(cmd, MAX_PATH + 8, L"\"%ls\"", exe);
    wchar_t pid[32];
    _snwprintf(pid, 32, L"%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"ODYSSEY_WAIT_PID", pid);
    SetEnvironmentVariableW(L"ODYSSEY_UPDATE_TEST_ZIP", NULL);  // (a test's update happens once)
    SetEnvironmentVariableW(L"ODYSSEY_UPDATE_TEST_AUTO", NULL);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableW(L"ODYSSEY_WAIT_PID", NULL);
    if (!ok) { set_state(UPD_FAILED, "Couldn't start the new version. Start the game again yourself."); return; }
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    LOG("[update] restarting\n");
    settings_save();
    plat_terminate();
}

void updater_startup(void) {
    const char *w = getenv("ODYSSEY_WAIT_PID");
    if (w && *w) {  // a restart after an update: the old process ends first (it holds the old files)
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(w, NULL, 10));
        if (p) { WaitForSingleObject(p, 30000); CloseHandle(p); }
        SetEnvironmentVariableW(L"ODYSSEY_WAIT_PID", NULL);
    }
    wchar_t dir[MAX_PATH], list[MAX_PATH + 32];
    if (!program_dir(dir, MAX_PATH)) return;
    _snwprintf(list, MAX_PATH + 32, L"%lsupdate-old-files.txt", dir);
    FILE *f = _wfopen(list, L"r");
    if (!f) return;
    char line[MAX_PATH * 6];
    int left = 0;
    FILE *keep = NULL;
    wchar_t tmp[MAX_PATH + 40];
    _snwprintf(tmp, MAX_PATH + 40, L"%lsupdate-old-files.tmp", dir);
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        size_t l = strlen(line);
        if (l < 5 || strcmp(line + l - 4, ".old")) continue;  // (only what the updater renamed)
        wchar_t *p = wide(line);
        if (!p) continue;
        SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(p) && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {  // (still in use: next time)
            if (!keep) keep = _wfopen(tmp, L"w");
            if (keep) fprintf(keep, "%s\n", line);
            left++;
        }
        free(p);
    }
    fclose(f);
    if (keep) fclose(keep);
    DeleteFileW(list);
    if (left) MoveFileExW(tmp, list, MOVEFILE_REPLACE_EXISTING);
}

#else  // macOS, Linux: updates come with the platform's own download for now
void updater_startup(void) {}
void updater_auto_check(void) {}
void updater_check(void) {}
void updater_install(void) {}
void updater_restart(void) {}
#endif
