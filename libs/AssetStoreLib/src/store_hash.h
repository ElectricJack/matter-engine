/* store_hash.h -- content hashing and integrity checksums, no dependencies.
 *
 * Path: libs/AssetStoreLib/src/store_hash.h. Internal to the library.
 *
 * Only crc32() is declared here. The content-hash entry points implemented in
 * the same .cpp -- hash_bytes(), hash_to_hex(), hash_to_string() -- are part
 * of the public API and are declared in ../include/asset_store.h instead, so
 * every translation unit that needs them already includes both headers. */
#ifndef ASSET_STORE_HASH_H
#define ASSET_STORE_HASH_H

#include <stdint.h>
#include <stddef.h>

namespace asset_store {

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) -- the integrity check on
 * every blob payload, every record header and the index file itself. */
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);

}  // namespace asset_store

#endif /* ASSET_STORE_HASH_H */
