// Legacy include/build boundary; the pure culling helpers are owned by
// MatterGraphics. part_store.h stays here: graphics may not depend on it, but
// existing consumers still receive it through this header.
#pragma once
#include "part_store.h"
#include "../../../libs/MatterGraphics/src/raster_cull.h"
