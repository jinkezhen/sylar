#ifndef __SYLAR_WORKER_H__
#define __SYLAR_WORKER_H__ 

#include "mutex.h"
#include "singleton.h"
#include "log.h"
#include "iomanager.h"

namaspace sylar{

// WorkerGroup 是 Sylar 框架中用于实现 一组任务批量并发调度与同步等待 的工具类。
// 它的核心功能是：
//	将多个任务（即多个 std::function<void()>）调度到指定的调度器（Scheduler）中并发执行；
//	然后在调用 waitAll() 时阻塞等待，直到所有调度的任务全部执行完成。
// 它提供了 一次性并发调度多个任务，并在主线程或当前协程中等待这些任务全部完成的能力
class WorkerGroup : Noncopyale: public std::enable_shared_from_this<WorkerGroup> {
public:
	std::shared_ptr<WorkGroup> ptr;

    /**
     * @brief 创建一个 WorkerGroup 实例
     * @param batch_size 要调度的任务总数（也是等待完成的任务总数）
     * @param s 调度器指针（默认使用当前线程的调度器）
     */
    static WorkerGroup::ptr Create(uint32_t batch_size, sylar::Scheduler* s = sylar::Scheduler:; GetThis()) {
        return std::make_shared<WorkerGroup>(batch_size, s);
    }

    /**
     * @brief 构造函数
     * @param batch_size 要调度的任务数量
     * @param s 调度器指针，负责执行任务的线程池/调度器
     */
    WorkerGroup(uint32_t batch_size, sylar::Scheduler* s = sylar::Scheduler::GetThis());
    ~WorkerGroup();

    /**
     * @brief 调度一个任务执行
     * @param cb 要执行的函数对象（即任务）
     * @param thread 指定线程号（可选，-1 表示由调度器自动选择线程）
     */
    void schedule(std::function<void()> cb, int thread = -1);

    // 阻塞等待所有任务完成
    void waitAll();

private:
    /**
     * @brief 实际任务执行器
     * @details 包装用户传入的 cb，确保任务执行完后减少信号量
     * @param cb 用户提供的任务函数
     */
    void doWork(std::function<void()> cb);

private:
    uint32_t m_batchSize;    // 需要等待完成的任务数量
    bool m_finish;           // 是否已完成所有任务
    Scheduler* m_scheduler;  // 所用的调度器
    FiberSemaphore m_sem;    // 信号量，用于同步等待所有任务执行完成
};



// 是 Sylar 框架中用于集中管理和使用多个具名线程池/协程调度器的调度中心，
// 它提供了按名称注册、获取、调度和关闭调度器的机制，是任务分类调度、
// 线程隔离和配置式线程池控制的核心组件
/**
 * @brief WorkerManager 线程池调度管理器
 *
 * WorkerManager 是 Sylar 框架中用于管理多个具名调度器（如 IOManager、Scheduler）的管理类。
 * 该类支持通过名称（string）快速查找调度器，并提供按名称提交任务的接口，方便模块间逻辑隔离和线程池划分。
 *
 * WorkerManager 主要负责：
 * 1. 注册多个线程池（调度器）并按名称进行索引管理。
 * 2. 支持通过名称提交任务，自动将任务分发到对应调度器中执行。
 * 3. 支持初始化、停止所有线程池，适配系统启动与关闭流程。
 * 4. 支持配置式线程池创建，方便灵活扩展不同业务场景。
 */
class WorkerManager {
public:
    WorkerManager();

    // 添加一个调度器到管理器中
    void add(Scheduler::ptr s);

    // 根据名称获取对应的调度器
    Scheduler::ptr get(const std::string& name);

    // 获取指定名称的调度器，并转换为 IOManager 类型
    IOManager::ptr  getAsIOManager(const std::string& name);

    /**
     * @brief 向指定名称的调度器提交单个任务
     * @tparam FiberOrCb 协程对象或函数对象
     * @param name 调度器名称
     * @param fc 提交的任务（协程或回调函数）
     * @param thread 指定线程号（默认-1，表示任意线程）
     */
    template<class FiberOrCb>
    void schedule(const std::string& name, FiberOrCb fc, int thread = -1) {
        auto s = get(name);
        if (s) {
            s->schedule(fc, thread);
        }
        else {
            static sylar::Logger::ptr s_logger = SYLAR_LOG_NAME("system");
            SYLAR_LOG_ERROR(s_logger) << "schedule name=" << name
                << " not exists";
        }
    }

    /**
     * @brief 向指定名称的调度器批量提交任务
     * @tparam Iter 迭代器类型
     * @param name 调度器名称
     * @param begin 任务迭代器起点
     * @param end 任务迭代器终点
     */
    template<class Iter>
    void schedule(const std::string& name, Iter begin, Iter end) {
        auto s = get(name);
        if (s) {
            s->schedule(begin, end);
        }
        else {
            static sylar::Logger::ptr s_logger = SYLAR_LOG_NAME("system");
            SYLAR_LOG_ERROR(s_logger) << "schedule name=" << name
                << " not exists";
        }
    }

    // 初始化调度器,从全局配置读取
    bool init();

    // 初始化调度器，从传入配置初始化
    bool init(const std::map<std::string, std::map<std::string, std::string>>& v);

    // 停止所有调度器
    void stop();

    // 判断是否已经停止
    bool isStoped() const { return m_stop; }

    // 输出所有调度器的状态信息
    std::ostream& dump(std::string& os);

    // 获取当前管理的调度器数量
    uint32_t getCount();

private:
    /// 名称到调度器列表的映射，一个名称可能对应多个调度器实例（用于负载均衡）
    std::map<std::string, std::vector<Scheduler::ptr>> m_datas;

    // 停止标志
    bool m_stop;
};

// 单例，提供全局唯一访问接口
typedef sylar::Singleton<WorkerManager> WorkerMgr;

}


#endif