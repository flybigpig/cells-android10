/*
 * Copyright (C) 2009 The Android Open Source Project
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

#define LOG_TAG "VibratorService"

#include <android/hardware/vibrator/1.0/IVibrator.h>
#include <android/hardware/vibrator/1.0/types.h>
#include <android/hardware/vibrator/1.0/IVibrator.h>
#include <android/hardware/vibrator/1.1/types.h>
#include <android/hardware/vibrator/1.2/IVibrator.h>
#include <android/hardware/vibrator/1.2/types.h>
#include <android/hardware/vibrator/1.3/IVibrator.h>

#include "jni.h"
#include <nativehelper/JNIHelp.h>
#include "android_runtime/AndroidRuntime.h"

#include <utils/misc.h>
#include <utils/Log.h>
#include <hardware/vibrator.h>

#include <inttypes.h>
#include <stdio.h>

using android::hardware::Return;
using android::hardware::vibrator::V1_0::EffectStrength;
using android::hardware::vibrator::V1_0::Status;
using android::hardware::vibrator::V1_1::Effect_1_1;

namespace V1_0 = android::hardware::vibrator::V1_0;
namespace V1_1 = android::hardware::vibrator::V1_1;
namespace V1_2 = android::hardware::vibrator::V1_2;
namespace V1_3 = android::hardware::vibrator::V1_3;

namespace android {

static constexpr int NUM_TRIES = 2;

// Creates a Return<R> with STATUS::EX_NULL_POINTER.
template<class R>
inline Return<R> NullptrStatus() {
    using ::android::hardware::Status;
    return Return<R>{Status::fromExceptionCode(Status::EX_NULL_POINTER)};
}

// Helper used to transparently deal with the vibrator HAL becoming unavailable.
template<class R, class I, class... Args0, class... Args1>
Return<R> halCall(Return<R> (I::* fn)(Args0...), Args1&&... args1) {
    // 模板参数说明：
    // R: HAL 方法的返回值类型
    // I: HAL 接口类类型 (如 IVibrator)
    // Args0: HAL 成员函数指针所需的参数类型包
    // Args1: 实际传入的参数类型包（用于完美转发）

    // 假设如果 getService 返回 nullptr，则设备上不可用该 HAL 服务。
    // 使用 static 局部变量，确保 HAL 代理对象只初始化一次（懒加载单例模式）
    static sp<I> sHal = I::getService();
    // 记录 HAL 服务的可用性状态，避免后续重复查询
    static bool sAvailable = sHal != nullptr;

    // 如果设备不支持该 HAL 服务，直接返回一个代表空指针状态的 Return 对象
    if (!sAvailable) {
        return NullptrStatus<R>();
    }

    // Return<R> 没有默认构造函数，因此需要显式构造一个 Return<R> 对象，
    // 并初始化为无异常状态 (EX_NONE)，以便在后续的循环中可以被安全赋值。
    using ::android::hardware::Status;
    Return<R> ret{Status::fromExceptionCode(Status::EX_NONE)};

    // 注意：ret 在此循环之后保证会被修改。
    // 循环尝试调用 HAL 方法，最多尝试 NUM_TRIES 次
    for (int i = 0; i < NUM_TRIES; ++i) {
        // 使用三元运算符检查 sHal 是否为空：
        // - 如果为空，返回 NullptrStatus
        // - 如果不为空，通过成员函数指针调用 HAL 方法
        // (*sHal.*fn) 是解引用智能指针并调用其成员函数指针的标准写法
        // std::forward<Args1>(args1)... 实现了完美转发，保留了传入参数的左值/右值属性
        ret = (sHal == nullptr) ? NullptrStatus<R>()
                : (*sHal.*fn)(std::forward<Args1>(args1)...);

        // 如果 HAL 调用成功（没有错误或异常），跳出重试循环
        if (ret.isOk()) {
            break;
        }

        // 调用失败，打印错误日志，准备重试
        ALOGE("Failed to issue command to vibrator HAL. Retrying.");

        // 尝试恢复与 HAL 服务的连接。
        // 使用 tryGetService 而不是 getService，因为 tryGetService 是非阻塞的，
        // 如果服务未就绪会立即返回 nullptr，而 getService 会阻塞等待服务上线。
        sHal = I::tryGetService();
    }
    // 返回最终的调用结果（可能是成功的结果，也可能是重试 NUM_TRIES 次后依然失败的结果）
    return ret;
}
template<class R>
bool isValidEffect(jlong effect) {
    if (effect < 0) {
        return false;
    }
    R val = static_cast<R>(effect);
    auto iter = hardware::hidl_enum_range<R>();
    return val >= *iter.begin() && val <= *std::prev(iter.end());
}

static void vibratorInit(JNIEnv /* env */, jobject /* clazz */)
{
    halCall(&V1_0::IVibrator::ping).isOk();
}

static jboolean vibratorExists(JNIEnv* /* env */, jobject /* clazz */)
{
    return halCall(&V1_0::IVibrator::ping).isOk() ? JNI_TRUE : JNI_FALSE;
}

static void vibratorOn(JNIEnv* /* env */, jobject /* clazz */, jlong timeout_ms)
{
    Status retStatus = halCall(&V1_0::IVibrator::on, timeout_ms).withDefault(Status::UNKNOWN_ERROR);
    if (retStatus != Status::OK) {
        ALOGE("vibratorOn command failed (%" PRIu32 ").", static_cast<uint32_t>(retStatus));
    }
}

static void vibratorOff(JNIEnv* /* env */, jobject /* clazz */)
{
    Status retStatus = halCall(&V1_0::IVibrator::off).withDefault(Status::UNKNOWN_ERROR);
    if (retStatus != Status::OK) {
        ALOGE("vibratorOff command failed (%" PRIu32 ").", static_cast<uint32_t>(retStatus));
    }
}

static jlong vibratorSupportsAmplitudeControl(JNIEnv*, jobject) {
    return halCall(&V1_0::IVibrator::supportsAmplitudeControl).withDefault(false);
}

static void vibratorSetAmplitude(JNIEnv*, jobject, jint amplitude) {
    Status status = halCall(&V1_0::IVibrator::setAmplitude, static_cast<uint32_t>(amplitude))
        .withDefault(Status::UNKNOWN_ERROR);
    if (status != Status::OK) {
      ALOGE("Failed to set vibrator amplitude (%" PRIu32 ").",
            static_cast<uint32_t>(status));
    }
}

static jboolean vibratorSupportsExternalControl(JNIEnv*, jobject) {
    return halCall(&V1_3::IVibrator::supportsExternalControl).withDefault(false);
}

static void vibratorSetExternalControl(JNIEnv*, jobject, jboolean enabled) {
    Status status = halCall(&V1_3::IVibrator::setExternalControl, static_cast<uint32_t>(enabled))
        .withDefault(Status::UNKNOWN_ERROR);
    if (status != Status::OK) {
      ALOGE("Failed to set vibrator external control (%" PRIu32 ").",
            static_cast<uint32_t>(status));
    }
}

static jlong vibratorPerformEffect(JNIEnv*, jobject, jlong effect, jint strength) {
    Status status;
    uint32_t lengthMs;
    auto callback = [&status, &lengthMs](Status retStatus, uint32_t retLengthMs) {
        status = retStatus;
        lengthMs = retLengthMs;
    };
    EffectStrength effectStrength(static_cast<EffectStrength>(strength));

    Return<void> ret;
    if (isValidEffect<V1_0::Effect>(effect)) {
        ret = halCall(&V1_0::IVibrator::perform, static_cast<V1_0::Effect>(effect),
                effectStrength, callback);
    } else if (isValidEffect<Effect_1_1>(effect)) {
        ret = halCall(&V1_1::IVibrator::perform_1_1, static_cast<Effect_1_1>(effect),
                           effectStrength, callback);
    } else if (isValidEffect<V1_2::Effect>(effect)) {
        ret = halCall(&V1_2::IVibrator::perform_1_2, static_cast<V1_2::Effect>(effect),
                           effectStrength, callback);
    } else if (isValidEffect<V1_3::Effect>(effect)) {
        ret = halCall(&V1_3::IVibrator::perform_1_3, static_cast<V1_3::Effect>(effect),
                           effectStrength, callback);
    } else {
        ALOGW("Unable to perform haptic effect, invalid effect ID (%" PRId32 ")",
                static_cast<int32_t>(effect));
        return -1;
    }

    if (!ret.isOk()) {
        ALOGW("Failed to perform effect (%" PRId32 ")", static_cast<int32_t>(effect));
        return -1;
    }

    if (status == Status::OK) {
        return lengthMs;
    } else if (status != Status::UNSUPPORTED_OPERATION) {
        // Don't warn on UNSUPPORTED_OPERATION, that's a normal event and just means the motor
        // doesn't have a pre-defined waveform to perform for it, so we should just give the
        // opportunity to fall back to the framework waveforms.
        ALOGE("Failed to perform haptic effect: effect=%" PRId64 ", strength=%" PRId32
                ", error=%" PRIu32 ").", static_cast<int64_t>(effect),
                static_cast<int32_t>(strength), static_cast<uint32_t>(status));
    }

    return -1;
}

static const JNINativeMethod method_table[] = {
    { "vibratorExists", "()Z", (void*)vibratorExists },
    { "vibratorInit", "()V", (void*)vibratorInit },
    { "vibratorOn", "(J)V", (void*)vibratorOn },
    { "vibratorOff", "()V", (void*)vibratorOff },
    { "vibratorSupportsAmplitudeControl", "()Z", (void*)vibratorSupportsAmplitudeControl},
    { "vibratorSetAmplitude", "(I)V", (void*)vibratorSetAmplitude},
    { "vibratorPerformEffect", "(JJ)J", (void*)vibratorPerformEffect},
    { "vibratorSupportsExternalControl", "()Z", (void*)vibratorSupportsExternalControl},
    { "vibratorSetExternalControl", "(Z)V", (void*)vibratorSetExternalControl},
};

int register_android_server_VibratorService(JNIEnv *env)
{
    return jniRegisterNativeMethods(env, "com/android/server/VibratorService",
            method_table, NELEM(method_table));
}

};
