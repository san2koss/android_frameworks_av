#pragma once

#include <android/native_window.h>
#include <utils/Errors.h>

namespace android::camera3::virtualcamera {

bool isEnabled();
status_t fillBuffer(ANativeWindowBuffer* buffer, int fenceFd);

} // namespace android::camera3::virtualcamera