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

#include "oneir_drm_select.h"
#include "drm_connector.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <securec.h>
#include "display_log.h"
#include "drm_device.h"
#include "drm_vsync_worker.h"

namespace OHOS {
namespace HDI {
namespace DISPLAY {
namespace {
/* oneir: уровень яркости в интерфейсе композитора — 0..255; подсветка панели
 * (sysfs backlight) имеет свою шкалу 0..max_brightness (у intel_backlight — тысячи). */
constexpr uint32_t OHOS_BRIGHTNESS_MAX = 255;

/* Путь к файлу рядом с brightness (OneirBacklightPath() указывает на .../brightness). */
std::string BacklightSibling(const char *name)
{
    std::string path = OneirBacklightPath();
    auto pos = path.rfind('/');
    return (pos == std::string::npos ? std::string() : path.substr(0, pos + 1)) + name;
}

int32_t ReadSysfsInt(const std::string &path)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 ? atoi(buf) : -1;
}

/* max_brightness читается один раз; -1 — шкала неизвестна (пишем уровень как есть). */
int32_t BacklightMax()
{
    static int32_t maxValue = ReadSysfsInt(BacklightSibling("max_brightness"));
    return maxValue;
}
} // namespace

void DrmMode::ConvertToHdiMode(DisplayModeInfo &hdiMode)
{
    hdiMode.height = mModeInfo.vdisplay;
    hdiMode.width = mModeInfo.hdisplay;
    hdiMode.freshRate = mModeInfo.vrefresh;
    hdiMode.id = mId;
}

DrmConnector::DrmConnector(drmModeConnector c, FdPtr &fd)
    : mId(c.connector_id),
      mPhyWidth(c.mmWidth),
      mPhyHeight(c.mmHeight),
      mEncoderId(c.encoder_id),
      mConnectState(c.connection),
      mDrmFdPtr(fd)
{
    DISPLAY_LOGD("encoder_id %{public}d", mEncoderId);
    DISPLAY_LOGD("the connect state is %{public}d", mConnectState);

    for (int i = 0; i < c.count_encoders; i++) {
        mPossibleEncoders.push_back(c.encoders[i]);
        DISPLAY_LOGD("add possible encoder id %{public}d", c.encoders[i]);
    }

    ConvertToHdiType(c.connector_type, mType);
    ConvertTypeToName(mType, mName);
    InitModes(c);
    DISPLAY_LOGD("name %{public}s", mName.c_str());
}

void DrmConnector::InitModes(drmModeConnector c)
{
    DISPLAY_LOGD("id %{public}d  mode size %{public}d", mId, c.count_modes);
    mModes.clear();
    mPreferenceId = INVALID_MODE_ID;
    for (int i = 0; i < c.count_modes; i++) {
        drmModeModeInfoPtr mode = c.modes + i;
        DISPLAY_LOGD("mode: hdisplay %{public}d, vdisplay %{public}d vrefresh %{public}d type %{public}d",
            mode->hdisplay, mode->vdisplay, mode->vrefresh, mode->type);
        if ((mPreferenceId == INVALID_MODE_ID) && (mode->type & DRM_MODE_TYPE_PREFERRED)) {
            DISPLAY_LOGD("set it to prefernce id %{public}d", i);
            mPreferenceId = i;
        }
        mModes.emplace(i, DrmMode { *mode, i });
    }
    DISPLAY_LOGD("mode count %{public}zd", mModes.size());
}

int32_t DrmConnector::Init(DrmDevice &drmDevice)
{
    int32_t ret;
    DrmProperty prop;
    DISPLAY_LOGD();
    DISPLAY_CHK_RETURN((mDrmFdPtr == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("the mDrmFdPtr is nullptr"));
    DISPLAY_CHK_RETURN((mDrmFdPtr->GetFd() == -1), DISPLAY_FAILURE, DISPLAY_LOGE("the drm fd is -1"));
    // find dpms prop
    ret = drmDevice.GetConnectorProperty(*this, PROP_DPMS, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("can not get mode prop id"));
    mPropDpmsId = prop.propId;
    mDpmsState = prop.value;
    DISPLAY_LOGD("dpms state : %{public}" PRIu64 "", mDpmsState);
    // find the crtcid
    ret = drmDevice.GetConnectorProperty(*this, PROP_CRTCID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get out fence prop id"));
    mPropCrtcId = prop.propId;
    DISPLAY_LOGD("encoder_id %{public}d", mEncoderId);
    DISPLAY_LOGD("mPropCrtcId %{public}d", mPropCrtcId);

    return DISPLAY_SUCCESS;
}

int32_t DrmConnector::GetBrightness(uint32_t &level)
{
    if (!mBrightnessSet) {
        /* До первой установки — текущая яркость панели, пересчитанная в 0..255. */
        int32_t maxValue = BacklightMax();
        int32_t cur = ReadSysfsInt(OneirBacklightPath());
        if (maxValue > 0 && cur >= 0) {
            mBrightnessLevel = static_cast<uint32_t>(
                (static_cast<int64_t>(cur) * OHOS_BRIGHTNESS_MAX + maxValue / 2) / maxValue);
        }
    }
    level = mBrightnessLevel;
    return DISPLAY_SUCCESS;
}

int32_t DrmConnector::SetBrightness(uint32_t level)
{
    static int32_t blFd = -1;
    if (blFd < 0) {
        blFd = open(OneirBacklightPath(), O_WRONLY | O_CLOEXEC);
        if (blFd < 0) {
            DISPLAY_LOGE("SetBrightness: open %{public}s failed: %{public}s", OneirBacklightPath(), strerror(errno));
            return DISPLAY_FAILURE;
        }
    }

    uint32_t clamped = std::min(level, OHOS_BRIGHTNESS_MAX);
    int32_t maxValue = BacklightMax();
    int64_t hw = clamped;                       /* шкала панели неизвестна — пишем как есть */
    if (maxValue > 0) {
        hw = (static_cast<int64_t>(clamped) * maxValue + OHOS_BRIGHTNESS_MAX / 2) / OHOS_BRIGHTNESS_MAX;
        if (clamped > 0 && hw == 0) {
            hw = 1;                             /* ненулевой уровень не должен гасить подсветку */
        }
    }

    char buffer[24] = {0};
    int bytes = sprintf_s(buffer, sizeof(buffer), "%lld", static_cast<long long>(hw));
    /* pwrite с нулевым смещением: файл открыт один раз, атрибут sysfs пишется целиком. */
    if (bytes < 0 || pwrite(blFd, buffer, bytes, 0) < 0) {
        DISPLAY_LOGE("SetBrightness: write %{public}s failed: %{public}s", buffer, strerror(errno));
        return DISPLAY_FAILURE;
    }

    /* Возвращаем ровно установленный уровень: «установил X — прочитал X». */
    mBrightnessLevel = level;
    mBrightnessSet = true;
    DISPLAY_LOGD("SetBrightness: level %{public}u -> %{public}lld of %{public}d", level,
        static_cast<long long>(hw), maxValue);
    return DISPLAY_SUCCESS;
}

void DrmConnector::GetDisplayCap(DisplayCapability &cap)
{
    cap.phyHeight = mPhyHeight;
    cap.phyWidth = mPhyWidth;
    cap.type = mType;
    cap.name = mName;
    cap.supportLayers = mSupportLayers;
    cap.virtualDispCount = mVirtualDispCount;
    cap.supportWriteBack = mSupportWriteBack;
    cap.propertyCount = mPropertyCount;
}

void DrmConnector::ConvertTypeToName(uint32_t type, std::string &name)
{
    DISPLAY_LOGD("type %{public}d", type);
    switch (type) {
        case DISP_INTF_VGA:
            name = "VGA";
            break;
        case DISP_INTF_HDMI:
            name = "HDMI";
            break;
        case DISP_INTF_MIPI:
            name = "MIPI";
            break;
        case DISP_INTF_PANEL:
            name = "Panel";
            break;
        default:
            name = "Unknown";
            break;
    }
}

void DrmConnector::ConvertToHdiType(uint32_t type, InterfaceType &hdiType)
{
    switch (type) {
        case DRM_MODE_CONNECTOR_VGA:
            hdiType = DISP_INTF_VGA;
            break;
        case DRM_MODE_CONNECTOR_DSI:
            hdiType = DISP_INTF_MIPI;
            break;
        case DRM_MODE_CONNECTOR_HDMIA:
        case DRM_MODE_CONNECTOR_HDMIB:
            hdiType = DISP_INTF_HDMI;
            break;
        /* oneir: встроенные панели ноутбуков (eDP, LVDS) — тоже «панель». */
        case DRM_MODE_CONNECTOR_eDP:
        case DRM_MODE_CONNECTOR_LVDS:
        case DRM_MODE_CONNECTOR_VIRTUAL:
            hdiType = DISP_INTF_PANEL;
            break;
        default:
            hdiType = DISP_INTF_BUTT;
            break;
    }
}
int32_t DrmConnector::TryPickEncoder(IdMapPtr<DrmEncoder> &encoders, uint32_t encoderId, IdMapPtr<DrmCrtc> &crtcs,
    uint32_t &crtcId)
{
    int ret;
    auto encoderIter = encoders.find(encoderId);
    if (encoderIter == encoders.end()) {
        DISPLAY_LOGW("can not find encoder for id : %{public}d", encoderId);
        return DISPLAY_FAILURE;
    }

    auto &encoder = encoderIter->second;
    DISPLAY_LOGD("connector : %{public}d encoder : %{public}d", mId, encoder->GetId());
    ret = encoder->PickIdleCrtcId(crtcs, crtcId);
    DISPLAY_CHK_RETURN((ret == DISPLAY_SUCCESS), DISPLAY_SUCCESS,
        DISPLAY_LOGD("connector : %{public}d pick encoder : %{public}d", mId, encoder->GetId()));
    return DISPLAY_FAILURE;
}

int32_t DrmConnector::PickIdleCrtcId(IdMapPtr<DrmEncoder> &encoders, IdMapPtr<DrmCrtc> &crtcs, uint32_t &crtcId)
{
    DISPLAY_LOGD();
    DISPLAY_LOGD("encoder_id %{public}d", mEncoderId);
    int ret = TryPickEncoder(encoders, mEncoderId, crtcs, crtcId);
    DISPLAY_CHK_RETURN((ret == DISPLAY_SUCCESS), DISPLAY_SUCCESS,
        DISPLAY_LOGD("connector : %{public}d pick endcoder : %{public}d crtcId : %{public}d",
        mId, mEncoderId, crtcId));

    for (auto encoder : mPossibleEncoders) {
        ret = TryPickEncoder(encoders, encoder, crtcs, crtcId);
        DISPLAY_CHK_RETURN((ret == DISPLAY_SUCCESS), DISPLAY_SUCCESS,
            DISPLAY_LOGD("connector : %{public}d pick endcoder : %{public}d crtcId : %{public}d", mId, mEncoderId,
            crtcId));
    }

    DISPLAY_LOGW("can not pick a crtc for connector");
    return DISPLAY_FAILURE;
}

int32_t DrmConnector::UpdateModes()
{
    int drmFd = mDrmFdPtr->GetFd();
    drmModeConnectorPtr c = drmModeGetConnector(drmFd, mId);
    DISPLAY_CHK_RETURN((c == nullptr), DISPLAY_FAILURE, DISPLAY_LOGE("can not get connector"));
    mConnectState = c->connection;
    // init the modes
    InitModes(*c);
    drmModeFreeConnector(c);
    return DISPLAY_SUCCESS;
}

std::shared_ptr<DrmCrtc> DrmConnector::UpdateCrtcId(IdMapPtr<DrmEncoder> &encoders,
    IdMapPtr<DrmCrtc> &crtcs, bool plugIn, drmModeConnectorPtr c, int *crtc_id)
{
    std::shared_ptr<DrmCrtc> crtc = nullptr;
    int  encoderid = c->encoders[0];
    auto encoderIter = encoders.find(encoderid);
    if (encoderIter == encoders.end()) {
        DISPLAY_LOGW("can not find encoder for id : %{public}d", encoderid);
        return crtc;
    }

    auto &encoder = encoderIter->second;
    int possibleCrtcs = encoder->GetPossibleCrtcs();

    for (auto crtcIter = crtcs.begin(); crtcIter != crtcs.end(); ++crtcIter) {
        auto &posCrts = crtcIter->second;
        if (possibleCrtcs == (1<<posCrts->GetPipe())) {
            DISPLAY_LOGD("find crtc id %{public}d, pipe %{public}d", posCrts->GetId(), posCrts->GetPipe());
            crtc = posCrts;
            *crtc_id = posCrts->GetId();
        }
    }
    if (plugIn) {
        encoder->SetCrtcId(*crtc_id);
        mEncoderId = c->encoders[0];
    } else if (!plugIn) {
        *crtc_id = 0;
        mEncoderId = 0;
        encoder->SetCrtcId(0);
    }
    return crtc;
}

bool DrmConnector::HandleHotplug(IdMapPtr<DrmEncoder> &encoders,
    IdMapPtr<DrmCrtc> &crtcs, bool plugIn)
{
    DISPLAY_LOGD("plug %{public}d", plugIn);
    int drmFd = mDrmFdPtr->GetFd();
    int ret;
    int crtc_id = 0;
    std::shared_ptr<DrmCrtc> crtc;
    uint32_t blob_id;
    drmModeAtomicReq *pset = drmModeAtomicAlloc();
    DISPLAY_CHK_RETURN((pset == nullptr), DISPLAY_NULL_PTR,
        DISPLAY_LOGE("drm atomic alloc failed errno %{public}d", errno));

    drmModeConnectorPtr c = drmModeGetConnector(drmFd, mId);
    DISPLAY_CHK_RETURN((c == nullptr), false, DISPLAY_LOGE("can not get connector"));
    if (mConnectState == c->connection) {
        drmModeFreeConnector(c);
        return false;
    } else {
        crtc = UpdateCrtcId(encoders, crtcs, plugIn, c, &crtc_id);
        if (crtc == nullptr) {
            return DISPLAY_FAILURE;
        }
        DISPLAY_LOGD("get crtc id %{public}d ", crtc_id);

        DrmVsyncWorker::GetInstance().EnableVsync(plugIn);
        drmModeCreatePropertyBlob(drmFd, &c->modes[0],
            sizeof(c->modes[0]), &blob_id);
        ret = drmModeAtomicAddProperty(pset, crtc->GetId(), crtc->GetActivePropId(), (int)plugIn);
        ret |= drmModeAtomicAddProperty(pset, crtc->GetId(), crtc->GetModePropId(), blob_id);
        ret |= drmModeAtomicAddProperty(pset, GetId(), GetPropCrtcId(), crtc_id);
        DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE,
            DISPLAY_LOGE("can not add the crtc id prop %{public}d", errno));

        ret = drmModeAtomicCommit(drmFd, pset, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
        DISPLAY_CHK_RETURN((ret < 0), DISPLAY_FAILURE,
            DISPLAY_LOGE("can not add the crtc id prop %{public}d", errno));
        drmModeAtomicFree(pset);

        mConnectState = c->connection;
        InitModes(*c);
        drmModeFreeConnector(c);
        return true;
    }
}

int32_t DrmConnector::GetDisplaySupportedModes(uint32_t *num, DisplayModeInfo *modes)
{
    DISPLAY_CHK_RETURN((num == nullptr), DISPLAY_NULL_PTR, DISPLAY_LOGE("num is nullptr"));
    *num = static_cast<int32_t>(mModes.size());
    if (modes != nullptr) {
        int i = 0;
        for (const auto &modeMap : mModes) {
            DrmMode mode = modeMap.second;
            mode.ConvertToHdiMode(*(modes + i));
            i++;
        }
    }
    return DISPLAY_SUCCESS;
}

int32_t DrmConnector::SetDpmsState(uint64_t dmps)
{
    DISPLAY_LOGD("dmps %{public}" PRIu64 "", dmps);
    int ret = drmModeConnectorSetProperty(mDrmFdPtr->GetFd(), mId, mPropDpmsId, dmps);
    DISPLAY_CHK_RETURN((ret != 0), DISPLAY_FAILURE, DISPLAY_LOGE("can not set dpms"));
    mDpmsState = dmps;
    return DISPLAY_SUCCESS;
}

bool DrmConnector::IsConnected()
{
    return (mConnectState == DRM_MODE_CONNECTED);
}

int32_t DrmConnector::GetModeFromId(int32_t id, DrmMode &mode)
{
    DISPLAY_LOGD();
    auto iter = mModes.find(id);
    if (iter == mModes.end()) {
        return DISPLAY_FAILURE;
    }
    mode = mModes[id];
    return DISPLAY_SUCCESS;
}

std::unique_ptr<DrmModeBlock> DrmConnector::GetModeBlockFromId(int32_t id)
{
    DISPLAY_LOGD("id %{public}d", id);
    auto iter = mModes.find(id);
    DISPLAY_CHK_RETURN((iter == mModes.end()), nullptr, DISPLAY_LOGE("can not the mode %{public}d", id));
    return std::make_unique<DrmModeBlock>(mModes[id]);
}

DrmModeBlock::DrmModeBlock(DrmMode &mode)
{
    DISPLAY_LOGD();
    Init(mode);
}

int32_t DrmModeBlock::Init(DrmMode &mode)
{
    int ret;
    int drmFd = DrmDevice::GetDrmFd();
    DISPLAY_CHK_RETURN((drmFd < 0), DISPLAY_FAILURE, DISPLAY_LOGE("the drm fd is invalid"));
    drmModeModeInfo modeInfo = *(mode.GetModeInfoPtr());
    ret = drmModeCreatePropertyBlob(drmFd, static_cast<void *>(&modeInfo), sizeof(modeInfo), &mBlockId);
    DISPLAY_CHK_RETURN((ret != 0), DISPLAY_FAILURE, DISPLAY_LOGE("create property blob failed"));
    DISPLAY_LOGD("mBlockId %{public}d", mBlockId);
    return DISPLAY_SUCCESS;
}

DrmModeBlock::~DrmModeBlock()
{
    DISPLAY_LOGD("mBlockId %{public}d", mBlockId);
    int drmFd = DrmDevice::GetDrmFd();
    if ((mBlockId != DRM_INVALID_ID) && (drmFd >= 0)) {
        int ret = drmModeDestroyPropertyBlob(drmFd, mBlockId);
        if (ret != 0) {
            DISPLAY_LOGE("destroy property blob failed errno %{public}d", errno);
        }
    } else {
        DISPLAY_LOGE("can not destruct the block id %{public}d drmFd %{public}d", mBlockId, drmFd);
    }
}
} // namespace OHOS
} // namespace HDI
} // namespace DISPLAY
