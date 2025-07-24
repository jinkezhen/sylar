#include "module.h"
#include "config.h"
#include "env.h"
#include "library.h"
#include "util.h"
#include "log.h"
#include "application.h"


namespace sylar {

// g_module_path存放所有模块（.so 文件） 的目录
static sylar::ConfigVar<std::string>::ptr g_module_path = 
                        Config::Lookup("module.path", std::string("module"), "module_path");

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

Module::Module(const std::string& name, const std::string& version, const std::string& filename, uint32_t type) 
    :m_name(name)
    ,m_version(version)
    ,m_filename(filename)
    ,m_id(name + "/" + version)
    ,m_type(type) {
}

void Module::onBeforeArgsParse(int argc, char** argv) {
}

void Module::onAfterArgsParse(int argc, char** argv) {
}

bool Module::handleRequest(sylar::Message::ptr req
                           ,sylar::Message::ptr rsp
                           ,sylar::Stream::ptr stream) {
    SYLAR_LOG_DEBUG(g_logger) << "handleRequest req=" << req->toString()
            << " rsp=" << rsp->toString() << " stream=" << stream;
    return true;
}

bool Module::handleNotify(sylar::Message::ptr notify
                          ,sylar::Stream::ptr stream) {
    SYLAR_LOG_DEBUG(g_logger) << "handleNotify nty=" << notify->toString()
            << " stream=" << stream;
    return true;
}

bool Module::onLoad() {
    return true;
}

bool Module::onUnload() {
    return true;
}

bool Module::onConnect(sylar::Stream::ptr stream) {
    return true;
}

bool Module::onDisconnect(sylar::Stream::ptr stream) {
    return true;
}

bool Module::onServerReady() {
    return true;
}

bool Module::onServerUp() {
    return true;
}

void Module::registerService(const std::string& server_type,
            const std::string& domain, const std::string& service) {
    // 获取当前应用中的服务发现模块ServiceDiscovery实例
    auto sd = Application::GetInstance()->getServiceDiscovery();
    if (!sd) return;

    std::vector<TcpServer::ptr> svrs;
    // 获取指定类型（如 "http"）的所有 TcpServer 对象。
    if (!Application::GetInstance()->getServer(server_type, svrs)) {
        return;
    }
    // 遍历所有同类型的服务器（一个类型可能启动了多个端口或监听多个地址）。
    // 一个 TcpServer 通常会有多个 socket，是因为它可能监听多个地址（IP 或端口），而不是因为有多个客户端连接。
    for (auto& i : svrs) {
        auto socks = i->getSocks();
        for (auto& s : socks) {
            // 拿到它的监听地址(服务器端)。
            auto addr = std::dynamic_pointer_cast<IPv4Address>(s->getLocalAddress());
            if (!addr) {
                continue;
            }
            auto str = addr->toString();
            if (str.find("127.0.0.1") == 0) {
                // 如果地址是 127.0.0.1，即本地环回地址，不注册（其他服务访问不到它）。
                continue;
            }
            std::string ip_and_port;
            if (str.find("0.0.0.0") == 0) {
                // 如果监听地址是 0.0.0.0（即监听所有 IP），那就用本机外网 IP 替代 0.0.0.0。
                // 不能直接注册 0.0.0.0:8080，服务消费者无法使用这个地址。必须转换为真实 IP
                ip_and_port = sylar::GetIPv4() + ":" + std::string(addr->getPort());
            } else {
                ip_and_port = addr->toString();
            }
            sd->registerServer(domain, service, ip_and_port, server_type);
        }
    }
}

std::string Module::statusString() {
    std::stringstream ss;
    ss << "Module name=" << getName()
       << " version=" << getVersion()
       << " filename=" << getFilename()
       << std::endl;
    return ss.str();
}

bool RockModule::handleRequest(sylar::Message::ptr req
                               ,sylar::Message::ptr rsp
                               ,sylar::Stream::ptr stream) {
    auto rock_req = std::dynamic_pointer_cast<sylar::RockRequest>(req);                            
    auto rock_rsp = std::dynamic_pointer_cast<sylar::RockResponse>(rsp);                            
    auto rock_stream = std::dynamic_pointer_cast<sylar::RockStream>(stream);                            
    return handleRockRequest(rock_req, rock_rsp, rock_stream);                            
}

bool RockModule::handleNotify(sylar::Message::ptr notify
                              ,sylar::Stream::ptr stream) {
    auto rock_nty = std::dynamic_pointer_cast<sylar::RockNotify>(notify);
    auto rock_stream = std::dynamic_pointer_cast<sylar::RockStream>(stream);
    return handleRockNotify(rock_nty, rock_stream);
}

ModuleManager::ModuleManager() {
}

Module::ptr ModuleManager::get(const std::string& name) {
    RWMutexType::ReadLock lock(m_mutex);
    auto it = m_modules.find(name);
    return it == m_modules.end() ? nullptr : it->second;
}


void ModuleManager::add(Module::ptr m) {
    del(m->getId());
    RWMutexType::WriteLock lock(m_mutex);
    m_modules[m->getId()] = m;
    m_type2Modules[m->getType()][m->getId()] = m;
}

void ModuleManager::del(const std::string& name) {
    Module::ptr module;
    RWMutexType::WriteLock lock(m_mutex);
    auto it = m_modules.find(name);
    if(it == m_modules.end()) {
        return;
    }
    module = it->second;
    m_modules.erase(it);
    m_type2Modules[module->getType()].erase(module->getId());
    if(m_type2Modules[module->getType()].empty()) {
        m_type2Modules.erase(module->getType());
    }
    lock.unlock();
    module->onUnload();
}

void ModuleManager::delAll() {
    RWMutexType::ReadLock lock(m_mutex);
    auto tmp = m_modules;
    lock.unlock();

    for(auto& i : tmp) {
        del(i.first);
    }
}

void ModuleManager::init() {
    auto path = EnvMgr::GetInstance()->getAbsolutePath(g_module_path->getValue());
    
    std::vector<std::string> files;
    sylar::FSUtil::ListAllFile(files, path, ".so");

    std::sort(files.begin(), files.end());
    for(auto& i : files) {
        initModule(i);
    }
}

void ModuleManager::listByType(uint32_t type, std::vector<Module::ptr>& ms) {
    RWMutexType::ReadLock lock(m_mutex);
    auto it = m_type2Modules.find(type);
    if(it == m_type2Modules.end()) {
        return;
    }
    for(auto& i : it->second) {
        ms.push_back(i.second);
    }
}

void ModuleManager::foreach(uint32_t type, std::function<void(Module::ptr)> cb) {
    std::vector<Module::ptr> ms;
    listByType(type, ms);
    for(auto& i : ms) {
        cb(i);
    }
}

void ModuleManager::onConnect(Stream::ptr stream) {
    std::vector<Module::ptr> ms;
    listAll(ms);

    for(auto& m : ms) {
        m->onConnect(stream);
    }
}

void ModuleManager::onDisconnect(Stream::ptr stream) {
    std::vector<Module::ptr> ms;
    listAll(ms);

    for(auto& m : ms) {
        m->onDisconnect(stream);
    }
}

void ModuleManager::listAll(std::vector<Module::ptr>& ms) {
    RWMutexType::ReadLock lock(m_mutex);
    for(auto& i : m_modules) {
        ms.push_back(i.second);
    }
}

void ModuleManager::initModule(const std::string& path) {
    Module::ptr m = Library::GetModule(path);
    if(m) {
        add(m);
    }
}

}


