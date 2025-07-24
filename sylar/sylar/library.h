#ifndef __SYLAR_LIBRARY_H__
#define __SYLAR_LIBRARY_H__

#include <memory>
#include "module.h"

namespace sylar {

//这个 sylar::Library 类，是 Sylar 框架中用于动态加载模块（Module） 
//的一个工具类，目的是支持“插件化架构” —— 即模块化部署功能
//（比如 HTTP 服务模块、RPC 服务模块、用户自定义模块等）可以在
//运行时动态加载，不需要编译进主程序。

class Library {
public:
	// path：某个共享库的路径
	static Module::ptr GetModule(const std::string& path);
};


}