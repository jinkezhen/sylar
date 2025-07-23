#ifndef __SYLAR_STREAMS_ASYNC_SOCKET_STREAAM_H__
#define __SYLAR_STREAMS_ASYNC_SOCKET_STREAAM_H__

#include "sylar/mutex.h"
#include "sylar/fiber.h"
#include "sylar/scheduler.h"
#include "socket_stream.h"
#include <list>
#include <memory>
#include <functional>
#include <unordered_map>
#include <boost/any.hpp>


namespace sylar {



/**
 * @brief 异步 Socket 流通信抽象类，封装基于协程和 IOManager 的非阻塞通信机制。
 * 
 * 该类继承自 SocketStream，并支持通过协程调度器进行异步读写。
 * 可用于实现基于自定义协议的异步客户端/服务端通信（如 Rock 协议）。
 * 
 * 核心特性：
 * - 支持异步发送、接收请求
 * - 支持连接/断开回调
 * - 支持请求超时处理
 * - 每个请求通过唯一 sn（序列号）标识上下文
 */
class AsyncSocketStream : public SocketStream,
                          public std::enable_shared_from_this<AsyncSocketStream> {
    typedef std::shared_ptr<AsyncSocketStream> ptr;
    typedef sylar::RWMutex RWMutexType;


    //连接建立和断开时的回调函数类型
    typedef std::function<bool(AsyncSocketStream::ptr)> connect_callback;
    typedef std::function<void(AsyncSocketStream::ptr)> disconnect_callback;

    /**
     * @brief 构造函数
     * @param sock Socket 对象
     * @param owner 是否拥有 Socket 生命周期
     */
    AsyncSocketStream(Socket::ptr sock, bool owner = true);

    //启动流，开启读写事件
    virtual bool start();
    //关闭流，关闭socket，清理上下文
    virtual void close() override;

public:
    //错误码定义
    enum Error {
        OK = 0,
        TIMEOUT = -1,
        IO_ERROR = -2,
        NOT_CONNECT = -3
    };

protected:
    /**
     * @brief 发送上下文抽象基类
     * 所有发送操作都会封装为 SendCtx 并加入发送队列
     */
    struct SendCtx {
        typedef std::shared_ptr<SendCtx> ptr;
        virtual ~SendCtx() {}

        //发送逻辑，由派生类实现
        virtual bool doSend(AsyncSocketStream::ptr stream) = 0;
    };
     
    /**
     * @brief 接收请求或响应的上下文，继承自 SendCtx
     * 带有协程、调度器、超时处理等逻辑
     */
    struct Ctx : public SendCtx {
        typedef std::shared_ptr<Ctx> ptr;
        virtual ~Ctx() {}
        Ctx();

        uint32_t sn;             ///< 唯一序列号
        uint32_t timeout;        ///< 超时时间（毫秒）
        uint32_t result;         ///< 请求执行结果
        bool timed;              ///< 是否超时标志

        Scheduler* scheduler;    ///< 协程调度器
        Fiber::ptr fiber;        ///< 请求对应的协程
        Timer::ptr timer;        ///< 超时计时器

        /// 响应处理逻辑（超时或收到响应后触发）
        virtual void doRsp();
    };

public:
    /// 异步读逻辑（注册读事件并触发 doRead）
    virtual void startRead();

    /// 异步写逻辑（注册写事件并触发 doWrite）
    virtual void startWrite();

    /// 实际处理读事件，读取完整请求或响应数据
    virtual void doRead();

    /// 实际处理写事件，从发送队列取出数据发送
    virtual void doWrite();

    /// 超时回调处理，触发上下文的 doRsp
    virtual void onTimeOut(Ctx::ptr ctx);

    /**
     * @brief 接收数据并生成上下文（需由派生类实现协议解析）
     * 通常用于解析完整的请求/响应对象
     */
    virtual Ctx::ptr doRecv() = 0;

    /// 根据 sn 获取上下文（不删除）
    Ctx::ptr getCtx(uint32_t sn);

    /// 获取并移除上下文
    Ctx::ptr getAndDelCtx(uint32_t sn);

    /// 获取特定类型上下文
    template<class T>
    std::shared_ptr<T> getCtxAs(uint32_t sn) {
        auto ctx = getCtx(sn);
        if (ctx) {
            return std::dynamic_pointer_cast<T>(ctx);
        }
        return nullptr;
    }

    /// 获取并移除特定类型上下文
    template<class T>
    std::shared_ptr<T> getAndDelCtxAs(uint32_t sn) {
        auto ctx = getAndDelCtx(sn);
        if (ctx) {
            return std::dynamic_pointer_cast<T>(ctx);
        }
        return nullptr;
    }

    /// 添加上下文（插入 sn->Ctx 映射表）
    bool addCtx(Ctx::ptr ctx);

    /// 入队待发送数据（写队列）
    bool enqueue(SendCtx::ptr ctx);

    /// 内部关闭逻辑（关闭 socket、清理资源）
    bool innerClose();

    /// 等待当前请求协程全部完成
    bool waitFiber();

protected:
    sylar::FiberSemaphore m_sem;       ///< 协程同步信号量，用于启动控制
    sylar::FiberSemaphore m_waitSem;   ///< 关闭时等待所有协程完成

    RWMutexType m_queueMutex;          ///< 发送队列锁
    std::list<SendCtx::ptr> m_queue;   ///< 发送上下文队列

    RWMutexType m_mutex;               ///< 上下文映射表锁
    std::unordered_map<uint32_t, Ctx::ptr> m_ctxs; ///< sn -> 上下文映射

    uint32_t m_sn = 0;                 ///< 全局序列号（自增）
    bool m_autoConnect = false;        ///< 是否自动重连

    sylar::Timer::ptr m_timer;         ///< 用于超时任务
    sylar::IOManager* m_iomanager = nullptr; ///< IO事件处理器
    sylar::IOManager* m_worker = nullptr;    ///< 回调调度器

    connect_callback m_connectCb;      ///< 连接成功回调
    disconnect_callback m_disconnectCb;///< 连接断开回调

    boost::any m_data;                 ///< 业务自定义数据（如客户端标识）
};

/**
 * @brief 管理多个 AsyncSocketStream 对象的管理器类，支持连接池的管理和负载均衡获取连接。
 * 
 * 该类主要用于维护一组异步 Socket 连接，提供添加、清空、批量设置连接的功能，
 * 并通过索引轮询方式获取当前可用连接，同时支持连接和断开回调的统一管理。
 */
/**
 * @brief 管理多个 AsyncSocketStream 对象的管理器类，支持连接池的管理和负载均衡获取连接。
 * 
 * 该类主要用于维护一组异步 Socket 连接，提供添加、清空、批量设置连接的功能，
 * 并通过索引轮询方式获取当前可用连接，同时支持连接和断开回调的统一管理。
 */
class AsyncSocketStreamManager {
public:
    // 读写锁类型，保护多线程环境下对内部数据的并发访问
    typedef sylar::RWMutex RWMutexType;

    // 连接建立回调函数类型，直接复用 AsyncSocketStream 中定义的 connect_callback
    typedef AsyncSocketStream::connect_callback connect_callback;

    // 连接断开回调函数类型，复用 AsyncSocketStream 中定义的 disconnect_callback
    typedef AsyncSocketStream::disconnect_callback disconnect_callback;

    /**
     * @brief 构造函数，初始化管理器状态
     */
    AsyncSocketStreamManager();

    /**
     * @brief 析构函数，默认空实现
     */
    virtual ~AsyncSocketStreamManager() {}

    /**
     * @brief 向管理器中添加一个异步 Socket 连接对象
     * @param stream 需要添加的 AsyncSocketStream 智能指针
     */
    void add(AsyncSocketStream::ptr stream);

    /**
     * @brief 清空管理器中所有的连接对象，释放资源
     */
    void clear();

    /**
     * @brief 批量设置管理器中的连接对象，替换当前所有连接
     * @param streams 一组 AsyncSocketStream 智能指针，覆盖管理器的连接集合
     */
    void setConnection(const std::vector<AsyncSocketStream::ptr>& streams);

    /**
     * @brief 获取当前管理器中的一个连接，采用轮询方式负载均衡获取
     * @return AsyncSocketStream::ptr 返回选中的连接指针，如果无可用连接则返回 nullptr
     */
    AsyncSocketStream::ptr get();

    /**
     * @brief 模板接口，按指定类型动态转换并返回连接对象
     * @tparam T 期望的连接类型
     * @return std::shared_ptr<T> 如果转换成功返回对应类型智能指针，否则返回 nullptr
     */
    // getAs<T>() 允许你拿到基类指针后，动态安全地转换成派生类指针
    template<class T>
    std::shared_ptr<T> getAs() {
        auto rt = get();
        if(rt) {
            return std::dynamic_pointer_cast<T>(rt);
        }
        return nullptr;
    }

    /**
     * @brief 获取连接建立回调函数
     * @return connect_callback 当前设置的连接回调
     */
    connect_callback getConnectCb() const { return m_connectCb; }

    /**
     * @brief 获取连接断开回调函数
     * @return disconnect_callback 当前设置的断开回调
     */
    disconnect_callback getDisconnectCb() const { return m_disconnectCb; }

    /**
     * @brief 设置连接建立时的回调函数
     * @param v 要设置的回调函数
     */
    void setConnectCb(connect_callback v);

    /**
     * @brief 设置连接断开时的回调函数
     * @param v 要设置的回调函数
     */
    void setDisconnectCb(disconnect_callback v);

private:
    RWMutexType m_mutex;                          ///< 读写锁，保护内部数据安全
    uint32_t m_size;                              ///< 当前管理的连接数量
    uint32_t m_idx;                               ///< 用于轮询选择连接的索引计数器
    std::vector<AsyncSocketStream::ptr> m_datas; ///< 连接对象容器，存储所有管理的连接
    connect_callback m_connectCb;                  ///< 连接建立时触发的回调函数
    disconnect_callback m_disconnectCb;            ///< 连接断开时触发的回调函数
};


}

#endif