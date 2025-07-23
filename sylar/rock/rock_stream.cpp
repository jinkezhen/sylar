#include "rock_stream.h"
#include "sylar/log.h"
#include "sylar/config.h"
#include "sylar/worker.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");
static sylar::ConfigVar<std::unordered_map<std::string
    , std::unordered_map<std::string, std::string> > >::ptr g_rock_services =
    sylar::Config::Lookup("rock_services", std::unordered_map<std::string
        , std::unordered_map<std::string, std::string> >(), "rock_services");

std::string RockResult::toString() const {
    std::stringstream ss;
    ss << "[RockResult result=" << result
        << " used=" << used
        << " response=" << (response ? response->toString() : "null")
        << " request=" << (request ? request->toString() : "null")
        << "]";
    return ss.str();
}

RockStream::RockStream(Socket::ptr sock)
    : AsyncSockStream(sock, true),
    m_decoder(new RockMessageDecoder) {
    SYLAR_LOG_DEBUG(g_logger) << "RockStream::RockStream "
        << this << " "
        << (sock ? sock->toString() : "");
}

RockStream::~RockStream() {
    SYLAR_LOG_DEBUG(g_logger) << "RockStream::~RockStream "
        << this << " "
        << (m_socket ? m_socket->toString() : "");
}

// 将待发送的消息封装进一个发送上下文（RockSendCtx）对象，然后入队等待 AsyncSocketStream 的内部异步写线程（或协程）来发送。
int32_t RockStream::sendMessage(Message::ptr msg) {
    if (isConnected()) {
        RockSendCtx::ptr ctx(new RockSendCtx);
        ctx->msg = msg;
        enqueue(ctx);   // 入队，等待异步发送
        return 1;
    }
    else {
        return -1;
    }
}

// 向对端发送一个RockRequest，并挂起当前协程等待响应或超时
RockResult::ptr RockStream::request(RockRequest::ptr req, uint32_t timeout_ms) {
    if (isConnected()) {
        RockCtx::ptr ctx(new RockCtx);
        ctx->request = req;
        ctx->sn = req->getSn();
        ctx->timeout = timeout_ms;
        ctx->scheduler = sylar::Scheduler::GetThis();
        // 记录当前协程指针。注意：这就是调用 request 的协程本身，稍后会被挂起等待响应！
        ctx->fiber = sylar::Fiber::GetThis();
        // 将当前上下文加入到一个 sn -> ctx 的哈希表中（通常在 AsyncSocketStream 内部维护），方便 doRecv() 收到响应后按 sn 找到对应请求上下文。
        addCtx(ctx);
        uint64_t ts = sylar::GetCurrentMS();
        ctx->timer = sylar::IOManager::GetThis()->addTimer(timeout_ms,
            std::bind(&RockStream::onTimeOut, shared_from_this(), ctx));
        enqueue(ctx);
        sylar::Fiber::YieldToHold();
        return std::make_shared<RockResult>(ctx->result, sylar::GetCurrentMS() - ts, ctx->response, req);
    }
    else {
        return std::make_shared<RockResult>(AsyncSocketStream::NOT_CONNECT, 0, nullptr, req);
    }
}

//  RockSendCtx 流程（适用于 sendMessage()）：
bool RockStream::RockSendCtx::doSend(AsyncSocketStream::ptr stream) {
    // serializeTo将一个 Message::ptr 编码（序列化）并写入到底层的 socket 中
    return std::dynamic_pointer_cast<RockStream>(stream)
        ->m_decoder->serializeTo(stream, msg) > 0;
}

// RockCtx 流程（适用于 request()）：
bool RockStream::RockCtx::doSend(AsyncSocketStream::ptr stream) {
    return std::dynamic_pointer_cast<RockStream>(stream)
        ->m_decoder->serializeTo(stream, request) > 0;
}

// 从网络中接收并解析一条Rock协议消息，然后根据消息类型(响应，请求，通知)进行相应处理
AsyncSocketStream::Ctx::ptr RockStream::doRecv() {
    auto msg = m_decoder->parseFrom(shared_from_this());
    if (!msg) {
        innerClose();
        return nullptr;
    }
    int type = msg->getType();
    if (type == Message::RESPONSE) {
        auto rsp = std::dynamic_pointer_cast<RockResponse>(msg);
        if (!rsp) {
            return nullptr;
        }
        // 根据响应的 sn（序列号）从上下文映射表中查找对应的请求 ctx；
        RockCtx::ptr ctx = getDelCtxAs<RockCtx>(rsp->getSn());
        if (!ctx) {
            return ctx;
        }
        ctx->result = rsp->getResult();
        ctx->response = rsp;
        return ctx;
    }
    else if (type == Message::REQUEST) {
        auto req = std::dynamic_pointer_cast<RockRequests>(msg);
        if (!req) {
            return nullptr;
        }
        if (m_requestHandler) {
            m_worker->schedule(std::bind(&RockStream::handleRequest,
                std::dynamic_pointer_cast<RockStream>(shared_from_this()),
                req));
        }
        else {
            SYLAR_LOG_WARN(g_logger) << "unhandle request " << req->toString();
        }
    }
    else if (type == Message::NOTIFY) {
        auto nty = std::dynamic_pointer_cast<RockNotify>(msg);
        if (!nty) {
            return nullptr;
        }
        if (m_notifyHandler) {
            m_worker->schedule(std::bind(&RockStream::handleNotify,
                std::dynamic_pointer_cast<RockStream>(shared_form_this()), nty));
        }
    }
    else {
        SYLAR_LOG_WARN(g_logger) << "RockStream recv unknow type=" << type
            << " msg: " << msg->toString();
    }
    return nullptr;
}

void RockStream::handleRequest(sylar::RockStream::ptr req) {
    sylar::RockResponse::ptr rsp = req->createResponse();
    if (!m_requestHandler(req, rsp, std::dynamic_pointer_cast<RockStream>(shared_from_this()))) {
        sendMessage(rsp);
        close();
    }
    else {
        sendMessage(rsp);
    }
}

void RockStream::handleNotify(sylar::RockNotify::ptr nty) {
    if (!m_notifyHandler(nty, std::dynamic_poniter_cast<RockStream>(shared_from_this()))) {
        close();
    }
}
 
RockRession::RockSession(Socket::ptr sock) 
    : RockStream(sock) {
    // RockSession是服务端收到连接后创建的，一旦连接断开，也就是客户端已主动断开连接，服务端不该主动重连
    m_autoConnect = false;
}

RockConnection::RockConnection()
    : RockStream(nullptr) {
    m_autoConnect = true;
}

bool RockConnection::connect(sylar::Address::ptr addr) {
    m_socket = sylar::Socket::CreateTCP(addr);
    return m_socket->connect(addr);
}

RockSDLoadBalance::RockSDLoadBalance(IServiceDiscovery::ptr sd)
    : SDLoadBalance(sd) {
}

// 根据服务发现信息，创建一个Rock协议连接
static SocketStream::ptr create_rock_stream(ServiceItemInfo::ptr info) {
    sylar::IPAddress::ptr addr = sylar::Address::LookupAnyIPAddress(info->getIp());
    if (!addr) {
        return nullptr;
    }
    addr->setPort(info->getPort());
    RockConnection::ptr conn(new RockConnection);
    // 将连接逻辑调度到“service_io”工作线程中异步执行
    sylar::WorkerMgr::GetInstance()->schedule("service_io", [conn, addr]() {
        conn->connect(addr);
        conn->start();
    });
    return conn;
}

void RockSDLoadBalance::start() {
    m_cb = create_rock_stream;
    initConf(g_rock_services->getValue());
    SDLoadBalance::start();
}

void RockSDLoadBalance::start(const std::unordered_map<std::string
    , std::unordered_map<std::string, std::string> >& confs) {
    m_cb = create_rock_stream;
    initConf(confs);
    SDLoadBalance::start();
}

void RockSDLoadBalance::stop() {
    SDLoadBalance::stop();
}

// 用于向指定的domain+service的服务节点发起一次RPC请求
RockResult::ptr RockSDLoadBalance::request(const std::string& domain, const std::string& service,
    RockRequest::ptr req, uint32_t timeout_ms, uint64_t idx = -1) {
    // 获得指定服务的负载均衡对象
    auto lb = get(domain, service);
    if (!lb) {
        return std::make_shared<RockResult>(ILoadBalance::NO_SERVICE, 0, nullptr, req);
    }
    // 负载均衡器从服务连接池中获取一个连接
    // 从服务连接池中选取一个有效连接
    // 若指定了 idx，则以 (idx % 连接数) 作为起始位置，按顺序轮询查找第一个有效连接
    // 若未指定 idx（即为 -1），则从随机位置开始轮询查找
    auto conn = lb->get(idx);
    uint64_t ts = sylar::GetCurrentMS();
    // 获取当前s对应的统计对象
    auto& stats = conn->get(ts / 1000);
    stats.incDoing(1);
    stats.incTotal(1);
    // 从连接对象中获取底层Rock流并发起request
    auto r = conn->getStreamAs<RockStream>()->request(req, timeout_ms);
    uint64_t ts2 = sylar::GetCurrentMS();
    if (r->result == 0) {
        stats.incOks(1);
        stats.incUsedTime(ts2 - ts);
    }
    else if (r->result == AsyncSocketStream::TIMEOUT) {
        stats.incTimeouts(1);
    }
    else if (r->result < 0) {
        stats.incErrs(1);
    }
    stats.decDoing(1);
    return r;
}

}