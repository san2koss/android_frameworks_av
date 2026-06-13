#define LOG_TAG "VirtualCameraFrameProvider"

#include "VirtualCameraFrameProvider.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include <cutils/properties.h>
#include <system/graphics.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>
#include <inttypes.h>

namespace android::camera3::virtualcamera {

namespace {

constexpr const char* PROP_ENABLED = "persist.lineagepro.virtual_camera.enabled";
constexpr const char* PROP_MODE = "persist.lineagepro.virtual_camera.mode";
constexpr const char* PROP_PATH = "persist.lineagepro.virtual_camera.path";
constexpr const char* PROP_LOOP = "persist.lineagepro.virtual_camera.loop";
constexpr const char* PROP_RAW_WIDTH = "persist.lineagepro.virtual_camera.width";
constexpr const char* PROP_RAW_HEIGHT = "persist.lineagepro.virtual_camera.height";
constexpr const char* PROP_RAW_FORMAT = "persist.lineagepro.virtual_camera.format";

constexpr const char* MODE_RAW = "raw";
constexpr const char* FORMAT_RGBA = "rgba";
constexpr const char* FORMAT_GRAY = "gray";

struct RawState {
    std::string path;
    std::string format;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t frameSize = 0;
    size_t frameIndex = 0;
    std::vector<uint8_t> data;
};

std::mutex gRawLock;
RawState gRawState;

std::string getPropertyString(const char* key, const char* defaultValue) {
    char value[PROPERTY_VALUE_MAX] = {};
    property_get(key, value, defaultValue);
    return value;
}

size_t bytesPerPixelForRawFormat(const std::string& format) {
    if (format == FORMAT_RGBA) {
        return 4;
    }
    if (format == FORMAT_GRAY) {
        return 1;
    }
    return 0;
}

size_t bytesPerPixelForPixelFormat(int format) {
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            return 4;
        case HAL_PIXEL_FORMAT_RGB_888:
            return 3;
        case HAL_PIXEL_FORMAT_RGB_565:
            return 2;
        default:
            return 1;
    }
}

void drawPattern(uint8_t* dst, uint32_t height, size_t rowBytes) {
    static uint32_t frame = 0;
    frame++;

    for (uint32_t y = 0; y < height; y++) {
        uint8_t* row = dst + static_cast<size_t>(y) * rowBytes;
        for (uint32_t x = 0; x < rowBytes; x++) {
            row[x] = static_cast<uint8_t>((x + y + frame) & 0xff);
        }
    }
}

bool loadRawStateLocked(const std::string& path, uint32_t width, uint32_t height,
        const std::string& format) {
    const size_t bytesPerPixel = bytesPerPixelForRawFormat(format);
    if (path.empty() || width == 0 || height == 0 || bytesPerPixel == 0) {
        return false;
    }

    const size_t frameSize = static_cast<size_t>(width) * height * bytesPerPixel;
    if (gRawState.path == path && gRawState.width == width && gRawState.height == height
            && gRawState.format == format && gRawState.frameSize == frameSize
            && !gRawState.data.empty()) {
        return true;
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        ALOGW("Unable to open virtual camera raw file %s: %s", path.c_str(), strerror(errno));
        return false;
    }

    const std::streamsize fileSize = input.tellg();
    if (fileSize <= 0 || static_cast<size_t>(fileSize) < frameSize) {
        ALOGW("Virtual camera raw file %s is too small for one frame", path.c_str());
        return false;
    }

    input.seekg(0, std::ios::beg);

    RawState next;
    next.path = path;
    next.format = format;
    next.width = width;
    next.height = height;
    next.frameSize = frameSize;
    next.data.resize(static_cast<size_t>(fileSize));

    if (!input.read(reinterpret_cast<char*>(next.data.data()), fileSize)) {
        ALOGW("Failed reading virtual camera raw file %s", path.c_str());
        return false;
    }

    gRawState = std::move(next);
    ALOGI("Loaded virtual camera raw file %s (%zu bytes, frame %ux%u)",
            path.c_str(), gRawState.data.size(), width, height);
    return true;
}

bool copyRawFrame(uint8_t* dst, uint32_t dstWidth, uint32_t dstHeight, size_t dstRowBytes,
        size_t dstBytesPerPixel) {
    const std::string path = getPropertyString(PROP_PATH, "");
    const std::string format = getPropertyString(PROP_RAW_FORMAT, FORMAT_GRAY);
    const uint32_t rawWidth = static_cast<uint32_t>(property_get_int32(PROP_RAW_WIDTH, 0));
    const uint32_t rawHeight = static_cast<uint32_t>(property_get_int32(PROP_RAW_HEIGHT, 0));
    const bool shouldLoop = property_get_bool(PROP_LOOP, true);
    const size_t bytesPerPixel = bytesPerPixelForRawFormat(format);

    std::lock_guard<std::mutex> lock(gRawLock);
    if (!loadRawStateLocked(path, rawWidth, rawHeight, format)) {
        return false;
    }

    const size_t frameCount = gRawState.data.size() / gRawState.frameSize;
    if (frameCount == 0) {
        return false;
    }
    if (gRawState.frameIndex >= frameCount) {
        gRawState.frameIndex = shouldLoop ? 0 : frameCount - 1;
    }

    const uint8_t* src = gRawState.data.data() + gRawState.frameIndex * gRawState.frameSize;
    const uint32_t rows = std::min(dstHeight, rawHeight);

    if (format == FORMAT_RGBA) {
        constexpr uint32_t kRgbaBytesPerPixel = 4;
        if (dstBytesPerPixel != kRgbaBytesPerPixel) {
            return false;
        }
        const uint32_t columns = std::min(dstWidth, rawWidth);
        for (uint32_t y = 0; y < rows; y++) {
            memcpy(dst + static_cast<size_t>(y) * dstRowBytes,
                    src + static_cast<size_t>(y) * rawWidth * bytesPerPixel,
                    static_cast<size_t>(columns) * kRgbaBytesPerPixel);
        }
    } else {
        const size_t columns = std::min(dstRowBytes, static_cast<size_t>(rawWidth));
        for (uint32_t y = 0; y < rows; y++) {
            memcpy(dst + static_cast<size_t>(y) * dstRowBytes,
                    src + static_cast<size_t>(y) * rawWidth * bytesPerPixel,
                    columns);
        }
    }

    gRawState.frameIndex++;
    return true;
}

} // namespace

bool isEnabled() {
    return property_get_bool(PROP_ENABLED, false);
}

status_t fillBuffer(ANativeWindowBuffer* anwBuffer, int fenceFd) {
    if (anwBuffer == nullptr) return BAD_VALUE;

    sp<GraphicBuffer> gb = GraphicBuffer::from(anwBuffer);
    if (gb == nullptr) return BAD_VALUE;

    void* mapped = nullptr;

    ALOGW("VCAM buffer w=%u h=%u stride=%u format=0x%x usage=0x%" PRIx64,
          gb->getWidth(), gb->getHeight(), gb->getStride(),
          gb->getPixelFormat(), gb->getUsage());

    status_t res = gb->lockAsync(
            GraphicBuffer::USAGE_SW_WRITE_OFTEN,
            &mapped,
            fenceFd >= 0 ? dup(fenceFd) : -1);
    if (res != OK) {
        ALOGW("Failed to lock virtual camera buffer: %s (%d)", strerror(-res), res);
        return res;
    }

    uint8_t* dst = static_cast<uint8_t*>(mapped);
    const uint32_t width = gb->getWidth();
    const uint32_t height = gb->getHeight();
    const uint32_t stride = gb->getStride();
    const size_t bytesPerPixel = bytesPerPixelForPixelFormat(gb->getPixelFormat());
    const size_t rowBytes = static_cast<size_t>(stride) * bytesPerPixel;

    const std::string mode = getPropertyString(PROP_MODE, "pattern");
    if (mode != MODE_RAW || !copyRawFrame(dst, width, height, rowBytes, bytesPerPixel)) {
        drawPattern(dst, height, rowBytes);
    }

    gb->unlock();
    return OK;
}

} // namespace android::camera3::virtualcamera
