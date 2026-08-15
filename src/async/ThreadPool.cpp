#include "quartz/client/async/ThreadPool.hpp"
#include <algorithm>

namespace quartz::client::async
{
    std::size_t ThreadPool::defaultWorkerCount() noexcept
    {
        const std::size_t hardware = std::max<std::size_t>(std::thread::hardware_concurrency(), 2);
        return std::clamp<std::size_t>((hardware + 2) / 3, 2, 4);
    }

    ThreadPool::ThreadPool(const std::size_t workerCount) : _workerCount(std::max<std::size_t>(workerCount, 1))
    {
        _workers.reserve(_workerCount);
        for (std::size_t i = 0; i < _workerCount; ++i) _workers.emplace_back([this](std::stop_token stop) { workerLoop(stop); });
    }

    ThreadPool::~ThreadPool()
    {
        for (auto& worker : _workers) worker.request_stop();
        _condition.notify_all();
        _workers.clear();
    }

    void ThreadPool::submit(Task task, const TaskPriority priority)
    {
        if (!task) return;
        {
            std::lock_guard lock(_mutex);
            _queues[static_cast<std::size_t>(priority)].emplace_back(std::move(task));
        }
        _condition.notify_one();
    }

    bool ThreadPool::hasTasks() const noexcept
    {
        for (const auto& queue : _queues) if (!queue.empty()) return true;
        return false;
    }

    bool ThreadPool::tryPop(Task& task) noexcept
    {
        for (std::size_t i = _queues.size(); i-- > 0;)
        {
            auto& queue = _queues[i];
            if (queue.empty()) continue;
            task = std::move(queue.front());
            queue.pop_front();
            return true;
        }
        return false;
    }

    void ThreadPool::workerLoop(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            Task task;
            {
                std::unique_lock lock(_mutex);
                if (!_condition.wait(lock, stop, [this] { return hasTasks(); })) return;
                if (!tryPop(task)) continue;
            }
            try { task(stop); }
            catch (...) { }
        }
    }

    ThreadPool& globalThreadPool()
    {
        static ThreadPool pool;
        return pool;
    }
}
