#include <matter/contracts/abi.h>
#include <cstddef>
#include <iostream>
#include <type_traits>
extern "C" {
matter_geometry_api fake_abi_create(void);
void fake_abi_dispose(matter_geometry_api *);
unsigned fake_abi_live(void);
}
static_assert(std::is_standard_layout_v<matter_geometry_api>);
static_assert(sizeof(matter_status) == 4 && sizeof(matter_result_handle) == 8);
static_assert(offsetof(matter_geometry_api, header) == 0);
static_assert(MATTER_OK == 0 && MATTER_CANCELLED == 7 && MATTER_INVALID_HANDLE == 10);
int main() {
  auto api = fake_abi_create();
  if (!api.instance || matter_check_abi(api.header, sizeof(api)) != MATTER_OK) return 1;
  uint8_t bytes[7]{}; uint64_t required = 0;
  if (api.capabilities(api.instance, {nullptr,0}, &required) != MATTER_RESOURCE_LIMIT || required != 7) return 1;
  if (api.capabilities(api.instance, {bytes,sizeof(bytes)}, &required) != MATTER_OK) return 1;
  matter_job_envelope job{{sizeof(job),1,0}, {bytes,1}, {bytes,1}, 1};
  matter_budget budget{{sizeof(budget),1,0}, 100,100,100,100,100,1000000,100,10,1};
  auto result = api.evaluate(api.instance, {bytes,1}, &job, &budget, nullptr);
  matter_bytes_view view{};
  if (result.status != MATTER_OK || !result.result ||
      api.read(api.instance, result.result, &view) != MATTER_OK || view.byte_length != 3) return 1;
  if (api.release(api.instance, result.result) != MATTER_OK || fake_abi_live() != 0) return 1;
  job.header.minor = 1;
  result = api.evaluate(api.instance, {bytes,1}, &job, &budget, nullptr);
  if (result.status != MATTER_UNSUPPORTED_VERSION || result.result) return 1;
  fake_abi_dispose(&api);
  std::cout << "PASS GATE-C-ABI C11 callee/C++17 caller; fake provider only\n";
}
