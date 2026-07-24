/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "demo_hidl_hal_test"

#include <VtsHalHidlTargetTestBase.h>
#include <VtsHalHidlTargetTestEnvBase.h>
#include <android-base/logging.h>
#include <android/hardware/demo/1.0/IDemo.h>
#include <android/hardware/demo/1.0/IDemoCallback.h>
#include <android/hardware/demo/1.0/types.h>

using ::android::hardware::demo::V1_0::DemoStatus;
using ::android::hardware::demo::V1_0::IDemo;
using ::android::hardware::demo::V1_0::IDemoCallback;
using ::android::hardware::demo::V1_0::Result;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

#define ASSERT_OK(ret) ASSERT_TRUE(ret.isOk())
#define EXPECT_OK(ret) EXPECT_TRUE(ret.isOk())

// Test environment for Demo HIDL HAL.
class DemoHidlEnvironment : public ::testing::VtsHalHidlTargetTestEnvBase {
   public:
    static DemoHidlEnvironment* Instance() {
        static DemoHidlEnvironment* instance = new DemoHidlEnvironment;
        return instance;
    }

    virtual void registerTestServices() override { registerTestService<IDemo>(); }

   private:
    DemoHidlEnvironment() {}
};

class DemoHidlTest : public ::testing::VtsHalHidlTargetTestBase {
   public:
    virtual void SetUp() override {
        demo = ::testing::VtsHalHidlTargetTestBase::getService<IDemo>(
            DemoHidlEnvironment::Instance()->getServiceName<IDemo>());
        ASSERT_NE(demo, nullptr);
        LOG(INFO) << "Test is remote " << demo->isRemote();
    }

    sp<IDemo> demo;
};

// 同步单返回值 + 同步多返回值：setValue 后 getValue 应取回相同值
TEST_F(DemoHidlTest, SetGetValue) {
    ASSERT_EQ(Result::OK, demo->setValue(42));
    demo->getValue([&](Result r, uint32_t v) {
        EXPECT_EQ(Result::OK, r);
        EXPECT_EQ(42u, v);
    });
}

// 返回复合 struct：getStatus 应包含有效字段
TEST_F(DemoHidlTest, GetStatus) {
    demo->getStatus([&](const DemoStatus& s) {
        EXPECT_TRUE(s.ready);
        EXPECT_FALSE(s.message.empty());
    });
}

// setCallback 对空指针应返回 INVALID_ARG
TEST_F(DemoHidlTest, SetCallbackInvalidArg) {
    Result r = demo->setCallback(nullptr);
    EXPECT_EQ(Result::INVALID_ARG, r);
}

// setCallback 接受有效回调应返回 OK
TEST_F(DemoHidlTest, SetCallbackOk) {
    struct NoopCallback : public IDemoCallback {
        Return<void> onValueChanged(uint32_t /* value */) override { return Void(); }
    };
    sp<IDemoCallback> cb = new NoopCallback();
    EXPECT_EQ(Result::OK, demo->setCallback(cb));
}

int main(int argc, char** argv) {
    ::testing::AddGlobalTestEnvironment(DemoHidlEnvironment::Instance());
    ::testing::InitGoogleTest(&argc, argv);
    DemoHidlEnvironment::Instance()->init(&argc, argv);
    int status = RUN_ALL_TESTS();
    LOG(INFO) << "Test result = " << status;
    return status;
}
