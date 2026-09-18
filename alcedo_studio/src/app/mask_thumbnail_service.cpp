//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/mask_thumbnail_service.hpp"

#include <QMetaObject>
#include <QObject>
#include <QPointer>

#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/mask_thumbnail_evaluate.hpp"

namespace alcedo {
namespace {

struct SpecHash {
  auto operator()(const MaskThumbnailSpec& spec) const noexcept -> std::size_t {
    return static_cast<std::size_t>(spec.key);
  }
};

class MaskThumbnailLru {
 public:
  explicit MaskThumbnailLru(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  [[nodiscard]] auto Get(const MaskThumbnailSpec& spec) -> std::optional<QImage> {
    auto found = map_.find(spec);
    if (found == map_.end()) {
      return std::nullopt;
    }
    order_.splice(order_.begin(), order_, found->second.order);
    return found->second.image;
  }

  [[nodiscard]] auto Peek(const MaskThumbnailSpec& spec) const -> std::optional<QImage> {
    const auto found = map_.find(spec);
    if (found == map_.end()) {
      return std::nullopt;
    }
    return found->second.image;
  }

  void Put(const MaskThumbnailSpec& spec, QImage image) {
    auto found = map_.find(spec);
    if (found != map_.end()) {
      found->second.image = std::move(image);
      order_.splice(order_.begin(), order_, found->second.order);
      return;
    }
    while (map_.size() >= capacity_ && !order_.empty()) {
      map_.erase(order_.back());
      order_.pop_back();
    }
    order_.push_front(spec);
    Entry entry;
    entry.image = std::move(image);
    entry.order = order_.begin();
    map_.emplace(spec, std::move(entry));
  }

  [[nodiscard]] auto size() const -> std::size_t { return map_.size(); }
  [[nodiscard]] auto capacity() const -> std::size_t { return capacity_; }

 private:
  struct Entry {
    QImage                                 image;
    std::list<MaskThumbnailSpec>::iterator order;
  };

  std::size_t                                           capacity_ = kMaskThumbnailCacheCapacity;
  std::list<MaskThumbnailSpec>                          order_;
  std::unordered_map<MaskThumbnailSpec, Entry, SpecHash> map_;
};

}  // namespace

class MaskThumbnailService::Impl {
 public:
  explicit Impl(std::size_t capacity) : cache_(capacity) {
    dispatcher_ = [poster = &poster_](QObject*, std::function<void()> fn) {
      if (fn == nullptr) {
        return;
      }
      QMetaObject::invokeMethod(poster, [fn = std::move(fn)]() { fn(); }, Qt::QueuedConnection);
    };
    runner_ = [this](std::function<void()> job) { Enqueue(std::move(job)); };
    worker_ = std::thread([this] { WorkerLoop(); });
  }

  ~Impl() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void SetJobRunner(JobRunner runner) {
    std::lock_guard lock(mutex_);
    if (runner) {
      runner_ = std::move(runner);
    }
  }

  void SetResultDispatcher(ResultDispatcher dispatcher) {
    std::lock_guard lock(mutex_);
    if (dispatcher) {
      dispatcher_ = std::move(dispatcher);
    }
  }

  void Request(MaskThumbnailSpec spec, QObject* receiver, std::uint64_t request_id,
               Callback callback) {
    JobRunner             runner;
    ResultDispatcher      dispatcher;
    std::optional<QImage> hit;
    {
      std::lock_guard lock(mutex_);
      if (stop_) {
        return;
      }
      hit        = cache_.Get(spec);
      runner     = runner_;
      dispatcher = dispatcher_;
    }
    QPointer<QObject> alive(receiver);
    if (hit.has_value()) {
      PostResult(alive, request_id, std::move(spec), *hit, QString(), std::move(callback),
                 std::move(dispatcher));
      return;
    }
    runner([this, spec = std::move(spec), alive, request_id, callback = std::move(callback),
            dispatcher = std::move(dispatcher)]() mutable {
      RunJob(std::move(spec), alive, request_id, std::move(callback), std::move(dispatcher));
    });
  }

  [[nodiscard]] auto size() const -> std::size_t {
    std::lock_guard lock(mutex_);
    return cache_.size();
  }

  [[nodiscard]] auto capacity() const -> std::size_t {
    std::lock_guard lock(mutex_);
    return cache_.capacity();
  }

  [[nodiscard]] auto generate_count() const -> std::uint64_t {
    return generate_count_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto Cached(const MaskThumbnailSpec& spec) const -> std::optional<QImage> {
    std::lock_guard lock(mutex_);
    return cache_.Peek(spec);
  }

 private:
  void Enqueue(std::function<void()> job) {
    {
      std::lock_guard lock(mutex_);
      if (stop_) {
        return;
      }
      jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

  void WorkerLoop() {
    while (true) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) {
          return;
        }
        job = std::move(jobs_.front());
        jobs_.erase(jobs_.begin());
      }
      if (job) {
        job();
      }
    }
  }

  void RunJob(MaskThumbnailSpec spec, QPointer<QObject> alive, std::uint64_t request_id,
              Callback callback, ResultDispatcher dispatcher) {
    QImage  image;
    QString error;
    try {
      std::optional<QImage> hit;
      {
        std::lock_guard lock(mutex_);
        if (stop_) {
          return;
        }
        hit = cache_.Get(spec);
      }
      if (hit.has_value()) {
        image = *hit;
      } else {
        image = RenderMaskThumbnail(spec);
        {
          std::lock_guard lock(mutex_);
          if (stop_) {
            return;
          }
          cache_.Put(spec, image);
        }
        generate_count_.fetch_add(1, std::memory_order_acq_rel);
      }
    } catch (const std::exception& ex) {
      error = QString::fromUtf8(ex.what());
      image = QImage();
    }
    if (!callback || !dispatcher) {
      return;
    }
    {
      std::lock_guard lock(mutex_);
      if (stop_) {
        return;
      }
    }
    PostResult(alive, request_id, std::move(spec), std::move(image), std::move(error),
               std::move(callback), std::move(dispatcher));
  }

  static void PostResult(QPointer<QObject> alive, std::uint64_t request_id, MaskThumbnailSpec spec,
                         QImage image, QString error, Callback callback,
                         ResultDispatcher dispatcher) {
    if (!callback || !dispatcher) {
      return;
    }
    dispatcher(alive.data(), [alive, callback = std::move(callback), request_id,
                              spec = std::move(spec), image = std::move(image),
                              error = std::move(error)]() mutable {
      if (!alive || !callback) {
        return;
      }
      callback(request_id, std::move(spec), std::move(image), std::move(error));
    });
  }

  mutable std::mutex                 mutex_;
  std::condition_variable            cv_;
  MaskThumbnailLru                   cache_;
  std::vector<std::function<void()>> jobs_;
  JobRunner                          runner_;
  ResultDispatcher                   dispatcher_;
  QObject                            poster_;
  std::thread                        worker_;
  std::atomic<std::uint64_t>         generate_count_{0};
  bool                               stop_ = false;
};

MaskThumbnailService::MaskThumbnailService(std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}

MaskThumbnailService::~MaskThumbnailService() = default;

void MaskThumbnailService::Request(MaskThumbnailSpec spec, QObject* receiver,
                                   std::uint64_t request_id, Callback callback) {
  impl_->Request(std::move(spec), receiver, request_id, std::move(callback));
}

void MaskThumbnailService::SetJobRunner(JobRunner runner) { impl_->SetJobRunner(std::move(runner)); }

void MaskThumbnailService::SetResultDispatcher(ResultDispatcher dispatcher) {
  impl_->SetResultDispatcher(std::move(dispatcher));
}

auto MaskThumbnailService::size() const -> std::size_t { return impl_->size(); }

auto MaskThumbnailService::capacity() const -> std::size_t { return impl_->capacity(); }

auto MaskThumbnailService::generate_count() const -> std::uint64_t {
  return impl_->generate_count();
}

auto MaskThumbnailService::Cached(const MaskThumbnailSpec& spec) const -> std::optional<QImage> {
  return impl_->Cached(spec);
}

}  // namespace alcedo
