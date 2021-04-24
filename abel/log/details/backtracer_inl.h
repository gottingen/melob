// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

namespace abel {
namespace details {
ABEL_FORCE_INLINE backtracer::backtracer(const backtracer &other) {
    std::lock_guard<std::mutex> lock(other.mutex_);
    enabled_ = other.enabled();
    messages_ = other.messages_;
}

ABEL_FORCE_INLINE backtracer::backtracer(backtracer &&other) ABEL_NOEXCEPT {
    std::lock_guard<std::mutex> lock(other.mutex_);
    enabled_ = other.enabled();
    messages_ = std::move(other.messages_);
}

ABEL_FORCE_INLINE backtracer &backtracer::operator=(backtracer other) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = other.enabled();
    messages_ = std::move(other.messages_);
    return *this;
}

ABEL_FORCE_INLINE void backtracer::enable(size_t size) {
    std::lock_guard<std::mutex> lock{mutex_};
    enabled_.store(true, std::memory_order_relaxed);
    messages_ = circular_q<log_msg_buffer>{size};
}

ABEL_FORCE_INLINE void backtracer::disable() {
    std::lock_guard<std::mutex> lock{mutex_};
    enabled_.store(false, std::memory_order_relaxed);
}

ABEL_FORCE_INLINE bool backtracer::enabled() const {
    return enabled_.load(std::memory_order_relaxed);
}

ABEL_FORCE_INLINE void backtracer::push_back(const log_msg &msg) {
    std::lock_guard<std::mutex> lock{mutex_};
    messages_.push_back(log_msg_buffer{msg});
}

// pop all items in the q and apply the given fun on each of them.
ABEL_FORCE_INLINE void backtracer::foreach_pop(std::function<void(const details::log_msg &)> fun) {
    std::lock_guard<std::mutex> lock{mutex_};
    while (!messages_.empty()) {
        auto &front_msg = messages_.front();
        fun(front_msg);
        messages_.pop_front();
    }
}
} // namespace details
}  // namespace abel
