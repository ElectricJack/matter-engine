#pragma once

// MatterEngine3/src/export/export_image.h
//
// Encoding an ExportImage to a file, and the one place that knows which
// container formats the exporter can write.
//
// TEXTURE FORMATS. `Png` is the shipped encoder. `Ktx2` is RESERVED — the
// `--texture-format ktx2` flag parses, reaches here, and fails with a specific
// "not implemented" message rather than silently writing a PNG under a .ktx2
// name. That is the whole point of reserving it: a caller (or a scene script)
// can name the format today and get a clear answer, and adding the encoder
// later changes nothing above this seam. `None` skips texture writing
// altogether, for a geometry-only export.
//
// DETERMINISM. stb_image_write's PNG encoder is deterministic for fixed
// settings, but two of its settings are GLOBALS with process-wide defaults
// (compression level, filter choice). encode_png sets both explicitly on every
// call so an export never depends on what some other part of the process left
// them at. The stable identity of a texture is nevertheless
// ExportImage::content_hash, taken over the decoded payload: it survives an
// encoder upgrade and a format switch, and a PNG byte hash would not.
//
// This TU owns a PRIVATE copy of the stb implementation (STB_IMAGE_WRITE_STATIC
// plus STB_IMAGE_WRITE_IMPLEMENTATION), so it links in every configuration —
// alongside raylib, which bundles its own non-static copy, and in the headless
// archive, which does not. tileset_gtex.cpp solves the same problem with a
// build-time macro; the static copy needs no coordination from the build files
// at all, at the cost of a few kilobytes of duplicated code.

#include "export/mesh_export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace matter_export {

enum class TextureFormat : uint32_t {
    None = 0,
    Png,
    Ktx2,
};

// "none" / "png" / "ktx2", case-sensitive. False for anything else.
bool parse_texture_format(const std::string& text, TextureFormat& out);
// The file extension a format writes, without the dot. Empty for None.
const char* texture_format_extension(TextureFormat format);

// Encode `image` into `out` (cleared first). Grey (1 channel), RGB (3) and
// RGBA (4) are supported; anything else fails.
bool encode_png(const ExportImage& image, std::vector<uint8_t>& out,
                std::string& error);

// Encode and write atomically-enough for a build artifact: a plain truncating
// binary write. Creates no directories — the caller owns the output tree.
bool write_image_file(const ExportImage& image, TextureFormat format,
                      const std::string& path, std::string& error);

// Write `bytes` to `path` in binary mode, truncating. Shared with the text
// writers so every file the exporter produces goes through one code path and
// gets LF line endings on every platform.
bool write_file_bytes(const std::string& path, const void* data, size_t size,
                      std::string& error);
inline bool write_file_text(const std::string& path, const std::string& text,
                            std::string& error) {
    return write_file_bytes(path, text.data(), text.size(), error);
}

} // namespace matter_export
