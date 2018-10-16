/*
 * Copyright 2018 The Android Open Source Project
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
#define LOG_TAG "TimeStats"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "TimeStats.h"

#include <android-base/stringprintf.h>

#include <log/log.h>

#include <utils/String8.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include <algorithm>
#include <regex>

namespace android {

TimeStats& TimeStats::getInstance() {
    static TimeStats sInstance;
    return sInstance;
}

void TimeStats::parseArgs(bool asProto, const Vector<String16>& args, size_t& index,
                          String8& result) {
    ATRACE_CALL();

    if (args.size() > index + 10) {
        ALOGD("Invalid args count");
        return;
    }

    std::unordered_map<std::string, int32_t> argsMap;
    while (index < args.size()) {
        argsMap[std::string(String8(args[index]).c_str())] = index;
        ++index;
    }

    if (argsMap.count("-disable")) {
        disable();
    }

    if (argsMap.count("-dump")) {
        std::optional<uint32_t> maxLayers = std::nullopt;
        auto iter = argsMap.find("-maxlayers");
        if (iter != argsMap.end() && iter->second + 1 < static_cast<int32_t>(args.size())) {
            int64_t value = strtol(String8(args[iter->second + 1]).c_str(), nullptr, 10);
            value = std::clamp(value, int64_t(0), int64_t(UINT32_MAX));
            maxLayers = static_cast<uint32_t>(value);
        }

        dump(asProto, maxLayers, result);
    }

    if (argsMap.count("-clear")) {
        clear();
    }

    if (argsMap.count("-enable")) {
        enable();
    }
}

void TimeStats::incrementTotalFrames() {
    if (!mEnabled.load()) return;

    ATRACE_CALL();

    std::lock_guard<std::mutex> lock(mMutex);
    mTimeStats.totalFrames++;
}

void TimeStats::incrementMissedFrames() {
    if (!mEnabled.load()) return;

    ATRACE_CALL();

    std::lock_guard<std::mutex> lock(mMutex);
    mTimeStats.missedFrames++;
}

void TimeStats::incrementClientCompositionFrames() {
    if (!mEnabled.load()) return;

    ATRACE_CALL();

    std::lock_guard<std::mutex> lock(mMutex);
    mTimeStats.clientCompositionFrames++;
}

bool TimeStats::recordReadyLocked(const std::string& layerName, TimeRecord* timeRecord) {
    if (!timeRecord->ready) {
        ALOGV("[%s]-[%" PRIu64 "]-presentFence is still not received", layerName.c_str(),
              timeRecord->frameTime.frameNumber);
        return false;
    }

    if (timeRecord->acquireFence != nullptr) {
        if (timeRecord->acquireFence->getSignalTime() == Fence::SIGNAL_TIME_PENDING) {
            return false;
        }
        if (timeRecord->acquireFence->getSignalTime() != Fence::SIGNAL_TIME_INVALID) {
            timeRecord->frameTime.acquireTime = timeRecord->acquireFence->getSignalTime();
            timeRecord->acquireFence = nullptr;
        } else {
            ALOGV("[%s]-[%" PRIu64 "]-acquireFence signal time is invalid", layerName.c_str(),
                  timeRecord->frameTime.frameNumber);
        }
    }

    if (timeRecord->presentFence != nullptr) {
        if (timeRecord->presentFence->getSignalTime() == Fence::SIGNAL_TIME_PENDING) {
            return false;
        }
        if (timeRecord->presentFence->getSignalTime() != Fence::SIGNAL_TIME_INVALID) {
            timeRecord->frameTime.presentTime = timeRecord->presentFence->getSignalTime();
            timeRecord->presentFence = nullptr;
        } else {
            ALOGV("[%s]-[%" PRIu64 "]-presentFence signal time invalid", layerName.c_str(),
                  timeRecord->frameTime.frameNumber);
        }
    }

    return true;
}

static int32_t msBetween(nsecs_t start, nsecs_t end) {
    int64_t delta = (end - start) / 1000000;
    delta = std::clamp(delta, int64_t(INT32_MIN), int64_t(INT32_MAX));
    return static_cast<int32_t>(delta);
}

// This regular expression captures the following for instance:
// StatusBar in StatusBar#0
// com.appname in com.appname/com.appname.activity#0
// com.appname in SurfaceView - com.appname/com.appname.activity#0
static const std::regex packageNameRegex("(?:SurfaceView[-\\s\\t]+)?([^/]+).*#\\d+");

static std::string getPackageName(const std::string& layerName) {
    std::smatch match;
    if (std::regex_match(layerName.begin(), layerName.end(), match, packageNameRegex)) {
        // There must be a match for group 1 otherwise the whole string is not
        // matched and the above will return false
        return match[1];
    }
    return "";
}

void TimeStats::flushAvailableRecordsToStatsLocked(const std::string& layerName) {
    ATRACE_CALL();

    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    TimeRecord& prevTimeRecord = layerRecord.prevTimeRecord;
    std::deque<TimeRecord>& timeRecords = layerRecord.timeRecords;
    while (!timeRecords.empty()) {
        if (!recordReadyLocked(layerName, &timeRecords[0])) break;
        ALOGV("[%s]-[%" PRIu64 "]-presentFenceTime[%" PRId64 "]", layerName.c_str(),
              timeRecords[0].frameTime.frameNumber, timeRecords[0].frameTime.presentTime);

        if (prevTimeRecord.ready) {
            if (!mTimeStats.stats.count(layerName)) {
                mTimeStats.stats[layerName].layerName = layerName;
                mTimeStats.stats[layerName].packageName = getPackageName(layerName);
            }
            TimeStatsHelper::TimeStatsLayer& timeStatsLayer = mTimeStats.stats[layerName];
            timeStatsLayer.totalFrames++;
            timeStatsLayer.droppedFrames += layerRecord.droppedFrames;
            layerRecord.droppedFrames = 0;

            const int32_t postToAcquireMs = msBetween(timeRecords[0].frameTime.postTime,
                                                      timeRecords[0].frameTime.acquireTime);
            ALOGV("[%s]-[%" PRIu64 "]-post2acquire[%d]", layerName.c_str(),
                  timeRecords[0].frameTime.frameNumber, postToAcquireMs);
            timeStatsLayer.deltas["post2acquire"].insert(postToAcquireMs);

            const int32_t postToPresentMs = msBetween(timeRecords[0].frameTime.postTime,
                                                      timeRecords[0].frameTime.presentTime);
            ALOGV("[%s]-[%" PRIu64 "]-post2present[%d]", layerName.c_str(),
                  timeRecords[0].frameTime.frameNumber, postToPresentMs);
            timeStatsLayer.deltas["post2present"].insert(postToPresentMs);

            const int32_t acquireToPresentMs = msBetween(timeRecords[0].frameTime.acquireTime,
                                                         timeRecords[0].frameTime.presentTime);
            ALOGV("[%s]-[%" PRIu64 "]-acquire2present[%d]", layerName.c_str(),
                  timeRecords[0].frameTime.frameNumber, acquireToPresentMs);
            timeStatsLayer.deltas["acquire2present"].insert(acquireToPresentMs);

            const int32_t latchToPresentMs = msBetween(timeRecords[0].frameTime.latchTime,
                                                       timeRecords[0].frameTime.presentTime);
            ALOGV("[%s]-[%" PRIu64 "]-latch2present[%d]", layerName.c_str(),
                  timeRecords[0].frameTime.frameNumber, latchToPresentMs);
            timeStatsLayer.deltas["latch2present"].insert(latchToPresentMs);

            const int32_t desiredToPresentMs = msBetween(timeRecords[0].frameTime.desiredTime,
                                                         timeRecords[0].frameTime.presentTime);
            ALOGV("[%s]-[%" PRIu64 "]-desired2present[%d]", layerName.c_str(),
                  timeRecords[0].frameTime.frameNumber, desiredToPresentMs);
            timeStatsLayer.deltas["desired2present"].insert(desiredToPresentMs);

            const int32_t presentToPresentMs = msBetween(prevTimeRecord.frameTime.presentTime,
                                                         timeRecords[0].frameTime.presentTime);
            ALOGV("[%s]-[%" PRIu64 "]-present2present[%d]", layerName.c_str(),
                  timeRecords[0].frameTime.frameNumber, presentToPresentMs);
            timeStatsLayer.deltas["present2present"].insert(presentToPresentMs);
        }
        prevTimeRecord = timeRecords[0];
        timeRecords.pop_front();
        layerRecord.waitData--;
    }
}

// This regular expression captures the following layer names for instance:
// 1) StatusBat#0
// 2) NavigationBar#1
// 3) co(m).*#0
// 4) SurfaceView - co(m).*#0
// Using [-\\s\t]+ for the conjunction part between SurfaceView and co(m).*
// is a bit more robust in case there's a slight change.
// The layer name would only consist of . / $ _ 0-9 a-z A-Z in most cases.
static const std::regex layerNameRegex(
        "(((SurfaceView[-\\s\\t]+)?com?\\.[./$\\w]+)|((Status|Navigation)Bar))#\\d+");

static bool layerNameIsValid(const std::string& layerName) {
    return std::regex_match(layerName.begin(), layerName.end(), layerNameRegex);
}

void TimeStats::setPostTime(const std::string& layerName, uint64_t frameNumber, nsecs_t postTime) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-[%" PRIu64 "]-PostTime[%" PRId64 "]", layerName.c_str(), frameNumber, postTime);

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName) && !layerNameIsValid(layerName)) {
        return;
    }
    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    if (layerRecord.timeRecords.size() == MAX_NUM_TIME_RECORDS) {
        ALOGV("[%s]-timeRecords is already at its maximum size[%zu]", layerName.c_str(),
              MAX_NUM_TIME_RECORDS);
        // TODO(zzyiwei): if this happens, there must be a present fence missing
        // or waitData is not in the correct position. Need to think out a
        // reasonable way to recover from this state.
        return;
    }
    // For most media content, the acquireFence is invalid because the buffer is
    // ready at the queueBuffer stage. In this case, acquireTime should be given
    // a default value as postTime.
    TimeRecord timeRecord = {
            .frameTime =
                    {
                            .frameNumber = frameNumber,
                            .postTime = postTime,
                            .acquireTime = postTime,
                    },
    };
    layerRecord.timeRecords.push_back(timeRecord);
    if (layerRecord.waitData < 0 ||
        layerRecord.waitData >= static_cast<int32_t>(layerRecord.timeRecords.size()))
        layerRecord.waitData = layerRecord.timeRecords.size() - 1;
}

void TimeStats::setLatchTime(const std::string& layerName, uint64_t frameNumber,
                             nsecs_t latchTime) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-[%" PRIu64 "]-LatchTime[%" PRId64 "]", layerName.c_str(), frameNumber, latchTime);

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    TimeRecord& timeRecord = layerRecord.timeRecords[layerRecord.waitData];
    if (timeRecord.frameTime.frameNumber == frameNumber) {
        timeRecord.frameTime.latchTime = latchTime;
    }
}

void TimeStats::setDesiredTime(const std::string& layerName, uint64_t frameNumber,
                               nsecs_t desiredTime) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-[%" PRIu64 "]-DesiredTime[%" PRId64 "]", layerName.c_str(), frameNumber,
          desiredTime);

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    TimeRecord& timeRecord = layerRecord.timeRecords[layerRecord.waitData];
    if (timeRecord.frameTime.frameNumber == frameNumber) {
        timeRecord.frameTime.desiredTime = desiredTime;
    }
}

void TimeStats::setAcquireTime(const std::string& layerName, uint64_t frameNumber,
                               nsecs_t acquireTime) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-[%" PRIu64 "]-AcquireTime[%" PRId64 "]", layerName.c_str(), frameNumber,
          acquireTime);

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    TimeRecord& timeRecord = layerRecord.timeRecords[layerRecord.waitData];
    if (timeRecord.frameTime.frameNumber == frameNumber) {
        timeRecord.frameTime.acquireTime = acquireTime;
    }
}

void TimeStats::setAcquireFence(const std::string& layerName, uint64_t frameNumber,
                                const std::shared_ptr<FenceTime>& acquireFence) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-[%" PRIu64 "]-AcquireFenceTime[%" PRId64 "]", layerName.c_str(), frameNumber,
          acquireFence->getSignalTime());

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    TimeRecord& timeRecord = layerRecord.timeRecords[layerRecord.waitData];
    if (timeRecord.frameTime.frameNumber == frameNumber) {
        timeRecord.acquireFence = acquireFence;
    }
}

void TimeStats::setPresentTime(const std::string& layerName, uint64_t frameNumber,
                               nsecs_t presentTime) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-[%" PRIu64 "]-PresentTime[%" PRId64 "]", layerName.c_str(), frameNumber,
          presentTime);

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    TimeRecord& timeRecord = layerRecord.timeRecords[layerRecord.waitData];
    if (timeRecord.frameTime.frameNumber == frameNumber) {
        timeRecord.frameTime.presentTime = presentTime;
        timeRecord.ready = true;
        layerRecord.waitData++;
    }

    flushAvailableRecordsToStatsLocked(layerName);
}

void TimeStats::setPresentFence(const std::string& layerName, uint64_t frameNumber,
                                const std::shared_ptr<FenceTime>& presentFence) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-[%" PRIu64 "]-PresentFenceTime[%" PRId64 "]", layerName.c_str(), frameNumber,
          presentFence->getSignalTime());

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    TimeRecord& timeRecord = layerRecord.timeRecords[layerRecord.waitData];
    if (timeRecord.frameTime.frameNumber == frameNumber) {
        timeRecord.presentFence = presentFence;
        timeRecord.ready = true;
        layerRecord.waitData++;
    }

    flushAvailableRecordsToStatsLocked(layerName);
}

void TimeStats::onDisconnect(const std::string& layerName) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-onDisconnect", layerName.c_str());

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    flushAvailableRecordsToStatsLocked(layerName);
    mTimeStatsTracker.erase(layerName);
}

void TimeStats::onDestroy(const std::string& layerName) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-onDestroy", layerName.c_str());

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    flushAvailableRecordsToStatsLocked(layerName);
    mTimeStatsTracker.erase(layerName);
}

void TimeStats::clearLayerRecord(const std::string& layerName) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-clearLayerRecord", layerName.c_str());

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    layerRecord.timeRecords.clear();
    layerRecord.prevTimeRecord.ready = false;
    layerRecord.waitData = -1;
    layerRecord.droppedFrames = 0;
}

void TimeStats::removeTimeRecord(const std::string& layerName, uint64_t frameNumber) {
    if (!mEnabled.load()) return;

    ATRACE_CALL();
    ALOGV("[%s]-[%" PRIu64 "]-removeTimeRecord", layerName.c_str(), frameNumber);

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mTimeStatsTracker.count(layerName)) return;
    LayerRecord& layerRecord = mTimeStatsTracker[layerName];
    size_t removeAt = 0;
    for (const TimeRecord& record : layerRecord.timeRecords) {
        if (record.frameTime.frameNumber == frameNumber) break;
        removeAt++;
    }
    if (removeAt == layerRecord.timeRecords.size()) return;
    layerRecord.timeRecords.erase(layerRecord.timeRecords.begin() + removeAt);
    if (layerRecord.waitData > static_cast<int32_t>(removeAt)) {
        layerRecord.waitData--;
    }
    layerRecord.droppedFrames++;
}

void TimeStats::flushPowerTimeLocked() {
    nsecs_t curTime = systemTime();
    // elapsedTime is in milliseconds.
    int64_t elapsedTime = (curTime - mPowerTime.prevTime) / 1000000;

    switch (mPowerTime.powerMode) {
        case HWC_POWER_MODE_NORMAL:
            mTimeStats.displayOnTime += elapsedTime;
            break;
        case HWC_POWER_MODE_OFF:
        case HWC_POWER_MODE_DOZE:
        case HWC_POWER_MODE_DOZE_SUSPEND:
        default:
            break;
    }

    mPowerTime.prevTime = curTime;
}

void TimeStats::setPowerMode(int32_t powerMode) {
    if (!mEnabled.load()) {
        std::lock_guard<std::mutex> lock(mMutex);
        mPowerTime.powerMode = powerMode;
        return;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    if (powerMode == mPowerTime.powerMode) return;

    flushPowerTimeLocked();
    mPowerTime.powerMode = powerMode;
}

void TimeStats::enable() {
    if (mEnabled.load()) return;

    ATRACE_CALL();

    std::lock_guard<std::mutex> lock(mMutex);
    ALOGD("Enabled");
    mEnabled.store(true);
    mTimeStats.statsStart = static_cast<int64_t>(std::time(0));
    mPowerTime.prevTime = systemTime();
}

void TimeStats::disable() {
    if (!mEnabled.load()) return;

    ATRACE_CALL();

    std::lock_guard<std::mutex> lock(mMutex);
    ALOGD("Disabled");
    mEnabled.store(false);
    mTimeStats.statsEnd = static_cast<int64_t>(std::time(0));
}

void TimeStats::clear() {
    ATRACE_CALL();

    std::lock_guard<std::mutex> lock(mMutex);
    ALOGD("Cleared");
    mTimeStats.stats.clear();
    mTimeStats.statsStart = (mEnabled.load() ? static_cast<int64_t>(std::time(0)) : 0);
    mTimeStats.statsEnd = 0;
    mTimeStats.totalFrames = 0;
    mTimeStats.missedFrames = 0;
    mTimeStats.clientCompositionFrames = 0;
    mTimeStats.displayOnTime = 0;
    mPowerTime.prevTime = systemTime();
}

bool TimeStats::isEnabled() {
    return mEnabled.load();
}

void TimeStats::dump(bool asProto, std::optional<uint32_t> maxLayers, String8& result) {
    ATRACE_CALL();

    std::lock_guard<std::mutex> lock(mMutex);
    if (mTimeStats.statsStart == 0) {
        return;
    }

    mTimeStats.statsEnd = static_cast<int64_t>(std::time(0));

    flushPowerTimeLocked();

    if (asProto) {
        ALOGD("Dumping TimeStats as proto");
        SFTimeStatsGlobalProto timeStatsProto = mTimeStats.toProto(maxLayers);
        result.append(timeStatsProto.SerializeAsString().c_str(), timeStatsProto.ByteSize());
    } else {
        ALOGD("Dumping TimeStats as text");
        result.append(mTimeStats.toString(maxLayers).c_str());
        result.append("\n");
    }
}

} // namespace android
