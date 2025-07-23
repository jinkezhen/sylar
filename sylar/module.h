#ifndef __SYLAR_MODULE_H__
#define __SYLAR_MODULE_H__

#include "sylar/stream.h"
#include "sylar/singleton.h"
#include "sylar/mutex.h"
#include "sylar/rock/rock_stream.h"
#include <map>
#include <unordered_map>

namespace sylar {

/**
* @brief 通用模块基类，支持生命周期管理、服务注册、网络请求处理等功能。
*
* 该类为框架中的所有模块提供统一的接口，支持模块加载、卸载、参数解析、连接管理、
* 消息请求与通知处理，以及服务注册功能。开发者可以继承此类来实现自定义模块。
*/
class Module {
public:
	enum Type {
		MODULE = 0,    // 通用模块
		ROCK = 1,      // 支持ROCK协议的模块
	};

	typedef std::shared_ptr<Module> ptr;

	Module(const std::string& name,
		   const std::string& version,
		   const std::string& filename,
		   uint32_t type = MODULE);
	virtual ~Module() {}

	/**
	 * @brief 参数解析前的钩子函数
	 * @param argc 命令行参数个数
	 * @param argv 命令行参数数组
	 *
	 * 可以在这里做参数检查、预处理等操作
	 */
	virtual void onBeforeArgsParse(int argc, char** argv);

	/**
	* @brief 参数解析后的钩子函数
	* @param argc 命令行参数个数
	* @param argv 命令行参数数组
	*
	* 可以在这里做参数确认、日志初始化等操作
	*/
	virtual void onAfterArgsParse(int argc, char** argv);

	// 模块加载时的回调函数，返回是否加载成功
	virtual bool onLoad();

	// 模块卸载时的回调函数，返回是否卸载成功
	virtual bool onUnload();

	/**
	 * @brief 有新连接时的回调函数（适用于 ROCK 模块）
	 * @param stream 与客户端的连接对象
	 * @return 是否处理成功
	 */
	virtual bool onConnect(sylar::Stream::ptr stream);

	/**
	 * @brief 连接断开时的回调函数（适用于 ROCK 模块）
	 * @param stream 与客户端的连接对象
	 * @return 是否处理成功
	 */
	virtual bool onDisconnect(sylar::Stream::ptr stream);

	/**
	 * @brief 服务器初始化完成（即将启动）时的回调函数
	 * @return 是否启动准备成功
	 */
	virtual bool onServerReady();

	/**
	 * @brief 服务器已经完全启动时的回调函数
	 * @return 是否启动成功
	 */
	virtual bool onServerUp();

	/**
	 * @brief 处理请求消息（适用于 ROCK 模块）
	 * @param req 请求消息对象
	 * @param rsp 响应消息对象
	 * @param stream 与客户端的连接对象
	 * @return 是否处理成功
	 */
	virtual bool handleRequest(sylar::Message::ptr req,
							   sylar::Message::ptr rsp,
					           sylar::Stream::ptr stream);

	/**
	 * @brief 处理通知消息（适用于 ROCK 模块）
	 * @param notify 通知消息对象
	 * @param stream 与客户端的连接对象
	 * @return 是否处理成功
	 */
	virtual bool handleNotify(sylar::Message::ptr notify,
		sylar::Stream::ptr stream);

	/**
	 * @brief 获取模块当前状态的字符串描述
	 * @return 状态字符串
	 */
	virtual std::string statusString();

	/**
	 * @brief 获取模块名称
	 * @return 模块名称
	 */
	const std::string& getName() const { return m_name; }

	/**
	 * @brief 获取模块版本
	 * @return 模块版本
	 */
	const std::string& getVersion() const { return m_version; }

	/**
	 * @brief 获取模块文件路径
	 * @return 模块文件路径
	 */
	const std::string& getFilename() const { return m_filename; }

	/**
	 * @brief 获取模块唯一 ID
	 * @return 模块 ID
	 */
	const std::string& getId() const { return m_id; }

	/**
	 * @brief 设置模块文件路径
	 * @param v 新的文件路径
	 */
	void setFilename(const std::string& v) { m_filename = v; }

	/**
	 * @brief 获取模块类型
	 * @return 模块类型
	 */
	uint32_t getType() const { return m_type; }

	/**
	 * @brief 注册服务信息（通常用于服务发现），向服务发现中心（如 ZooKeeper）注册服务节点。
	 * @param server_type 服务类型（如 HTTP、ROCK）
	 * @param domain 服务所属域
	 * @param service 服务名称
	 */
	void registerService(const std::string& server_type,
						 const std::string& domain,
			             const std::string& service);



protected:
	std::string m_name;      // 模块名称
	std::string m_version;   // 模块版本
	std::string m_filename;  // 模块文件路径
	std::string m_id;        // 模块唯一id
	uint32_t m_type;         // 模块类型（MODULE/ROCK）
};

// RockModule 是一个继承自 Module 的抽象类，
// 专门用于处理基于 Rock 协议的模块化服务逻辑。
// 它定义了 Rock 协议下的请求和通知的处理接口，
// 并重载了基类中的 handleRequest/handleNotify 来进行分发。
class RockModule : public Module {
public:
    typedef std::shared_ptr<RockModule> ptr;
    RockModule(const std::string& name
               ,const std::string& version
               ,const std::string& filename);
    
    // 处理 Rock 协议的请求消息
    virtual bool handleRockRequest(sylar::RockRequest::ptr request,
                                   sylar::RockResponse::ptr response,
                                   sylar::RockStream::ptr stream) = 0;
    // 处理 Rock 协议的通知消息
    virtual bool handleRockNotify(sylar::RockNotify::ptr notify,
                                  sylar::RockStream::ptr stream) = 0;    

    // 重载基类 Module 的 handleRequest
    // 将 Message 转换为 RockRequest/RockResponse 并分发给 handleRockRequest
    virtual bool handleRequest(sylar::Message::ptr req,
                               sylar::Message::ptr rsp,
                               sylar::Stream::ptr stream) override;

    // 重载基类 Module 的 handleNotify
    // 将 Message 转换为 RockNotify 并分发给 handleRockNotify
    virtual bool handleNotify(sylar::Message::ptr notify,
                              sylar::Stream::ptr stream) override;
};

// ModuleManager 类作用概述：
// ModuleManager 是一个 模块管理器类，主要负责：
// 管理系统中所有加载的模块（Module）
// 提供模块的注册、注销、遍历等功能
// 支持模块按名字或类型访问
// 在连接/断开时调用模块钩子
// 配合模块系统支持动态模块加载（.so 文件）
class ModuleManager {
public:
    typedef RWMutex RWMutexType;

    ModuleManager();

    // 添加模块
    void add(Module::ptr m);
    // 删除模块
    void del(const std::string& name);
    // 删除所有模块
    void delAll();
    // 初始化模块（通常在启动阶段调用）
    void init();
    // 获取某个模块（按名称）
    Module::ptr get(const std::string& name);
    // 通知所有模块：某个连接建立 
    // 当系统接收到新的连接时，会调用每个模块的 onConnect()。
    void onConnect(Stream::ptr stream);
    // 通知所有模块：某个连接断开
    // 当连接断开时，调用模块的 onDisconnect() 方法。
    void onDisconnect(Stream::ptr stream);
    // 获取所有模块列表
    void listAll(std::vector<Module::ptr>& ms);
    // 获取指定类型的模块列表
    // 根据模块类型（例如 ROCK 或 MODULE）筛选出对应的模块。
    void listByType(uint32_t type, std::vector<Module::ptr>& ms);
    // 遍历指定类型的模块并执行回调
    void foreach(uint32 type, std::function<void(Module::ptr)> cb);

private:
    void initModule(const std::string& path);

private:
    RWMutexType m_mutex;
    // 模块名 -> 模块对象的映射表。
    std::unordered_map<std::string, Module::ptr> m_modules;
    // 按模块类型分类的模块映射：
    // 第一层 key 是模块类型（如 MODULE = 0, ROCK = 1）
    // 第二层 key 是模块名，value 是模块对象
    std::unordered_map<uint32_t, std::unordered_map<std::string, Module::ptr>> m_type2Modules;
};


}

#endif