/* store_os.cpp -- Win32 and POSIX backings for the shim in store_os.h. */

#include "store_os.h"

#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <stdio.h>
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace asset_store {
namespace os {

/* ============================================================== Windows == */
#ifdef _WIN32

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t cursor = 0;   /* append position */
};

struct Lock {
    HANDLE h = INVALID_HANDLE_VALUE;
};

static std::string g_err;

std::string last_error() { return g_err; }

static void set_err(const char* what) {
    DWORD e = GetLastError();
    char buf[256];
    snprintf(buf, sizeof(buf), "%s (win32 error %lu)", what, (unsigned long)e);
    g_err = buf;
}

File* open_read(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { set_err("open_read"); return nullptr; }
    File* f = new File();
    f->h = h;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz)) f->cursor = (uint64_t)sz.QuadPart;
    return f;
}

File* open_rw(const std::string& path, bool create) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { set_err("open_rw"); return nullptr; }
    File* f = new File();
    f->h = h;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz)) f->cursor = (uint64_t)sz.QuadPart;
    return f;
}

void close(File* f) {
    if (!f) return;
    if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h);
    delete f;
}

bool read_at(File* f, uint64_t offset, void* buf, size_t len) {
    if (!f) return false;
    uint8_t* p = (uint8_t*)buf;
    while (len > 0) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD want = (DWORD)(len > 0x10000000u ? 0x10000000u : len);
        DWORD got = 0;
        if (!ReadFile(f->h, p, want, &got, &ov)) { set_err("read_at"); return false; }
        if (got == 0) { g_err = "read_at: short read"; return false; }
        p += got; offset += got; len -= got;
    }
    return true;
}

bool write_at(File* f, uint64_t offset, const void* buf, size_t len) {
    if (!f) return false;
    const uint8_t* p = (const uint8_t*)buf;
    while (len > 0) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.Offset = (DWORD)(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD want = (DWORD)(len > 0x10000000u ? 0x10000000u : len);
        DWORD put = 0;
        if (!WriteFile(f->h, p, want, &put, &ov)) { set_err("write_at"); return false; }
        if (put == 0) { g_err = "write_at: short write"; return false; }
        p += put; offset += put; len -= put;
    }
    return true;
}

bool append(File* f, const void* buf, size_t len) {
    if (!f) return false;
    if (!write_at(f, f->cursor, buf, len)) return false;
    f->cursor += len;
    return true;
}

uint64_t file_size(File* f) {
    if (!f) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->h, &sz)) return 0;
    return (uint64_t)sz.QuadPart;
}

bool truncate(File* f, uint64_t size) {
    if (!f) return false;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(f->h, li, nullptr, FILE_BEGIN)) { set_err("truncate seek"); return false; }
    if (!SetEndOfFile(f->h)) { set_err("truncate"); return false; }
    f->cursor = size;
    return true;
}

bool sync(File* f) {
    if (!f) return false;
    if (!FlushFileBuffers(f->h)) { set_err("sync"); return false; }
    return true;
}

bool file_exists(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

uint64_t file_size_of(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

bool remove_file(const std::string& path) {
    if (DeleteFileA(path.c_str())) return true;
    set_err("remove_file");
    return false;
}

bool make_dir(const std::string& path) {
    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return true;
    set_err("make_dir");
    return false;
}

bool rename_over(const std::string& from, const std::string& to) {
    /* The rename is the store's commit point, so it has to actually happen.
     *
     * Windows will refuse to replace a destination that another process has
     * open, even one opened with FILE_SHARE_DELETE -- and a reader picking up
     * the index is doing exactly that, for the microsecond it takes to slurp
     * the file. Under the concurrent soak (six reader threads against a writer
     * committing sixty times) that window is hit regularly. Virus scanners and
     * the search indexer open files behind our back for the same reason.
     *
     * None of that is a corruption hazard -- the rename either happens or it
     * does not, and a reader always sees one whole index or the other. It is
     * purely a liveness problem, so the answer is to wait for the other handle
     * to close. A second of patience, then give up and report failure. */
    const int kAttempts = 500;
    for (int i = 0; i < kAttempts; ++i) {
        if (MoveFileExA(from.c_str(), to.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DWORD e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION &&
            e != ERROR_LOCK_VIOLATION) break;
        Sleep(2);
    }
    set_err("rename_over");
    return false;
}

std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

bool stamp_of(const std::string& path, uint64_t* out_stamp) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return false;
    uint64_t t = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) |
                 d.ftLastWriteTime.dwLowDateTime;
    uint64_t sz = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    *out_stamp = t ^ (sz * 0x9E3779B97F4A7C15ull);
    return true;
}

Lock* lock_exclusive(const std::string& path, bool block) {
    for (;;) {
        /* No sharing at all: a second opener fails outright. That is the lock. */
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            Lock* l = new Lock();
            l->h = h;
            return l;
        }
        if (!block) { set_err("lock_exclusive"); return nullptr; }
        Sleep(10);
    }
}

void unlock(Lock* l) {
    if (!l) return;
    if (l->h != INVALID_HANDLE_VALUE) CloseHandle(l->h);
    delete l;
}

/* ================================================================ POSIX == */
#else

struct File {
    int fd = -1;
    uint64_t cursor = 0;
};

struct Lock {
    int fd = -1;
    std::string path;
};

static std::string g_err;

std::string last_error() { return g_err; }

static void set_err(const char* what) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s (%s)", what, strerror(errno));
    g_err = buf;
}

File* open_read(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { set_err("open_read"); return nullptr; }
    File* f = new File();
    f->fd = fd;
    struct stat st;
    if (fstat(fd, &st) == 0) f->cursor = (uint64_t)st.st_size;
    return f;
}

File* open_rw(const std::string& path, bool create) {
    int fd = ::open(path.c_str(), O_RDWR | (create ? O_CREAT : 0), 0644);
    if (fd < 0) { set_err("open_rw"); return nullptr; }
    File* f = new File();
    f->fd = fd;
    struct stat st;
    if (fstat(fd, &st) == 0) f->cursor = (uint64_t)st.st_size;
    return f;
}

void close(File* f) {
    if (!f) return;
    if (f->fd >= 0) ::close(f->fd);
    delete f;
}

bool read_at(File* f, uint64_t offset, void* buf, size_t len) {
    if (!f) return false;
    uint8_t* p = (uint8_t*)buf;
    while (len > 0) {
        ssize_t got = pread(f->fd, p, len, (off_t)offset);
        if (got < 0) { if (errno == EINTR) continue; set_err("read_at"); return false; }
        if (got == 0) { g_err = "read_at: short read"; return false; }
        p += got; offset += (uint64_t)got; len -= (size_t)got;
    }
    return true;
}

bool write_at(File* f, uint64_t offset, const void* buf, size_t len) {
    if (!f) return false;
    const uint8_t* p = (const uint8_t*)buf;
    while (len > 0) {
        ssize_t put = pwrite(f->fd, p, len, (off_t)offset);
        if (put < 0) { if (errno == EINTR) continue; set_err("write_at"); return false; }
        if (put == 0) { g_err = "write_at: short write"; return false; }
        p += put; offset += (uint64_t)put; len -= (size_t)put;
    }
    return true;
}

bool append(File* f, const void* buf, size_t len) {
    if (!f) return false;
    if (!write_at(f, f->cursor, buf, len)) return false;
    f->cursor += len;
    return true;
}

uint64_t file_size(File* f) {
    if (!f) return 0;
    struct stat st;
    if (fstat(f->fd, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

bool truncate(File* f, uint64_t size) {
    if (!f) return false;
    if (ftruncate(f->fd, (off_t)size) != 0) { set_err("truncate"); return false; }
    f->cursor = size;
    return true;
}

bool sync(File* f) {
    if (!f) return false;
    if (fsync(f->fd) != 0) { set_err("sync"); return false; }
    return true;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

uint64_t file_size_of(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

bool remove_file(const std::string& path) {
    if (unlink(path.c_str()) == 0) return true;
    set_err("remove_file");
    return false;
}

bool make_dir(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == 0) return true;
    if (errno == EEXIST) return true;
    set_err("make_dir");
    return false;
}

bool rename_over(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str()) == 0) return true;
    set_err("rename_over");
    return false;
}

std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        struct stat st;
        if (stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
        out.push_back(name);
    }
    closedir(d);
    return out;
}

bool stamp_of(const std::string& path, uint64_t* out_stamp) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    uint64_t t = (uint64_t)st.st_mtime;
    *out_stamp = t ^ ((uint64_t)st.st_size * 0x9E3779B97F4A7C15ull);
    return true;
}

Lock* lock_exclusive(const std::string& path, bool block) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) { set_err("lock_exclusive open"); return nullptr; }
    int op = LOCK_EX | (block ? 0 : LOCK_NB);
    if (flock(fd, op) != 0) {
        set_err("lock_exclusive");
        ::close(fd);
        return nullptr;
    }
    Lock* l = new Lock();
    l->fd = fd;
    l->path = path;
    return l;
}

void unlock(Lock* l) {
    if (!l) return;
    if (l->fd >= 0) { flock(l->fd, LOCK_UN); ::close(l->fd); }
    delete l;
}

#endif

/* ------------------------------------------------------------- portable -- */

bool make_dirs(const std::string& path) {
    if (path.empty()) return true;
    std::string acc;
    size_t i = 0;
    /* Preserve a leading "/" or a "C:" drive prefix. */
    if (path.size() >= 2 && path[1] == ':') { acc = path.substr(0, 2); i = 2; }
    for (; i < path.size(); ++i) {
        char c = path[i];
        if (c == '/' || c == '\\') {
            if (!acc.empty() && acc != "." && !(acc.size() == 2 && acc[1] == ':')) {
                if (!file_exists(acc)) make_dir(acc);
            }
            acc.push_back('/');
        } else {
            acc.push_back(c);
        }
    }
    if (!acc.empty() && !file_exists(acc)) return make_dir(acc);
    return true;
}

}  // namespace os
}  // namespace asset_store
