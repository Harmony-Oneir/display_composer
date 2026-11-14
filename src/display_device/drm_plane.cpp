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

#include "drm_plane.h"
#include "drm_device.h"
#include <algorithm>
#include <cstring>

namespace OHOS {
namespace HDI {
namespace DISPLAY {

DrmPlane::DrmPlane(drmModePlane &p)
    : mId(p.plane_id), mPossibleCrtcs(p.possible_crtcs), mCrtcId(p.crtc_id),
    mFormats(p.formats, p.formats + p.count_formats)
{}

DrmPlane::~DrmPlane()
{
    DISPLAY_LOGD();
}

int DrmPlane::GetCrtcProp(DrmDevice &drmDevice)
{
    int32_t ret;
    int32_t crtc_x, crtc_y, crtc_w, crtc_h;
    DrmProperty prop;

    ret = drmDevice.GetPlaneProperty(*this, PROP_CRTC_X_ID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane crtc_x prop id"));
    mPropCrtc_xId = prop.propId;
    crtc_x = prop.value;

    ret = drmDevice.GetPlaneProperty(*this, PROP_CRTC_Y_ID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane crtc_y prop id"));
    mPropCrtc_yId = prop.propId;
    crtc_y = prop.value;

    ret = drmDevice.GetPlaneProperty(*this, PROP_CRTC_W_ID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane crtc_w prop id"));
    mPropCrtc_wId = prop.propId;
    crtc_w = prop.value;

    ret = drmDevice.GetPlaneProperty(*this, PROP_CRTC_H_ID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane crtc_h prop id"));
    mPropCrtc_hId = prop.propId;
    crtc_h = prop.value;

    DISPLAY_LOGE("plane %{public}d crtc_x %{public}d crtc_y %{public}d crtc_w %{public}d crtc_h %{public}d",
        GetId(), crtc_x, crtc_y, crtc_w, crtc_h);

    return 0;
}

int  DrmPlane::GetSrcProp(DrmDevice &drmDevice)
{
    int32_t ret;
    int32_t src_x, src_y, src_w, src_h;
    DrmProperty prop;

    ret = drmDevice.GetPlaneProperty(*this, PROP_SRC_X_ID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane src_x prop id"));
    mPropSrc_xId = prop.propId;
    src_x = prop.value;

    ret = drmDevice.GetPlaneProperty(*this, PROP_SRC_Y_ID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane src_y prop id"));
    mPropSrc_yId = prop.propId;
    src_y = prop.value;

    ret = drmDevice.GetPlaneProperty(*this, PROP_SRC_W_ID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane src_w prop id"));
    mPropSrc_wId = prop.propId;
    src_w = prop.value;

    ret = drmDevice.GetPlaneProperty(*this, PROP_SRC_H_ID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane src_h prop id"));
    mPropSrc_hId = prop.propId;
    src_h = prop.value;

    DISPLAY_LOGE("plane %{public}d src_x %{public}d src_y %{public}d src_w %{public}d src_h %{public}d",
        GetId(), src_x, src_y, src_w, src_h);

    return 0;
}

int32_t DrmPlane::Init(DrmDevice &drmDevice)
{
    DISPLAY_LOGD();
    int32_t ret;

    DrmProperty prop;
    GetCrtcProp(drmDevice);
    GetSrcProp(drmDevice);
    ret = drmDevice.GetPlaneProperty(*this, PROP_FBID, prop);
    mPropFbId = prop.propId;
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("can not get plane fb id"));
    ret = drmDevice.GetPlaneProperty(*this, PROP_IN_FENCE_FD, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get plane in fence prop id"));
    mPropFenceInId = prop.propId;
    ret = drmDevice.GetPlaneProperty(*this, PROP_CRTC_ID, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane crtc prop id"));
    mPropCrtcId = prop.propId;

    ret = drmDevice.GetPlaneProperty(*this, PROP_TYPE, prop);
    DISPLAY_CHK_RETURN((ret != DISPLAY_SUCCESS), DISPLAY_FAILURE, DISPLAY_LOGE("cat not get pane crtc prop id"));
    switch (prop.value) {
        case DRM_PLANE_TYPE_OVERLAY:
        case DRM_PLANE_TYPE_PRIMARY:
        case DRM_PLANE_TYPE_CURSOR:
            mType = static_cast<uint32_t>(prop.value);
            break;
        default:
            DISPLAY_LOGE("unknown type value %{public}" PRIu64 "", prop.value);
            return DISPLAY_FAILURE;
    }
    ReadOptionalProps(drmDevice);
    return DISPLAY_SUCCESS;
}

// oneir: hwc — свойства, которых может не быть на другом оборудовании; их отсутствие не ошибка
void DrmPlane::ReadOptionalProps(DrmDevice &drmDevice)
{
    DrmProperty prop;
    if (drmDevice.GetPlaneProperty(*this, PROP_ZPOS, prop) == DISPLAY_SUCCESS) {
        mPropZposId = prop.propId;
        mZpos = prop.value;
        mZposImmutable = (prop.flags & DRM_MODE_PROP_IMMUTABLE) != 0;
    }
    if (drmDevice.GetPlaneProperty(*this, PROP_ALPHA, prop) == DISPLAY_SUCCESS) {
        mPropAlphaId = prop.propId;
    }
    if (drmDevice.GetPlaneProperty(*this, PROP_ROTATION, prop) == DISPLAY_SUCCESS) {
        mPropRotationId = prop.propId;
    }
    if (drmDevice.GetPlaneProperty(*this, PROP_PIXEL_BLEND_MODE, prop) == DISPLAY_SUCCESS) {
        mPropBlendModeId = prop.propId;
        drmModePropertyPtr p = drmModeGetProperty(DrmDevice::GetDrmFd(), prop.propId);
        if (p != nullptr) {
            for (int i = 0; i < p->count_enums; i++) {
                mBlendModes.emplace_back(p->enums[i].name, p->enums[i].value);
            }
            drmModeFreeProperty(p);
        }
    }
    DISPLAY_LOGI("plane %{public}u type %{public}u formats %{public}zu zpos %{public}" PRIu64 "%{public}s "
        "alpha %{public}d rotation %{public}d blend %{public}d", mId, mType, mFormats.size(), mZpos,
        mZposImmutable ? " (fixed)" : "", mPropAlphaId != 0, mPropRotationId != 0, mPropBlendModeId != 0);
}

bool DrmPlane::SupportsFormat(uint32_t fourcc) const
{
    return std::find(mFormats.begin(), mFormats.end(), fourcc) != mFormats.end();
}

int64_t DrmPlane::GetBlendModeValue(const std::string &name) const
{
    for (const auto &m : mBlendModes) {
        if (m.first == name) {
            return static_cast<int64_t>(m.second);
        }
    }
    return -1;
}
} // namespace OHOS
} // namespace HDI
} // namespace DISPLAY
