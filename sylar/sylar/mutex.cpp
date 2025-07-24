#include "mutex.h"
#include <stdexcept>

namespace sylar {

Semaphore::Semaphore(uint32_t count) {
    if (sem_init(&m_semaphore, 0, count)) {
        throw std::logic_error("sem_init_error");
    }
}

Semaphore::~Semaphore() {
    //sem_destory只能在所有等待线程已退出时调用，否则可能导致未定义行为
    sem_destroy(&m_semaphore);
}

void Semaphore::wait() {
    //等待信号量，如果信号值m_semaphore大于0，则减1并继续执行，如果信号量==0，则阻塞当前线程，直到其他线程notify()增加信号量
    if (sem_wait(&m_semaphore)) {
        throw std::logic_error("sem_wait_error");
    }
}

void Semaphore::notify() {
    //增加信号量，如果有线程在wait中阻塞，则唤醒其中一个线程继续执行
    //sem_post是原子操作，可以安全的被多个线程调用
    //如果有多个线程在wait，sem_post只会唤醒一个线程(按照FIFO顺序)
    if (sem_post(&m_semaphore)) {
        std::logic_error("sem_post_error");
    }
}


FiberSemaphore::FiberSemaphore(size_t initial_concurrency)
    :m_concurrency(initial_concurrency) {
}

// 尝试获取信号量（非阻塞）
bool FiberSemaphore::tryWait() {
    SYLAR_ASSERT(Scheduler::GetThis()); // 断言当前线程在协程调度器中
    {
        MutexType::Lock lock(m_mutex); // 加锁，防止并发访问
        if (m_concurrency > 0u) { // 若信号量可用
            --m_concurrency; // 占用一个信号量
            return true; // 获取成功
        }
        return false; // 获取失败
    }
}

// 获取信号量（可能阻塞）
void FiberSemaphore::wait() {
    SYLAR_ASSERT(Scheduler::GetThis()); // 断言当前线程在协程调度器中
    {
        MutexType::Lock lock(m_mutex); // 进入临界区
        if (m_concurrency > 0u) { // 若信号量可用
            --m_concurrency; // 占用一个信号量
            return; // 直接返回
        }
        // 信号量不可用，将当前协程加入等待队列
        m_waiters.push_back(std::make_pair(Scheduler::GetThis(), Fiber::GetThis()));
    }
    Fiber::YieldToHold(); // 挂起当前协程，等待被唤醒
}

// 释放信号量
void FiberSemaphore::notify() {
    MutexType::Lock lock(m_mutex); // 进入临界区
    if (!m_waiters.empty()) { // 若有等待的协程
        auto next = m_waiters.front(); // 取出队列中最早等待的协程
        m_waiters.pop_front(); // 移除该协程
        next.first->schedule(next.second); // 让调度器调度该协程，使其恢复运行
    }
    else {
        ++m_concurrency; // 没有等待的协程，增加可用信号量
    }
}

}