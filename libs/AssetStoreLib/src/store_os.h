/* store_os.h -- the only place AssetStoreLib touches the operating system.
 *
 * Deliberately tiny: positional read/write, append, size, truncate, sync, an
 * atomic rename-over, directory listing, and a cross-process advisory lock.
 * Everything above this file is portable C++.
 *
 * A File is a plain handle with an internal cursor used only by append().
 * read_at()/write_at() are positional and do not disturb it. A File is not
 * thread-safe; open one per thread (which is exactly the store's model).
 */
#ifndef ASSET_STORE_OS_H
#define ASSET_STORE_OS_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

namespace asset_store {
namespace os {

struct File;

/* mode: open an existing file for reading only. Returns null if absent. */
File* open_read(const std::string& path);
/* Opens for read+write, creating if absent. */
File* open_rw(const std::string& path, bool create);
void close(File* f);

bool read_at(File* f, uint64_t offset, void* buf, size_t len);
bool write_at(File* f, uint64_t offset, const void* buf, size_t len);
/* Appends at the file's current end and advances the internal cursor. */
bool append(File* f, const void* buf, size_t len);
uint64_t file_size(File* f);
bool truncate(File* f, uint64_t size);
bool sync(File* f);

bool file_exists(const std::string& path);
uint64_t file_size_of(const std::string& path);
bool remove_file(const std::string& path);
bool make_dir(const std::string& path);          /* single level, ok if present */
bool make_dirs(const std::string& path);         /* recursive */
/* Atomic within a filesystem: `to` is replaced whole or not at all. */
bool rename_over(const std::string& from, const std::string& to);
/* Names (not paths) of entries in dir, files only. */
std::vector<std::string> list_dir(const std::string& dir);
/* Something that changes whenever the file is rewritten: (mtime, size). */
bool stamp_of(const std::string& path, uint64_t* out_stamp);

/* Cross-process advisory lock held on an open file. */
struct Lock;
/* Returns null if the lock is held elsewhere and `block` is false. */
Lock* lock_exclusive(const std::string& path, bool block);
void unlock(Lock* l);

std::string last_error();

}  // namespace os
}  // namespace asset_store

#endif /* ASSET_STORE_OS_H */
