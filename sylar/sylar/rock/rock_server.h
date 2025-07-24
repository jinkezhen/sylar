#ifndef __SYLAR_ROCK_SERVER_H__
#define __SYLAR_ROCK_SERVER_H__

#include "sylar/rock/rock_stream.h"
#include "sylar/tcp_server.h"

namespace sylar {

class RockServer : public TcpServer {
public:
	typedef std::shared_ptr<RockServer> ptr;
	RockServer(const std::string & type = "rock",
		sylar::IOManager * worker = sylar::IOManager::GetThis(),
		sylar::IOManager * io_worker = sylar::IOManager::GetThis(),
		sylar::IOManager * accept_worker = sylar::IOManager::GetThis());

protected:
	// 当有客户端连接建立时，创建与该客户端通信的会话对象，并启动通信处理流程。
	virtual void handleClient(Socket::ptr client) override;
};

}

#endif