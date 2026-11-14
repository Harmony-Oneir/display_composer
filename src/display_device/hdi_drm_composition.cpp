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

#include "hdi_drm_composition.h"
#include <cerrno>
#include <poll.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <thread>
#include <xf86drm.h>
#include "param/sys_param.h"
#include <map>
#include <string>
#include <mutex>
#include <unistd.h>
#include "hdi_drm_layer.h"
#include <algorithm>

namespace {
// oneir: не более одного незавершённого неблокирующего коммита на CRTC.
// Иначе ядро отвергает новый коммит с EBUSY, композитор не обновляет release fence,
// и RS начинает рисовать в буфер, который ещё на экране (мерцание движущихся объектов).
std::mutex g_prevCommitMutex;
std::map<uint32_t, int> g_prevCommitFence; // CRTC -> OUT_FENCE последнего коммита

void WaitPrevCommit(uint32_t crtcId, int timeoutMs)
{
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_prevCommitMutex);
        auto it = g_prevCommitFence.find(crtcId);
        if (it != g_prevCommitFence.end()) {
            fd = it->second;
        }
    }
    if (fd >= 0) {
        struct pollfd pfd = {fd, POLLIN, 0};
        poll(&pfd, 1, timeoutMs);
    }
}

void StorePrevCommit(uint32_t crtcId, int outFence)
{
    if (outFence < 0) {
        return;
    }
    int copy = dup(outFence);
    std::lock_guard<std::mutex> lock(g_prevCommitMutex);
    auto it = g_prevCommitFence.find(crtcId);
    if (it != g_prevCommitFence.end()) {
        if (it->second >= 0) {
            close(it->second);
        }
        it->second = copy;
    } else {
        g_prevCommitFence.emplace(crtcId, copy);
    }
}

// oneir: режимы композитора через системные параметры (кэшированные: дёшево читать на каждом
// коммите, изменение через `param set` подхватывается без перезапуска).
bool ParamEnabled(const char *name)
{
    static std::mutex m;
    static std::map<std::string, CachedHandle> handles;
    std::lock_guard<std::mutex> lock(m);
    auto it = handles.find(name);
    if (it == handles.end()) {
        it = handles.emplace(name, CachedParameterCreate(name, "0")).first;
    }
    const char *v = it->second ? CachedParameterGet(it->second) : nullptr;
    return v != nullptr && v[0] == '1';
}

// oneir: flip events: отслеживание завершения смены кадра по событиям DRM.
// Ядро отвергает неблокирующий коммит (EBUSY), пока у предыдущего не завершён flip_done; событие
// DRM_MODE_PAGE_FLIP_EVENT приходит ровно в этот момент. Поток событий читает их из fd DRM и
// снимает отметку «смена кадра в процессе» для CRTC.
class FlipTracker {
public:
    void EnsureStarted(int fd)
    {
        std::call_once(once_, [this, fd]() {
            fd_ = fd;
            std::thread([this]() { Loop(); }).detach();
        });
    }

    void MarkPending(uint32_t crtcId)
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_[crtcId] = true;
    }

    void ClearPending(uint32_t crtcId)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_[crtcId] = false;
        }
        cv_.notify_all();
    }

    // true — смена кадра завершена (или её не было); false — таймаут
    bool WaitIdle(uint32_t crtcId, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, crtcId]() {
            auto it = pending_.find(crtcId);
            return it == pending_.end() || !it->second;
        });
    }

private:
    static void OnFlip(int, unsigned int, unsigned int, unsigned int, unsigned int crtcId, void *userData)
    {
        // номер CRTC приходит в событии, если ядро поддерживает DRM_CAP_CRTC_IN_VBLANK_EVENT
        // (Linux >= 4.12); иначе 0 — тогда берём его из user_data, переданного в коммит
        if (crtcId == 0) {
            crtcId = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(userData));
        }
        Instance().ClearPending(crtcId);
    }

    void Loop()
    {
        drmEventContext ev = {};
        ev.version = 3; // page_flip_handler2 (с номером CRTC)
        ev.page_flip_handler2 = &FlipTracker::OnFlip;
        while (true) {
            struct pollfd pfd = {fd_, POLLIN, 0};
            int r = poll(&pfd, 1, 1000);
            if (r > 0 && (pfd.revents & POLLIN)) {
                drmHandleEvent(fd_, &ev);
            }
        }
    }

public:
    static FlipTracker &Instance()
    {
        static FlipTracker tracker;
        return tracker;
    }

private:
    std::once_flag once_;
    int fd_ = -1;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<uint32_t, bool> pending_;
};

constexpr int FLIP_WAIT_MS = 100;
constexpr int EBUSY_RETRIES = 3;
} // namespace

namespace OHOS {
namespace HDI {
namespace DISPLAY {
HdiDrmComposition::HdiDrmComposition(const std::shared_ptr<DrmConnector> &connector,
                                     const std::shared_ptr<DrmCrtc> &crtc,
                                     const std::shared_ptr<DrmDevice> &drmDevice)
    : mDrmDevice(drmDevice), mConnector(connector), mCrtc(crtc)
{
    DISPLAY_LOGD();
}

int32_t HdiDrmComposition::Init()
{
    DISPLAY_LOGD();
    mPrimPlanes.clear();
    mOverlayPlanes.clear();
    mPlanes.clear();
    DISPLAY_CHK_RETURN((mCrtc == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("crtc is null"));
    DISPLAY_CHK_RETURN((mConnector == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("connector is null"));
    DISPLAY_CHK_RETURN((mDrmDevice == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("drmDevice is null"));
    mPrimPlanes = mDrmDevice->GetDrmPlane(mCrtc->GetPipe(), DRM_PLANE_TYPE_PRIMARY);
    mOverlayPlanes = mDrmDevice->GetDrmPlane(mCrtc->GetPipe(), DRM_PLANE_TYPE_OVERLAY);
    mPlanes.insert(mPlanes.end(), mPrimPlanes.begin(), mPrimPlanes.end());
    mPlanes.insert(mPlanes.end(), mOverlayPlanes.begin(), mOverlayPlanes.end());
    // oneir: hwc — overlay-плоскости по порядку наложения (zpos), чтобы слои выше по Z шли на верхние
    std::stable_sort(mOverlayPlanes.begin(), mOverlayPlanes.end(),
        [](const std::shared_ptr<DrmPlane> &a, const std::shared_ptr<DrmPlane> &b) {
            return a->GetZpos() < b->GetZpos();
        });
    // RS узнаёт о возможностях дисплея из GetDisplayCapability: сколько слоёв можно вывести аппаратно
    mConnector->SetSupportLayers(static_cast<uint32_t>(mPrimPlanes.size() + mOverlayPlanes.size()));
    DISPLAY_LOGI("hwc: pipe %{public}d primary %{public}zu overlay %{public}zu", mCrtc->GetPipe(),
        mPrimPlanes.size(), mOverlayPlanes.size());
    return DISPLAY_SUCCESS;
}

// oneir: hwc — слой покрывает весь экран (размер активного режима)
bool HdiDrmComposition::IsFullScreen(HdiLayer &layer) const
{
    DrmMode mode;
    if (mConnector->GetModeFromId(mCrtc->GetActiveModeId(), mode) != DISPLAY_SUCCESS) {
        return false;
    }
    const drmModeModeInfo *info = mode.GetModeInfoPtr();
    const IRect &d = layer.GetLayerDisplayRect();
    return info != nullptr && d.x <= 0 && d.y <= 0 && d.x + d.w >= info->hdisplay && d.y + d.h >= info->vdisplay;
}

// oneir: hwc — можно ли вывести слой на эту плоскость как есть (без композиции на GPU)
bool HdiDrmComposition::CanScanout(HdiLayer &layer, const DrmPlane &plane) const
{
    // причина отказа пишется в отладочный лог: «hwc: layer N plane M: …»
    auto reject = [&layer, &plane](const char *why) {
        DISPLAY_LOGD("hwc: layer %{public}u plane %{public}u: %{public}s", layer.GetId(), plane.GetId(), why);
        return false;
    };
    if (layer.GetCompositionType() != COMPOSITION_DEVICE) {
        return reject("RS did not request DEVICE");
    }
    HdiLayerBuffer *buf = layer.GetCurrentBuffer();
    if (buf == nullptr || buf->GetWight() <= 0 || buf->GetHeight() <= 0) {
        return reject("no buffer");
    }
    uint32_t fourcc = DrmDevice::ConvertToDrmFormat(static_cast<PixelFormat>(buf->GetFormat()));
    // пока только одноплоскостные RGB-форматы (YUV — вместе с внешними форматами Vulkan)
    switch (fourcc) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGB565:
            break;
        default:
            DISPLAY_LOGD("hwc: layer %{public}u: pixel format %{public}d (drm 0x%{public}x) not supported",
                layer.GetId(), buf->GetFormat(), fourcc);
            return false;
    }
    if (!plane.SupportsFormat(fourcc)) {
        return reject("format not supported by plane");
    }
    TransformType t = layer.GetTransFormType();
    if (t != ROTATE_NONE && t != ROTATE_BUTT) {
        DISPLAY_LOGD("hwc: layer %{public}u: transform %{public}d", layer.GetId(), static_cast<int>(t));
        return false;  // поворот — на GPU
    }
    const LayerAlpha &alpha = layer.GetAlpha();
    if (alpha.enGlobalAlpha && alpha.gAlpha != 0xff && plane.GetPropAlphaId() == 0) {
        return reject("global alpha without plane alpha property");
    }
    const IRect &d = layer.GetLayerDisplayRect();
    if (d.w <= 0 || d.h <= 0) {
        return reject("empty display rect");
    }
    return true;
}

int32_t HdiDrmComposition::SetLayers(std::vector<HdiLayer *> &layers, HdiLayer &clientLayer)
{
    DISPLAY_LOGD();
    mCompLayers.clear();
    mAssignment.clear();
    mFrameLayers = layers;
    for (auto *layer : layers) {
        layer->SetDeviceSelect(COMPOSITION_CLIENT);
    }

    // Раскладка. В режиме единого рендеринга RS рисует весь экран в свой буфер и предлагает его
    // как DEVICE-слой (обычно единственный); отдельные слои — только у поверхностей с аппаратным
    // выводом. Поэтому:
    //  * все слои подходят — нижний на основную плоскость, остальные на overlay; клиентский буфер
    //    не нужен (иначе RS ещё раз копирует весь кадр на GPU в клиентский буфер);
    //  * иначе — клиентский буфер на основной плоскости, подходящие верхние слои — на overlay.
    std::vector<HdiLayer *> device;  // слои на overlay, сверху вниз
    bool bottomOnPrimary = false;
    const bool enabled = ParamEnabled("persist.oneir.composer.device");
    if (enabled && !layers.empty()) {
        // Нижний слой может заменить клиентский буфер на основной плоскости, только если он покрывает
        // весь экран (кадр RS без аппаратных поверхностей). Иначе (например, видео поверх кадра,
        // который RS рисует в клиентский буфер) все слои, включая нижний, претендуют на overlay.
        // Нижний слой — кандидат на основную плоскость (zpos 0), слои над ним — на overlay.
        // Так выводится и кадр RS без аппаратных поверхностей (нижний слой во весь экран), и схема
        // OHOS underlay: поверхность (видео) снизу в своём прямоугольнике, сверху слой экрана с
        // прозрачным окном на её месте (RS даёт поверхности меньший zorder, чем слою экрана).
        const bool bottomFull = IsFullScreen(*layers[0]);
        size_t i = layers.size();
        while (i-- > 1 && device.size() < mOverlayPlanes.size()) {
            const auto &plane = mOverlayPlanes[mOverlayPlanes.size() - 1 - device.size()];
            if (!CanScanout(*layers[i], *plane)) {
                break;
            }
            device.push_back(layers[i]);
        }
        const bool aboveOnOverlay = (device.size() == layers.size() - 1);
        if (aboveOnOverlay && !mPrimPlanes.empty() && CanScanout(*layers[0], *mPrimPlanes[0])) {
            bottomOnPrimary = true;
        } else if (!bottomFull && aboveOnOverlay && device.size() < mOverlayPlanes.size()) {
            // основная плоскость не подошла: нижний слой (не во весь экран) — на overlay
            // поверх клиентского буфера
            const auto &plane = mOverlayPlanes[mOverlayPlanes.size() - 1 - device.size()];
            if (CanScanout(*layers[0], *plane)) {
                device.push_back(layers[0]);
            }
        }
        std::reverse(device.begin(), device.end());  // снизу вверх
    }

    std::vector<PlaneAssignment> assignment;
    if (!mPrimPlanes.empty()) {
        if (bottomOnPrimary) {
            assignment.push_back({layers[0], mPrimPlanes[0], false, 0, true});
        } else {
            assignment.push_back({&clientLayer, mPrimPlanes[0], true, 0, true});
        }
    }
    size_t firstOverlay = mOverlayPlanes.size() - device.size();
    for (size_t k = 0; k < device.size(); k++) {
        assignment.push_back({device[k], mOverlayPlanes[firstOverlay + k], false, static_cast<uint32_t>(k + 1), false});
    }
    if (bottomOnPrimary) {
        device.insert(device.begin(), layers[0]);
    }

    // проверка у ядра: примет ли оно такую раскладку (масштабирование, scaler'ы, пропускная способность)
    if (!device.empty() && !TestAssignment(assignment)) {
        DISPLAY_LOGI("hwc: test-only rejected %{public}zu device layers, fallback to client", device.size());
        assignment.clear();
        if (!mPrimPlanes.empty()) {
            assignment.push_back({&clientLayer, mPrimPlanes[0], true, 0, true});
        }
        device.clear();
    }
    for (auto *layer : device) {
        layer->SetDeviceSelect(COMPOSITION_DEVICE);
        DISPLAY_LOGD("hwc: layer %{public}u -> device", layer->GetId());
    }
    mAssignment = std::move(assignment);
    for (const auto &as : mAssignment) {
        mCompLayers.push_back(as.layer);
    }
    return DISPLAY_SUCCESS;
}

bool HdiDrmComposition::TestAssignment(const std::vector<PlaneAssignment> &assignment)
{
    drmModeAtomicReqPtr pset = drmModeAtomicAlloc();
    if (pset == nullptr) {
        return false;
    }
    AtomicReqPtr req(pset);
    for (const auto &as : assignment) {
        if (as.isClient && as.layer->GetCurrentBuffer() == nullptr) {
            continue;  // первый кадр: клиентского буфера ещё нет
        }
        if (ApplyPlane(as, pset, false) != DISPLAY_SUCCESS) {
            return false;
        }
    }
    int ret = drmModeAtomicCommit(mDrmDevice->GetDrmFd(), pset, DRM_MODE_ATOMIC_TEST_ONLY, nullptr);
    if (ret != 0) {
        DISPLAY_LOGD("hwc: test-only commit failed errno %{public}d", errno);
    }
    return ret == 0;
}

int32_t HdiDrmComposition::SetCrtcProperty(DrmPlane &drmPlane,
                                           drmModeAtomicReqPtr pset,
                                           int32_t bufferW,
                                           int32_t bufferH)
{
    int ret;

    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropCrtc_xId(), 0);
    DISPLAY_LOGD("set the fb planeid %{public}d, GetPropCrtc_xId %{public}d, crop.x %{public}d", drmPlane.GetId(),
        drmPlane.GetPropCrtc_xId(), 0);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set the fb planeid fialed errno : %{public}d", errno));

    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropCrtc_yId(), 0);
    DISPLAY_LOGD("set the fb planeid %{public}d, GetPropCrtc_yId %{public}d, crop.y %{public}d", drmPlane.GetId(),
        drmPlane.GetPropCrtc_yId(), 0);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set the fb planeid fialed errno : %{public}d", errno));

    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropCrtc_wId(), bufferW);
    DISPLAY_LOGD("set the fb planeid %{public}d, GetPropCrtc_wId %{public}d, crop.w %{public}d", drmPlane.GetId(),
        drmPlane.GetPropCrtc_wId(), bufferW);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set the fb planeid fialed errno : %{public}d", errno));

    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropCrtc_hId(), bufferH);
    DISPLAY_LOGD("set the fb planeid %{public}d, GetPropCrtc_hId %{public}d, crop.h %{public}d", drmPlane.GetId(),
        drmPlane.GetPropCrtc_xId(), bufferH);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set the fb planeid fialed errno : %{public}d", errno));

    return DISPLAY_SUCCESS;
}

int32_t HdiDrmComposition::SetSrcProperty(DrmPlane &drmPlane,
                                          drmModeAtomicReqPtr pset,
                                          int32_t bufferW,
                                          int32_t bufferH)
{
    int ret;

    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropSrc_xId(), 0 << 16); // 16:shift left 16 bits
    DISPLAY_LOGD("set the fb planeid %{public}d, GetPropSrc_xId %{public}d, displayRect.x %{public}d",
        drmPlane.GetId(), drmPlane.GetPropSrc_xId(), 0);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set the fb planeid fialed errno : %{public}d", errno));

    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropSrc_yId(), 0 << 16); // 16:shift left 16 bits
    DISPLAY_LOGD("set the fb planeid %{public}d, GetPropSrc_yId %{public}d, displayRect.y %{public}d",
        drmPlane.GetId(), drmPlane.GetPropSrc_yId(), 0);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set the fb planeid fialed errno : %{public}d", errno));

    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropSrc_wId(),
                                   bufferW << 16); // 16:shift left 16 bits
    DISPLAY_LOGD("set the fb planeid %{public}d, GetPropCrtc_wId %{public}d, displayRect.w %{public}d",
        drmPlane.GetId(), drmPlane.GetPropSrc_wId(), bufferW);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set the fb planeid fialed errno : %{public}d", errno));

    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropSrc_hId(),
        bufferH << 16); // 16:shift left 16 bits
    DISPLAY_LOGD("set the fb planeid %{public}d, GetPropSrc_hId %{public}d, displayRect.h %{public}d",
        drmPlane.GetId(), drmPlane.GetPropSrc_hId(), bufferH);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set the fb planeid fialed errno : %{public}d", errno));

    return DISPLAY_SUCCESS;
}

int32_t HdiDrmComposition::ApplyPlane(HdiDrmLayer &layer,
                                      HdiLayer &hlayer,
                                      DrmPlane &drmPlane,
                                      drmModeAtomicReqPtr pset)
{
    int ret;
    int fenceFd = layer.GetAcquireFenceFd();
    int propId = drmPlane.GetPropFenceInId();
    HdiLayerBuffer *layerBuffer = hlayer.GetCurrentBuffer();
    int32_t bufferW = layerBuffer->GetWight();
    int32_t bufferH = layerBuffer->GetHeight();

    DISPLAY_LOGD();
    if (propId != 0) {
        DISPLAY_LOGD("set the fence in prop");
        if (fenceFd >= 0) {
            ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), propId, fenceFd);
            DISPLAY_LOGD("set the IfenceProp plane id %{public}d, propId %{public}d, fenceFd %{public}d",
                drmPlane.GetId(), propId, fenceFd);
            DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set IN_FENCE_FD failed"));
        }
    }

    ret = SetCrtcProperty(drmPlane, pset, bufferW, bufferH);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set Crtc fialed errno : %{public}d", errno));

    ret = SetSrcProperty(drmPlane, pset, bufferW, bufferH);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set Src fialed errno : %{public}d", errno));


    // set fb id
    DrmGemBuffer *gemBuffer = layer.GetGemBuffer();
    DISPLAY_CHK_RETURN((gemBuffer == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("current gemBuffer is nullptr"));
    DISPLAY_CHK_RETURN((!gemBuffer->IsValid()), DISPLAY_FAILURE, DISPLAY_LOGE("the DrmGemBuffer is invalid"));
    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropFbId(), gemBuffer->GetFbId());
    DISPLAY_LOGD("set the fb planeid %{public}d, propId %{public}d, fbId %{public}d",
        drmPlane.GetId(), drmPlane.GetPropFbId(), gemBuffer->GetFbId());
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set fb id fialed errno : %{public}d", errno));

    // set crtc id
    ret = drmModeAtomicAddProperty(pset, drmPlane.GetId(), drmPlane.GetPropCrtcId(), mCrtc->GetId());
    DISPLAY_LOGD("set the crtc planeId %{public}d, propId %{public}d, crtcId %{public}d",
        drmPlane.GetId(), drmPlane.GetPropCrtcId(), mCrtc->GetId());
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set crtc id fialed errno : %{public}d", errno));
    return DISPLAY_SUCCESS;
}

int32_t HdiDrmComposition::SetPlaneRects(DrmPlane &drmPlane, drmModeAtomicReqPtr pset, const PlaneRect &crtc,
    const PlaneRect &src)
{
    const uint32_t id = drmPlane.GetId();
    const int shift = 16;  // SRC_* в формате 16.16
    int ret = 0;
    ret |= drmModeAtomicAddProperty(pset, id, drmPlane.GetPropCrtc_xId(), static_cast<uint64_t>(crtc.x));
    ret |= drmModeAtomicAddProperty(pset, id, drmPlane.GetPropCrtc_yId(), static_cast<uint64_t>(crtc.y));
    ret |= drmModeAtomicAddProperty(pset, id, drmPlane.GetPropCrtc_wId(), static_cast<uint64_t>(crtc.w));
    ret |= drmModeAtomicAddProperty(pset, id, drmPlane.GetPropCrtc_hId(), static_cast<uint64_t>(crtc.h));
    ret |= drmModeAtomicAddProperty(pset, id, drmPlane.GetPropSrc_xId(), static_cast<uint64_t>(src.x) << shift);
    ret |= drmModeAtomicAddProperty(pset, id, drmPlane.GetPropSrc_yId(), static_cast<uint64_t>(src.y) << shift);
    ret |= drmModeAtomicAddProperty(pset, id, drmPlane.GetPropSrc_wId(), static_cast<uint64_t>(src.w) << shift);
    ret |= drmModeAtomicAddProperty(pset, id, drmPlane.GetPropSrc_hId(), static_cast<uint64_t>(src.h) << shift);
    return ret < 0 ? DISPLAY_FAILURE : DISPLAY_SUCCESS;
}

// oneir: hwc — вывод слоя на назначенную плоскость: геометрия слоя, формат буфера, альфа, порядок
int32_t HdiDrmComposition::ApplyPlane(const PlaneAssignment &as, drmModeAtomicReqPtr pset, bool withFence)
{
    HdiDrmLayer &layer = *static_cast<HdiDrmLayer *>(as.layer);
    DrmPlane &plane = *as.plane;
    HdiLayerBuffer *buf = layer.GetCurrentBuffer();
    DISPLAY_CHK_RETURN((buf == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("layer %{public}u has no buffer", layer.GetId()));

    // область источника: обрезка слоя, иначе весь буфер; область на экране: прямоугольник слоя
    PlaneRect src {0, 0, buf->GetWight(), buf->GetHeight()};
    const IRect &crop = layer.GetLayerCrop();
    if (!as.isClient && crop.w > 0 && crop.h > 0) {
        src = {crop.x, crop.y, crop.w, crop.h};
    }
    PlaneRect dst {0, 0, buf->GetWight(), buf->GetHeight()};
    const IRect &d = layer.GetLayerDisplayRect();
    if (!as.isClient && d.w > 0 && d.h > 0) {
        dst = {d.x, d.y, d.w, d.h};
    }

    if (withFence && plane.GetPropFenceInId() != 0 && layer.GetAcquireFenceFd() >= 0) {
        int ret = drmModeAtomicAddProperty(pset, plane.GetId(), plane.GetPropFenceInId(), layer.GetAcquireFenceFd());
        DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set IN_FENCE_FD failed"));
    }
    DISPLAY_CHK_RETURN((SetPlaneRects(plane, pset, dst, src) != DISPLAY_SUCCESS), DISPLAY_FAILURE,
        DISPLAY_LOGE("set plane rects failed errno %{public}d", errno));

    // нижний слой (клиентский буфер или слой на основной плоскости) — непрозрачный, остальные — в своём формате
    DrmGemBuffer *gem = layer.GetGemBuffer(as.isClient || as.opaque);
    DISPLAY_CHK_RETURN((gem == nullptr || !gem->IsValid()), DISPLAY_FAILURE,
        DISPLAY_LOGE("no valid framebuffer for layer %{public}u", layer.GetId()));
    int ret = drmModeAtomicAddProperty(pset, plane.GetId(), plane.GetPropFbId(), gem->GetFbId());
    ret |= drmModeAtomicAddProperty(pset, plane.GetId(), plane.GetPropCrtcId(), mCrtc->GetId());
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set fb/crtc failed errno %{public}d", errno));

    // необязательные свойства — только если плоскость их поддерживает
    if (plane.GetPropAlphaId() != 0) {
        const LayerAlpha &alpha = layer.GetAlpha();
        uint64_t a = (!as.isClient && alpha.enGlobalAlpha) ? (static_cast<uint64_t>(alpha.gAlpha) * 0x101) : 0xffff;
        drmModeAtomicAddProperty(pset, plane.GetId(), plane.GetPropAlphaId(), a);
    }
    if (plane.GetPropBlendModeId() != 0 && !as.isClient) {
        int64_t mode = plane.GetBlendModeValue(layer.GetLayerPreMulti() ? "Pre-multiplied" : "Coverage");
        if (mode < 0) {
            mode = plane.GetBlendModeValue("Pre-multiplied");
        }
        if (mode >= 0) {
            drmModeAtomicAddProperty(pset, plane.GetId(), plane.GetPropBlendModeId(), static_cast<uint64_t>(mode));
        }
    }
    if (plane.GetPropZposId() != 0 && !plane.IsZposImmutable()) {
        drmModeAtomicAddProperty(pset, plane.GetId(), plane.GetPropZposId(), as.zposOrder);
    }
    // в лог: кто выводится (client — клиентский буфер, device — слой RS) и проверка ли это (test)
    DISPLAY_LOGD("hwc: %{public}s plane %{public}u %{public}s layer %{public}u fb %{public}u fmt 0x%{public}x "
        "dst %{public}d,%{public}d %{public}dx%{public}d src %{public}d,%{public}d %{public}dx%{public}d",
        withFence ? "apply" : "test", plane.GetId(), as.isClient ? "client" : "device", layer.GetId(),
        gem->GetFbId(), gem->GetDrmFormat(), dst.x, dst.y, dst.w, dst.h, src.x, src.y, src.w, src.h);
    return DISPLAY_SUCCESS;
}

int32_t HdiDrmComposition::UpdateMode(std::unique_ptr<DrmModeBlock> &modeBlock)
{
    // set the mode
    DISPLAY_LOGD();
    if (mCrtc->NeedModeSet()) {
        int drmFd = mDrmDevice->GetDrmFd();
        // oneir: display modes: запрос освобождается на всех путях (раньше утекал)
        std::unique_ptr<drmModeAtomicReq, void (*)(drmModeAtomicReqPtr)> psetGuard(drmModeAtomicAlloc(),
            drmModeAtomicFree);
        drmModeAtomicReqPtr pset = psetGuard.get();
        DISPLAY_CHK_RETURN((pset == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("drmModeAtomicAlloc failed"));
        modeBlock = mConnector->GetModeBlockFromId(mCrtc->GetActiveModeId());
        if ((modeBlock != nullptr) && (modeBlock->GetBlockId() != DRM_INVALID_ID)) {
            // set to active
            DISPLAY_LOGD("set crtc to active");
            int ret = drmModeAtomicAddProperty(pset, mCrtc->GetId(), mCrtc->GetActivePropId(), 1);
            DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE,
                DISPLAY_LOGE("can not add the active prop errno %{public}d", errno));

            // set the mode id
            DISPLAY_LOGD("set the mode");
            ret = drmModeAtomicAddProperty(pset, mCrtc->GetId(), mCrtc->GetModePropId(), modeBlock->GetBlockId());
            DISPLAY_LOGD("set the mode planeId %{public}d, propId %{public}d, GetBlockId: %{public}d",
                mCrtc->GetId(), mCrtc->GetModePropId(), modeBlock->GetBlockId());
            DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE,
                DISPLAY_LOGE("can not add the mode prop errno %{public}d", errno));
            ret = drmModeAtomicAddProperty(pset, mConnector->GetId(), mConnector->GetPropCrtcId(), mCrtc->GetId());
            DISPLAY_LOGD("set the connector id: %{public}d, propId %{public}d, crtcId %{public}d",
                mConnector->GetId(), mConnector->GetPropCrtcId(), mCrtc->GetId());
            DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE,
                DISPLAY_LOGE("can not add the crtc id prop %{public}d", errno));

            uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
            WaitPrevCommit(mCrtc->GetId(), 100); // oneir: display modes: не застать незавершённый коммит кадра
            ret = drmModeAtomicCommit(drmFd, pset, flags, nullptr);
            DISPLAY_CHK_RETURN((ret != 0), DISPLAY_FAILURE,
                DISPLAY_LOGE("drmModeAtomicCommit failed %{public}d errno %{public}d", ret, errno));
            mCrtc->ClearModeSet();
        }
    }
    return DISPLAY_SUCCESS;
}

// oneir: hwc — выключить плоскости pipe, не занятые в этом кадре (FB_ID и CRTC_ID вместе)
int32_t HdiDrmComposition::RemoveUnusePlane(drmModeAtomicReqPtr pset)
{
    for (auto &drmPlane : mPlanes) {
        bool used = std::any_of(mAssignment.begin(), mAssignment.end(),
            [&drmPlane](const PlaneAssignment &as) { return as.plane == drmPlane; });
        if (!used) {
            drmModeAtomicAddProperty(pset, drmPlane->GetId(), drmPlane->GetPropFbId(), 0);
            drmModeAtomicAddProperty(pset, drmPlane->GetId(), drmPlane->GetPropCrtcId(), 0);
            drmPlane->UnBindPipe();
        }
    }
    return DISPLAY_SUCCESS;
}

// oneir: hwc — применить раскладку, выбранную в SetLayers
int32_t HdiDrmComposition::FindPlaneAndApply(drmModeAtomicReqPtr pset)
{
    for (const auto &as : mAssignment) {
        if (ApplyPlane(as, pset, true) != DISPLAY_SUCCESS) {
            DISPLAY_LOGE("apply plane %{public}u failed", as.plane->GetId());
            continue;
        }
        as.plane->BindToPipe(1 << mCrtc->GetPipe());
    }
    return DISPLAY_SUCCESS;
}

int32_t HdiDrmComposition::Apply(bool modeSet)
{
    uint64_t crtcOutFence = -1;
    int ret = 0;
    std::unique_ptr<DrmModeBlock> modeBlock;
    int drmFd = mDrmDevice->GetDrmFd();

    ret = UpdateMode(modeBlock);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("update mode failed"));

    DISPLAY_LOGD("mPlane size: %{public}zd mCompLayers size: %{public}zd", mPlanes.size(), mCompLayers.size());
    DISPLAY_CHK_RETURN((mAssignment.empty()), DISPLAY_FAILURE, DISPLAY_LOGE("no planes assigned"));
    drmModeAtomicReqPtr pset = drmModeAtomicAlloc();
    DISPLAY_CHK_RETURN((pset == nullptr), DISPLAY_NULL_PTR,
        DISPLAY_LOGE("drm atomic alloc failed errno %{public}d", errno));
    AtomicReqPtr atomicReqPtr = AtomicReqPtr(pset);

    // set the outFence property
    ret = drmModeAtomicAddProperty(atomicReqPtr.Get(), mCrtc->GetId(), mCrtc->GetOutFencePropId(),
        (uint64_t)&crtcOutFence);

    DISPLAY_LOGD("Apply Set OutFence crtc id: %{public}d, fencePropId %{public}d", mCrtc->GetId(),
        mCrtc->GetOutFencePropId());
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE, DISPLAY_LOGE("set the outfence property of crtc failed "));

    // set the plane info.
    DISPLAY_LOGD("mCompLayers size %{public}zd", mCompLayers.size());
    DISPLAY_LOGD("crtc id %{public}d connect id %{public}d encoder id %{public}d", mCrtc->GetId(),
        mConnector->GetId(), mConnector->GetEncoderId());

    /*  Bind the plane not used by other crtcs to the crtc. */
    FindPlaneAndApply(atomicReqPtr.Get());
    /* Remove useless planes from the drm */
    RemoveUnusePlane(atomicReqPtr.Get());

    ret = drmModeAtomicAddProperty(pset, mConnector->GetId(), mConnector->GetPropCrtcId(), mCrtc->GetId());
    DISPLAY_LOGD("set the connector id: %{public}d, propId %{public}d, crtcId %{public}d", mConnector->GetId(),
        mConnector->GetPropCrtcId(), mCrtc->GetId());
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE,
        DISPLAY_LOGE("can not add the crtc id prop %{public}d", errno));

    // set to active
    DISPLAY_LOGD("set crtc to active");
    ret = drmModeAtomicAddProperty(pset, mCrtc->GetId(), mCrtc->GetActivePropId(), 1);
    DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE,
        DISPLAY_LOGE("can not add the active prop errno %{public}d", errno));

    // oneir: отладочные переключатели для поиска причины мерцания (системные параметры, без перезапуска).
    //   oneir.composer.cpuwait=1          — перед коммитом ждать acquire fence слоёв на CPU (до 100 мс)
    //   persist.oneir.composer.nonblock=1 — неблокирующий атомарный коммит (по умолчанию блокирующий)
    if (ParamEnabled("oneir.composer.cpuwait")) {
        for (auto layer : mCompLayers) {
            int fd = layer->GetAcquireFenceFd();
            if (fd >= 0) {
                struct pollfd pfd = {fd, POLLIN, 0};
                int pr = poll(&pfd, 1, 100);
                DISPLAY_LOGD("cpuwait: layer %{public}d fence %{public}d poll %{public}d", layer->GetId(), fd, pr);
            }
        }
    }
    // oneir: по умолчанию блокирующий коммит — кадр выведен к моменту возврата, коммиты не
    // накладываются (неблокирующий режим давал EBUSY и мерцание движущихся объектов).
    uint32_t flags = ParamEnabled("persist.oneir.composer.nonblock") ? DRM_MODE_ATOMIC_NONBLOCK : 0;

    // oneir: flip events: в неблокирующем режиме — ждать события завершения предыдущей смены кадра и
    // запрашивать событие для этого коммита; в блокирующем — как раньше.
    const uint32_t crtcId = mCrtc->GetId();
    const bool nonblock = (flags & DRM_MODE_ATOMIC_NONBLOCK) != 0;
    void *userData = nullptr;
    if (nonblock) {
        auto &tracker = FlipTracker::Instance();
        tracker.EnsureStarted(drmFd);
        if (!tracker.WaitIdle(crtcId, FLIP_WAIT_MS)) {
            DISPLAY_LOGW("page flip event not received in %{public}d ms", FLIP_WAIT_MS);
            tracker.ClearPending(crtcId); // событие потеряно — не блокировать вывод навсегда
        }
        flags |= DRM_MODE_PAGE_FLIP_EVENT;
        userData = reinterpret_cast<void *>(static_cast<uintptr_t>(crtcId));
        tracker.MarkPending(crtcId);
    } else {
        WaitPrevCommit(crtcId, 50);
    }
    auto commitStart = std::chrono::steady_clock::now();
    ret = drmModeAtomicCommit(drmFd, atomicReqPtr.Get(), flags, userData);
    for (int attempt = 1; ret != 0 && errno == EBUSY && attempt <= EBUSY_RETRIES; attempt++) {
        DISPLAY_LOGW("drmModeAtomicCommit busy, retry %{public}d", attempt);
        WaitPrevCommit(crtcId, 20);
        usleep(1000);
        ret = drmModeAtomicCommit(drmFd, atomicReqPtr.Get(), flags, userData);
    }
    if (ret != 0 && nonblock) {
        FlipTracker::Instance().ClearPending(crtcId); // события не будет
    }
    DISPLAY_LOGD("commit %{public}s %{public}lld us", nonblock ? "nonblock" : "block",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - commitStart).count()));
    DISPLAY_CHK_RETURN((ret != 0), DISPLAY_FAILURE,
        DISPLAY_LOGE("drmModeAtomicCommit failed %{public}d errno %{public}d", ret, errno));
    StorePrevCommit(mCrtc->GetId(), static_cast<int>(crtcOutFence));
    // oneir: hwc — release fence: у каждого слоя своя копия OUT_FENCE (HdiFd владеет fd; раньше один fd
    // отдавался нескольким слоям и закрывался повторно). Получают все слои кадра: и выведенные аппаратно,
    // и скомпонованные RS — их прежние буферы могли оставаться на экране до этого коммита.
    const int outFence = static_cast<int>(crtcOutFence);
    if (outFence >= 0) {
        for (auto layer : mCompLayers) {
            layer->SetReleaseFence(dup(outFence));
        }
        for (auto layer : mFrameLayers) {
            if (std::find(mCompLayers.begin(), mCompLayers.end(), layer) == mCompLayers.end()) {
                layer->SetReleaseFence(dup(outFence));
            }
        }
        close(outFence);
    }

    return DISPLAY_SUCCESS;
}
} // OHOS
} // HDI
} // DISPLAY
