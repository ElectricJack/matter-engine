#include "images.h"
// Keep codec symbols/state private when a host also links raylib's stb copy.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <memory>
#include <stb_image_write.h>

namespace matter::asset::detail {
Bytes png(const Bytes &pixels, uint32_t w, uint32_t h) {
  need(w <= INT32_MAX / 4 && h <= INT32_MAX &&
           uint64_t(w) * h * 4 == pixels.size(),
       "PNG size overflow", E::resource_limit);
  Bytes out;
  struct Sink {
    Bytes &out;
    std::exception_ptr error;
  } sink{out, {}};
  auto append = [](void *context, void *p, int n) {
    auto &s = *static_cast<Sink *>(context);
    if (s.error)
      return;
    try {
      auto bytes = static_cast<uint8_t *>(p);
      s.out.insert(s.out.end(), bytes, bytes + n);
    } catch (...) {
      s.error = std::current_exception();
    }
  };
  need(stbi_write_png_to_func(append, &sink, int(w), int(h), 4, pixels.data(),
                              int(w) * 4) != 0,
       "PNG encoding failed", E::backend_failure);
  if (sink.error)
    std::rethrow_exception(sink.error);
  return out;
}
Bytes image_rgba(const Bytes &encoded, uint32_t &width, uint32_t &height,
                 uint64_t max_bytes) {
  need(encoded.size() <= INT32_MAX, "encoded image exceeds decoder limit",
       E::resource_limit);
  int w = 0, h = 0, channels = 0;
  need(stbi_info_from_memory(encoded.data(), int(encoded.size()), &w, &h,
                             &channels) != 0 &&
           w > 0 && h > 0,
       "invalid PNG/JPEG image");
  need(uint64_t(w) * uint64_t(h) * 4 <= max_bytes,
       "decoded image exceeds budget", E::resource_limit);
  using Image = std::unique_ptr<unsigned char, decltype(&stbi_image_free)>;
  Image pixels(stbi_load_from_memory(encoded.data(), int(encoded.size()), &w,
                                     &h, &channels, 4),
               stbi_image_free);
  need(bool(pixels), "PNG/JPEG decoding failed");
  width = uint32_t(w);
  height = uint32_t(h);
  return Bytes(pixels.get(), pixels.get() + size_t(w) * size_t(h) * 4);
}
} // namespace matter::asset::detail
