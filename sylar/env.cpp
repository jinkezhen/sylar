#include "env.h"
#include "sylar/log.h"
#include <string.h>
#include <iostream>
#include <iomanip>
#include <unistd.h>
#include <stdlib.h>
#include "config.h"

namespace sylar {

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

// 传入的是main函数的argc/argv参数
// argc：参数个数（argument count），包括程序名。
// argv：参数数组（argument vector），是一个 char* []，每一项都是一个以 \0 结尾的 C 字符串。
// ./my_server -config conf.yaml -d
//| `argv[i]` |       值         |
//| -------- -| -------------- - |
//| `argv[0]` | `". / my_server"`|
//| `argv[1]` | `"-config"`      |
//| `argv[2]` | `"conf.yaml"`    |
//| `argv[3]` | `"-d"`           | 

bool Env::init(int argc, char** argv) {
	// 用来存储要读取的符号链接路径  /proc/pid/exe
	char link[1024] = { 0 };
	// 读取出来的真实可执行文件路径
	char path[1024] = { 0 };

	// 构造 Linux 下 /proc 文件系统中当前进程的 exe 链接路径。
	// /proc/<pid>/exe 是 Linux 提供的符号链接，指向当前进程正在执行的可执行文件，非常有用。
	sprintf(link, "proc/%d/exe", getpid());
	// 调用readline读取/proc/<pid>/exe的符号链接内容，得到的是当前进程中可执行文件的绝对路径，写入path中
	readlink(link, path, sizeof(path));

	// m_exe = "/home/user/sylar/bin/my_server"
	m_exe = path;

	auto pos = m_exe.find_last_of("/");
	// 截取可执行文件所在的目录，也就是执行目录
	// m_cwd = "/home/user/sylar/bin/"
	m_cwd = m_exe.substr(0, pos) + "/";

	// 保存程序名（通常是路径），就是命令行中 argv[0] 的内容，代表的是执行程序的命令。
	// ./my_server → argv[0] = "./my_server"
	m_program = argv[0];

	// 当前正在解析的“参数键”的名字，例如--config中的config
	const char* now_key = nullptr;
	for (int i = 1; i < argc; ++i) {
		//  如果当前参数以 - 开头，认为是“参数名”（键）。
		if (argv[i][0] == "-") {
			if (strlen(argv[i]) > 1) {
				// key没有对应的值，将它的值设置为""
				// ./server -d → now_key = "d"，没有值 → 添加 "d" = ""
				if (now_key) {
					add(now_key, "");
				}
				// argv[i] 是一个 char* ，指向命令行参数字符串，比如 "-config"
				// argv[i] + 1 是把指针向后偏移一个字符，相当于跳过开头的 -
				now_key = argv[i] + 1;
			} 
			else { // 排除只有一个 - 的情况（例如 - 本身，不合法）
				return false;
			}
		}
		else {
			// 如果当前有 key，说明这是它的值 → 添加 key/value
			if (now_key) {
				add(now_key, argv[i]);
				now_key = nullptr;
			}
			else {
				return false;
			}
		}
	}
	if (now_key) {
		add(now_key, "");
	}
	return true;
}

void Env::add(const std::string& key, const std::string& val) {
	RWMutexType::WriteLock lock(m_mutex);
	m_args[key] = val;
}

bool Env::has(const std::string& key) {
	RWMutexType::ReadLock lock(m_mutex);
	auto it = m_args.find(key);
	return it != m_args.end();
}

void Env::del(const std::string& key) {
	RWMutexType::WriteLock lock(m_mutex);
	m_args.erase(key);
}

std::string Env::get(const std::string& key, const std::string& default_value) {
	RWMutexType::ReadLock lock(m_mutex);
	auto it = m_args.find(key);
	return it != args.end() ? it->second : default_value;
}

std::string Env::addHelp(const std::string& key, const std::string& desc) {
	removeHelp(key);
	RWMutexType::WriteLock lock(m_mutex);
	m_helps.push_back(std::make_pair<key, desc>);
}

void Env::removeHelp(const std::string& key) {
	RWMutexType::WriteLock lock(m_mutex);
	for (auto& it = m_helps.begin(); it != m_helps.end()) {
		if (it->first == key) {
			it->m_helps.erase(it);
		}
		else {
			++it;
		}
	}
}

void Env::printHelp() {
	RWMutexType::ReadLock lock(m_mutex);
	for (auto& i : m_helps) {
		std::cout << std::setw(5) << "-" << i.first << ":" << i.second << std::endl;
	}
}

bool Env::setEnv(const std::string& key, const std::string& val) {
	return !setenv(key.c_str(), val.c_str(), 1);
}

std::string Env::getEnv(const std::string& key, const std::string& default_value) {
	const char* v = getenv(key.c_str());
	if (v == nullptr) {
		return default_value;
	}
	return v;
}

std::string Env::getAbsolutePath(const std::string& path) const {
	if (path.empty()) {
		return "/";
	}
	if (path[0] == "/") {
		return path;
	}
	return m_cwd + path;
}

std::string Env::getAbsoluteWorkPath(const std::string& path) const {
	if (path.empty()) {
		return "/";
	}
	if (path[0] == '/') {
		return path;
	}
	static sylar::ConfigVar<std::string>::ptr g_server_work_path =
		sylar::Config::Lookup<std::string>("server.work_path");
	return g_server_work_path->getValue() + "/" + path;
}

std::string Env::getConfigPath() {
	return getAbsolutePath(get("g", "conf"));
}

}