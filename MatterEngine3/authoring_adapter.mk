# The portable authoring pipeline (contracts, geometry, build, QuickJS host)
# behind engine-core's legacy part route keeps its own CMake build under
# src/component_adapters/authoring. Its archives, in link order, relative to
# that build's binary dir -- shared by every Makefile that links the engine.
AUTHORING_ADAPTER_ARCHIVES = libmatter_authoring_adapter.a build/libmatter_build.a \
                             host/libmatter_host_quickjs.a geometry/libmatter_geometry.a \
                             contracts/libmatter_contracts.a
