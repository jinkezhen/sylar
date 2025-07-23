#include "rock_server.h"
#include "sylar/log.h"
#include "sylar/module.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

RockServer::RockServer(const std::string& type
    , sylar::IOManager* worker
    , sylar::IOManager* io_worker
    , sylar::IOManager* accept_worker)
    :TcpServer(worker, io_worker, accept_worker) {
    m_type = type;
}

// 当服务端监听到有新连接的，会自动调用此函数
void RockServer::handleClient(Socket::ptr client) {
    SYLAR_LOG_DEBUG(g_logger) << "handleClient " << *client;
    sylar::RockSession::ptr session(new sylar::RockSession(client));
    session->setWorker(m_worker);
    // 通知所有 ROCK 类型模块有连接建立
    ModuleMgr::GetInstance()->foreach(Module::ROCK,
        [session](Module::ptr m) {
            m->onConnect(session);
    });
    // 设置连接断开时的回调
    session->setDisconnectCb([](AsyncSocketStream::ptr stream) {
        ModuleMgr::GetInstance()->foreach(Module::ROCK,
        [stream](Module::ptr m) {
                m->onDisconnect(stream);
            })
            });
    // 设置请求处理函数
    // 当客户端发来一个 RockRequest（ROCK协议的请求包）时，调用这个 lambda。
    // 遍历所有注册的 ROCK 模块：
    // 调用每个模块的 handleRequest() 来处理请求。
    // 如果有一个模块返回了 true（表示处理了该请求），就不再继续调用其他模块（短路逻辑）。
    session->setRequestHandler([](sylar::RockRequest::ptr req, sylar::RockResponse::ptr rsp, sylar::RockStream::ptr conn)->bool {
            bool rt = false;
            ModuleMgr::GetInstance()->forearch(Module::ROCK, [&rt, req, rsp, conn](Module::ptr m) {
                if (rt) {
                    return;
                }
                rt = m->handleRequest(req, rsp, conn);
                });
            return rt;
        })
    // 设置通知处理函数
    session->setNotifyHandler([](sylar::RockNotify::ptr nty, sylar::RockStream::ptr conn)->bool {
        SYLAR_LOG_INFO(g_logger) << "handleNty " << nty->toString()
            << " body=" << nty->getBody();
        bool rt = false;
        ModuleMgr::GetInstance()->foreach(Module::ROCK, [&rt, nty, conn](Module::ptr m) {
            if (rt) return;
            rt = m->handleNotify(nty, conn);});
        return rt;
    }
    );
    // 启动会话，开始异步读写
    // 会在设置的m_worker中持续进行
    session->start();
}

}