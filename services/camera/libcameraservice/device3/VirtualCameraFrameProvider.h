#pragma once

#include <utils/Errors.h>

struct ANativeWindowBuffer;

namespace android::camera3::virtualcamera {

bool isEnabled();
status_t fillBuffer(ANativeWindowBuffer* buffer, int fenceFd);

} // namespace android::camera3::virtualcamera
