#pragma once

#include "ipc/bridge.h"
#include "mge/dlcomposite.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>

using VisibleCellSet = std::unordered_set<CellId, CellIdHash>;

enum class CompositeAdmissionResult : std::uint8_t {
    AlreadyResident,
    Admitted,
    BudgetRejected,
    UploadFailed,
};

inline bool compositeAdmissionSucceeded(CompositeAdmissionResult result) {
    return result == CompositeAdmissionResult::AlreadyResident ||
           result == CompositeAdmissionResult::Admitted;
}

struct CompositeRequestCounts {
    std::uint32_t budgetBlocked = 0;
    std::uint32_t retryDeferred = 0;
    std::uint32_t missing = 0;
    std::uint32_t pending = 0;
};

// Tracks non-resident composite request outcomes across frames. Budget failures remain
// blocked until visibility or available residency changes; absent cells remain negatively
// cached while visible; transient transport/upload failures retry with bounded backoff.
class CompositeRequestController {
public:
    void reset() {
        records_.clear();
        frameIndex_ = 0;
        budgetGeneration_ = 0;
        lastResidentBytes_ = 0;
        lastBudgetBytes_ = 0;
        capacityValid_ = false;
    }

    void beginFrame(const VisibleCellSet& visible, bool visibleSetChanged,
                    std::uint32_t residentBytes, std::uint32_t budgetBytes) {
        ++frameIndex_;
        if (!capacityValid_ || visibleSetChanged ||
            residentBytes < lastResidentBytes_ || budgetBytes != lastBudgetBytes_) {
            ++budgetGeneration_;
        }
        lastResidentBytes_ = residentBytes;
        lastBudgetBytes_ = budgetBytes;
        capacityValid_ = true;

        for (auto it = records_.begin(); it != records_.end();) {
            if (visible.find(it->first) == visible.end()) {
                it = records_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool shouldRequest(CellId cell, bool transitionTarget) {
        auto it = records_.find(cell);
        if (it == records_.end()) {
            return true;
        }

        RequestRecord& record = it->second;
        switch (record.status) {
        case RequestStatus::Pending:
        case RequestStatus::Missing:
            return false;
        case RequestStatus::BudgetBlocked:
            if (record.budgetGeneration != budgetGeneration_ ||
                (!record.transitionTarget && transitionTarget)) {
                records_.erase(it);
                return true;
            }
            record.transitionTarget = transitionTarget;
            return false;
        case RequestStatus::RetryDeferred:
            return frameIndex_ >= record.retryAfterFrame;
        }
        return false;
    }

    void markPending(CellId cell, bool transitionTarget) {
        RequestRecord& record = records_[cell];
        record.status = RequestStatus::Pending;
        record.transitionTarget = transitionTarget;
    }

    void markResident(CellId cell) {
        records_.erase(cell);
    }

    void markBudgetBlocked(CellId cell, bool transitionTarget) {
        RequestRecord& record = records_[cell];
        record.status = RequestStatus::BudgetBlocked;
        record.budgetGeneration = budgetGeneration_;
        record.retryAfterFrame = 0;
        record.failureCount = 0;
        record.transitionTarget = transitionTarget;
    }

    void markMissing(CellId cell) {
        RequestRecord& record = records_[cell];
        record.status = RequestStatus::Missing;
        record.retryAfterFrame = 0;
        record.failureCount = 0;
    }

    // Capacity deferral is not a transport failure. Retry on the next frame without
    // increasing the exponential failure backoff.
    void markDeferred(CellId cell) {
        RequestRecord& record = records_[cell];
        record.status = RequestStatus::RetryDeferred;
        record.retryAfterFrame = frameIndex_ + 1;
    }

    void markRetry(CellId cell) {
        RequestRecord& record = records_[cell];
        record.status = RequestStatus::RetryDeferred;
        if (record.failureCount < kMaxBackoffSteps) {
            ++record.failureCount;
        }

        std::uint64_t delay = kInitialRetryFrames;
        for (std::uint32_t step = 1; step < record.failureCount; ++step) {
            delay = delay >= kMaxRetryFrames / 2 ? kMaxRetryFrames : delay * 2;
        }
        record.retryAfterFrame = frameIndex_ + delay;
    }

    bool isPending(CellId cell) const {
        const auto it = records_.find(cell);
        return it != records_.end() && it->second.status == RequestStatus::Pending;
    }

    CompositeRequestCounts counts() const {
        CompositeRequestCounts result;
        for (const auto& item : records_) {
            switch (item.second.status) {
            case RequestStatus::BudgetBlocked:
                ++result.budgetBlocked;
                break;
            case RequestStatus::RetryDeferred:
                if (frameIndex_ < item.second.retryAfterFrame) {
                    ++result.retryDeferred;
                }
                break;
            case RequestStatus::Missing:
                ++result.missing;
                break;
            case RequestStatus::Pending:
                ++result.pending;
                break;
            }
        }
        return result;
    }

private:
    enum class RequestStatus : std::uint8_t {
        Pending,
        BudgetBlocked,
        RetryDeferred,
        Missing,
    };

    struct RequestRecord {
        RequestStatus status = RequestStatus::Pending;
        std::uint64_t retryAfterFrame = 0;
        std::uint64_t budgetGeneration = 0;
        std::uint32_t failureCount = 0;
        bool transitionTarget = false;
    };

    static constexpr std::uint64_t kInitialRetryFrames = 30;
    static constexpr std::uint64_t kMaxRetryFrames = 600;
    static constexpr std::uint32_t kMaxBackoffSteps = 6;

    std::unordered_map<CellId, RequestRecord, CellIdHash> records_;
    std::uint64_t frameIndex_ = 0;
    std::uint64_t budgetGeneration_ = 0;
    std::uint32_t lastResidentBytes_ = 0;
    std::uint32_t lastBudgetBytes_ = 0;
    bool capacityValid_ = false;
};

// Owns the bounded set of uploaded per-cell DXT1 composites used by distant terrain.
class ResidentCompositeCache {
public:
    ResidentCompositeCache();
    ~ResidentCompositeCache();

    void init(IDirect3DDevice9* device);
    std::uint64_t generation() const { return generation_; }
    void setBudgetMB(std::uint32_t mb);
    void setTransitionTargets(const VisibleCellSet& cells);
    void includeTransitionTargets(VisibleCellSet& cells) const;
    bool isTransitionTarget(CellId cell) const;
    void trimToBudget();

    // Returns the precise admission outcome so the streaming controller can distinguish a
    // stable budget miss from a transient device/upload failure.
    CompositeAdmissionResult admitWithResult(const CompositeChunkMsg& msg,
                                              const std::uint8_t* dxt1Bytes);

    // Compatibility convenience for callers that only need resident/not-resident.
    bool admit(const CompositeChunkMsg& msg, const std::uint8_t* dxt1Bytes);

    IDirect3DTexture9* lookup(CellId cell) const;
    IDirect3DTexture9* pin(CellId cell);
    void unpin(CellId cell);
    void evictNotVisible(const VisibleCellSet& visible);
    void releaseAll();

    std::uint32_t residentCount() const;
    std::uint32_t visibleResidentCount(const VisibleCellSet& visible) const;
    std::uint32_t residentBytes() const;
    std::uint32_t atlasServedThisFrame() const;
    void setAtlasServedThisFrame(std::uint32_t count);

    // Validate a Found payload's bounded DXT1 base/full-chain shape before callers allocate
    // or copy its byte range.
    static bool validatePayload(const CompositeChunkMsg& msg);
    static std::uint32_t compositeBytes(std::uint32_t edgeTexels);
    std::uint32_t budgetBytes() const { return budgetBytes_; }

private:
    struct ResidentComposite {
        CellId cell;
        IDirect3DTexture9* tex;
        std::uint32_t bytes;
        std::uint32_t lastSeenFrame;
        std::uint32_t pins;
        bool evictionPending;
    };

    bool uploadDXT1(const CompositeChunkMsg& msg, const std::uint8_t* dxt1Bytes,
                    IDirect3DTexture9** outTex) const;

    IDirect3DDevice9* device_;
    std::uint64_t generation_;
    std::uint32_t budgetBytes_;
    std::uint32_t residentBytes_;
    std::uint32_t atlasServedThisFrame_;
    VisibleCellSet transitionTargets_;
    std::unordered_map<CellId, ResidentComposite, CellIdHash> resident_;
};
