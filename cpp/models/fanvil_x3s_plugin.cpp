#include "fanvil_x3s.hpp"

/// Plugin entry for a separately built Fanvil X3S library. The executable already contains this id.
extern "C" PR_DEVICE_EXPORT const PrDeviceModelV1* pr_device_model_v1(void) { return fanvilX3sModel(); }
