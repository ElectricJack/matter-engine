#include "vulkan_device_fixture.h"
#include <matter/graphics/vulkan_raster.h>
#include <fstream>
#include <iterator>
#include <cmath>
#include "raster_native.h"
int main() {
  std::cout << std::unitbuf;
  auto native = std::make_shared<NativeDevice>(true);
  exercise_raster(native);
  native.reset();
  check(validation_errors == 0, "GFX-RASTER no validation errors including teardown");
}
