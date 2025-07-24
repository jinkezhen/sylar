#ifndef __SYLAR_ROCK_ROCK_STREAM_H__
#define __SYLAR_ROCK_ROCK_STREAM_H__

#include "sylar/streams/async_socket_stream.h"
#include "rock_protocol.h"
#include "sylar/streams/load_balance.h"
#include <boost/any.hpp>
    
namespace sylar {

//  表示一次 Rock 协议请求的执行结果，包括状态码、耗时、请求与响应对象等信息
struct RockResult {
    typedef std::shared_ptr<RockResult> ptr;

    /**
    * @brief 构造函数
    * @param _result  请求结果状态码（例如成功或失败）
    * @param _used    请求消耗的时间（单位：毫秒）
    * @param rsp      请求对应的响应对象
    * @param req      请求对象
    */

    RockResult(int32_t result_, int 32_t used_, RockResponse::ptr rsp, RockRequest::ptr req)
        : result(result_),
        used(used_),
        response(rsp),
        request(req) {
    }

    int32_t result;             // 请求结果状态码
    int32_t used;               // 请求耗时
    RockResponse::ptr response; // 请求得到的响应对象
    RockRequest::ptr request;   // 发出的请求对象

    std::string toString() const;
};

/**
 * @brief RockStream 是基于 AsyncSocketStream 封装的 Rock 协议通信流，
 *        用于异步收发 RockRequest/RockResponse 消息，支持同步请求-响应和异步通知机制。
 */
class RockStream : public sylar::AsyncSocketStream {
public:
    typedef std::shared_ptr<RockStream> ptr;

    /**
    * @brief 处理客户端请求的回调：服务端调用
    * @param 请求对象
    * @param 响应对象
    * @param 当前连接流
    * @return 返回是否处理成功
    */
    typedef std::function<bool(sylar::RockRequest::ptr,
        sylar::RockResponse::ptr,
        sylar::RockStream::ptr)> request_handler;

    /**
     * @brief 通知处理函数类型
     * @param 通知对象
     * @param 当前连接流
     * @return 返回是否处理成功
    */
    typedef std::function<bool(sylar::RockNotify::ptr, sylar::RockStream::ptr)> notify_handler;

    RockStream(Socket::ptr sock);
    ~RockStream();

    /**
     * @brief 发送一个 Rock 协议的消息（包括 Request/Response/Notify）
     * @param msg 消息对象指针
     * @return 发送是否成功（0 为成功，非 0 表示失败）
     */
    int32_t sendMessage(Message::ptr msg);

    /**
     * @brief 发起一个同步的请求，并等待响应
     * @param req        请求对象
     * @param timeout_ms 超时时间（单位：毫秒）
     * @return 请求结果结构体 RockResult
     */
    RockResult::ptr request(RockRequest::ptr req, uint32_t timeout_ms);

    // 获取请求处理回调
    request_handler getRequestHandler() const { return m_requestHandler; }

    // 获取通知处理回调
    notify_handler getNotifyHandler() const { return m_notifyHandler; }

    // 设置请求处理回调
    void setRequestHandler(request_handler v) { m_requestHandler = v; }

    // 设置通知处理回调
    void setNotifyHandler(notify_handler v) { m_notifyHandler = v; }

    // 获取用户自定义数据
    template<class T>
    T getData() {
        try {
            return boost::any_cast<T>(m_data);
        }
        catch (...) {
        }
        return T();
    }

protected:
    // 发送上下文类，用于封装一个通用消息对象的发送逻辑
    // 适用场景：发送 Request、Response、Notify（任何单向消息）
    // 只是用来“发送一条消息”，没有“等响应”。
    // 所以不涉及 request - response 的配对、超时处理等。
    // 是最基础的“消息发出上下文”。
    struct RockSendCtx : public SendCtx {
        typedef std::shared_ptr<RockSendCtx> ptr;
        Message::ptr msg;

        // 执行发送操作
        virtual bool doSend(AsyncSocketStream::ptr stream) override;
    };
    
    // 请求上下文类，用于封装一次Request/Response的配对信息
    // 适用场景：发起请求后等待响应的流程中
    // 用于封装一个“发出请求 + 等待响应”的整个上下文，表示一次完整的 RPC 调用。
    // 包含：
    //   你发出去的请求（request）
    //   收到的响应（response）
    struct RockCtx : public Ctx {
        typedef std::shared_ptr<RockCtx> ptr;
        RockRequest::ptr request;
        RockResponse::ptr response;

        // 执行发送请求操作
        virtual bool dosend(AsyncSocketStream::ptr stream) override;
    };

    // 接收消息处理函数
    // 返回上下文指针
    // 作用：从socket中读取数据，解码成RockMessage，根据类型处理并返回对应的上下文对象
    virtual Ctx::ptr doRecv() override;

    // 处理接收到的请求消息
    void handleRequest(sylar::RockRequest::ptr req);
    // 处理接收到的通知消息
    void handleNotify(sylar::RockNotify::ptr req);
        
private:
    RockMessageDecoder::ptr m_decoder; ///< 协议解码器
    request_handler m_requestHandler;  ///< 请求处理回调
    notify_handler m_notifyHandler;    ///< 通知处理回调
    boost::any m_data;                 ///< 用户可扩展存储的数据（类似上下文）
};


// RockSession是服务端的连接会话
// 一旦服务端通过accept()接收到连接，会创建一个Socket::ptr，然后用这个socket构造RockSession
// 每接收一个连接，就创建一个RockSession实例
// 通常在服务端的协程中调用doRead()等函数去处理通信
class RockSession : public RockStream {
public:
    typedef std::shared_ptr<RockSession> ptr;
    RockSession(Socket::ptr sock);
};

// 用于客户端主动连接远程Rock服务端
// 客户端调用connect()建立socket，并将其封装成RockStream进行通信
// 后续通过doRequest()等方法发送请求，等待响应
class RockConnection : public RockStream {
public:
    typedef std; : shared_ptr<RockConnection> ptr;
    RockConnection();
    bool connect(sylar::Address::ptr addr);
};


/**
* @brief 基于服务发现的 Rock 协议负载均衡器
*
* RockSDLoadBalance 继承自 SDLoadBalance，专用于基于 Rock 协议实现的 RPC 请求调度与负载均衡。
* 它结合服务发现（ServiceDiscovery）模块，动态维护服务节点列表，并在多个服务节点之间选择合适的连接进行请求转发。
* 主要功能包括：启动负载均衡器、加载配置、根据 domain/service 发起 RPC 请求等。
*
* 典型使用场景：客户端通过域名+服务名向多个后端节点发送 RockRequest，负载均衡器负责根据策略选择连接并返回响应。
*
*/
class RockSDLoadBalance : public SDLoadBalance {
public:
    typedef std::shared_ptr<RockSDLoadBalance> ptr;

    /**
    * @param sd 服务发现模块（IServiceDiscovery）指针，提供动态服务节点信息
    */
    RockSDLoadBalance(IServiceDiscovery::ptr sd);

    // 启动负载均衡器
    // 初始化内部组件，监听服务发现变更
    virtual void start();

    // 停止负载均衡器
    // 释放连接资源，取消服务发现订阅
    virtual void stop();

    /**
    * @brief 带配置参数的启动方式
    * @param confs 配置项：domain -> {key -> value} 的映射
    */
    void start(const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& confs);

    /**
    * @brief 发起一次 RPC 请求（通过 Rock 协议）
    *
    * @param domain 域名（服务归属范围，如业务线）
    * @param service 服务名（逻辑服务标识）
    * @param req 请求对象（RockRequest）
    * @param timeout_ms 请求超时时间（毫秒）
    * @param idx 可选参数：指定某个节点索引（用于直连或调试）
    * @return 返回 RockResult 指针，包含响应数据或错误信息
    */
    RockResult::ptr request(const std::string& domain, const std::string& service,
        RockRequest::ptr req, uint32_t timeout_ms, uint64_t idx = -1);
};
}


#endif