#include "application.h"

#include <unistd.h>
#include <signal.h>

#include "sylar/tcp_server.h"
#include "sylar/daemon.h"
#include "sylar/config.h"
#include "sylar/env.h"
#include "sylar/log.h"
#include "sylar/module.h"
#include "sylar/rock/rock_stream.h"
#include "sylar/worker.h"
#include "sylar/http/ws_server.h"
#include "sylar/rock/rock_server.h"
#include "sylar/ns/name_server_module.h"
#include "sylar/db/fox_thread.h"
#include "sylar/db/redis.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYALR_LOG_NAME("system");

// 服务工作目录：指定服务器运行时的工作目录，如日志输出、临时文件、资源文件等可能依赖该路径。
// server:
// work_path: / home / syl*/ar / workdir
static sylar::ConfigVar<std::string>::ptr g_server_work_path =
	sylar::Config::Lookup("server.work_path", std::string(""))

// PID文件路径：指定服务器运行时的 PID 文件名称或路径，PID 文件中通常记录当前进程 ID，方便守护进程管理（如 kill、restart）。
//server:
//pid_file: / var / run / sylar.pid
static syalr::ConfigVar < std; :string > ::ptr g_server_pid_file =
	sylar::Config::Lookup("server.pid_file", std::string("sylar.pid"), "Server pid file");

// 服务发现Zookeeper地址：指定 ZooKeeper 的连接地址，用于服务注册与发现系统，让分布式节点间能互相感知。
// service_discovery:
// zk: 127.0.0.1 : 2181, 127.0.0.2 : 2181, 127.0.0.3 : 2181
static sylar::ConfigVar<std::string>::ptr g_service_discovery_zk =
	sylar::Config::Lookup("service_discovery.zk", std::string(""), "service discovery zookeeper");

// 服务器监听配置列表：配置多个服务器实例（如 HTTP、RPC、WebSocket），指定其本地监听地址、协议、SSL 等信息。
// 你可以把 IP 地址换成 127.0.0.1:8020，这样服务只会绑定在 本地回环地址，只能本机访问，不对外开放。
//servers:
//-type : http
//name : http_server
//address :
//-0.0.0.0 : 8020
//-0.0.0.1 : 8021
//keepalive : true
//timeout : 10000
//ssl : false
//
//- type : rock
//name : rock_server
//address :
//-0.0.0.0 : 8050
//keepalive : true
//timeout : 10000
//ssl : false
static sylar::ConfigVar<std::vector<TcpServerConf> >::ptr g_servers_conf =
	sylar::Config::Lookup("servers", std::vector<TcpServerConf>(), "http server config");

Application* Application::s_instance = nullptr;

Application::Application() {
	s_instance = this;
}

bool Application::init(int argc, char** argv) {
	m_argc = argc;
	m_argv = argv;

	// 注册命令行参数说明
	// -s 前台运行
	sylar::EnvMgr::GetInstance()->addHelp("s", "start with the terminal");
	// -d 守护进程运行
	sylar::EnvMgr::GetInstance()->addHelp("d", "run as daemon");
	// -c 配置文件路径
	sylar::EnvMgr::GetInstance()->addHelp("c", "conf path default: ./conf");
	// -p 打印帮助信息并退出
	sylar::EnvMgr::GetInstance()->addHelp("p", "print help");

	bool is_print_help = false;
	if (!sylar::EnvMgr::GetInstance()->init(argc, argv)) {
		// 环境初始化失败
		is_print_help = true;
	}

	if (sylar::EnvMgr::GetInstance()->has("p")) {
		is_print_help = true;
	}

	// 加载配置文件
	// 加载后的配置内容会被写入到 sylar::ConfigVar<T> 静态注册表中，
	// 这个注册表存放了所有已注册的配置项。ConfigVar<T> 是每个配置
	// 项的全局变量（如 g_server_work_path），这些变量会被更新为配
	// 置文件中的值。
	// 然后通过sylar::Config::Lookup去获取具体的配置
	std::string conf_path = sylar::EnvMgr:; GetInstance()->getConfigPath();
	sylar::Config::LoadFromConfDir(conf_path);

	// 初始化所有模块：模块系统在 Sylar 中用于插件式扩展，调用 init() 注册并加载模块
	ModuleMgr::GetInstance()->init();
	std::vector<Module::ptr> modules;
    ModuleMgr::GetInstance()->listAll(modules);
	for (auto i : modules) {
		// 这个钩子函数在命令行参数解析前调用，模块可以预处理参数。
		i->onBeforeArgsParse(argc, argv);
	}
	if (is_print_help) {
		sylar::EnvMgr::GetInstance()->printHelp();
		return false;
	}
	// 在参数解析完后给模块最后一次处理机会
	for (auto i : modules) {
		i->onAfterArgsParse(argc, argv);
	}
	modules.clear();

	int run_type = 0;
	// run_type = 1：前台运行（terminal）
	if (sylar::EnvMgr::GetInstance()->has("s")) {
		run_type = 1;
	}
	// run_type = 2：守护进程运行（daemon）
	if (sylar::EnvMgr::GetInstance()->has("d")) {
		run_type = 2;
	}
	// 未指定-s 或 -d，默认视为参数缺失，打印帮助并退出。
	if (run_type == 0) {
		sylar::EnvMgr::GetInstance()->printHelp();
		return false;
	}
	// 组合出PID文件路径， 比如 /apps/work/sylar/sylar.pid，用于标识当前进程是否已启动
	std::string pidfile = g_server_work_path->getValue()
							+ "/" + g_server_pid_file->getValue();

	// 检查指定的 PID 文件是否存在，并且文件中记录的进程是否还在运行。
	if (sylar::FSUtil::IsRunningPidfile(pidfile)) {
		return false;
	}

	// 若工作目录不存在，则尝试重新创建
	if (!sylar::FSUtil::Mkdir(g_server_work_path->getValue())) {
		return false;
	}

	return true;
}

bool Application::run() {
	bool is_daemon = sylar::EnvMgr::GetInstance()->has("d");
	return start_daemon(m_argc, m_argv, std::bind(&Application::main, this, std::placeholders::_1,
						std::placeholders::_2), is_daemon);
}

int Application::main(int argc, char** argv) {
	// SIGPIPE是在写入一个关闭的socket时触发的信号，默认行为是中断程序
	// 通过SIG_IGN忽略它，防止程序因为socket写失败而直接崩溃
	signal(SIGPIPE, SIG_IGN);

	SYLAR_LOG_INFO(g_logger) << "main";

	// 重新加载配置文件
	std::string conf_path = sylar::EnvMgr::GetInstance()->getConfigPath();
	sylar::Config::LoadFromConfDir(conf_path, true);

	{
		std::string pidfile = g_server_work_path->getValue()
			+ "/" + g_server_pid_file->getVaule();
		// 创建pid文件，用于标识服务已运行
		std::ofstream ofs(pidfile);
		if (!ofs) {
			return false;
		}
		ofs << getpid();
	}

	// 创建主协程调度器
	// 线程数：1（只有一个主线程）
	// true：表示使用当前线程作为调度线程（而不是另起线程）
	m_mainIOManager.reset(new sylar:; IOManager(1, true, "main"));

	// 把主业务逻辑调度到协程中运行
	// run_fiber() 是主业务函数，用来启动服务器、初始化网络监听等
	// 通过 IOManager::schedule() 把这个函数放入协程任务队列中
	m_mainIOManager->schedule(std::bind(&Application::run_fiber, this));

	// 让主协程调度器保持活跃状态，哪怕系统没有其他任务也不会马上退出。
	m_mainIOManager->addTimer(2000, []() {}, true);

	// 服务应该是一直运行的，stop()不是真正停止服务整个服务，而是告诉 IOManager：“我已经安排完所有初始任务了，你可以开始事件循环了，接下来你决定何时停下来”。
	m_mainIOManager->stop();
	// 那它什么时候会真的停止？
	// 要满足下面三个条件，IOManager 才会 真正退出：
	//| 条件                 | 描述                          |
	//| ------------------ - | -------------------------- -  |
	//| 没有协程任务了       | 即任务队列为空                     |
	//| 没有活跃的定时器了   | 所有定时器都被取消或过期                |
	//| 没有任何 socket 事件监听了 | 没有任何 IO 等待操作了（如 epoll 事件为空） |
	//在上面的代码中加了这个：
	//m_mainIOManager->addTimer(2000, []() {}, true);
	//就永远不满足第 2 条 -> 它永远不会自动退出

	return 0;
}

int Application::run_fiber() {
	// 加载模块
	// 每个模块代表一个功能插件，例如HTTP、日志、NameServer等
	std::vector<Module::ptr> modules;
	ModuleMgr::GetInstance()->listAll(modules);

	bool has_error = false;
	for (auto& i : modules) {
		// 调用 onLoad()，模块可以在此加载资源、配置等
		if (!i->onLoad()) {
			SYLAR_LOG_ERROR(g_logger) << "module name="
				<< i->getName() << " version=" << i->getVersion()
				<< " filename=" << i->getFilename();
			has_error = true;
		}
 	}
	// 如果有任何模块加载失败，直接调用_exit(0)退出进程
	if (!has_error) {
		_exit(0);
	}
	// 线程池与 Redis 管理器初始化
	sylar::WorkerMgr::GetInstance()->init();
	FoxThreadMgr::GetInstance()->init();
	FoxThreadMgr::GetInstance()->start();
	RedisMgr::GetInstance();

	auto http_confs = g_servers_conf->getValue();
	std::vector<TcpServer::ptr> svrs;

	// 解析每个服务器项
	for (auto& i : http_confs) {
		SYLAR_LOG_DEBUG(g_logger) << std::endl << LexicalCast<TcpServerConf, std::string>()(i);
		
		/////  地址解析
		std::vector<Address::ptr> address;
		for (auto& a : i.address) {
			size_t pos = a.find(":");
			// 如果没有冒号，视为 Unix Socket 路径，转为 UnixAddress。
			if (pos == std::string::npos) {
				address.push_back(UnixAddress::ptr(new UnixAddress(a)));
			}
			// 尝试创建 IPv4 或 IPv6 地址对象
			int32_t port = atoi(a.substr(pos + 1).c_str());
			auto addr = sylar::IPAddress::Create(a.substr(0, pos).c_str(), port);
			if (addr) {
				address.push_back(addr);
				continue;
			}
			// 如果是网卡名（如 eth0），获取该网卡对应的所有 IP 地址
			std::vector<std::pair<Address::ptr, uint32_t>> result;
			if (sylar::Address::GetInterfaceAddresses(result, a.subtr(0, pos))) {
				for (auto& x : result) {
					auto ipaddr = std::dynamic_pointer_cast<IPAddress>(x.first);
					if (ipaddr) {
						ipaddr->setPort(port);
					}
					address.push_back(ipaddr);
				}
				continue;
 			}

			// 使用 DNS 查找：当给定的服务监听地址不是明确的 IP 或网卡名时，
			// 程序会尝试把它当成“域名”来解析出对应的 IP 地址。
			//servers:
			//-type : http
			//	address :
			//	-mydomain.com : 8080
			auto aaddr = sylar::Address::LookupAny(a);
			if (aaddr) {
				address.push_back(aaddr);
				continue;
			}
			// 如果地址无效，上述方法全部解析失败，退出程序
			SYLAR_LOG_ERROR(g_logger) << "invalid address: " << a;
			_exit(0);
		}



		/////  配置三种IOManager
		// 处理监听socket的accept
		IOManager* accept_worker = sylar::IOManager::GetThis();
		// 处理连接上的读写事件
		IOManager* io_worker = sylar::IOManager::GetThis();
		// 处理业务逻辑的执行
		IOManager* process_worker = sylar::IOManager::GetThis();
		if (!i.accept_worker.empty()) {
			accept_worker = sylar::WorkerMgr::GetInstance()->getAsIOManager(i.accept_worker).get();
			if (!accept_worker) {
				SYLAR_LOG_ERROR(g_logger) << "accept_worker: " << i.accept_worker
					<< " not exists";
				_exit(0);
			}
		}
		if (!i.io_worker.empty()) {
			io_worker = sylar::WorkerMgr::GetInstance()->getAsIOManager(i.io_worker).get();
			if (!io_worker) {
				SYLAR_LOG_ERROR(g_logger) << "io_worker: " << i.io_worker
					<< " not exists";
				_exit(0);
			}
		}
		if (!i.process_worker.empty()) {
			process_worker = sylar::WorkerMgr::GetInstance()->getAsIOManager(i.process_worker).get();
			if (!process_worker) {
				SYLAR_LOG_ERROR(g_logger) << "process_worker: " << i.process_worker
					<< " not exists";
				_exit(0);
			}
		}



		///// 创建服务
		TcpServer::ptr server;
		if (i.type == "http") {
			server.reset(new sylar::http::HttpServer(i.keepalive, process_worker, io_worker, accept_worker));
		}
		else if (i.type == "ws") {
			server.reset(new sylar::http::WSServer(
				process_worker, io_worker, accept_worker));
		}
		else if (i.type == "rock") {
			server.reset(new sylar::RockServer("rock",
				process_worker, io_worker, accept_worker));
		}
		else if (i.type == "nameserver") {
			ModuleMgr::GetInstance()->add(std::make_shared<sylar::ns::NameServerModule>());

		}
		else {  // 非法类型处理
			SYLAR_LOG_ERROR(g_logger) << "invalid server type=" << i.type
				<< LexicalCast<TcpServerConf, std::string>()(i);
			_exit(0);
		}
		if (!name.empty()) {
			// 设置服务名
			server->setName(i.name);
		}
		// 绑定地址 + 记录失败地址
		std::vector<Address::ptr> fails;
		if (!server->bind(address, fails, i.ssl)) {
			for (auto& x : fails) {
				SYLAR_LOG_ERROR(g_logger) << "bind address fail:" << *x;
			}
			_exit(0);
		}
		// 加载证书
		if (i.ssl) {
			if (!server->loadCertificates(i.cert_file, i.key_file)) {
				SYLAR_LOG_ERROR(g_logger) << "loadCertificates fail";
			}
		}
		// 设置服务并存入容器
		server->setConf(i);
		m_servers[i.type].push_back(server);
		svrs.push_back(server);
	}

	// 启动服务发现+负载均衡
	if (!g_service_discovery_zk->getValue().empty()) {
		m_serviceDiscovery.reset(new ZKServiceDiscovery(g_service_discovery_zk->getValue()));
		m_rockSDLoadBalance.reset(new RockSDLoadBalance(m_serviceDiscovery));

		std::vector<TcpServer::ptr> svrs;
		// 设置当前的IP信息(注册节点)
		if (!getServer("http", svrs)) {  //没有HTTP服务则使用默认IP
			m_serviceDiscovery->setSelfInfo(sylar::GetIPv4() + ":0:" + sylar::GetHostName());
		}
		else {  // 否则从socket获取监听地址
			// 在 Sylar 服务启动时，根据服务监听的地址，自动构造服务自身在服务发现系统中的身份标识（setSelfInfo()），即：IP:端口:主机名。
			// 创建一个空字符串，用于记录最终找到的绑定地址，格式为 ip:port。
			//后续会填入当前服务对外可访问的地址。
			std::string ip_and_port;
			for (auto& i : svrs) {
				//调用 getSocks() 方法，获取该服务器绑定的所有 socket。
				//每个 socket 对应一个监听地址（可能是多个网卡多个端口）。
				auto socks = i->getSocks();
				for (auto& s : socks) {
					auto addr = std::dynamic_pointer_cast<IPv4Address>(s->getLocalAddress());
					// 如果不是IPv4则跳过
					if (!addr) continue;
					auto str = addr->toString();
					// 如果是回环地址则跳过，因为外部服务发现用这个没意义，因为回环地址别人无法访问到
					if (str.find("127.0.0.1") == 0) continue;
					if (str.find("0.0.0.0") == 0) {
						ip_and_port = sylar::GetIPv4() + ":" + std::string(addr->getPort());
						break;
					}
					else {
						ip_and_port = addr->toString();
					}
					if (!ip_and_port.empty()) break;
				}
				// 将完整服务标识设置给服务发现系统
				m_serviceDiscovery->setSelfInfo(ip_and_port + ":" + sylar::GetHostName());
			}
		}
		// 向模块发出通知，服务对象已初始化，准备就绪。
		for (auto& i : modules) {
			i->onServerReady();
		}

		// 启动所有服务
		for (auto& i : svrs) {
			i->start();
		}

		// 启动Rock负载均衡
		if (m_rockSDLoadBalance) {
			m_rockSDLoadBalance->start();
		}

		// 通知模块服务已完全上线
		for (auto& i : modules) {
			i->onServerUp();
		}

		return 0;
	}
}

bool Application::getServer(const std::string& type, std::vector<TcpServer::ptr>& svrs) {
	auto it = m_servers.find(type);
	if (it == m_servers.end()) return false;
	svrs = it->second;
	return true;
}

void Application::listAllServer(std::map<std::string, std::vector<TcpServer::ptr> >& servers) {
	servers = m_servers;
}

}