// MatterEngine3/src/export/export_image.cpp — see export_image.h.

#include "export/export_image.h"

#include <cstdio>

// A PRIVATE static copy of the stb encoder. STB_IMAGE_WRITE_STATIC gives every
// symbol internal linkage, so this TU can own an implementation whether or not
// raylib (which bundles its own) is in the link. See the header's note.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)   // unreferenced local function removed
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace matter_export {
namespace {

void png_sink(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

} // namespace

bool parse_texture_format(const std::string& text, TextureFormat& out) {
    if (text == "none") { out = TextureFormat::None; return true; }
    if (text == "png")  { out = TextureFormat::Png;  return true; }
    if (text == "ktx2") { out = TextureFormat::Ktx2; return true; }
    return false;
}

const char* texture_format_extension(TextureFormat format) {
    switch (format) {
        case TextureFormat::Png:  return "png";
        case TextureFormat::Ktx2: return "ktx2";
        default:                  return "";
    }
}

bool encode_png(const ExportImage& image, std::vector<uint8_t>& out,
                std::string& error) {
    out.clear();
    error.clear();
    if (image.empty()) {
        error = "cannot encode an empty image";
        return false;
    }
    if (image.channels != 1u && image.channels != 3u && image.channels != 4u) {
        error = "unsupported channel count for PNG encoding";
        return false;
    }
    const size_t expected =
        static_cast<size_t>(image.width) * image.height * image.channels;
    if (image.texels.size() != expected) {
        error = "image payload size does not match its dimensions";
        return false;
    }

    // Pin both encoder globals: their defaults are process state, and an export
    // must not depend on process state.
    stbi_write_png_compression_level = 8;
    stbi_write_force_png_filter = -1;

    const int ok = stbi_write_png_to_func(
        &png_sink, &out, static_cast<int>(image.width), static_cast<int>(image.height),
        static_cast<int>(image.channels), image.texels.data(),
        static_cast<int>(image.width * image.channels));
    if (!ok || out.empty()) {
        out.clear();
        error = "PNG encoding failed";
        return false;
    }
    return true;
}

bool write_file_bytes(const std::string& path, const void* data, size_t size,
                      std::string& error) {
    error.clear();
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        error = "could not open '" + path + "' for writing";
        return false;
    }
    const size_t written = (size == 0u) ? 0u : std::fwrite(data, 1, size, file);
    const bool complete = (written == size);
    const bool closed = (std::fclose(file) == 0);
    if (!complete || !closed) {
        error = "could not write '" + path + "'";
        return false;
    }
    return true;
}

bool write_image_file(const ExportImage& image, TextureFormat format,
                      const std::string& path, std::string& error) {
    error.clear();
    switch (format) {
        case TextureFormat::None:
            return true;   // nothing to write; not an error
        case TextureFormat::Png: {
            std::vector<uint8_t> encoded;
            if (!encode_png(image, encoded, error)) return false;
            return write_file_bytes(path, encoded.data(), encoded.size(), error);
        }
        case TextureFormat::Ktx2:
            error =
                "--texture-format ktx2 is reserved but not implemented yet; "
                "use png (docs/export-obj.md, 'Texture formats')";
            return false;
    }
    error = "unknown texture format";
    return false;
}

} // namespace matter_export
