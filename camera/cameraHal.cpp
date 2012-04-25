/*
 *
 * Copyright (C) 2012, rondoval (ms2), Epsylon3 (defy)
 * Copyright (C) 2012, Won-Kyu Park
 * Copyright (C) 2012, Raviprasad V Mummidi
 * Copyright (C) 2011, Ivan Zupan
 * Copyright (C) 2012, JB1tz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "CameraHAL"
//#define LOG_NDEBUG 0
#define LOG_FULL_PARAMS

//#define STORE_METADATA_IN_BUFFER

#include <hardware/camera.h>
#include <ui/Overlay.h>
#include <binder/IMemory.h>
#include <hardware/gralloc.h>
#include <utils/Errors.h>
#include <vector>
#include <ctype.h>

#define CAMHAL_GRALLOC_USAGE GRALLOC_USAGE_HW_TEXTURE | \
			     GRALLOC_USAGE_HW_RENDER | \
			     GRALLOC_USAGE_SW_READ_RARELY | \
			     GRALLOC_USAGE_SW_WRITE_NEVER

using namespace std;

#include "CameraHardwareInterface.h"

/* Prototypes and extern functions. */
extern "C" android::sp<android::CameraHardwareInterface> HAL_openCameraHardware(int cameraId);
extern "C" int HAL_getNumberOfCameras();
extern "C" void HAL_getCameraInfo(int cameraId, struct CameraInfo* cameraInfo);

namespace android {
     int camera_device_open(const hw_module_t* module, const char* name, hw_device_t** device);
     int CameraHAL_GetCam_Info(int camera_id, struct camera_info *info);
}

static hw_module_methods_t camera_module_methods = {
    open: android::camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 1,
        id: CAMERA_HARDWARE_MODULE_ID,
        name: "Camera HAL for ICS/CM9",
        author: "Won-Kyu Park, Raviprasad V Mummidi, Ivan Zupan, Epsylon3, rondoval",
        methods: &camera_module_methods,
        dso: NULL,
        reserved: {0},
    },
    get_number_of_cameras: android::HAL_getNumberOfCameras,
    get_camera_info: android::CameraHAL_GetCam_Info,
};


namespace android {

int camera_set_preview_window(struct camera_device *device, struct preview_stream_ops *window);

struct legacy_camera_device {
    camera_device_t device;
    int id;

    /* New world */
    camera_notify_callback         notify_callback;
    camera_data_callback           data_callback;
    camera_data_timestamp_callback data_timestamp_callback;
    camera_request_memory          request_memory;
    void                          *user;
    preview_stream_ops            *window;

    /* Old world */
    sp<CameraHardwareInterface>    hwif;
    gralloc_module_t const        *gralloc;
    sp<Overlay>                    overlay;

    int32_t                        previewWidth;
    int32_t                        previewHeight;
};

static inline void log_camera_params(const char* name,
                                     const CameraParameters params)
{
#ifdef LOG_FULL_PARAMS
    params.dump();
#endif
}

void Yuv422iToRgb565 (char* rgb, char* yuv422i, int width, int height)
{
    int yuv_index = 0;
    int rgb_index = 0;
    int j, i, y1192;
    int y1, u, y2, v;
    int r, g, b;

    for (j = 0; j < height; j++) {
        for (i = 0; i < width / 2; i++) {

            y1 = (0xff & yuv422i[yuv_index++]) - 16;
            u  = (0xff & yuv422i[yuv_index++]) - 128;
            y2 = (0xff & yuv422i[yuv_index++]) - 16;
            v  = (0xff & yuv422i[yuv_index++]) - 128;

            if (y1 < 0) y1 = 0;
            if (y2 < 0) y2 = 0;

            y1192 = 1192 * y1;
            r = (y1192 + 1634 * v);
            g = (y1192 - 833 * v - 400 * u);
            b = (y1192 + 2066 * u);

            if (r < 0) r = 0; else if (r > 262143) r = 262143;
            if (g < 0) g = 0; else if (g > 262143) g = 262143;
            if (b < 0) b = 0; else if (b > 262143) b = 262143;

            /* for RGB565 */
            r = (r >> 13) & 0x1f;
            g = (g >> 12) & 0x3f;
            b = (b >> 13) & 0x1f;

            rgb[rgb_index++] = g << 5 | b;
            rgb[rgb_index++] = r << 3 | g >> 3;

            y1192 = 1192 * y2;
            r = (y1192 + 1634 * v);
            g = (y1192 - 833 * v - 400 * u);
            b = (y1192 + 2066 * u);

            if (r < 0) r = 0; else if (r > 262143) r = 262143;
            if (g < 0) g = 0; else if (g > 262143) g = 262143;
            if (b < 0) b = 0; else if (b > 262143) b = 262143;

            /* for RGB565 */
            r = (r >> 13) & 0x1f;
            g = (g >> 12) & 0x3f;
            b = (b >> 13) & 0x1f;

            rgb[rgb_index++] = g << 5 | b;
            rgb[rgb_index++] = r << 3 | g >> 3;
        }
    }
}

void CameraHAL_ProcessPreviewData(char *frame, size_t size,
                                  legacy_camera_device *lcdev)
{
    int32_t stride;
    buffer_handle_t *bufHandle = NULL;
    void *vaddr;

    if (lcdev->window == NULL)
        return;

    if (lcdev->window->dequeue_buffer(lcdev->window, &bufHandle,
                                      &stride) != NO_ERROR) {
        LOGE("%s: ERROR dequeueing the buffer\n", __FUNCTION__);
        return;
    }

    if (lcdev->window->lock_buffer(lcdev->window, bufHandle) != NO_ERROR) {
        LOGE("%s: ERROR locking the buffer\n", __FUNCTION__);
        lcdev->window->cancel_buffer(lcdev->window, bufHandle);
        return;
    }

    if (lcdev->gralloc->lock(lcdev->gralloc, *bufHandle, CAMHAL_GRALLOC_USAGE,
                             0, 0, lcdev->previewWidth, lcdev->previewHeight,
                             &vaddr) != NO_ERROR) {
        return;
    }

    /* The data we get is in YUV... but Window is RGBB565. It needs to be converted */
    Yuv422iToRgb565((char*)vaddr, frame, lcdev->previewWidth, lcdev->previewHeight);
    lcdev->gralloc->unlock(lcdev->gralloc, *bufHandle);

    if (lcdev->window->enqueue_buffer(lcdev->window, bufHandle) != NO_ERROR) {
        LOGE("%s: could not enqueue gralloc buffer", __FUNCTION__);
        return;
    }
}

/* Overlay hooks */
void queue_buffer_hook(void *data, void *buffer, size_t size)
{
    if (data != NULL && buffer != NULL) {
        CameraHAL_ProcessPreviewData((char*)buffer, size, (legacy_camera_device*) data);
    }
}

camera_memory_t* CameraHAL_GenClientData(const sp<IMemory> &dataPtr,
                                         legacy_camera_device *lcdev)
{
    ssize_t offset;
    size_t size;
    void *data;
    camera_memory_t *clientData = NULL;
    sp<IMemoryHeap> mHeap;

    if (!lcdev->request_memory)
        return NULL;

    mHeap = dataPtr->getMemory(&offset, &size);
    data = (void *)((char *)(mHeap->base()) + offset);

    clientData = lcdev->request_memory(-1, size, 1, lcdev->user);
    memcpy(clientData->data, data, size);

    return clientData;
}

void CameraHAL_DataCb(int32_t msg_type, const sp<IMemory>& dataPtr,
                      void *user)
{
    legacy_camera_device *lcdev = NULL;
    camera_memory_t *mem = NULL;

    if (!user)
        return;

    if (msg_type ==CAMERA_MSG_RAW_IMAGE) {
        lcdev->hwif->disableMsgType(CAMERA_MSG_RAW_IMAGE);
        return;
    }

    lcdev = (legacy_camera_device *) user;
    mem = CameraHAL_GenClientData(dataPtr, lcdev);

    if (lcdev->data_callback)
        lcdev->data_callback(msg_type, mem, 0, NULL, lcdev->user);
}

void CameraHAL_DataTSCb(nsecs_t timestamp, int32_t msg_type,
                         const sp<IMemory>& dataPtr, void *user)
{
    legacy_camera_device *lcdev = NULL;
    camera_memory_t *mem = NULL;

    if (!user)
        return;

    lcdev = (legacy_camera_device *) user;
    mem = CameraHAL_GenClientData(dataPtr, lcdev);

    if (lcdev->data_timestamp_callback)
        lcdev->data_timestamp_callback(timestamp, msg_type, mem, 0, lcdev->user);

    lcdev->hwif->releaseRecordingFrame(dataPtr);

    if (mem)
        mem->release(mem);

}

/* HAL helper functions. */
void CameraHAL_NotifyCb(int32_t msg_type, int32_t ext1, int32_t ext2, void *user)
{
    legacy_camera_device *lcdev = NULL;

    if (!user)
        return;

    lcdev = (legacy_camera_device *) user;

    if (lcdev->notify_callback)
        lcdev->notify_callback(msg_type, ext1, ext2, lcdev->user);
}

int CameraHAL_GetCam_Info(int camera_id, struct camera_info *info)
{
    int rv = 0;
    LOGV("CameraHAL_GetCam_Info()");

    CameraInfo cam_info;
    HAL_getCameraInfo(camera_id, &cam_info);

    info->facing = cam_info.facing;
    info->orientation = 90;

    LOGD("%s: id:%i faceing:%i orientation: %i", __FUNCTION__,
          camera_id, info->facing, info->orientation);

    return rv;
}

void CameraHAL_FixupParams(struct camera_device *device,
                           CameraParameters &settings)
{
    settings.set(CameraParameters::KEY_VIDEO_FRAME_FORMAT,
                 CameraParameters::PIXEL_FORMAT_YUV422I);

    settings.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS,
                 CameraParameters::PIXEL_FORMAT_YUV422I);

    settings.setPreviewFormat(CameraParameters::PIXEL_FORMAT_YUV422I);

    settings.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, "640x480");

    if (!settings.get("preview-size-values"))
        settings.set("preview-size-values", "176x144,320x240,352x288,480x360,640x480,848x480");

    if (!settings.get("picture-size-values"))
        settings.set("picture-size-values", "320x240,640x480,1280x960,1600x1200,2048x1536,2592x1456,2592x1936");

    if (!settings.get("mot-video-size-values"))
        settings.set("mot-video-size-values", "176x144,320x240,352x288,640x480,848x480");

    settings.set(android::CameraParameters::KEY_FOCUS_MODE, "auto");

    /* defy: focus locks the camera, but dunno how to disable it... */
    if (!settings.get(android::CameraParameters::KEY_SUPPORTED_FOCUS_MODES))
        settings.set(android::CameraParameters::KEY_SUPPORTED_FOCUS_MODES, "auto,macro,fixed,infinity,off");

    if (!settings.get(android::CameraParameters::KEY_SUPPORTED_EFFECTS))
        settings.set(android::CameraParameters::KEY_SUPPORTED_EFFECTS, "none,mono,sepia,negative,solarize,red-tint,green-tint,blue-tint");

    if (!settings.get(android::CameraParameters::KEY_SUPPORTED_SCENE_MODES))
        settings.set(android::CameraParameters::KEY_SUPPORTED_SCENE_MODES,
                     "auto,portrait,landscape,action,night-portrait,sunset,steadyphoto");

    if (!settings.get(android::CameraParameters::KEY_EXPOSURE_COMPENSATION))
        settings.set(android::CameraParameters::KEY_EXPOSURE_COMPENSATION, "0");

    if (!settings.get("mot-max-areas-to-focus"))
        settings.set("mot-max-areas-to-focus", "1");
    if (!settings.get("mot-areas-to-focus"))
        settings.set("mot-areas-to-focus", "0");

    settings.set("zoom-ratios", "100,200,300,400,500,600");

    settings.set("max-zoom", "4");

    /* ISO */
    settings.set("iso", "auto");
    char *moto_iso_values = strdup(settings.get("mot-picture-iso-values"));
    char iso_values[256];
    memset(iso_values, '\0', sizeof(iso_values));
    if ((!settings.get("iso-values") && moto_iso_values)) {
        char *iso = strtok(moto_iso_values, ",");
        while (iso != NULL) {
            strcat(iso_values, ",");
            if (isdigit(iso[0]))
                strcat(iso_values, "ISO");

            strcat(iso_values, iso);
            iso = strtok(NULL, " ,");
        }
    }
    settings.set("iso-values", iso_values);
    free(moto_iso_values);

    /* defy: required to prevent panorama crash, but require also opengl ui */
    const char *fps_range_values = "(1000,30000),(1000,25000),(1000,20000),"
                                   "(1000,24000),(1000,15000),(1000,10000)";
    if (!settings.get(android::CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE))
        settings.set(android::CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE, fps_range_values);

    const char *preview_fps_range = "1000,30000";
    if (!settings.get(android::CameraParameters::KEY_PREVIEW_FPS_RANGE))
        settings.set(android::CameraParameters::KEY_PREVIEW_FPS_RANGE, preview_fps_range);

    LOGD("Parameters fixed up");
}

/* Hardware Camera interface handlers. */
int camera_set_preview_window(struct camera_device *device,
                              struct preview_stream_ops *window)
{
    int rv = -EINVAL;
    int min_bufs = -1;
    const int kBufferCount = 4;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    lcdev->window = window;

    if (!window) {
        LOGV("%s: window is NULL", __FUNCTION__);
        return NO_ERROR;
    }

    if (!lcdev->gralloc) {
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                          (const hw_module_t **)&(lcdev->gralloc))) {
            LOGE("%s: Fail on loading gralloc HAL", __FUNCTION__);
        }
    }

    if (window->get_min_undequeued_buffer_count(window, &min_bufs)) {
        LOGE("%s: could not retrieve min undequeued buffer count", __FUNCTION__);
        return -1;
    }

    if (min_bufs >= kBufferCount) {
        LOGE("%s: min undequeued buffer count %i is too high"
             " (expecting at most %i)", __FUNCTION__, min_bufs, kBufferCount - 1);
    }

    LOGD("%s: setting buffer count to %i", __FUNCTION__, kBufferCount);
    if (window->set_buffer_count(window, kBufferCount)) {
        LOGE("%s: could not set buffer count", __FUNCTION__);
        return -1;
    }

    CameraParameters params(lcdev->hwif->getParameters());
    params.getPreviewSize(&lcdev->previewWidth, &lcdev->previewHeight);
    const char *str_preview_format = params.getPreviewFormat();

    if (window->set_usage(window, CAMHAL_GRALLOC_USAGE)) {
        LOGE("%s: could not set usage on gralloc buffer", __FUNCTION__);
        return -1;
    }

    if (window->set_buffers_geometry(window,
                                     lcdev->previewWidth,
                                     lcdev->previewHeight,
                                     HAL_PIXEL_FORMAT_RGB_565)) {
        LOGE("%s: could not set buffers geometry", __FUNCTION__);
        return -1;
    }

    lcdev->overlay = new Overlay(lcdev->previewWidth,
                                 lcdev->previewHeight,
                                 OVERLAY_FORMAT_YUV422I,
                                 queue_buffer_hook,
                                 (void *) lcdev);
    lcdev->hwif->setOverlay(lcdev->overlay);

    return NO_ERROR;
}

void camera_set_callbacks(struct camera_device *device,
                             camera_notify_callback notify_cb,
                             camera_data_callback data_cb,
                             camera_data_timestamp_callback data_cb_timestamp,
                             camera_request_memory get_memory,
                             void *user)
{
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return;

    lcdev = (legacy_camera_device*) device;
    lcdev->notify_callback = notify_cb;
    lcdev->data_callback = data_cb;
    lcdev->data_timestamp_callback = data_cb_timestamp;
    lcdev->request_memory = get_memory;
    lcdev->user = user;
    lcdev->hwif->setCallbacks(CameraHAL_NotifyCb,
                              CameraHAL_DataCb,
                              CameraHAL_DataTSCb,
                              (void *) lcdev);
}

void camera_enable_msg_type(struct camera_device *device, int32_t msg_type)
{
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return;

    lcdev = (legacy_camera_device*) device;
    lcdev->hwif->enableMsgType(msg_type);
}

void camera_disable_msg_type(struct camera_device *device, int32_t msg_type)
{
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return;

    lcdev = (legacy_camera_device*) device;
    lcdev->hwif->disableMsgType(msg_type);
}

int camera_msg_type_enabled(struct camera_device *device, int32_t msg_type)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->msgTypeEnabled(msg_type);
}

int camera_start_preview(struct camera_device *device)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->startPreview();
}

void camera_stop_preview(struct camera_device *device)
{
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return;

    lcdev = (legacy_camera_device*) device;
    lcdev->hwif->stopPreview();
}

int camera_preview_enabled(struct camera_device *device)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->previewEnabled();
}

int camera_store_meta_data_in_buffers(struct camera_device *device, int enable)
{
    int rv = -EINVAL;
#ifdef STORE_METADATA_IN_BUFFER
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return ret = lcdev->hwif->storeMetaDataInBuffers(enable);
#else
    LOGW("camera_store_meta_data_in_buffers:\n");
    return rv;
#endif
}

int camera_start_recording(struct camera_device *device)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->startRecording();
}

void camera_stop_recording(struct camera_device *device)
{
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return;

    lcdev = (legacy_camera_device*) device;
    lcdev->hwif->stopRecording();
    lcdev->hwif->startPreview();
}

int camera_recording_enabled(struct camera_device *device)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->recordingEnabled();
}

void camera_release_recording_frame(struct camera_device *device,
                                    const void *opaque)
{
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return;

    /* TODO Implement */
    lcdev = (legacy_camera_device*) device;
    return;
}

int camera_auto_focus(struct camera_device *device)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->autoFocus();
}

int camera_cancel_auto_focus(struct camera_device *device)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->cancelAutoFocus();
}

int camera_take_picture(struct camera_device *device)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->takePicture();
}

int camera_cancel_picture(struct camera_device *device)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->cancelPicture();
}

int camera_set_parameters(struct camera_device *device,
                          const char *params)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device || !params)
        return rv;

    String8 s(params);
    CameraParameters p(s);
    log_camera_params(__FUNCTION__, p);

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->setParameters(p);
}

char *camera_get_parameters(struct camera_device *device)
{
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return NULL;

    lcdev = (legacy_camera_device*) device;
    CameraParameters params(lcdev->hwif->getParameters());
    CameraHAL_FixupParams(device, params);
    log_camera_params(__FUNCTION__, params);

    return strdup((char *)params.flatten().string());
}

void camera_put_parameters(struct camera_device *device, char *params)
{
    if (params) {
        free(params);
    }
}

int camera_send_command(struct camera_device *device,
                        int32_t cmd, int32_t arg0, int32_t arg1)
{
    int rv = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;

    lcdev = (legacy_camera_device*) device;
    return lcdev->hwif->sendCommand(cmd, arg0, arg1);
}

void camera_release(struct camera_device *device)
{
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return;

    lcdev->hwif->release();
}

int camera_dump(struct camera_device *device, int fd)
{
    int rv = -EINVAL;
    Vector<String16> args;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rv;


    lcdev = (legacy_camera_device*) device;
//    return lcdev->hwif->dump(fd, args);
    return rv;
}

int camera_device_close(hw_device_t* device)
{
    int rc = -EINVAL;
    legacy_camera_device *lcdev = NULL;

    if (!device)
        return rc;

    lcdev = (legacy_camera_device*) device;
    if (lcdev) {
        lcdev->hwif = NULL;
        if (lcdev->device.ops) {
            free(lcdev->device.ops);
        }
        free(lcdev);
    }
    rc = NO_ERROR;

    return rc;
}

int camera_device_open(const hw_module_t* module, const char *name,
                       hw_device_t** device)
{
    int ret = NO_ERROR;
    struct legacy_camera_device *lcdev;
    camera_device_t* camera_device;
    camera_device_ops_t* camera_ops;

    if (!name)
        return ret;

    int cameraId = atoi(name);

    LOGD("%s: name:%s device:%p cameraId:%d\n", __FUNCTION__, name, device, cameraId);

    lcdev = (legacy_camera_device *)malloc(sizeof(*lcdev));
    camera_ops = (camera_device_ops_t *)malloc(sizeof(*camera_ops));
    memset(lcdev, 0, sizeof(*lcdev));
    memset(camera_ops, 0, sizeof(*camera_ops));

    lcdev->device.common.tag               = HARDWARE_DEVICE_TAG;
    lcdev->device.common.version           = 0;
    lcdev->device.common.module            = (hw_module_t *)(module);
    lcdev->device.common.close             = camera_device_close;
    lcdev->device.ops                      = camera_ops;

    camera_ops->set_preview_window         = camera_set_preview_window;
    camera_ops->set_callbacks              = camera_set_callbacks;
    camera_ops->enable_msg_type            = camera_enable_msg_type;
    camera_ops->disable_msg_type           = camera_disable_msg_type;
    camera_ops->msg_type_enabled           = camera_msg_type_enabled;
    camera_ops->start_preview              = camera_start_preview;
    camera_ops->stop_preview               = camera_stop_preview;
    camera_ops->preview_enabled            = camera_preview_enabled;
    camera_ops->store_meta_data_in_buffers = camera_store_meta_data_in_buffers;
    camera_ops->start_recording            = camera_start_recording;
    camera_ops->stop_recording             = camera_stop_recording;
    camera_ops->recording_enabled          = camera_recording_enabled;
    camera_ops->release_recording_frame    = camera_release_recording_frame;
    camera_ops->auto_focus                 = camera_auto_focus;
    camera_ops->cancel_auto_focus          = camera_cancel_auto_focus;
    camera_ops->take_picture               = camera_take_picture;
    camera_ops->cancel_picture             = camera_cancel_picture;

    camera_ops->set_parameters             = camera_set_parameters;
    camera_ops->get_parameters             = camera_get_parameters;
    camera_ops->put_parameters             = camera_put_parameters;
    camera_ops->send_command               = camera_send_command;
    camera_ops->release                    = camera_release;
    camera_ops->dump                       = camera_dump;

    lcdev->id = cameraId;
    lcdev->hwif = HAL_openCameraHardware(cameraId);
    if (lcdev->hwif == NULL) {
         ret = -EIO;
         goto err_create_camera_hw;
    }
    *device = &lcdev->device.common;
    return NO_ERROR;

err_create_camera_hw:
    free(lcdev);
    free(camera_ops);
    return ret;
}

} /* namespace android */
