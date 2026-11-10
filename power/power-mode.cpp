/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aidl/android/hardware/power/BnPower.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <sys/ioctl.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <unistd.h>

#define CMD_DATA_BUF_SIZE 256
#define COMMON_DATA_CMD 0
#define SELECT_TOUCH_ID 3
#define SET_CUR_VALUE 0
#define TOUCH_DOUBLETAP_MODE 14
#define TOUCH_FOD_ENABLE_MODE 10
#define TOUCH_MAGIC 0x54
#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_ID 0

typedef struct {
    int8_t   touch_id;
    uint8_t  cmd;
    uint16_t mode;
    uint16_t data_len;
    int32_t  data_buf[CMD_DATA_BUF_SIZE];
} touch_data;

#define TOUCH_IOC_COMMON_DATA _IOW(TOUCH_MAGIC, COMMON_DATA_CMD, touch_data)
#define TOUCH_IOC_SELECT_TOUCH_ID _IOW(TOUCH_MAGIC, SELECT_TOUCH_ID, int)

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace impl {

using ::aidl::android::hardware::power::Mode;

bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return) {
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE:
        case Mode::DISPLAY_INACTIVE:
            *_aidl_return = true;
            return true;
        default:
            return false;
    }
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE: {
            ::android::base::unique_fd fd(open(TOUCH_DEV_PATH, O_RDWR | O_CLOEXEC));
            if (fd.get() < 0) {
                PLOG(ERROR) << "Opening touch device";
                return false;
            }
            if (ioctl(fd.get(), TOUCH_IOC_SELECT_TOUCH_ID, TOUCH_ID) < 0) {
                PLOG(ERROR) << "Selecting touch device";
                return false;
            }
            touch_data data = {};
            data.touch_id = TOUCH_ID;
            data.cmd = SET_CUR_VALUE;
            data.mode = TOUCH_DOUBLETAP_MODE;
            data.data_len = 1;
            data.data_buf[0] = enabled ? 1 : 0;
            if (ioctl(fd.get(), TOUCH_IOC_COMMON_DATA, &data) < 0) {
                PLOG(ERROR) << "Applying touch mode " << data.mode;
                return false;
            }
            return true;
        }
        case Mode::DISPLAY_INACTIVE: {
            ::android::base::unique_fd fd(open(TOUCH_DEV_PATH, O_RDWR | O_CLOEXEC));
            if (fd.get() < 0) {
                PLOG(ERROR) << "Opening touch device";
                return false;
            }
            if (ioctl(fd.get(), TOUCH_IOC_SELECT_TOUCH_ID, TOUCH_ID) < 0) {
                PLOG(ERROR) << "Selecting touch device";
                return false;
            }
            touch_data data = {};
            data.touch_id = TOUCH_ID;
            data.cmd = SET_CUR_VALUE;
            data.mode = TOUCH_FOD_ENABLE_MODE;
            data.data_len = 1;
            data.data_buf[0] = enabled ? 1 : 0;
            if (ioctl(fd.get(), TOUCH_IOC_COMMON_DATA, &data) < 0) {
                PLOG(ERROR) << "Applying touch mode " << data.mode;
                return false;
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
