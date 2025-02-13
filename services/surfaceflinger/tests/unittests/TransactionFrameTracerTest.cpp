/*
 * Copyright (C) 2021 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "LibSurfaceFlingerUnittests"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <gui/SurfaceComposerClient.h>
#include <log/log.h>
#include <renderengine/ExternalTexture.h>
#include <renderengine/mock/FakeExternalTexture.h>
#include <renderengine/mock/RenderEngine.h>
#include <utils/String8.h>

#include "TestableSurfaceFlinger.h"
#include "mock/DisplayHardware/MockComposer.h"

namespace android {

using testing::_;
using testing::Mock;
using testing::Return;

using PresentState = frametimeline::SurfaceFrame::PresentState;

class TransactionFrameTracerTest : public testing::Test {
public:
    TransactionFrameTracerTest() {
        const ::testing::TestInfo* const test_info =
                ::testing::UnitTest::GetInstance()->current_test_info();
        ALOGD("**** Setting up for %s.%s\n", test_info->test_case_name(), test_info->name());
        mFlinger.setupMockScheduler();
        mFlinger.setupComposer(std::make_unique<Hwc2::mock::Composer>());
        mFlinger.setupRenderEngine(std::unique_ptr<renderengine::RenderEngine>(mRenderEngine));
    }

    ~TransactionFrameTracerTest() {
        const ::testing::TestInfo* const test_info =
                ::testing::UnitTest::GetInstance()->current_test_info();
        ALOGD("**** Tearing down after %s.%s\n", test_info->test_case_name(), test_info->name());
    }

    sp<Layer> createLayer() {
        sp<Client> client;
        LayerCreationArgs args(mFlinger.flinger(), client, "buffer-state-layer", 0,
                               LayerMetadata());
        return sp<Layer>::make(args);
    }

    void commitTransaction(Layer* layer) { layer->commitTransaction(); }

    TestableSurfaceFlinger mFlinger;
    renderengine::mock::RenderEngine* mRenderEngine = new renderengine::mock::RenderEngine();

    FenceToFenceTimeMap fenceFactory;

    void BLASTTransactionSendsFrameTracerEvents() {
        sp<Layer> layer = createLayer();

        sp<Fence> fence(sp<Fence>::make());
        int32_t layerId = layer->getSequence();
        uint64_t bufferId = 42;
        uint64_t frameNumber = 5;
        nsecs_t dequeueTime = 10;
        nsecs_t postTime = 20;
        EXPECT_CALL(*mFlinger.getFrameTracer(), traceNewLayer(layerId, "buffer-state-layer"));
        EXPECT_CALL(*mFlinger.getFrameTracer(),
                    traceTimestamp(layerId, bufferId, frameNumber, dequeueTime,
                                   FrameTracer::FrameEvent::DEQUEUE, /*duration*/ 0));
        EXPECT_CALL(*mFlinger.getFrameTracer(),
                    traceTimestamp(layerId, bufferId, frameNumber, postTime,
                                   FrameTracer::FrameEvent::QUEUE, /*duration*/ 0));
        BufferData bufferData;
        bufferData.acquireFence = fence;
        bufferData.frameNumber = frameNumber;
        bufferData.flags |= BufferData::BufferDataChange::fenceChanged;
        bufferData.flags |= BufferData::BufferDataChange::frameNumberChanged;
        std::shared_ptr<renderengine::ExternalTexture> externalTexture = std::make_shared<
                renderengine::mock::FakeExternalTexture>(1U /*width*/, 1U /*height*/, bufferId,
                                                         HAL_PIXEL_FORMAT_RGBA_8888,
                                                         0ULL /*usage*/);
        layer->setBuffer(externalTexture, bufferData, postTime, /*desiredPresentTime*/ 30, false,
                         FrameTimelineInfo{}, gui::GameMode::Unsupported);

        commitTransaction(layer.get());
        nsecs_t latchTime = 25;
        EXPECT_CALL(*mFlinger.getFrameTracer(),
                    traceFence(layerId, bufferId, frameNumber, _,
                               FrameTracer::FrameEvent::ACQUIRE_FENCE, /*startTime*/ 0));
        EXPECT_CALL(*mFlinger.getFrameTracer(),
                    traceTimestamp(layerId, bufferId, frameNumber, latchTime,
                                   FrameTracer::FrameEvent::LATCH, /*duration*/ 0));
        layer->updateTexImage(latchTime);

        auto glDoneFence = fenceFactory.createFenceTimeForTest(fence);
        auto presentFence = fenceFactory.createFenceTimeForTest(fence);
        CompositorTiming compositorTiming;
        EXPECT_CALL(*mFlinger.getFrameTracer(),
                    traceFence(layerId, bufferId, frameNumber, presentFence,
                               FrameTracer::FrameEvent::PRESENT_FENCE, /*startTime*/ 0));
        layer->onCompositionPresented(nullptr, glDoneFence, presentFence, compositorTiming,
                                      gui::GameMode::Unsupported);
    }
};

TEST_F(TransactionFrameTracerTest, BLASTTransactionSendsFrameTracerEvents) {
    BLASTTransactionSendsFrameTracerEvents();
}

} // namespace android
