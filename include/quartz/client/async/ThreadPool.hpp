#pragma once
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace quartz::client::async
{
    enum class TaskPriority : std::uint8_t
    {
        Background,
        Normal,
        High,
        Count
    };

    class ThreadPool
    {
    public:
        using Task = std::function<void(std::stop_token)>;

        static std::size_t defaultWorkerCount() noexcept;

        explicit ThreadPool(std::size_t workerCount = defaultWorkerCount());
        ~ThreadPool();

        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;
        ThreadPool(ThreadPool&&) = delete;
        ThreadPool& operator=(ThreadPool&&) = delete;

        void submit(Task task, TaskPriority priority = TaskPriority::Normal);
        [[nodiscard]] std::size_t workerCount() const noexcept { return _workerCount; }

    private:
        [[nodiscard]] bool hasTasks() const noexcept;
        bool tryPop(Task& task) noexcept;
        void workerLoop(std::stop_token stop);

        std::size_t _workerCount = 0;
        std::array<std::deque<Task>, static_cast<std::size_t>(TaskPriority::Count)> _queues;
        std::mutex _mutex;
        std::condition_variable_any _condition;
        // Keep workers last: they stop/join before queue/synchronization state is destroyed.
        std::vector<std::jthread> _workers;
    };

    ThreadPool& globalThreadPool();
}
