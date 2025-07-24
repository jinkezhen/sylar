#include "library.h"

#include <dlfcn.h>
#include "sylar/config.h"
#include "sylar/env.h"
#include "sylar/log.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

typedef Module* (create_module)();
typedef void (*destory_module)(Module*);

// 模块的资源释放类
// ModuleCloser 是一个仿函数（functor），用于在智能指针释放 Module* 时执行自定义清理逻辑。
class ModuleCloser {
public:
	ModuleCloser(void* handle, destory_module d)
		: m_handle(handle), m_destory(d) {
	}

	// 重载()运算符  仿函数
	void operator()(Module* module) {
		// 当 std::shared_ptr<Module> 被销毁时会调用这个 operator() 来做清理工作。
		std::string name = module->getName();
		std::string version = module->getVersion();
		std::string path = module->getFilename();
		m_destory(module);
		int rt = dlclose(m_handle);
		if (rt) {
			SYLAR_LOG_ERROR(g_logger) << "dlclose handle fail handle="
				<< m_handle << " name=" << name
				<< " version=" << version
				<< " path=" << path
				<< " error=" << dlerror();
		}
		else {
			SYLAR_LOG_INFO(g_logger) << "destory module=" << name
				<< " version=" << version
				<< " path=" << path
				<< " handle=" << m_handle
				<< " success";
		}
	}

private:
	// 动态库句柄（由dlopen返回）
	void* m_handle;
	// Module的销毁函数
	destory_module m_destory;
};


//在 Sylar 框架中，每个模块（.so 文件）必须按照统一的接口格式实现下面两个函数：
//extern "C" sylar::Module * CreateModule();
//extern "C" void DestoryModule(sylar::Module * m);
Module::ptr Library::GetModule(const std::string& path) {
	// RTLD_NOW 表示立即解析出所有符号（函数、变量），不延迟
	void* handle = dlopen(path.c_str(), RTLD_NOW);
	if (!handle) {
		SYLAR_LOG_ERROR(g_logger) << "cannot load library path="
			<< path << " error=" << dlerror();
		return nullptr;
	}
	// 从共享库中获取函数指针CreateModule
	create_module create = (create_module)dlsym(handle, "CreateModule");
	if (!create) {
		SYLAR_LOG_ERROR(g_logger) << "cannot load symbol CreateModule in "
			<< path << " error=" << dlerror();
		dlclose(handle);
		return nullptr;
	}
	// 从共享库中获取销毁函数
	destory_module destory = (destory_module)dlsym(handle, "DestoryModule");
	if (!destory) {
		SYLAR_LOG_ERROR(g_logger) << "cannot load symbol DestoryModule in "
			<< path << " error=" << dlerror();
		dlclose(handle);
		return nullptr;
	}
	Module::ptr module(create(), ModuleCloser(handle, destory));
	module->setFilename(path);
	SYLAR_LOG_INFO(g_logger) << "load module name=" << module->getName()
		<< " version=" << module->getVersion()
		<< " path=" << module->getFilename()
		<< " success";
	// 重新加载配置目录
	//强制重新加载配置目录（. / conf 或用户通过 - c 参数指定的路径）。
	//	原因：
	//	加载模块后，模块可能会注册新的配置项；
	//	因此需要重新读取.yml 文件，以确保新模块的配置生效。
	Config::LoadFromConfDir(sylar::EnvMgr::GetInstance()->getConfigPath(), true);
	// 该module已经具备自动销毁和资源释放的机制
	return module;
}

}