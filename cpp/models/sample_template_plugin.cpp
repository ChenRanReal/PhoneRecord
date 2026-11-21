#include "sample_template.hpp"

/// Plugin entry for the template. The executable already contains sample.template, so a copy in plugins/ is skipped.
extern "C" PR_DEVICE_EXPORT const PrDeviceModelV1* pr_device_model_v1(void) { return sampleTemplateModel(); }
