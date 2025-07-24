#include "async_socket_stream.h"
#include "sylar/util.h"
#include "sylar/log.h"
#include "sylar/macro.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

AsyncSokcetStream::Ctx::Ctx() 
    : sn(0),timeout(0),
      result(0),
      timed(false),
      scheduler(nullptr) {
}

/// 当用户在协程中发送一个异步请求后，当前协程会被挂起(通过yield)，当该请求的响应到来(或超时)时，
/// 恢复原来的协程，并进行必要的清理与状态设置
void AsyncSocketStream::Ctx::doRsp() {
    Scheduler* scd = scheduler;
    // 使用原子操作尝试把scheduler从scd改为nullptr
    // 如果替换失败说明已经被别的线程或超时逻辑调用，就直接返回，避免重复执行doRsp()
    // 如果是响应先到了，doRsp() 会由 doRead() 触发；
    // 如果是超时先到了，doRsp() 会由 onTimeOut() 触发；
    if (!sylar::Atomic::compareAndSwapBool(scheduler, scd, (Scheduler*)nullptr)) {
        return;
    }
    if (!scd || !fiber) {
        return;
    }
    if (timer) {
        timer->cancel();
        timer = nullptr;
    }
    if (timed) {
        result = TIMEOUT;
    }
    // 将协程重新调度回调度器，即让原来等待的写成被唤醒，继续执行后续的逻辑
    scd->schedule(&fiber);
}

AsyncSocketStream::AsyncSocketStream(Socket::ptr sock, bool owner) 
    : SocketStream(sock, owner),
      m_waitSem(2),
      m_sn(0),
      m_autoConnect(false),
      m_iomanager(nullptr),
      m_worker(nullptr) {
}

// 它不是直接执行读写操作，而是：
// “启动异步机制”，将读写任务注册/调度给 IOManager，真正的读写是事件触发时再去做的。
bool AsyncSocketStream::start() {
    // 如果没有指定 IOManager，默认用当前协程所在的 IOManager 实例
    if (!m_iomanager) {
        m_iomanager = sylar::IOManager::GetThis();
    }
    // 如果没有指定 worker 线程池，也默认用当前线程所在的 IOManager
    if (!m_worker) {
        m_worker = sylar::IOManager::GetThis();
    }

    
    do {
        // 挂起当前协程，直到某处调用m_waitSem.notify()来唤醒它。
        // 只有当连接成功并通过 connect_cb 后，waitFiber() 
        // 挂起的协程才会继续往下执行，进入 startRead()、startWrite()。
        // 在上一次的读写协程完全退出之后，才允许新一轮的连接初始化和读写启动。
        waitFiber();

        if (m_timer) {
            m_timer->cancel();
            m_timer = nullptr;
        }

        // 如果当前socket连接已经断开，尝试重新连接
        if (!isConnected()) {
            if (!m_socket->reconnect()) {
                // 重新连接失败，关闭流并通知等待协程，跳出循环
                innerClose();
                m_waitSem.notify();
                m_waitSem.notify();
                break;
            }
        }

        // 如果设置了连接回调则执行他
        if (m_connectCb) {
            // 如果回调返回 false，表示连接不成功或不接受，关闭流并通知等待协程，跳出循环
            if (!m_connectCb(shared_from_this())) {
                innerclose();
                m_waitSem.notify();
                m_waitSem.notify();
                break;
            }
        }

        // 启动异步读写操作
        startRead();
        startWrite();

        // 成功启动后，返回true
        return true;

    } while (false);
    
    // 执行到此处 说明启动失败
    // 如果允许自动重连
    if (m_autoConnect) {
        if (m_timer) {
            m_timer->cancel();
            m_timer = nullptr;
        }

        // 在2秒后重新调用 start() 实现自动重连
        m_timer = m_iomanager->addTimer(2 * 1000,
                        std::bind(&AsyncSocketStream::start, shared_from_this()));
    }
    return false;
}

// 异步读取函数，会被startRead()调度到协程中执行
void AsyncSocketStream::doRead() {
    try {
        // 只要连接状态正常，就持续读取数据
        while (isConnected()) {
            // 执行协议层的收包操作
            // 一般会读取socket中的数据并解析成自定义的协议包(如RockMessage)
            // 解析结果封装到一个Ctx对象中返回，表示一个完整的请求上下文
            auto ctx = doRecv();
            if (ctx) {
                // 如果成功接收到一个包，会调用doRsp，把之前发起请求时挂起的协程恢复执行
                ctx->doRsp();
            }
        }  // 读取完一个包就继续读取下一个，直到连接断开
    } catch(...) {
        //TODO log
    }

    SYLAR_LOG_DEBUG(g_logger) << "doRead out " << this;
    innerClose();
    // 通知 waitFiber()，释放一个协程信号（配合连接管理使用，表示 read 线程结束了）。
    m_waitSem.notify();

    // 如果设置了自动重连，则在 10ms 后重新调用 `start()`，尝试连接并重新注册读写事件。
    if(m_autoConnect) {
        m_iomanager->addTimer(10, std::bind(&AsyncSocketStream::start, shared_from_this()));
    }
}

// 异步写操作，从队列中取出待发送的数据上下文然后写到socket中
void AsyncSocketStream::doWrite() {
    try {
        while (isConnected()) {
            // 阻塞当前协程，直到有可写数据(发送请求)到来时才继续执行
            // 这里的数据到来是通过方法enqueue实现的，输入到来后会入队，并且唤醒该协程
            m_sem.wait();

            std::list<SendCtx::ptr> ctxs;
            {
                RWMutexType::WriteLock lock(m_queueMutex);
                m_queue.swap(ctxs);
            }
            
            auto self = shared_from_this();
            for (auto& i : ctxs) {
                if (!i->doSend(self)) {
                    innerClose();
                    break;
                }
            }
        }
    } catch (...) {
        //TODO log
    }

    SYLAR_LOG_DEBUG(g_logger) << "doWrite out " << this;
    {
        RWMutexType::WriteLock lock(m_queueMutex);
        m_queue.clear();
    }
    m_waitSem.notify();
}

// 启动异步读操作
void AsyncSocketStream::startRead() {
    m_iomanager->schedule(std::bind(&AsyncSocketStream::doRead, shared_from_this()))
}

// 启动异步写操作
void AsyncSocketStream::startWrite() {
    m_iomanager->schedule(std::bind(&AsyncSokcerStream::doWrite, shared_from_this()));
}

void AsyncSocketStream::onTimeOut(Ctx::ptr ctx) {
    {
        RWMutexType::WriteLock lock(m_mutex);
        // 移除已经超时的上下文
        m_ctxs.erase(ctx->sn);
    }
    // 设置超时标志
    ctx->timed = true;
    ctx->doRsp();
}

AsyncSocketStream::Ctx::ptr AsyncSocketStream::getCtx(uint32_t sn) {
    auto it = m_ctxs.find(sn);
    return it == m_ctxs.end() ? nullptr : it->second;
}

AsyncSocketStream::Ctx::ptr AsyncSocketStream::getAndDelCtx(uint32_t sn) {
    Ctx::ptr ctx;
    RWMutexType::WriteLock lock(m_mutex);
    auto it = m_ctxs.find(sn);
    if (it != m_ctxs.end()) {
        ctx = it->second;
        m_ctxs.erase(it);
    }
    return ctx;
}

bool AsyncSocketStream::addCtx(Ctx::ptr ctx) {
    RWMutexType::WriteLock lock(m_mutex);
    m_ctxs.insert(std::make_pair(ctx->sn, ctx));
    return true;
}

bool AsyncSocketStream::enqueue(SendCtx::ptr ctx) {
    SYLAR_ASSERT(ctx);
    RWMutexLockType::WriteLock lock(m_queueMutex);
    bool empty = m_queue.empty();
    m_queue.push_back(ctx);
    lock.unlock();
    if (empty()) {
        m_sem.notify();
    }
    return empty();
}

// innerClose()一般是异常关闭
bool AsyncSocketStream::innerClose() {
    SYLAR_ASSERT(m_iomanager == sylar::IOManager::GetThis());
    if (isConnected() && m_disconnectCb) {
        m_disconnectCb(shared_from_this());
    }
    // 关闭底层socket连接，释放资源
    SocketStream::close();
    // 通知阻塞在doWrite()操作的协程
    m_sem.notify();
    // 保存当前所有活动的上下文
    std::unordered_map<uint32_t, Ctx::ptr> ctxs;
    {
        RWMutexType::WriteLock lock(m_mutex);
        ctxs.swap(m_ctxs);
    }
    {
        RWMutexType::WriteLock lock(m_queueMutex);
        m_queue.clear();
    }
    for(auto& i : ctxs) {
        i.second->result = IO_ERROR;
        i.second->doRsp();
    }
}

bool AsyncSocketStream::waitFiber() {
    // m_waitSem初始为2，连续两次调用会从2减到0，
    m_waitSem.wait();
    m_waitSem.wait();
    return true;
}

void AsyncSocketStream::close() {
    m_autoConnect = false;
    // 当前协程运行的调度器不一定是 m_iomanager，可能是业务线程池或者其他协程调度器。
    // 临时切换当前协程所在的调度器,并在析构时恢复原来的调度器
    SchedulerSwitcher ss(m_iomanager);
    if (m_timer) {
        m_timer->cancel();
    }
    SocketStream::close();
}

AsyncSocketStreamManager::AsyncSocketStreamManager()
    : m_size(0),
      m_idx(0) {
}

void AsyncSocketStreamManager::add(AsyncSocketStream::ptr stream) {
    RWMutexType::WriteLock lock(m_mutex);
    m_datas.push_back(stream);
    ++m_size;

    if(m_connectCb) {
        stream->setConnectCb(m_connectCb);
    }

    if(m_disconnectCb) {
        stream->setDisconnectCb(m_disconnectCb);
    }
}

void AsyncSocketStreamManager::clear() {
    RWMutexType::WriteLock lock(m_mutex);
    for(auto& i : m_datas) {
        i->close();
    }
    m_datas.clear();
    m_size = 0;
}

void AsyncSocketStreamManager::setConnection(const std::vector<AsyncSocketStream::ptr>& streams) {
    auto cs = streams;
    RWMutexType::WriteLock lock(m_mutex);
    cs.swap(m_datas);
    m_size = m_data.size();
    if(m_connectCb || m_disconnectCb) {
        for(auto& i : m_datas) {
            if(m_connectCb) {
                i->setConnectCb(m_connectCb);
            }
            if(m_disconnectCb) {
                i->setDisconnectCb(m_disconnectCb);
            }
        }
    }
    lock.unlock();
    for(auto& i : cs) {
        i->close();
    }
}

AsyncSocketStream::ptr AsyncSocketStreamManager::get() {
    RWMutexType::ReadLock lock(m_mutex);
    for (uint32_t i = 0; i < m_size; ++i) {
        auto idx = sylar::Atomic::addFetch(m_idx, 1);
        if (m_datas[idx % m_size]->isConnected()) {
            return m_datas[idx % m_size];
        }
    }
    return nullptr;
}

void AsyncSocketStreamManager::setConnectCb(connect_callback v) {
    m_connectCb = v;
    RWMutexType::WriteLock lock(m_mutex);
    for(auto& i : m_datas) {
        i->setConnectCb(m_connectCb);
    }
}

void AsyncSocketStreamManager::setDisconnectCb(disconnect_callback v) {
    m_disconnectCb = v;
    RWMutexType::WriteLock lock(m_mutex);
    for(auto& i : m_datas) {
        i->setDisconnectCb(m_disconnectCb);
    }
}

}

