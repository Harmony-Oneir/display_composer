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

#include "drm_vsync_worker.h"
#include <unistd.h>
#include <ctime>
#include <cerrno>
#include <xf86drm.h>
#include <chrono>
#include "display_log.h"
#include "drm_device.h"

namespace OHOS {
namespace HDI {
namespace DISPLAY {
DrmVsyncWorker::DrmVsyncWorker() {}

int32_t DrmVsyncWorker::Init(int fd)
{
    DISPLAY_CHK_RETURN((fd < 0), DISPLAY_FAILURE, DISPLAY_LOGE("the fd is invalid"));
    mDrmFd = fd;
    DISPLAY_LOGD("the drm fd is %{public}d", fd);
    mThread = std::make_unique<std::thread>([this]() { WorkThread(); });
    DISPLAY_CHK_RETURN((mThread == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("can not create thread"));
    mRunning = true;
    return DISPLAY_SUCCESS;
}

DrmVsyncWorker &DrmVsyncWorker::GetInstance()
{
    static DrmVsyncWorker instance;
    static std::once_flag once;
    std::call_once(once, [&]() {
        int ret = instance.Init(DrmDevice::GetDrmFd());
        if (ret != DISPLAY_SUCCESS) {
            DISPLAY_LOGE("Vsync Worker Init failed");
        }
    });
    return instance;
}

DrmVsyncWorker::~DrmVsyncWorker()
{
    DISPLAY_LOGD();
    {
        std::lock_guard<std::mutex> lg(mMutex);
        mRunning = false;
    }
    DISPLAY_LOGD();
    mCondition.notify_one();
    if (mThread != nullptr) {
        mThread->join();
    }
    DISPLAY_LOGD();
}

bool DrmVsyncWorker::WaitSignalAndCheckRuning()
{
    std::unique_lock<std::mutex> ul(mMutex);
    mCondition.wait(ul, [this]() { return (mEnable || !mRunning); });
    return mRunning;
}


uint64_t DrmVsyncWorker::WaitNextVBlank(unsigned int &sq)
{
    // oneir: настоящий VBlank ядра вместо таймера QEMU-реализации (usleep(16666) давал период
    // ~17,5 мс при развёртке панели 16,65 мс, и генератор VSync в RS отставал от экрана — рывки).
    // Время из ответа ядра — момент VBlank по CLOCK_MONOTONIC (те же часы, что у RS), в нс.
    // Pipe 0 (pipe A) — встроенная панель стенда; для других pipe нужны DRM_VBLANK_SECONDARY
    // (pipe 1) или pipe << DRM_VBLANK_HIGH_CRTC_SHIFT.
    static constexpr uint32_t VBLANK_PIPE = 0;
    drmVBlank vbl = {};
    uint32_t type = DRM_VBLANK_RELATIVE;
    if (VBLANK_PIPE == 1) {
        type |= DRM_VBLANK_SECONDARY;
    } else if (VBLANK_PIPE > 1) {
        type |= (VBLANK_PIPE << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK;
    }
    vbl.request.type = static_cast<drmVBlankSeqType>(type);
    vbl.request.sequence = 1; // ближайший VBlank
    if (drmWaitVBlank(mDrmFd, &vbl) == 0) {
        sq = vbl.reply.sequence;
        return static_cast<uint64_t>(vbl.reply.tval_sec) * 1000000000ULL +
               static_cast<uint64_t>(vbl.reply.tval_usec) * 1000ULL;
    }

    // Запасной путь (CRTC выключен и т.п.): программный период, но с правильным временем в нс.
    static uint32_t failures = 0;
    if ((failures++ % 600) == 0) { // не чаще раза в ~10 с
        DISPLAY_LOGW("drmWaitVBlank failed, errno %{public}d; software vsync", errno);
    }
    usleep(16666);
    struct timespec curTime = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &curTime);
    sq++;
    return static_cast<uint64_t>(curTime.tv_sec) * 1000000000ULL + static_cast<uint64_t>(curTime.tv_nsec);
}


void DrmVsyncWorker::EnableVsync(bool enable)
{
    DISPLAY_LOGD();
    {
        std::lock_guard<std::mutex> lg(mMutex);
        mEnable = enable;
    }
    mCondition.notify_one();
}

void DrmVsyncWorker::WorkThread()
{
    DISPLAY_LOGD();
    unsigned int seq = 0;
    while (WaitSignalAndCheckRuning()) {
        // wait the vblank
        uint64_t time = WaitNextVBlank(seq);
        if (mCallBack != nullptr) {
            mCallBack->Vsync(seq, time);
        } else {
            DISPLAY_LOGE("the callbac is nullptr");
        }
    }
}

void DrmVsyncWorker::ReqesterVBlankCb(std::shared_ptr<VsyncCallBack> &cb)
{
    DISPLAY_LOGD();
    DISPLAY_CHK_RETURN_NOT_VALUE((cb == nullptr), DISPLAY_LOGE("the VBlankCallback is nullptr "));
    mCallBack = cb;
}
} // namespace OHOS
} // namespace HDI
} // namespace DISPLAY
