# Fixture: a find module that fails the way FindVulkan does when the SDK is absent.
find_path(FxSdk_INCLUDE_DIR fxsdk/fxsdk.h)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FxSdk REQUIRED_VARS FxSdk_INCLUDE_DIR)
