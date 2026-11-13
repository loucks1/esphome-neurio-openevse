#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>

namespace esphome {
namespace neurio_evse {

// Queue capacity in hourly records: 14 days, aligned with neurio-recorder's
// 14-day settle window for late-arriving cloud data, so an ESP32 backlog
// outlives the window in which the recorder might still replace its hours.
inline constexpr size_t kOutageQueueCapacity = 336;

class OutageQueue {
 public:
  explicit OutageQueue(size_t max_capacity = kOutageQueueCapacity) : max_capacity_(max_capacity) {}

  // Adds a new record ID. If max capacity is exceeded, evicts and returns the oldest ID.
  // Returns 0 if no record was evicted.
  uint32_t push_record(uint32_t id) {
    uint32_t evicted_id = 0;
    if (this->pending_ids_.size() >= this->max_capacity_) {
      evicted_id = this->pending_ids_.front();
      this->pending_ids_.erase(this->pending_ids_.begin());
    }
    this->pending_ids_.push_back(id);
    return evicted_id;
  }

  // Processes an acknowledgment for a given ID.
  // Returns true if the ID was present and removed, false otherwise.
  bool handle_ack(uint32_t id) {
    auto it = std::find(this->pending_ids_.begin(), this->pending_ids_.end(), id);
    if (it != this->pending_ids_.end()) {
      this->pending_ids_.erase(it);
      return true;
    }
    return false;
  }

  size_t size() const { return this->pending_ids_.size(); }
  size_t capacity() const { return this->max_capacity_; }
  bool empty() const { return this->pending_ids_.empty(); }

  uint32_t oldest_id() const {
    if (this->pending_ids_.empty()) {
      return 0;
    }
    return this->pending_ids_.front();
  }

  const std::vector<uint32_t> &get_pending_ids() const {
    return this->pending_ids_;
  }

  void load_ids(const std::vector<uint32_t> &ids) {
    this->pending_ids_ = ids;
    std::sort(this->pending_ids_.begin(), this->pending_ids_.end());
    // Enforce capacity on loaded IDs
    while (this->pending_ids_.size() > this->max_capacity_) {
      this->pending_ids_.erase(this->pending_ids_.begin());
    }
  }

  void clear() { this->pending_ids_.clear(); }

 private:
  size_t max_capacity_{kOutageQueueCapacity};
  std::vector<uint32_t> pending_ids_;
};

}  // namespace neurio_evse
}  // namespace esphome
