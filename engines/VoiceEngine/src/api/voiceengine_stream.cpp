#include "voiceengine/voiceengine_api.h"
#include "core/internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kDefaultChunkMs = 2000;
constexpr uint32_t kDefaultOverlapMs = 200;
constexpr uint32_t kDefaultEndpointMs = 800;
constexpr uint32_t kDefaultPreRollMs = 200;
constexpr float kDefaultVadThreshold = 0.004f;
constexpr uint32_t kVadFrameMs = 20;
constexpr size_t kMaxEvents = 128;
constexpr uint32_t kMaxBacklogMs = 120000;
constexpr uint32_t kRollTargetMs = 30000;
constexpr uint32_t kRollHardMs = 45000;
constexpr uint32_t kRollSearchMs = 6000;
constexpr uint32_t kRollPauseMs = 200;
constexpr uint32_t kRollOverlapMs = 1600;
constexpr size_t kRollHoldbackChars = 6;
constexpr size_t kRollMinClauseChars = 3;
constexpr size_t kRollMaxClauseChars = 10;

struct StreamSnapshot {
    uint64_t segmentId = 0;
    uint64_t beginFrame = 0;
    uint64_t endFrame = 0;
    uint64_t overlapParentSegmentId = 0;
    bool final = false;
    bool rollFinal = false;
    std::vector<float> samples;
};

struct StreamEventRecord {
    ve_stream_event_type type = VE_STREAM_EVENT_NONE;
    uint64_t sequence = 0;
    uint64_t segmentId = 0;
    int64_t beginMs = 0;
    int64_t endMs = 0;
    bool stable = false;
    ve_status error = VE_OK;
    std::string text;
};

struct SegmentDecodeState {
    uint64_t beginFrame = 0;
    uint64_t endFrame = 0;
    bool initialized = false;
    std::string overlapParentText;
};

struct Utf8Unit {
    uint32_t codepoint = 0;
    size_t begin = 0;
    size_t end = 0;
};

std::vector<Utf8Unit> utf8Units(const std::string& text) {
    std::vector<Utf8Unit> units;
    size_t index = 0;
    while (index < text.size()) {
        const size_t begin = index;
        const unsigned char lead = static_cast<unsigned char>(text[index++]);
        uint32_t codepoint = lead;
        int continuation = 0;
        if ((lead & 0xE0) == 0xC0) {
            codepoint = lead & 0x1F;
            continuation = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            codepoint = lead & 0x0F;
            continuation = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            codepoint = lead & 0x07;
            continuation = 3;
        }
        bool valid = index + static_cast<size_t>(continuation) <= text.size();
        for (int part = 0; valid && part < continuation; ++part) {
            const unsigned char byte = static_cast<unsigned char>(text[index]);
            if ((byte & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (byte & 0x3F);
            ++index;
        }
        if (!valid) {
            index = begin + 1;
            codepoint = lead;
        }
        units.push_back({codepoint, begin, index});
    }
    return units;
}

bool isBoundaryPunctuation(uint32_t cp) {
    switch (cp) {
    case ',': case '.': case '!': case '?': case ';': case ':':
    case 0x3001: case 0x3002: case 0xFF01: case 0xFF0C:
    case 0xFF1A: case 0xFF1B: case 0xFF1F:
        return true;
    default:
        return false;
    }
}

bool isIgnoredForOverlap(uint32_t cp) {
    if (cp <= 0x20 || cp == 0x3000 || isBoundaryPunctuation(cp))
        return true;
    switch (cp) {
    case '"': case '\'': case '(': case ')': case '[': case ']':
    case '{': case '}': case '-': case '_':
    case 0x2018: case 0x2019: case 0x201C: case 0x201D:
    case 0xFF08: case 0xFF09: case 0x3010: case 0x3011:
        return true;
    default:
        return false;
    }
}

std::string stableRolledPrefix(const std::string& text) {
    const auto units = utf8Units(text);
    std::vector<size_t> significant;
    significant.reserve(units.size());
    for (size_t index = 0; index < units.size(); ++index) {
        if (!isIgnoredForOverlap(units[index].codepoint))
            significant.push_back(index);
    }
    if (significant.size() <= kRollHoldbackChars)
        return {};

    std::vector<size_t> suffixSignificant(units.size() + 1, 0);
    for (size_t index = units.size(); index-- > 0;) {
        suffixSignificant[index] = suffixSignificant[index + 1]
            + (isIgnoredForOverlap(units[index].codepoint) ? 0 : 1);
    }
    for (size_t index = units.size(); index-- > 0;) {
        if (!isBoundaryPunctuation(units[index].codepoint))
            continue;
        const size_t tailChars = suffixSignificant[index + 1];
        if (tailChars >= kRollMinClauseChars
            && tailChars <= kRollMaxClauseChars) {
            return text.substr(0, units[index].end);
        }
    }

    const size_t firstHeld = significant[
        significant.size() - kRollHoldbackChars];
    return text.substr(0, units[firstHeld].begin);
}

std::string removeRolledOverlap(const std::string& previous,
                                const std::string& current) {
    if (previous.empty() || current.empty())
        return current;

    const auto previousUnits = utf8Units(previous);
    const auto currentUnits = utf8Units(current);
    std::vector<uint32_t> previousText;
    std::vector<uint32_t> currentText;
    std::vector<size_t> currentEnds;
    for (const auto& unit : previousUnits) {
        if (!isIgnoredForOverlap(unit.codepoint))
            previousText.push_back(unit.codepoint);
    }
    for (const auto& unit : currentUnits) {
        if (!isIgnoredForOverlap(unit.codepoint)) {
            currentText.push_back(unit.codepoint);
            currentEnds.push_back(unit.end);
        }
    }
    const size_t limit = (std::min)(
        static_cast<size_t>(32),
        (std::min)(previousText.size(), currentText.size()));
    for (size_t count = limit; count >= 2; --count) {
        if (std::equal(previousText.end() - count, previousText.end(),
                       currentText.begin())) {
            size_t removeBytes = currentEnds[count - 1];
            while (removeBytes < current.size()) {
                const auto trailing = utf8Units(current.substr(removeBytes));
                if (trailing.empty()
                    || !isIgnoredForOverlap(trailing.front().codepoint)) {
                    break;
                }
                removeBytes += trailing.front().end;
            }
            return current.substr(removeBytes);
        }
    }
    return current;
}

class SpeechStream {
public:
    SpeechStream(voiceengine::Engine* engine, const ve_stream_params& params)
        : engine_(engine), params_(params) {
        sampleRate_ = params_.sample_rate;
        channels_ = params_.channels;
        chunkFrames_ = msToFrames(params_.chunk_ms);
        nextDecodeFrames_ = chunkFrames_;
        endpointFrames_ = msToFrames(params_.endpoint_silence_ms);
        preRollFrames_ = msToFrames(params_.pre_roll_ms);
        vadFrameSize_ = (std::max<uint64_t>)(1, msToFrames(kVadFrameMs));
        maxBacklogFrames_ = msToFrames(kMaxBacklogMs);
        rollTargetFrames_ = msToFrames(kRollTargetMs);
        rollHardFrames_ = msToFrames(kRollHardMs);
        rollSearchFrames_ = msToFrames(kRollSearchMs);
        rollPauseFrames_ = msToFrames(kRollPauseMs);
        rollOverlapFrames_ = msToFrames(kRollOverlapMs);
        worker_ = std::thread([this] { workerEntry(); });
    }

    ~SpeechStream() {
        cancel();
        if (worker_.joinable())
            worker_.join();
    }

    ve_status push(const float* samples, uint64_t frameCount) {
        if (!samples || frameCount == 0)
            return VE_ERR_ARGUMENT;
        if (frameCount > UINT64_MAX / static_cast<uint64_t>(channels_))
            return VE_ERR_ARGUMENT;

        std::lock_guard<std::mutex> lock(mutex_);
        if (finished_ || cancelled_)
            return VE_ERR_STATE;
        const uint64_t oldestFrame = oldestBufferedFrameLocked();
        const uint64_t acceptedEnd = totalFrames_ + frameCount;
        if (acceptedEnd < totalFrames_
            || acceptedEnd - oldestFrame > maxBacklogFrames_)
            return VE_ERR_BUSY;

        const uint64_t sampleCount = frameCount * static_cast<uint64_t>(channels_);
        pending_.reserve(pending_.size() + static_cast<size_t>(sampleCount));
        for (uint64_t frame = 0; frame < frameCount; ++frame) {
            double mono = 0.0;
            for (int32_t channel = 0; channel < channels_; ++channel) {
                const float value = samples[frame * static_cast<uint64_t>(channels_)
                                            + static_cast<uint64_t>(channel)];
                pending_.push_back(value);
                mono += value;
            }
            mono /= static_cast<double>(channels_);
            vadEnergy_ += mono * mono;
            ++vadFrames_;
            ++totalFrames_;
            if (vadFrames_ >= vadFrameSize_)
                evaluateVadWindowLocked();
        }
        condition_.notify_all();
        return VE_OK;
    }

    ve_status poll(ve_stream_event* out) {
        if (!out)
            return VE_ERR_ARGUMENT;
        const size_t capacity = out->struct_size;
        const size_t minimum = offsetof(ve_stream_event, text_utf8)
            + sizeof(out->text_utf8);
        if (capacity < minimum)
            return VE_ERR_ARGUMENT;
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.empty())
            return VE_ERR_BUSY;
        const StreamEventRecord event = std::move(events_.front());
        events_.pop_front();
        ve_stream_event result{};
        result.struct_size = sizeof(result);
        result.api_version = VOICEENGINE_API_VERSION;
        result.type = event.type;
        result.sequence = event.sequence;
        result.segment_id = event.segmentId;
        result.begin_ms = event.beginMs;
        result.end_ms = event.endMs;
        result.is_stable = event.stable ? 1 : 0;
        result.error = event.error;
        result.text_bytes = event.text.size() > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(event.text.size());
        result.text_truncated = event.text.size() >= sizeof(result.text_utf8)
            ? 1 : 0;
        std::snprintf(result.text_utf8, sizeof(result.text_utf8), "%s",
                      event.text.c_str());
        std::memcpy(out, &result, (std::min)(capacity, sizeof(result)));
        return VE_OK;
    }

    ve_status flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (finished_ || cancelled_)
            return VE_ERR_STATE;
        completeVadWindowLocked();
        finalizeSegmentLocked();
        condition_.notify_all();
        return VE_OK;
    }

    ve_status finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_)
            return VE_ERR_STATE;
        if (finished_)
            return VE_OK;
        completeVadWindowLocked();
        finalizeSegmentLocked();
        pending_.clear();
        finished_ = true;
        condition_.notify_all();
        return VE_OK;
    }

    ve_status cancel() {
        uint64_t activeTask = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (workerDone_)
                return VE_OK;
            cancelled_ = true;
            finished_ = true;
            snapshots_.clear();
            pending_.clear();
            activeTask = activeTask_;
        }
        if (activeTask != 0 && engine_)
            engine_->cancel(activeTask);
        condition_.notify_all();
        return VE_OK;
    }

private:
    void workerEntry() noexcept {
        try {
            workerLoop();
        } catch (const std::exception& error) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                workerDone_ = true;
                finished_ = true;
            }
            try {
                pushEvent(VE_STREAM_EVENT_ERROR, 0, totalFrames_, totalFrames_,
                          true, VE_ERR_INTERNAL, error.what());
                pushEvent(VE_STREAM_EVENT_END, 0, totalFrames_, totalFrames_,
                          true, VE_ERR_INTERNAL, {});
            } catch (...) {
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                workerDone_ = true;
                finished_ = true;
            }
            try {
                pushEvent(VE_STREAM_EVENT_ERROR, 0, totalFrames_, totalFrames_,
                          true, VE_ERR_INTERNAL, "unhandled stream exception");
                pushEvent(VE_STREAM_EVENT_END, 0, totalFrames_, totalFrames_,
                          true, VE_ERR_INTERNAL, {});
            } catch (...) {
            }
        }
    }

    uint64_t msToFrames(uint32_t ms) const {
        return (static_cast<uint64_t>(sampleRate_) * ms + 999) / 1000;
    }

    int64_t frameToMs(uint64_t frame) const {
        return static_cast<int64_t>(frame * 1000
                                    / static_cast<uint64_t>(sampleRate_));
    }

    uint64_t pendingFramesLocked() const {
        return pending_.size() / static_cast<size_t>(channels_);
    }

    uint64_t oldestBufferedFrameLocked() const {
        uint64_t oldest = totalFrames_;
        if (!pending_.empty())
            oldest = (std::min)(oldest, pendingStartFrame_);
        if (activeSnapshot_)
            oldest = (std::min)(oldest, activeSnapshotBeginFrame_);
        for (const auto& snapshot : snapshots_) {
            if (!snapshot.samples.empty()) {
                oldest = (std::min)(oldest, snapshot.beginFrame);
                break;
            }
        }
        return oldest;
    }

    void completeVadWindowLocked() {
        if (vadFrames_ > 0)
            evaluateVadWindowLocked();
    }

    void evaluateVadWindowLocked() {
        if (vadFrames_ == 0)
            return;
        const double rms = std::sqrt(vadEnergy_ / static_cast<double>(vadFrames_));
        const bool voice = rms >= static_cast<double>(params_.vad_rms_threshold);
        const uint64_t windowFrames = vadFrames_;
        vadEnergy_ = 0.0;
        vadFrames_ = 0;

        if (voice) {
            inSpeech_ = true;
            silenceFrames_ = 0;
            continuationIdleFrames_ = 0;
        } else if (inSpeech_) {
            silenceFrames_ += windowFrames;
        } else if (continuationSegmentId_ != 0) {
            continuationIdleFrames_ += windowFrames;
            if (continuationIdleFrames_ >= endpointFrames_) {
                continuationSegmentId_ = 0;
                continuationIdleFrames_ = 0;
            }
        }

        if (!inSpeech_) {
            trimToPreRollLocked();
            return;
        }

        enqueueReadySnapshotLocked();
        considerRollingLocked(rms);
        if (inSpeech_ && silenceFrames_ >= endpointFrames_)
            finalizeSegmentLocked();
    }

    void trimToPreRollLocked() {
        const uint64_t frames = pendingFramesLocked();
        if (frames <= preRollFrames_)
            return;
        const uint64_t removeFrames = frames - preRollFrames_;
        const size_t removeSamples = static_cast<size_t>(
            removeFrames * static_cast<uint64_t>(channels_));
        pending_.erase(pending_.begin(), pending_.begin() + removeSamples);
        pendingStartFrame_ += removeFrames;
    }

    void resetRollCandidateLocked() {
        rollCandidateFrame_ = 0;
        rollCandidateRms_ = (std::numeric_limits<double>::max)();
    }

    void considerRollingLocked(double rms) {
        const uint64_t frames = pendingFramesLocked();
        const uint64_t searchStart = rollHardFrames_ > rollSearchFrames_
            ? rollHardFrames_ - rollSearchFrames_ : rollTargetFrames_;
        if (frames >= searchStart && rms < rollCandidateRms_) {
            rollCandidateRms_ = rms;
            rollCandidateFrame_ = totalFrames_;
        }

        if (frames >= rollTargetFrames_ && silenceFrames_ >= rollPauseFrames_) {
            rolloverSegmentLocked(totalFrames_, true);
            return;
        }
        if (frames < rollHardFrames_)
            return;

        const uint64_t hardCut = pendingStartFrame_ + rollHardFrames_;
        uint64_t cutFrame = rollCandidateFrame_;
        if (cutFrame <= pendingStartFrame_ || cutFrame > totalFrames_)
            cutFrame = (std::min)(hardCut, totalFrames_);
        rolloverSegmentLocked(cutFrame, false);
    }

    void enqueueReadySnapshotLocked() {
        const uint64_t frames = pendingFramesLocked();
        if (frames < nextDecodeFrames_)
            return;
        enqueueSnapshotLocked(frames, false);
        do {
            nextDecodeFrames_ += chunkFrames_;
        } while (nextDecodeFrames_ <= frames);
    }

    void enqueueSnapshotLocked(uint64_t frameCount, bool final,
                               bool rollFinal = false) {
        StreamSnapshot snapshot;
        snapshot.segmentId = segmentId_;
        snapshot.beginFrame = pendingStartFrame_;
        snapshot.endFrame = pendingStartFrame_ + frameCount;
        snapshot.overlapParentSegmentId = continuationSegmentId_;
        snapshot.final = final;
        snapshot.rollFinal = rollFinal;
        const size_t sampleCount = static_cast<size_t>(
            frameCount * static_cast<uint64_t>(channels_));
        snapshot.samples.assign(pending_.begin(), pending_.begin() + sampleCount);

        for (auto it = snapshots_.begin(); it != snapshots_.end();) {
            if (it->segmentId == segmentId_ && !it->final) {
                it = snapshots_.erase(it);
            } else {
                ++it;
            }
        }
        snapshots_.push_back(std::move(snapshot));
        segmentHasSnapshots_ = true;
    }

    void rolloverSegmentLocked(uint64_t cutFrame, bool endedOnShortPause) {
        const uint64_t availableEnd = pendingStartFrame_ + pendingFramesLocked();
        if (cutFrame <= pendingStartFrame_ || cutFrame > availableEnd)
            return;

        const uint64_t oldSegmentId = segmentId_;
        const uint64_t cutFrames = cutFrame - pendingStartFrame_;
        const bool audioBridge = !endedOnShortPause;
        enqueueSnapshotLocked(cutFrames, true, audioBridge);

        uint64_t nextStartFrame = cutFrame;
        if (audioBridge) {
            const uint64_t availableOverlap = cutFrame - pendingStartFrame_;
            nextStartFrame -= (std::min)(rollOverlapFrames_, availableOverlap);
        }
        const uint64_t removeFrames = nextStartFrame - pendingStartFrame_;
        const size_t removeSamples = static_cast<size_t>(
            removeFrames * static_cast<uint64_t>(channels_));
        pending_.erase(pending_.begin(), pending_.begin() + removeSamples);
        pendingStartFrame_ = nextStartFrame;
        segmentHasSnapshots_ = false;
        nextDecodeFrames_ = chunkFrames_;
        ++segmentId_;
        continuationSegmentId_ = audioBridge ? oldSegmentId : 0;
        continuationIdleFrames_ = 0;
        resetRollCandidateLocked();

        if (endedOnShortPause) {
            inSpeech_ = false;
            silenceFrames_ = 0;
        } else {
            inSpeech_ = true;
        }
        enqueueReadySnapshotLocked();
    }

    void finalizeSegmentLocked() {
        if (!inSpeech_ && !segmentHasSnapshots_)
            return;
        const uint64_t frames = pendingFramesLocked();
        if (frames == 0 && !segmentHasSnapshots_) {
            inSpeech_ = false;
            silenceFrames_ = 0;
            continuationSegmentId_ = 0;
            continuationIdleFrames_ = 0;
            resetRollCandidateLocked();
            return;
        }
        if (frames > 0) {
            enqueueSnapshotLocked(frames, true);
        } else {
            StreamSnapshot marker;
            marker.segmentId = segmentId_;
            marker.beginFrame = pendingStartFrame_;
            marker.endFrame = pendingStartFrame_;
            marker.overlapParentSegmentId = continuationSegmentId_;
            marker.final = true;
            snapshots_.push_back(std::move(marker));
        }
        pending_.clear();
        pendingStartFrame_ = totalFrames_;
        inSpeech_ = false;
        silenceFrames_ = 0;
        segmentHasSnapshots_ = false;
        nextDecodeFrames_ = chunkFrames_;
        continuationSegmentId_ = 0;
        continuationIdleFrames_ = 0;
        resetRollCandidateLocked();
        ++segmentId_;
    }

    void pushEvent(ve_stream_event_type type,
                   uint64_t segmentId,
                   uint64_t beginFrame,
                   uint64_t endFrame,
                   bool stable,
                   ve_status error,
                   std::string text) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.size() >= kMaxEvents) {
            const auto partial = std::find_if(
                events_.begin(), events_.end(), [](const StreamEventRecord& event) {
                    return event.type == VE_STREAM_EVENT_PARTIAL;
                });
            if (partial != events_.end())
                events_.erase(partial);
            else if (type == VE_STREAM_EVENT_PARTIAL)
                return;
        }
        StreamEventRecord event;
        event.type = type;
        event.sequence = ++eventSequence_;
        event.segmentId = segmentId;
        event.beginMs = frameToMs(beginFrame);
        event.endMs = frameToMs(endFrame);
        event.stable = stable;
        event.error = error;
        event.text = std::move(text);
        events_.push_back(std::move(event));
    }

    bool streamCancelled() {
        std::lock_guard<std::mutex> lock(mutex_);
        return cancelled_;
    }

    ve_status runSnapshot(const StreamSnapshot& snapshot, std::string* text) {
        if (snapshot.samples.empty()) {
            text->clear();
            return VE_OK;
        }
        ve_pcm_desc pcm{};
        pcm.struct_size = sizeof(pcm);
        pcm.api_version = VOICEENGINE_API_VERSION;
        pcm.samples = snapshot.samples.data();
        pcm.frame_count = static_cast<uint64_t>(snapshot.samples.size())
            / static_cast<uint64_t>(channels_);
        pcm.sample_rate = sampleRate_;
        pcm.channels = channels_;
        uint64_t taskId = 0;
        const ve_status submitted = engine_->submit_stream_pcm(&pcm, &taskId);
        if (submitted != VE_OK) {
            *text = engine_->last_error();
            return submitted;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeTask_ = taskId;
        }

        ve_task_status task{};
        for (;;) {
            if (streamCancelled())
                engine_->cancel(taskId);
            task.struct_size = sizeof(task);
            task.api_version = VOICEENGINE_API_VERSION;
            const ve_status polled = engine_->poll(taskId, &task);
            if (polled != VE_OK) {
                *text = engine_->last_error();
                std::lock_guard<std::mutex> lock(mutex_);
                activeTask_ = 0;
                return polled;
            }
            if (task.state == VE_TASK_DONE
                || task.state == VE_TASK_FAILED
                || task.state == VE_TASK_CANCELLED) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ve_result result{};
        result.struct_size = sizeof(result);
        result.api_version = VOICEENGINE_API_VERSION;
        const ve_status resultStatus = engine_->result(taskId, &result);
        if (result.text_utf8 && result.text_utf8[0])
            *text = result.text_utf8;
        else if (resultStatus != VE_OK)
            *text = std::string("stream inference failed (status ")
                + std::to_string(static_cast<int>(resultStatus)) + ")";
        voiceengine_result_free(&result);
        engine_->release(taskId);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeTask_ = 0;
        }
        return resultStatus;
    }

    void emitCancelledAndEnd() {
        pushEvent(VE_STREAM_EVENT_ERROR, 0, totalFrames_, totalFrames_,
                  true, VE_ERR_CANCELLED, "cancelled");
        pushEvent(VE_STREAM_EVENT_END, 0, totalFrames_, totalFrames_,
                  true, VE_ERR_CANCELLED, {});
    }

    void workerLoop() {
        for (;;) {
            StreamSnapshot snapshot;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] {
                    return cancelled_ || !snapshots_.empty() || finished_;
                });
                if (cancelled_) {
                    workerDone_ = true;
                    lock.unlock();
                    emitCancelledAndEnd();
                    return;
                }
                if (snapshots_.empty()) {
                    if (finished_) {
                        workerDone_ = true;
                        const uint64_t endFrame = totalFrames_;
                        lock.unlock();
                        pushEvent(VE_STREAM_EVENT_END, 0, endFrame, endFrame,
                                  true, VE_OK, {});
                        return;
                    }
                    continue;
                }
                snapshot = std::move(snapshots_.front());
                snapshots_.pop_front();
                activeSnapshot_ = !snapshot.samples.empty();
                activeSnapshotBeginFrame_ = snapshot.beginFrame;
                auto& state = segmentStates_[snapshot.segmentId];
                if (state.beginFrame == 0)
                    state.beginFrame = snapshot.beginFrame;
                state.endFrame = snapshot.endFrame;
            }

            SegmentDecodeState& state = segmentStates_[snapshot.segmentId];
            if (!state.initialized) {
                state.initialized = true;
                if (snapshot.overlapParentSegmentId != 0) {
                    const auto parent = rollFinalTexts_.find(
                        snapshot.overlapParentSegmentId);
                    if (parent != rollFinalTexts_.end()) {
                        state.overlapParentText = std::move(parent->second);
                        rollFinalTexts_.erase(parent);
                    }
                }
            }
            std::string snapshotText;
            const ve_status snapshotStatus = runSnapshot(snapshot, &snapshotText);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                activeSnapshot_ = false;
            }
            if (snapshotStatus != VE_OK) {
                const ve_status status = streamCancelled()
                    ? VE_ERR_CANCELLED : snapshotStatus;
                pushEvent(VE_STREAM_EVENT_ERROR, snapshot.segmentId,
                          snapshot.beginFrame, snapshot.endFrame, true,
                          status, snapshotText);
                if (snapshot.final) {
                    if (snapshot.rollFinal)
                        rollFinalTexts_[snapshot.segmentId].clear();
                    segmentStates_.erase(snapshot.segmentId);
                }
                continue;
            }

            const std::string displayText = removeRolledOverlap(
                state.overlapParentText, snapshotText);
            if (snapshot.final) {
                const std::string stableText = snapshot.rollFinal
                    ? stableRolledPrefix(displayText) : displayText;
                if (snapshot.rollFinal)
                    rollFinalTexts_[snapshot.segmentId] = stableText;
                pushEvent(VE_STREAM_EVENT_FINAL, snapshot.segmentId,
                          state.beginFrame, state.endFrame,
                          true, VE_OK, stableText);
                segmentStates_.erase(snapshot.segmentId);
            } else {
                pushEvent(VE_STREAM_EVENT_PARTIAL, snapshot.segmentId,
                          state.beginFrame, state.endFrame,
                          false, VE_OK, displayText);
            }
        }
    }

    voiceengine::Engine* engine_ = nullptr;
    ve_stream_params params_{};
    int32_t sampleRate_ = 16000;
    int32_t channels_ = 1;
    uint64_t chunkFrames_ = 0;
    uint64_t nextDecodeFrames_ = 0;
    uint64_t endpointFrames_ = 0;
    uint64_t preRollFrames_ = 0;
    uint64_t vadFrameSize_ = 0;
    uint64_t maxBacklogFrames_ = 0;
    uint64_t rollTargetFrames_ = 0;
    uint64_t rollHardFrames_ = 0;
    uint64_t rollSearchFrames_ = 0;
    uint64_t rollPauseFrames_ = 0;
    uint64_t rollOverlapFrames_ = 0;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    std::deque<StreamSnapshot> snapshots_;
    std::deque<StreamEventRecord> events_;
    std::vector<float> pending_;
    uint64_t pendingStartFrame_ = 0;
    uint64_t totalFrames_ = 0;
    uint64_t vadFrames_ = 0;
    double vadEnergy_ = 0.0;
    uint64_t silenceFrames_ = 0;
    uint64_t continuationSegmentId_ = 0;
    uint64_t continuationIdleFrames_ = 0;
    uint64_t rollCandidateFrame_ = 0;
    double rollCandidateRms_ = (std::numeric_limits<double>::max)();
    bool inSpeech_ = false;
    bool segmentHasSnapshots_ = false;
    uint64_t segmentId_ = 1;
    uint64_t eventSequence_ = 0;
    uint64_t activeTask_ = 0;
    uint64_t activeSnapshotBeginFrame_ = 0;
    bool activeSnapshot_ = false;
    bool finished_ = false;
    bool cancelled_ = false;
    bool workerDone_ = false;

    std::map<uint64_t, SegmentDecodeState> segmentStates_;
    std::map<uint64_t, std::string> rollFinalTexts_;
};

ve_stream_params normalizedParams(const ve_stream_params* input) {
    ve_stream_params params{};
    params.struct_size = sizeof(params);
    params.api_version = VOICEENGINE_API_VERSION;
    params.sample_rate = input && input->sample_rate > 0
        ? input->sample_rate : 16000;
    params.channels = input && input->channels > 0 ? input->channels : 1;
    params.chunk_ms = input && input->chunk_ms > 0
        ? input->chunk_ms : kDefaultChunkMs;
    params.overlap_ms = input && input->overlap_ms > 0
        ? input->overlap_ms : kDefaultOverlapMs;
    params.endpoint_silence_ms = input && input->endpoint_silence_ms > 0
        ? input->endpoint_silence_ms : kDefaultEndpointMs;
    params.pre_roll_ms = input && input->pre_roll_ms > 0
        ? input->pre_roll_ms : kDefaultPreRollMs;
    params.vad_rms_threshold = input && input->vad_rms_threshold > 0
        ? input->vad_rms_threshold : kDefaultVadThreshold;
    return params;
}

bool validParams(const ve_stream_params& params) {
    return params.sample_rate >= 8000
        && params.sample_rate <= 192000
        && params.channels >= 1
        && params.channels <= 8
        && params.chunk_ms >= 500
        && params.chunk_ms <= 10000
        && params.overlap_ms < params.chunk_ms
        && params.endpoint_silence_ms >= 100
        && params.endpoint_silence_ms <= 10000
        && params.pre_roll_ms <= 2000
        && params.vad_rms_threshold > 0.0f
        && params.vad_rms_threshold < 1.0f;
}

SpeechStream* fromStream(ve_stream* stream) {
    return reinterpret_cast<SpeechStream*>(stream);
}

}  // namespace

ve_status voiceengine_stream_create(ve_context* ctx,
                                   const ve_stream_params* params,
                                   ve_stream** out_stream) {
    auto* engine = voiceengine::engine_from_handle(ctx);
    if (!engine || !out_stream)
        return VE_ERR_ARGUMENT;
    *out_stream = nullptr;
    if (!engine->loaded())
        return VE_ERR_MODEL;
    const size_t minimum = offsetof(ve_stream_params, vad_rms_threshold)
        + sizeof(params->vad_rms_threshold);
    if (params && (params->struct_size < minimum
        || (params->api_version != 0
            && params->api_version != VOICEENGINE_API_VERSION))) {
        return VE_ERR_ARGUMENT;
    }
    const ve_stream_params normalized = normalizedParams(params);
    if (!validParams(normalized))
        return VE_ERR_ARGUMENT;
    try {
        *out_stream = reinterpret_cast<ve_stream*>(
            new SpeechStream(engine, normalized));
        return VE_OK;
    } catch (const std::bad_alloc&) {
        return VE_ERR_INTERNAL;
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_stream_push_f32(ve_stream* stream,
                                     const float* interleaved_samples,
                                     uint64_t frame_count) {
    SpeechStream* instance = fromStream(stream);
    if (!instance)
        return VE_ERR_ARGUMENT;
    try {
        return instance->push(interleaved_samples, frame_count);
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_stream_poll(ve_stream* stream, ve_stream_event* out_event) {
    SpeechStream* instance = fromStream(stream);
    if (!instance)
        return VE_ERR_ARGUMENT;
    try {
        return instance->poll(out_event);
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_stream_flush(ve_stream* stream) {
    SpeechStream* instance = fromStream(stream);
    if (!instance)
        return VE_ERR_ARGUMENT;
    try {
        return instance->flush();
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_stream_finish(ve_stream* stream) {
    SpeechStream* instance = fromStream(stream);
    if (!instance)
        return VE_ERR_ARGUMENT;
    try {
        return instance->finish();
    } catch (...) {
        return VE_ERR_INTERNAL;
    }
}

ve_status voiceengine_stream_cancel(ve_stream* stream) {
    SpeechStream* instance = fromStream(stream);
    return instance ? instance->cancel() : VE_ERR_ARGUMENT;
}

void voiceengine_stream_destroy(ve_stream* stream) {
    delete fromStream(stream);
}
