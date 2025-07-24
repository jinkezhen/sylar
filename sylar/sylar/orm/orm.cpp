#include "table.h"
#include "sylar/util.h"
#include "sylar/log.h"

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("orm");

// 自动生成一个 CMakeLists.txt 文件，用来构建由 ORM 工具生成的 .cc 源文件组成的静态库 orm_data
// path: 要生成 CMakeLists.txt 的目录路径（通常是生成 ORM 文件的输出路径）
// tbs: 所有 ORM 表结构（Table）对象组成的 map，key 是表名，value 是对应的 Table::ptr（指针）
void gen_cmake(const std::string& path, const std::map<std::string, sylar::orm::Table::ptr>& tbs) {
    std::ofstream ofs(path, "/CMakeLists.txt");
    ofs << "cmake_minimun_required(VERSION 3.0)" << std::endl;
    ofs << "project(orm_data)" << std::endl;
    ofs << std::endl;

    // 开始定义源文件列表变量 LIB_SRC
    ofs << "set(LIB_SRC" << std::endl;
    // sylar::replace("my.project.db", ".", "/")  →  "my/project/db"
    // sylar::ToLower("user")                     →  "user"
    // 最终路径为：my/project/db/user.cc
    for (auto& i : tbs) {
        ofs << "    " << sylar::replace(i.second->getNamespace(), ".", "/")
        << "/" << sylar::ToLower(i.second->getFilename()) << ".cc" << std::endl;    
    }
    ofs << ")" << std::endl;

    // 添加库并写宏指令
    ofs << "add_library(orm_data ${LIB_SRC})" << std::endl;
    ofs << "force_redefine_file_macro_for_sources(orm_data)" << std::endl;
}
// cmake_minimum_required(VERSION 3.0)
// project(orm_data)
// set(LIB_SRC
//     my/app/user.cc
//     my/app/comment.cc
// )
// add_library(orm_data ${LIB_SRC})
// force_redefine_file_macro_for_sources(orm_data)


// 这是一个基于 XML 配置文件生成 ORM（对象关系映射）代码的工具的入口函数。它主要负责：
// 从指定的配置目录读取所有 XML 文件（每个 XML 定义了一张数据库表的结构）。
// 解析这些 XML 文件，生成对应的 ORM 表对象。
// 基于这些 ORM 表对象，生成对应的代码文件到指定输出目录。
// 生成一个 CMake 构建文件，方便后续将生成的代码编译成可用的库或程序。
// 简单来说，这个程序是一个 ORM 代码生成器，它从表结构的 XML 配置入手，自动生成对应的数据库访问代码（可能是 C++ 代码），并帮助用户生成相应的构建脚本。
int main(int argc, char** argv) {
    if (argc < 2) {
        // 如果少于2个，提示用户程序的正确使用方法（执行程序时需要提供orm_config_path和orm_output_path两个参数）。
        std::cout << "use as[" << argv[0] << " orm_config_path orm_output_path]" << std::endl;        
    }
    std::string out_path = "./orm_out";
    std::string input_path = "bin/orm_conf";

    if (argc > 1) {
        input_path = argv[1];
    }
    if (argc > 2) {
        out_path = argv[2];
    }

    std::vector<std::string> files;
    sylar::FSUtil::ListAllFile(files, input_path, ".xml");
    std::vector<sylar::orm::Table::ptr> tbs;

    bool has_error = false;
    for (auto& i : files) {
        SYLAR_LOG_INFO(g_logger) << "init xml=" << i << " begin";
        tinyxml2::XMLDocument doc;
        if (doc.LoadFile(i.c_str())) {
            SYLAR_LOG_ERROR(g_logger) << "error: " << doc.ErrorStr();
            has_error = true;            
        } else {
            sylar::orm::Table::ptr table(new sylar::orm::Table);
            if (!table->init(*doc.RootElement())) {
                SYLAR_LOG_ERROR(g_logger) << "table init error";
                has_error = true;        
            } else {
                tbs.push_back(table);
            }
        }
        SYLAR_LOG_INFO(g_logger) << "init xml=" << i << " end";
    }
    if (has_error) {
        return 0;
    }
    std::map<std::string, sylar::orm::Table::ptr> orm_tbs;
    for (auto& i : tbs) {
        i->gen(out_path);
        orm_tbs[i->getName()] = i;
    }
    gen_cmake(out_path, orm_tbs);
    return 0;
}
