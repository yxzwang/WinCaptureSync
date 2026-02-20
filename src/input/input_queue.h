#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include "input/input_types.h"

namespace wcs::input {

class InputEventQueue {
public:
    explicit InputEventQueue(const size_t max_size) : max_size_(max_size) {}

    bool Push(const InputEvent& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return false;
        }

        if (queue_.size() >= max_size_) {
            // Preserve critical inputs under pressure by sacrificing older move samples.
            if (event.type != InputEventType::MouseMove) {
                const auto move_it = std::find_if(queue_.begin(), queue_.end(), [](const InputEvent& e) {
                    return e.type == InputEventType::MouseMove;
                });
                if (move_it != queue_.end()) {
                    queue_.erase(move_it);
                } else {
                    return false;
                }
            } else {
                return false;
            }
        }

        queue_.push_back(event);
        cv_.notify_one();
        return true;
    }

    size_t PopBatch(std::vector<InputEvent>* out,
                    const size_t max_batch,
                    const std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return stopped_ || !queue_.empty(); });
        if (queue_.empty()) {
            return 0;
        }

        const size_t count = (std::min)(max_batch, queue_.size());
        out->reserve(out->size() + count);
        for (size_t i = 0; i < count; ++i) {
            out->push_back(queue_.front());
            queue_.pop_front();
        }
        return count;
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    bool Stopped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopped_;
    }

private:
    const size_t max_size_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<InputEvent> queue_;
    bool stopped_ = false;
};

}  // namespace wcs::input
