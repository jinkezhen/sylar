#ifndef __SYLAR_APPLICATION_H__
#define __SYLAR_APPLICATION_H__

#include "sylar/http/http_server.h"
#include "sylar/streams/service_discovery.h"
#include "sylar/rock/rock_stream.h"

namespace sylar {

/**
 * @brief 应用程序核心管理类（单例模式）
 * 
 * 功能概述：
 * 1. 管理所有类型的 TCP 服务器（HTTP/Rock 等）
 * 2. 提供服务发现和负载均衡能力
 * 3. 统一初始化入口和主循环控制
 * 4. 协程调度管理
 * 
 * 使用示例：
 * int main(int argc, char** argv) {
 *     sylar::Application app;
 *     if(app.init(argc, argv)) {
 *         return app.run();
 *     }
 *     return -1;
 * }
 */
// sylar::Application 类是 Sylar 框架的应用程序入口封装类，
//其作用是对应用启动流程、服务注册与发现、服务器初始化、
//运行等进行统一管理与调度。它是整个框架的启动核心，
//结合 IOManager、服务发现组件（如 ZooKeeper）、服务负载
//均衡（如 Rock 协议）以及多个 TCP 服务器实例，协调地完成
//一个完整的服务端程序启动和运行过程。

//Application 类提供了以下关键功能：
//应用初始化与启动
//init(int argc, char** argv)：负责解析命令行参数、初始化运行环境、配置加载、日志系统初始化等。
//run()：启动 IOManager 协程调度器，执行主协程逻辑，即进入业务主循环。
//TCP 服务器管理
//支持按类型（如 HTTP、RPC 等）组织多个 TcpServer 实例。
//getServer 与 listAllServer 提供了获取和遍历所有服务器实例的接口。
//服务注册与发现集成
//内部集成了 ZKServiceDiscovery（基于 ZooKeeper 的服务发现）和 RockSDLoadBalance（Rock 协议的负载均衡器）。
//启动时自动将本地服务节点注册到 ZooKeeper，并支持从 ZooKeeper 动态发现其他服务节点。
//IO 管理器支持
//m_mainIOManager：主 IO 协程调度器，承载了主线程的事件循环（如 accept、读写等 IO 事件），是整个网络调度的核心。
//单例设计
//使用 s_instance 静态成员维护唯一实例，方便其他模块统一访问（例如模块系统、守护进程控制器、配置系统等都可以通过 Application::GetInstance() 获取全局上下文）。


class Application {
public:
	Application();

	// 全局唯一实例
	static Application* GetInstance() { return s_instance; }

	// 初始化函数
	void init(argc, char** argv);

	// 启动运行主循环，启动IOManager以及所有服务
	bool run();

	/**
	 * @brief 按类型获取服务器实例列表（如 "http", "rpc"）
	 * @param type 服务器类型
	 * @param svrs 输出服务器列表
	 */
	bool getServer(const std::string& type, std::vector<TcpServer::ptr>& svrs);

	// 获取所有类型的服务器列表
	void listAllServer(std::map<std::string, std::vector<TcpServer::ptr>>& servers);

	/**
	 * @brief 获取服务发现模块（ZooKeeper）
	 */
	ZKServiceDiscovery::ptr getServiceDiscovery() const { return m_serviceDiscovery; }

	/**
	 * @brief 获取 Rock 协议的负载均衡管理器
	 */
	RockSDLoadBalance::ptr getRockSDLoadBalance() const { return m_rockSDLoadBalance; }

private:
	// 内部主逻辑入口
	int main(int argc, char** argv);

	// 启动主协程执行体（实际调度入口）
	int run_fiber();

private:
	int m_argc = 0;           //命令行参数个数
	char** m_argv = nullptr;  //命令行参数数组

	/// 服务器分类存储映射表
	/// Key: 服务器类型标识（如 "http"）
	/// Value: 该类型服务器实例列表
	/// 一个服务类型（比如 "http"）可能对应多个监听点（socket），
	///		所以需要用一个 vector 来存储多个 TcpServer::ptr 实例
	std::map<std::string, std::vector<TcpServer::ptr>> m_servers;

	// 主协程IO调度器(管理整个应用事件循环)
	IOManager::ptr m_mainIOManager;
	// 单例实例指针
	static Application* s_instance;

	// 服务发现组件（基于Zookeeper实现）
	ZKServiceDiscovery::ptr m_serviceDiscovery;
	// Rock协议专用的服务发现负载均衡器
	RockSDLoadBalance::ptr m_rockSDLoadBalance;
};


}


#endif