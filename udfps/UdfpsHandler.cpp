/*
 * Copyright (C) 2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.xiaomi_sm8850"

#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include <fstream>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <sys/eventfd.h>
#include <cerrno>
#include <atomic>
#include <functional>
#include <utility>

#include "UdfpsHandler.h"

#define COMMAND_FOD_PRESS_STATUS 1
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define FOD_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"
#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

#define FINGERPRINT_ACQUIRED_VENDOR 7

using ::aidl::android::hardware::biometrics::fingerprint::AcquiredInfo;

namespace {

template <typename T>
static void set(const std::string& path, const T& value) {
    std::ofstream file(path);
    file << value;
}

}  // anonymous namespace

class XiaomiSM8850UdfpsHander : public UdfpsHandler {
  public:
    ~XiaomiSM8850UdfpsHander() override {
        if (mEventThread.joinable()) {
            const uint64_t stop = 1;
            const ssize_t result = TEMP_FAILURE_RETRY(write(mStopFd.get(), &stop, sizeof(stop)));
            if (result != static_cast<ssize_t>(sizeof(stop)) && errno != EAGAIN) {
                PLOG(FATAL) << "Stopping FOD event thread";
            }
            mEventThread.join();
        }
    }

    void init(fingerprint_device_t* device) {
        mDevice = device;
        if (mDevice == nullptr || mDevice->extCmd == nullptr) {
            reportObservationFailure("Fingerprint device or extension unavailable");
            return;
        }

        mUinputFd.reset(open("/dev/uinput", O_WRONLY | O_NONBLOCK));
        if (mUinputFd.get() < 0) {
            LOG(ERROR) << "Failed to open uinput";
        } else {
            ioctl(mUinputFd.get(), UI_SET_EVBIT, EV_KEY);
            ioctl(mUinputFd.get(), UI_SET_KEYBIT, KEY_WAKEUP);
            struct uinput_setup usetup = { .id = { .bustype = BUS_VIRTUAL }, .name = "UdfpsWakeup" };
            ioctl(mUinputFd.get(), UI_DEV_SETUP, &usetup);
            ioctl(mUinputFd.get(), UI_DEV_CREATE);
        }

        mStopFd.reset(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
        if (mStopFd.get() < 0) {
            reportObservationFailure("Creating FOD stop event failed");
            return;
        }
        mFodFd.reset(open(FOD_STATUS_PATH, O_RDONLY | O_CLOEXEC));
        if (mFodFd.get() < 0) {
            reportObservationFailure("Opening FOD status path failed");
            return;
        }
        char initial;
        if (TEMP_FAILURE_RETRY(pread(mFodFd.get(), &initial, 1, 0)) != 1
                || (initial != '0' && initial != '1')) {
            reportObservationFailure("Reading initial FOD state failed");
            return;
        }
        mObservationAvailable.store(true);
        mEventThread = std::thread([this, initial]() {
            bool lastState = initial == '1';
            if (lastState) setFingerDown(true);
            pollfd events[2] = {{mFodFd.get(), POLLPRI | POLLERR, 0},
                                {mStopFd.get(), POLLIN, 0}};
            for (;;) {
                if (poll(events, 2, -1) < 0) {
                    if (errno == EINTR) continue;
                    reportObservationFailure("FOD event observation failed");
                    return;
                }
                if (events[1].revents & POLLIN) return;
                if ((events[0].revents | events[1].revents) & (POLLNVAL | POLLHUP)) {
                    reportObservationFailure("FOD event descriptor closed");
                    return;
                }
                if (events[1].revents & POLLERR) {
                    reportObservationFailure("FOD stop event failed");
                    return;
                }
                // sysfs POLLPRI|POLLERR is an event notification, with no timeout.
                if (events[0].revents & (POLLPRI | POLLERR)) {
                    char value;
                    if (TEMP_FAILURE_RETRY(pread(mFodFd.get(), &value, 1, 0)) != 1
                            || (value != '0' && value != '1')) {
                        reportObservationFailure("FOD event state read failed");
                        return;
                    }
                    const bool pressed = value == '1';
                    if (pressed != lastState) {
                        lastState = pressed;
                        setFingerDown(pressed);
                    }
                }
            }
        });
    }

    bool isHardwareAvailable() const override {
        return mObservationAvailable.load();
    }

    void setErrorCallback(std::function<void()> callback) override {
        mErrorCallback = std::move(callback);
    }

    void onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__;
        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        setFingerDown(false);
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        if (result != FINGERPRINT_ACQUIRED_VENDOR) {
            setFingerDown(false);
            if (static_cast<AcquiredInfo>(result) == AcquiredInfo::GOOD) {
                setFodStatus(FOD_STATUS_OFF);
                wakeDevice();
            }
        } else if (vendorCode == 201 || vendorCode == 202) {
            /*
             * vendorCode = 201 waiting for fingerprint authentication
             * vendorCode = 202 waiting for fingerprint enroll
             */
            setFodStatus(FOD_STATUS_ON);
        } else if (vendorCode == 44) {
            /*
             * vendorCode = 44 fingerprint scan failed
             */
            setFingerDown(false);
        }
    }

    void cancel() {
        LOG(INFO) << __func__;
        setFingerDown(false);
        setFodStatus(FOD_STATUS_OFF);
    }

  private:
    fingerprint_device_t* mDevice = nullptr;
    std::atomic<bool> mObservationAvailable{false};
    std::function<void()> mErrorCallback;
    android::base::unique_fd mFodFd;
    android::base::unique_fd mStopFd;
    std::thread mEventThread;
    android::base::unique_fd mUinputFd;

    void reportObservationFailure(const char* reason) {
        LOG(ERROR) << reason;
        mObservationAvailable.store(false);
        if (mErrorCallback) mErrorCallback();
    }

    void setFodStatus(int value) {
        set(FOD_STATUS_PATH, value);
    }

    void setFingerDown(bool pressed) {
        if (!mObservationAvailable.load()) {
            // The observation thread already reported this failure. Binder-driven
            // touch/cancel calls must not invoke a framework callback recursively.
            LOG(ERROR) << "FOD operation requested after observation failure";
            return;
        }
        if (pressed) {
            mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS, PARAM_FOD_PRESSED);
        }
    }

    void wakeDevice() {
        if (mUinputFd.get() >= 0) {
            struct input_event ev[3] = {};
            ev[0].type = EV_KEY; ev[0].code = KEY_WAKEUP; ev[0].value = 1;
            ev[1].type = EV_KEY; ev[1].code = KEY_WAKEUP; ev[1].value = 0;
            ev[2].type = EV_SYN; ev[2].code = SYN_REPORT; ev[2].value = 0;
            write(mUinputFd.get(), ev, sizeof(ev));
        }
    }
};

static UdfpsHandler* create() {
    return new XiaomiSM8850UdfpsHander();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
        .create = create,
        .destroy = destroy,
};
