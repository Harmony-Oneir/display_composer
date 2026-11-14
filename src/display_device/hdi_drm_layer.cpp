/*
 * Copyright (c) 2021-2023 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hdi_drm_layer.h"
#include <cinttypes>
#include <cerrno>
#include "display_log.h"
#include "drm_device.h"
#include <sys/stat.h>

namespace OHOS {
namespace HDI {
namespace DISPLAY {
DrmGemBuffer::DrmGemBuffer(int drmFd, HdiLayerBuffer &hdl, bool opaque) : mDrmFd(drmFd)
{
    DISPLAY_LOGD();
    Init(mDrmFd, hdl, opaque);
}

// oneir: hwc — вариант формата без альфы (для непрозрачного вывода)
static uint32_t OpaqueDrmFormat(uint32_t fmt)
{
    switch (fmt) {
        case DRM_FORMAT_ARGB8888:
            return DRM_FORMAT_XRGB8888;
        case DRM_FORMAT_ABGR8888:
            return DRM_FORMAT_XBGR8888;
        case DRM_FORMAT_RGBA8888:
            return DRM_FORMAT_RGBX8888;
        case DRM_FORMAT_BGRA8888:
            return DRM_FORMAT_BGRX8888;
        default:
            return fmt;
    }
}

void DrmGemBuffer::Init(int drmFd, HdiLayerBuffer &hdl, bool opaque)
{
    int ret;
    const int MAX_COUNT = 4;
    uint32_t pitches[MAX_COUNT] = {0};
    uint32_t gemHandles[MAX_COUNT] = {0};
    uint32_t offsets[MAX_COUNT] = {0};
    DISPLAY_LOGD("hdl %{public}" PRIx64 "", hdl.GetPhysicalAddr());
    DISPLAY_CHK_RETURN_NOT_VALUE((drmFd < 0), DISPLAY_LOGE("can not init drmfd %{public}d", drmFd));
    mDrmFormat = DrmDevice::ConvertToDrmFormat(static_cast<PixelFormat>(hdl.GetFormat()));
    // oneir: hwc — настоящий формат буфера вместо жёсткого XRGB8888; неизвестный — как раньше
    if (mDrmFormat == 0) {
        mDrmFormat = DRM_FORMAT_XRGB8888;
    }
    if (opaque) {
        mDrmFormat = OpaqueDrmFormat(mDrmFormat);
    }
    ret = drmPrimeFDToHandle(drmFd, hdl.GetFb(), &mGemHandle);
    DISPLAY_CHK_RETURN_NOT_VALUE((ret != 0), DISPLAY_LOGE("can not get handle errno %{public}d", errno));

    pitches[0] = hdl.GetStride();
    gemHandles[0] = mGemHandle;
    offsets[0] = 0;
    ret = drmModeAddFB2(drmFd, hdl.GetWight(), hdl.GetHeight(), mDrmFormat, gemHandles, pitches, offsets, &mFdId, 0);
    DISPLAY_LOGD("mGemHandle %{public}d  mFdId %{public}d", mGemHandle, mFdId);
    DISPLAY_LOGD("w: %{public}d  h: %{public}d mDrmFormat : %{public}d gemHandles: %{public}d pitches: %{public}d "
        "offsets: %{public}d",
        hdl.GetWight(), hdl.GetHeight(), mDrmFormat, gemHandles[0], pitches[0], offsets[0]);
    DISPLAY_CHK_RETURN_NOT_VALUE((ret != 0), DISPLAY_LOGE("can not add fb errno %{public}d", errno));
}

DrmGemBuffer::~DrmGemBuffer()
{
    DISPLAY_LOGD();
    if (mFdId) {
        if (drmModeRmFB(mDrmFd, mFdId)) {
            DISPLAY_LOGE("can not free fdid %{public}d errno %{public}d", mFdId, errno);
        }
    }

    if (mGemHandle) {
        struct drm_gem_close gemClose = { 0 };
        gemClose.handle = mGemHandle;
        if (drmIoctl(mDrmFd, DRM_IOCTL_GEM_CLOSE, &gemClose)) {
            DISPLAY_LOGD("can not free gem handle %{public}d errno : %{public}d", mGemHandle, errno);
        }
    }
}

bool DrmGemBuffer::IsValid()
{
    DISPLAY_LOGD();
    return (mGemHandle != INVALID_DRM_ID) && (mFdId != INVALID_DRM_ID);
}

DrmGemBuffer *HdiDrmLayer::GetGemBuffer(bool opaque)
{
    DISPLAY_LOGD();
    HdiLayerBuffer *buf = GetCurrentBuffer();
    if (buf == nullptr) {
        return nullptr;
    }
    // oneir: hwc — один FB на буфер: ключ — устройство и inode dma-buf (номер fd в слое меняется)
    uint64_t key = 0;
    struct stat st = {};
    if (fstat(buf->GetFb(), &st) == 0) {
        key = (static_cast<uint64_t>(st.st_dev) << 32) ^ static_cast<uint64_t>(st.st_ino);
    }
    if (mCurrentBuffer != nullptr && key != 0 && key == mCurrentKey && opaque == mCurrentOpaque) {
        return mCurrentBuffer.get();
    }
    std::unique_ptr<DrmGemBuffer> ptr = std::make_unique<DrmGemBuffer>(DrmDevice::GetDrmFd(), *buf, opaque);
    mLastBuffer = std::move(mCurrentBuffer);
    mCurrentBuffer = std::move(ptr);
    mCurrentKey = key;
    mCurrentOpaque = opaque;
    return mCurrentBuffer.get();
}
} // namespace OHOS
} // namespace HDI
} // namespace DISPLAY
