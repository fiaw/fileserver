#define CPPHTTPLIB_OPENSSL_SUPPORT // 必须位于 include 之前
#include <httplib.h>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <json/json.h>
#include <chrono>
// 添加在现有头文件之后
#include <favourite_ico.h>
#include <sqlite3.h>
#include <zip.h> // 需要安装libzip-dev：sudo apt-get install libzip-dev

//# sudo apt-get install libavahi-client-dev libavahi-common-dev
//g++ -o x -I. c.cpp  -ljsoncpp
//g++ -std=c++20 -o x -I. c.cpp -ljsoncpp
/*
   mkdir build
   cd build
   cmake -DCMAKE_BUILD_TYPE=Release -DJSONCPP_BUILD_STATIC=ON ..
   sudo apt install cmake
   cmake -DCMAKE_BUILD_TYPE=Release -DJSONCPP_BUILD_STATIC=ON ..
   sudo make install
   export LD_LIBRARY_PATH=/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
   g++ -std=c++20 -o x -I. c.cpp -L/lib/x86_64-linux-gnu -ljsoncpp -lssl -lcrypto
   g++ -std=c++20 -o x -I. c.cpp -L/lib/x86_64-linux-gnu -ljsoncpp -static 
   g++ -std=c++20 -o x -I. c.cpp -L/lib/x86_64-linux-gnu -ljsoncpp -lssl -lcrypto -lz -lzstd -ldl -lpthread -lavahi-client -lavahi-common -static
   g++ -std=c++20 -o x -I. fileserver.cpp -L/lib/x86_64-linux-gnu -ljsoncpp -lssl -lcrypto -lz -lzstd -ldl   -lsqlite3

  g++ -std=c++20 -o x fileserver.cpp -I. -L/usr/lib/x86_64-linux-gnu -ljsoncpp -lssl -lcrypto -lsqlite3 -lzip -lzstd -lz -ldl -static
  
   # 使用手动编译的静态库
	g++ -std=c++20 -o x -I. fileserver.cpp \
	-I/usr/local/include \
	-L/usr/local/lib \
	-lzip -lssl -lcrypto -lsqlite3 \
	-static
	

  file ./x
*/

  
const unsigned int favourite_ico_len = 5559;
sqlite3* db = nullptr;
namespace fs = std::filesystem;

struct FileInfo {
    std::string name;
    uint64_t size;
    std::string created;
};

// 统一错误处理函数
void send_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    Json::Value error;
    error["success"] = false;
    error["message"] = message;
    Json::StreamWriterBuilder writer;
    res.set_content(Json::writeString(writer, error), "application/json");
}
// 统一正常处理函数
void send_success(httplib::Response& res, int status, const std::string& message) {
    res.status = 200;
    Json::Value error;
    error["success"] = true;
    error["message"] = message;
    Json::StreamWriterBuilder writer;
    res.set_content(Json::writeString(writer, error), "application/json");
}


void init_database() {
    int rc = sqlite3_open("text_boxes.db", &db);
    if (rc != SQLITE_OK) {
        std::cerr << "无法打开数据库: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    const char* sqls[] = {
        // Projects表
        "CREATE TABLE IF NOT EXISTS projects ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT UNIQUE NOT NULL,"
        "status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active', 'archived')),"
        "created_time DATETIME DEFAULT CURRENT_TIMESTAMP,"
		"description TEXT NOT NULL default '',"
        "end_time DATETIME);",

        // TextBoxes表
        "CREATE TABLE IF NOT EXISTS text_boxes ("
        "project_id INTEGER NOT NULL,"
        "content TEXT,"
        "name TEXT,"
        "created_time DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "PRIMARY KEY(project_id),"
        "FOREIGN KEY(project_id) REFERENCES projects(id));",

        // Files表
        "CREATE TABLE IF NOT EXISTS files ("
        "project_id INTEGER NOT NULL,"
        "filename TEXT NOT NULL,"
        "created_time DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "PRIMARY KEY(project_id, filename),"
        "FOREIGN KEY(project_id) REFERENCES projects(id));",
		
		"CREATE TABLE IF NOT EXISTS system_settings ("
		"key TEXT PRIMARY KEY,"
		"value TEXT);",
		
        "INSERT OR IGNORE INTO system_settings (key, value) VALUES ('current_project_id', '0');"
    };
    
	
	for (const char* sql : sqls) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "数据库初始化失败: " << std::endl;
        }
    }
	
	
}
void handle_export_project(const httplib::Request& req, httplib::Response& res) {
    int project_id = std::stoi(req.matches[1]);
    
    // 获取项目信息
    sqlite3_stmt* stmt;
    const char* sql = 
        "SELECT p.name, t.content "
        "FROM projects p "
        "LEFT JOIN text_boxes t ON p.id = t.project_id "
        "WHERE p.id = ?";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        send_error(res, 500, "Database error");
        return;
    }

    sqlite3_bind_int(stmt, 1, project_id);
    
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        send_error(res, 404, "Project not found");
        sqlite3_finalize(stmt);
        return;
    }

    const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const char* content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    
    // 创建临时目录
    std::string tmp_dir = "/tmp/project_export_" + std::to_string(time(nullptr));
    fs::create_directories(tmp_dir);

    // 写入日志文件
    std::ofstream log_file(tmp_dir + "/" + std::string(name) + ".log");
    if (log_file.is_open()) {
        log_file << (content ? content : "");
        log_file.close();
    }

    // 复制项目文件
    std::string project_dir = "./files/project_" + std::to_string(project_id);
    try {
        for (const auto& entry : fs::directory_iterator(project_dir)) {
            if (fs::is_regular_file(entry)) {
                fs::copy(entry.path(), tmp_dir);
            }
        }
    } catch (...) {
        send_error(res, 500, "Failed to copy project files");
        return;
    }

    // 创建ZIP压缩包
    std::string zip_path = tmp_dir + ".zip";
    zip_t* zip = zip_open(zip_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    if (!zip) {
        send_error(res, 500, "Failed to create zip file");
        return;
    }

    try {
        for (const auto& entry : fs::directory_iterator(tmp_dir)) {
            std::string file_path = entry.path().string();
            std::string arcname = entry.path().filename().string();

            zip_source_t* source = zip_source_file(zip, file_path.c_str(), 0, 0);
            if (source) {
                zip_file_add(zip, arcname.c_str(), source, ZIP_FL_ENC_UTF_8);
            }
        }
    } catch (...) {
        zip_close(zip);
        send_error(res, 500, "Failed to add files to zip");
        return;
    }

    zip_close(zip);

    // 读取ZIP文件内容
    std::ifstream ifs(zip_path, std::ios::binary);
    if (!ifs) {
        send_error(res, 500, "Failed to read zip file");
        return;
    }

    // 设置响应头
    res.set_header("Content-Type", "application/zip");
    res.set_header("Content-Disposition", 
        "attachment; filename=\"" + std::string(name) + "_export.zip\"");

    // 发送文件内容
    res.set_content(std::string((std::istreambuf_iterator<char>(ifs)), 
                  std::istreambuf_iterator<char>()), 
                  "application/zip");

    // 清理临时文件
    fs::remove_all(tmp_dir);
    fs::remove(zip_path);
    sqlite3_finalize(stmt);
}

bool compareByDate(const FileInfo& a, const FileInfo& b) {
    // 比较文件的创建时间，确保按时间顺序排序（从最早到最新）
    return a.created > b.created;
}


void handle_get_projects(const httplib::Request&, httplib::Response& res) {
    Json::Value projects(Json::arrayValue);
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT id, name, created_time, status, end_time,description FROM projects ORDER BY status,created_time DESC";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        // 添加错误处理
        res.status = 500;
        res.set_content("Database query preparation failed", "text/plain");
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Json::Value p;
        
        // 处理每个字段（带空值检查）
        p["id"] = sqlite3_column_int(stmt, 0);
        
        // name字段
        p["name"] = (sqlite3_column_type(stmt, 1) == SQLITE_TEXT) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) : "";
        
        // created_time字段
        p["created"] = (sqlite3_column_type(stmt, 2) == SQLITE_TEXT) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) : "";

        // status字段
        p["status"] = (sqlite3_column_type(stmt, 3) == SQLITE_TEXT) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) : "";

        // end_time字段
        p["ended"] = (sqlite3_column_type(stmt, 4) == SQLITE_TEXT) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) : "";
        p["description"] = (sqlite3_column_type(stmt, 5) == SQLITE_TEXT) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)) : "";

        projects.append(p);
    }
    
    sqlite3_finalize(stmt);

    Json::StreamWriterBuilder writer;
    res.set_content(Json::writeString(writer, projects), "application/json");
}

// 辅助函数：删除数据库记录
void rollback_project(int project_id) {
    const char* delete_sql = "DELETE FROM projects WHERE id = ?;"
                            "DELETE FROM text_boxes WHERE project_id = ?;";
    sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, delete_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, project_id);
        sqlite3_bind_int(stmt, 2, project_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

// 辅助函数：清空目录内容
bool clear_directory(const std::string& path) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        if (ec) return false;
        fs::remove_all(entry.path(), ec);
        if (ec) return false;
    }
    return true;
}
// 创建项目
void handle_create_project(const httplib::Request& req, httplib::Response& res) {
    Json::Value json;
    if (!Json::Reader().parse(req.body, json)) {
        send_error(res, 400, "Invalid JSON format");
        return;
    }

    std::string name = json["name"].asString();
    if (name.empty()) {
        send_error(res, 400, "Project name cannot be empty");
        return;
    }

	 // 检查项目名称是否已存在（排除当前项目）
    sqlite3_stmt* check_stmt;
    const char* check_sql = "SELECT id FROM projects WHERE name = ?";
    
    if (sqlite3_prepare_v2(db, check_sql, -1, &check_stmt, nullptr) != SQLITE_OK) {
        send_error(res, 500, "Database error");
        return;
    }

    sqlite3_bind_text(check_stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(check_stmt) == SQLITE_ROW) {
        sqlite3_finalize(check_stmt);
        send_error(res, 400, "Project name already exists");
        return;
    }
    sqlite3_finalize(check_stmt);
	
    // 开始事务
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        send_error(res, 500, "Failed to start transaction: " + std::string(err_msg ? err_msg : ""));
        sqlite3_free(err_msg);
        return;
    }

    // 插入projects表
    sqlite3_stmt* project_stmt;
    const char* project_sql = "INSERT INTO projects (name) VALUES (?)";
    if ((rc = sqlite3_prepare_v2(db, project_sql, -1, &project_stmt, nullptr)) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        send_error(res, 500, "Prepare projects failed: " + std::string(sqlite3_errmsg(db)));
        return;
    }

    sqlite3_bind_text(project_stmt, 1, name.c_str(), -1, SQLITE_STATIC);
    if ((rc = sqlite3_step(project_stmt)) != SQLITE_DONE) {
        sqlite3_finalize(project_stmt);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        send_error(res, 400, "Project already exists." );
        return;
    }
    
    const int project_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(project_stmt);

    // 初始化text_boxes表
    sqlite3_stmt* text_stmt;
    const char* text_sql = "INSERT INTO text_boxes (project_id, content,name) VALUES (?, '',?)";
    if ((rc = sqlite3_prepare_v2(db, text_sql, -1, &text_stmt, nullptr)) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        send_error(res, 500, "Prepare text_boxes failed: " + std::string(sqlite3_errmsg(db)));
        return;
    }

    sqlite3_bind_int(text_stmt, 1, project_id);
    sqlite3_bind_text(text_stmt, 2, name.c_str(), -1, SQLITE_STATIC); // 明确绑定空字符串

	std::cerr << project_id << std::endl;
	
    if ((rc = sqlite3_step(text_stmt)) != SQLITE_DONE) {
        sqlite3_finalize(text_stmt);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        send_error(res, 500, "Initialize text_boxes failed: " + std::string(sqlite3_errmsg(db)));
        return;
    }
    sqlite3_finalize(text_stmt);

    // 提交事务
    if ((rc = sqlite3_exec(db, "COMMIT", nullptr, nullptr, &err_msg)) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        send_error(res, 500, "Commit failed: " + std::string(err_msg ? err_msg : ""));
        sqlite3_free(err_msg);
        return;
    }

    // 创建项目目录（必须在事务外处理）
    std::error_code ec;
    const std::string project_dir = "./files/project_" + std::to_string(project_id);
	
	if (fs::exists(project_dir, ec)) {
		send_success(res, 200, "Create Success, directory already exists.");
		return;
	}else{
		if (!fs::create_directories(project_dir, ec) || ec) {
			// 回滚数据库操作（需要手动删除）
			rollback_project(project_id);
			send_error(res, 500, "Create directory failed." );
			return;
		}
	}
	
    send_success(res, 200, "创建项目成功.");
}

// 设置当前项目
void handle_set_current_project(const httplib::Request& req, httplib::Response& res) {
    Json::Value json;
    if (!Json::Reader().parse(req.body, json)) {
        send_error(res, 400, "Invalid JSON");
        return;
    }

    int project_id = json["id"].asInt();
    
    // 验证项目有效性
    sqlite3_stmt* check;
    const char* check_sql = 
        "SELECT id FROM projects "
        "WHERE id = ? AND status = 'active'";
    
    if (sqlite3_prepare_v2(db, check_sql, -1, &check, nullptr) != SQLITE_OK) {
        send_error(res, 500, "Database error");
        return;
    }

    sqlite3_bind_int(check, 1, project_id);
    if (sqlite3_step(check) != SQLITE_ROW) {
        send_error(res, 404, "Project not active");
        sqlite3_finalize(check);
        return;
    }
    sqlite3_finalize(check);

    // 更新当前项目
    sqlite3_stmt* update;
    const char* update_sql = 
        "UPDATE system_settings "
        "SET value = ? "
        "WHERE key = 'current_project_id'";
    
    if (sqlite3_prepare_v2(db, update_sql, -1, &update, nullptr) != SQLITE_OK) {
        send_error(res, 500, "Database error");
        return;
    }

    sqlite3_bind_text(update, 1, std::to_string(project_id).c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(update) != SQLITE_DONE) {
        send_error(res, 500, "Update failed");
        sqlite3_finalize(update);
        return;
    }

    sqlite3_finalize(update);
    send_success(res, 200, "Current project updated");
}
void handle_update_project_status(const httplib::Request& req, httplib::Response& res) {
    int project_id = std::stoi(req.matches[1]);
    Json::Value json;
    if (!Json::Reader().parse(req.body, json)) {
        send_error(res, 400, "Invalid JSON");
        return;
    }

    const std::string new_status = json["status"].asString();
    if (new_status != "active" && new_status != "archived") {
        send_error(res, 400, "Invalid status value");
        return;
    }

    sqlite3_stmt* stmt;
    const char* sql = 
        "UPDATE projects SET "
        "status = ?, "
        "end_time = CASE WHEN ? = 'archived' THEN CURRENT_TIMESTAMP ELSE NULL END "
        "WHERE id = ?";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        send_error(res, 500, "Database error");
        return;
    }

    sqlite3_bind_text(stmt, 1, new_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, new_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, project_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        send_error(res, 500, "Update failed");
        sqlite3_finalize(stmt);
        return;
    }

    sqlite3_finalize(stmt);
    
    // 如果归档的是当前项目，重置当前项目
    if (new_status == "archived") {
        sqlite3_exec(db, 
            "UPDATE system_settings SET value = '0' "
            "WHERE key = 'current_project_id' AND value = ?",
            [](void*,int,char**,char**){ return 0; }, 
            &project_id, nullptr);
    }

    send_success(res, 200, "Project status updated");
}

//备用
void handle_get_current_project(const httplib::Request&, httplib::Response& res) {
    sqlite3_stmt* stmt;
    const char* sql = 
        "SELECT p.id, p.name, p.status, p.end_time "
        "FROM system_settings s "
        "LEFT JOIN projects p ON p.id = CAST(s.value AS INTEGER) "
        "WHERE s.key = 'current_project_id'";

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL准备失败: " << sqlite3_errmsg(db) << "\n";
        send_error(res, 500, "Database preparation error");
        return;
    }

    Json::Value response;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        // 处理每列数据
        response["id"] = sqlite3_column_type(stmt, 0) == SQLITE_INTEGER ?
                        sqlite3_column_int(stmt, 0) : Json::nullValue;

        response["name"] = sqlite3_column_type(stmt, 1) == SQLITE_TEXT ?
                        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) : "";

        // 处理状态字段（假设status是字符串类型）
        if (sqlite3_column_type(stmt, 2) == SQLITE_TEXT) {
            const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            response["status"] = status ? status : "";
        } else {
            response["status"] = "unknown";
        }

        // 处理时间字段
        if (sqlite3_column_type(stmt, 3) == SQLITE_TEXT) {
            const char* end_time = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            response["end_time"] = end_time ? end_time : "";
        } else {
            response["end_time"] = Json::nullValue;
        }
    } else if (rc == SQLITE_DONE) {
        send_error(res, 404, "No current project configured");
        sqlite3_finalize(stmt);
        return;
    } else {
        std::cerr << "SQL执行失败: " << sqlite3_errmsg(db) << "\n";
        send_error(res, 500, "Database query error");
        sqlite3_finalize(stmt);
        return;
    }

    sqlite3_finalize(stmt);

    // 构造响应
    if (response.isMember("id") && !response["id"].isNull()) {
        Json::StreamWriterBuilder writer;
        res.set_content(Json::writeString(writer, response), "application/json");
    } else {
        send_error(res, 404, "Current project not found");
    }
}

int get_current_project_id() {
    int project_id = 0;
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT value FROM system_settings WHERE key = 'current_project_id'";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            project_id = std::stoi(value);
        }
        sqlite3_finalize(stmt);
    }
    
    return project_id;
}


void handle_upload(const httplib::Request& req, httplib::Response& res) {
    // 设置 CORS 头
    res.set_header("Access-Control-Allow-Origin", "*");  // 允许所有域访问
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS"); // 允许的方法
    res.set_header("Access-Control-Allow-Headers", "Content-Type");  // 允许的头部

	int project_id = 0;

	if (req.has_file("project_id")) {
            auto project_field = req.get_file_value("project_id");
            project_id = std::stoi(project_field.content);
	}
		
	if (std::to_string(project_id).empty()) {
		send_error(res, 400, "未选择项目");
		return;
	}
    std::string base_dir = "./files/project_" + std::to_string(project_id);
	
    fs::create_directories(base_dir);
    
	// 获取上传的多个文件
    auto files = req.get_file_values("file");

    // 如果没有文件上传，返回错误
    if (files.empty()) {
		send_error(res, 400, "未选择任何文件");
		return;
    }

    // 遍历每个文件并保存
	for (auto& file : files) {
		std::replace(file.filename.begin(), file.filename.end(), '\\', '/');

		// 检查路径是否包含非法的路径遍历部分（如 '../'）
		if (file.filename.find("..") != std::string::npos) {
			send_error(res, 400, "Access denied");
			return;
		}

		if (file.filename.find("/") != std::string::npos) {
			send_error(res, 400, "Access denied");
			return;
		}

        std::string target_path = base_dir +"/"+ file.filename;
        std::ofstream ofs(target_path, std::ios::binary);
        if (!ofs) {
			send_error(res, 500, "Failed to open file for writing");
            return;
        }

        ofs.write(file.content.c_str(), file.content.size());
        if (!ofs) {
			send_error(res, 500, "Failed to write file");
            return;
        }
    }

    // 成功响应
    res.set_content(R"({"success": true})", "application/json");
}
// 将字节大小转换为适合的单位（GB、MB、KB、B）
std::string format_file_size(uint64_t size) {
    const double kb = 1024.0;
    const double mb = kb * 1024.0;
    const double gb = mb * 1024.0;

    std::ostringstream oss;
    if (size >= gb) {
        oss << std::fixed << std::setprecision(2) << (size / gb) << "GB";
    }
    else if (size >= mb) {
        oss << std::fixed << std::setprecision(2) << (size / mb) << "MB";
    }
    else if (size >= kb) {
        oss << std::fixed << std::setprecision(2) << (size / kb) << "KB";
    }
    else {
        oss << size << "B";
    }
    return oss.str();
}

// 处理Windows与Linux文件时间差异的转换
std::string format_creation_time(const fs::file_time_type& ftime) {
    using namespace std::chrono;

    // 统一转换为系统时钟的时间点

    // Linux直接使用clock_cast转换
    auto sys_time = clock_cast<system_clock>(ftime);

    // 转换为本地时间
    std::time_t c_time = system_clock::to_time_t(sys_time);
    std::tm tm_buf;

    localtime_r(&c_time, &tm_buf); // Linux本地时间

    // 格式化输出（包含毫秒）
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
 
    return oss.str();
}


// 处理获取文件列表的请求
void handle_file_list(const httplib::Request& req, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
	
	int project_id = 0;
    if (req.has_param("project_id")) {
        project_id = std::stoi(req.get_param_value("project_id"));
    }
	if (std::to_string(project_id).empty()) {
		send_error(res, 400, "未选择项目");
		return;
	}

    std::string base_dir = "./files/project_" + std::to_string(project_id);
    std::vector<FileInfo> file_info_list;
	
    // 获取文件列表
    for (const auto& entry : fs::directory_iterator(base_dir)) {
        if (fs::is_regular_file(entry.status())) {
            FileInfo file;
            file.name = entry.path().filename().string();
			
			// 跳过文件名完全为 .text 的文件
            if (file.name == ".text") {
                continue;  // 跳过 .text 文件
            }
			
            file.size = fs::file_size(entry.path());
            
            // 获取文件的最后修改时间
            auto ftime = fs::last_write_time(entry.path());

            // 格式化文件创建时间
            file.created = format_creation_time(ftime);

            file_info_list.push_back(file);
        }
    }

    // 按照文件的创建时间排序
    std::sort(file_info_list.begin(), file_info_list.end(), compareByDate);

    // 返回文件列表
	Json::Value files(Json::arrayValue);
	// 使用静态集合存储可预览的扩展名 (统一小写)
	static const std::unordered_set<std::string> previewable_exts = {
		"png", "jpg", "jpeg", "gif", 
		"txt", "log", "md"  // 示例新增csv格式
	};
	
	
	static const std::unordered_set<std::string> copyable_exts = {
		"png", "jpg", "jpeg", "gif", 
	};	
	
	
    for (const auto& file : file_info_list) {
        Json::Value json_file;
        json_file["name"] = file.name;
        json_file["size"] = format_file_size(file.size);
        json_file["created"] = file.created;
		// 提取并转换小写扩展名
		std::string ext;
		if (const size_t dot_pos = file.name.find_last_of('.');
			dot_pos != std::string::npos) 
		{
			ext = file.name.substr(dot_pos + 1);
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		}

		// 判断是否可预览
		json_file["previewable"] = previewable_exts.contains(ext);
		json_file["copyable"] = copyable_exts.contains(ext);
        files.append(json_file);
    }


    Json::StreamWriterBuilder writer;
    res.set_content(Json::writeString(writer, files), "application/json");
}
void handle_view(const httplib::Request& req, httplib::Response& res) {
    
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");	
	std::string filename = req.get_param_value("file");
    if (filename.empty()) {
		send_error(res, 400, "No file specified");
        return;
    }

    try {
		int project_id = 0;
		if (req.has_param("project_id")) {
			project_id = std::stoi(req.get_param_value("project_id"));
		}
		if (std::to_string(project_id).empty()) {
			send_error(res, 400, "未选择项目");
			return;
		}

		
        // 复用下载功能的安全检查
        std::replace(filename.begin(), filename.end(), '\\', '/');
        if (filename.find("/") != std::string::npos || filename.find("..") != std::string::npos) {
			send_error(res, 400, "Access denied");
            return;
        }
		
		
		
		std::string filepath = "./files/project_" + std::to_string(project_id) + "/" +filename;
        fs::path abs_path = fs::canonical(filepath);

        // 检查文件是否存在
        if (!fs::exists(abs_path)) {
			send_error(res, 400, "File not found");
            return;
        }

        // 读取文件内容
        std::ifstream ifs(abs_path, std::ios::binary);
        std::ostringstream oss;
        oss << ifs.rdbuf();

        // 根据文件类型设置MIME
        std::string content_type = "text/plain";
		std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
        if (filename.ends_with(".png")) content_type = "image/png";
        else if (filename.ends_with(".jpg") || filename.ends_with(".jpeg")) content_type = "image/jpeg";
        else if (filename.ends_with(".gif")) content_type = "image/gif";

        res.set_content(oss.str(), content_type);
    } catch (...) {
		send_error(res, 500, "Error reading file");
		
    }
}
void handle_download(const httplib::Request& req, httplib::Response& res) {
    // 获取文件名参数
    std::string filename = req.get_param_value("file");
    if (filename.empty()) {
		send_error(res, 400, "No file specified");
        return;
    }
	
	
	
	
	int project_id = 0;
    if (req.has_param("project_id")) {
        project_id = std::stoi(req.get_param_value("project_id"));
    }
	if (std::to_string(project_id).empty()) {
		send_error(res, 400, "未选择项目");
		return;
	}



    try {
		
		
		std::replace(filename.begin(), filename.end(), '\\', '/');
		if (filename.find("/") != std::string::npos) {
			send_error(res, 400, "Access denied");
			return;
		}
		
		// 检查路径是否包含非法的路径遍历部分（如 '../'）
		if (filename.find("..") != std::string::npos) {
			send_error(res, 400, "Access denied");
			return;
		}
		

		// 拼接文件路径
		std::string filepath = "./files/project_" + std::to_string(project_id) + "/" +filename;

		fs::path base_dir = fs::canonical("./files");
		fs::path abs_path = fs::canonical(filepath);
		
		if (fs::canonical(abs_path).string().find(base_dir.string()) != 0) {
			send_error(res, 400, "Access denied");
			return;
		}


        // 检查文件是否存在
        if (!fs::exists(abs_path)) {
			send_error(res, 400, "File not found");
            return;
        }
		
		

        // 打开文件并读取内容
        std::ifstream ifs(abs_path, std::ios::binary);
        std::ostringstream oss;
        oss << ifs.rdbuf();

        // 设置下载文件的响应头
        res.set_header("Content-Disposition", "attachment; filename=" + filename);
        res.set_content(oss.str(), "application/octet-stream");
    } catch (const std::exception& e) {
        // 捕获文件路径处理中的异常，例如无法访问路径等
		std::string err_msg = e.what();
		send_error(res, 400, err_msg);
	}
}

void handle_save_text(const httplib::Request& req, httplib::Response& res) {
    int project_id = 0;
	std::string project_id_str = req.get_param_value("project_id");
	if (project_id_str.empty()) {
		send_error(res, 400, "未选择项目");
		return;
	}

	project_id = std::stoi(project_id_str);
	
	Json::Value json_data;
    Json::Reader reader;
    if (!reader.parse(req.body, json_data)) {
        send_error(res, 400, "无效的JSON");
        return;
    }

    std::string content = json_data["content"].asString();

    const char* sql = "UPDATE text_boxes SET content = ? WHERE project_id = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        send_error(res, 500, "数据库错误");
        return;
    }

    sqlite3_bind_text(stmt, 1, content.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, project_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        send_error(res, 500, "保存失败");
        sqlite3_finalize(stmt);
        return;
    }

    sqlite3_finalize(stmt);
    send_success(res, 200, "保存成功");
}

void handle_load_text(const httplib::Request& req, httplib::Response& res) {
	int project_id = 0;
	std::string project_id_str = req.get_param_value("project_id");
	if (project_id_str.empty()) {
		send_error(res, 400, "未选择项目");
		return;
	}

	project_id = std::stoi(project_id_str);
	const char* sql = "SELECT content FROM text_boxes WHERE project_id = ? ORDER BY created_time DESC LIMIT 1;";
	sqlite3_stmt* stmt;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
	if (rc != SQLITE_OK) {
		send_error(res, 500, "数据库错误");
		return;
	}

	sqlite3_bind_int(stmt, 1, project_id);
	rc = sqlite3_step(stmt);
	Json::Value response;

	if (rc == SQLITE_ROW) {
		const char* content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
		response["success"] = true;
		response["text"] = content;

	} else {
		response["success"] = true;
		response["text"] = "";
	}
	sqlite3_finalize(stmt);
	Json::StreamWriterBuilder writer;
	res.set_content(Json::writeString(writer, response), "application/json");

}


void handle_update_project(const httplib::Request& req, httplib::Response& res) {
    int project_id = std::stoi(req.matches[1]);
	if (project_id==0) {
		send_error(res, 400, "未选择项目");
		return;
	}
	
    Json::Value json;
    if (!Json::Reader().parse(req.body, json)) {
        send_error(res, 400, "Invalid JSON");
        return;
    }

    const std::string new_name = json["name"].asString();
    const std::string new_status = json["status"].asString();
    const std::string end_time = json["end_time"].asString();
    const std::string new_description = json["description"].asString();
	/*
    if (new_status != "active" && new_status != "archived") {
        send_error(res, 400, "Invalid status value");
        return;
    }
	*/
	
	 // 检查项目名称是否已存在（排除当前项目）
    sqlite3_stmt* check_stmt;
    const char* check_sql = "SELECT id FROM projects WHERE name = ? AND id != ?";
    
    if (sqlite3_prepare_v2(db, check_sql, -1, &check_stmt, nullptr) != SQLITE_OK) {
        send_error(res, 500, "Database error");
        return;
    }

    sqlite3_bind_text(check_stmt, 1, new_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(check_stmt, 2, project_id);

    if (sqlite3_step(check_stmt) == SQLITE_ROW) {
        sqlite3_finalize(check_stmt);
        send_error(res, 400, "Project name already exists");
        return;
    }
    sqlite3_finalize(check_stmt);
	

    sqlite3_stmt* stmt;
    const char* sql = 
        "UPDATE projects SET "
        "name = ?, "
        "status = ?, "
        "end_time = CASE WHEN ? = '' THEN NULL ELSE ? END , "
        "description = ? "
        "WHERE id = ?";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        send_error(res, 500, "Database error");
        return;
    }

    sqlite3_bind_text(stmt, 1, new_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, new_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, end_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, end_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, new_description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 	6, project_id);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        send_error(res, 500, "Update failed");
        sqlite3_finalize(stmt);
        return;
    }

    sqlite3_finalize(stmt);
    
    // 如果归档的是当前项目，重置当前项目
    /*
	if (new_status == "archived") {
        sqlite3_exec(db, 
            "UPDATE system_settings SET value = '0' "
            "WHERE key = 'current_project_id' AND value = ?",
            [](void*,int,char**,char**){ return 0; }, 
            &project_id, nullptr);
    }
	*/

    send_success(res, 200, "Project updated");
}


const std::string html_page = R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
	<link rel="icon" href="/favicon.ico" type="image/x-icon">
    <link rel="shortcut icon" href="/favicon.ico" type="image/x-icon">
    <title>传输助手</title>
	<meta name="author" content="Will Wang" />
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: 'Arial', sans-serif;
            background-color: #f4f7fc;
            display: flex;
            flex-direction: column;
            align-items: center;
            height: 100vh;
        }

        h1 {
            text-align: center;
            color: #333;
            margin-top: 20px;
            font-size: 2rem;
            font-weight: 600;
        }
		

        .tabs {
			display: flex;
			justify-content: left;
			width: 85%;
			max-width: 1350px;
			margin-top: 10px;
			background-color: #fff;
			border-bottom: 1px solid #ddd;
			box-shadow: 0 2px 10px rgba(0, 0, 0, 0.1);
			overflow: hidden; /* 新增：隐藏溢出内容 */
            border-radius: 8px 8px 0px 0px;
		}

		/* 修改后的单个选项卡 */
		.tab {
			padding: 10px 30px;
			cursor: pointer;
			background-color: #ffffff;
			color: #333;
			font-weight: 500;
			border: 1px solid #ddd;  /* 合并三个边框 */
			border-bottom: none;     /* 移除底部边框 */
			margin-right: -1px; /* 新增：消除边框重叠 */
			border-radius: 0; /* 移除圆角 */
			transition: all 0.3s ease;
			position: relative;
		}

		/* 首尾选项卡特殊处理 */
		.tab:first-child {
			border-radius: 8px 0 0 0; /* 仅左侧圆角 */
		}

		.tab:last-child {
			border-radius: 0 8px 0 0; /* 仅右侧圆角 */
		}


        .tab.active {
            background-color: #0066cc;
            color: #FFFFFF;
        }

        .tab-content {
            display: none;
            width: 85%;
            max-width: 1350px;
            padding: 20px;
            background-color: white;
            border-radius: 0 0 8px 8px;
            margin-top: 0px;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);

			flex: 1; /* 填充剩余空间 */
			flex-direction: column; /* 垂直布局 */
			
        }

        .tab-content.active {
            display: block;
        }


		.menu-icon {
			cursor: pointer;
			padding: 10px 15px;
			margin-left: auto; /* 推到最右侧 */
			display: flex;
			flex-direction: column;
			justify-content: center;
			gap: 4px;
			transition: all 0.3s ease;
		}

		.menu-icon:hover {
			background: #f0f0f0;
		}

		.menu-icon span {
			display: block;
			width: 20px;
			height: 3px;
			border-radius:2px;
			background: #333;
			transition: all 0.3s ease;
		}

		.menu-icon:hover span {
			background: #0066cc;
		}



        #upload-area {
            width: 100%;
            height: 150px;
            border: 2px dashed #ccc;
            border-radius: 10px;
            text-align: center;
            line-height: 150px;
            margin-bottom: 0px;
            background-color: #fafafa;
            cursor: pointer;
            transition: all 0.3s ease;
			flex-shrink: 0; /* 防止被压缩 */
        }

        #upload-area:hover {
            border-color: #0066cc;
            background-color: #f0f8ff;
        }

        #file-list {
            list-style-type: none;
			padding: 0;
			margin: 0;
			max-height: calc(100vh - 260px); /* 根据视窗高度动态计算 */
			overflow-y: auto; /* 添加垂直滚动 */
			scrollbar-width: thin; /* 现代浏览器细滚动条 */
        }
		#file-list::-webkit-scrollbar {
			width: 6px;
		}
		#file-list::-webkit-scrollbar-track {
			background: #f1f1f1;
		}
		#file-list::-webkit-scrollbar-thumb {
			background: #888;
			border-radius: 3px;
		}
		
		#file-list:after {clear: both;}
        #file-list li {
            background-color: #fff;
            margin: 5px 0;
            padding: 8px;
            border-radius: 5px;
            border: 1px solid #ddd;
            display: flex;
            justify-content: space-between;
            align-items: center;
            transition: all 0.3s ease;
			
        }
		

		#file-list li button:disabled {
			background-color: #cccccc;  /* 灰色背景 */
			cursor: not-allowed;       /* 禁用光标 */
			opacity: 0.6;              /* 半透明效果 */
		}
				
		
		
        #file-list li:hover {
            background-color: #f0f8ff;
            box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);
        }
        #file-list li button {
            background-color: #0066cc;
            color: white;
            border: none;
            padding: 5px 15px;
			font-size:14px;
            border-radius: 5px;
            cursor: pointer;
            transition: background-color 0.3s ease;
			margin-right:5px;
			margin-top:2px;
			margin-bottom:2px;
        }
        #file-list li button:hover {
            background-color: #005bb5;
        }
		
		#file-list li span:last-child {
			text-align: right;
		}
		
		#file-list li strong {
			cursor: pointer;
		}

        #text-box {
            width: 100%;
            padding: 15px;
            border-radius: 8px;
            border: 1px solid #ccc;
            font-size: 1rem;
            color: #333;
            margin-bottom: 10px;
            transition: border 0.3s ease;
			height: calc(100vh - 160px);
        }

        #text-box:focus {
            border-color: #0066cc;
        }
        button {
            background-color: #0066cc;
            color: white;
            border: none;
            padding: 10px 20px;
            border-radius: 8px;
            cursor: pointer;
            font-size: 1rem;
            transition: background-color 0.3s ease;
        }
        button:hover {
            background-color: #005bb5;
        }

        .toast {
            position: fixed;
            top: 10px;
            left: 50%;
            transform: translateX(-50%);
            background-color: #ffffff;
            color: #333333;
            padding: 3px 20px;
            border-radius: 3px;
            font-size: 0.9rem;
            display: none;
			opacity: 0;
            transition: opacity 0.5s ease-in-out;
            display: flex;
            align-items: center;
			z-index:9999
        }
        
        .toast.show {
            display: flex;
            opacity: 1;
        }

        .toast .icon {
			font-family: 'Segoe UI Symbol', sans-serif; /* 确保符号显示正确 */
            font-size: 1.5rem;
            margin-right: 10px;
        }

		.toast[data-status="success"] .icon { color: #28a745; }
		.toast[data-status="error"] .icon { color: #dc3545; }
		.toast[data-status="warning"] .icon { color: #ffc107; }
		.toast[data-status="info"] .icon { color: #17a2b8; }
		.toast[data-status="wait"] .icon { color: #6c757d; }
		

		.toast[data-status="success"]  	{ background-color: #f0f9eb;border: 1px solid #e1f3d8 }
		.toast[data-status="error"]  	{ background-color: #fef0f0; border: 1px solid  #fde2e2}
		
		/* 弹窗基础样式 */
		.modal {
			display: none;
			position: fixed;
			top: 0;
			left: 0;
			width: 100%;
			height: 100%;
			background: rgba(0,0,0,0.5);
			justify-content: center;
			align-items: center;
			z-index: 1000;
		}

		/* 尺寸变体 */
		.modal-large .modal-panel {
			width: 90%;
			height: 90%;
		}

		.modal-medium .modal-panel {
			width: 70%;
			max-width: 1000px;
			height: 80%;
		}

		.modal-small .modal-panel {
			width: 30%;
			min-width: 400px;
			max-height: 65%;
		}
		
		.modal-mini .modal-panel {
			width: 20%;
			min-width: 400px;
			max-height: 45%;
		}
		

		/* 堆叠效果 */
		.modal:nth-last-child(1) { z-index: 1003; }
		.modal:nth-last-child(2) { z-index: 1002; }
		.modal:nth-last-child(3) { z-index: 1001; }

		
        .modal-content {
            background: white;
            padding: 10px;
            max-width: 100%;
			min-width: 50%;
           	height: calc(100% - 90px);
            overflow: auto;
            position: static;
			align-items: center;  /* 垂直居中 */
			justify-content: center; /* 水平居中 */
        }
		
		.modal-content::-webkit-scrollbar {
			width: 6px;
		}
		.modal-content::-webkit-scrollbar-track {
			background: #f1f1f1;
		}
		.modal-content::-webkit-scrollbar-thumb {
			background: #888;
			border-radius: 3px;
		}
		
		
		
		.modal-panel{
			position:relative;
		}
		
		.modal-title {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 5px 10px;
            background-color: #f8f9fa;
            border-bottom: 1px solid #dee2e6;
            border-radius: 8px 8px 0 0;
            font-size: 1rem;
        }

        /* 调整关闭按钮位置 */
        .modal-close {
            position: relative;
            top: auto;
            right: auto;
            font-size: 28px;
            line-height: 1;
            color: #6c757d;
            transition: color 0.2s;
        }
		
		.modal-close:hover {
            color: #343a40;
			cursor: pointer;
        }
		

		.modal-actions {
			display: flex;
            justify-content: right;
            align-items: center;
            padding: 5px 10px;
            background-color: #f8f9fa;
            border-bottom: 1px solid #dee2e6;
            border-radius: 0 0 8px 8px;
            font-size: 1rem;
			
		}

		.modal-actions button {
			padding: 8px 20px;
			font-size: 0.9rem;
			border-radius: 6px;
			transition: all 0.2s;
			margin-right:10px;
		}

		.modal-actions button.copy-btn {
			background: #0066cc;
			color: white;
		}

		.modal-actions button.copy-btn:hover {
			background: #005bb5;
		}

		.modal-actions button.download-btn {
			background: #28a745;
			color: white;
		}

		.modal-actions button.download-btn:hover {
			background: #218838;
		}
       
        #file-content {
         
        }
		#file-content img {
			display: block;
			margin: 0 auto;
			max-width: 100%;
			height: auto;
			cursor: pointer;
			transition: transform 0.3s ease;
			object-fit: contain; /* 保持比例 */
		
		}

		/* 保持文本默认对齐 */
		#file-content pre {
			text-align: left;
			margin: 0;
			padding: 10px;
			background: #f5f5f5;
			border-radius: 4px;
		}
		
		* 新增样式 */
		.project-item {
			display: flex;
			align-items: center;
			padding: 10px;
			margin: 5px 0;
			border-radius: 8px;
			transition: background 0.3s;
		}

		.project-item:hover {
			background: #f5f5f5;
		}
		
		

		.project-actions {
			margin-left: auto;
		}

		.project-item:hover .project-actions {
		}

		.current-badge {
			background: #4CAF50;
			color: white;
			padding: 2px 8px;
			border-radius: 4px;
			font-size: 0.8em;
			margin-left: 10px;
		}
		.table-container {
			height: calc(100% - 100px); /* 根据实际空间调整 */
			overflow-y: auto;
			position: relative;
		}

		.project-table {
			width: 100%;
			border-collapse: collapse;
			margin: 0;
			font-size: 0.9em;
			min-width: 600px;
		}
		/* 固定表头 */
		.project-table thead {
			position: sticky;
			top: 0;
			z-index: 1;
			background: #f8f9fa;
			box-shadow: 0 2px 2px -1px rgba(0,0,0,0.1);
		}


		/* 调整表头单元格样式 */
		.project-table th,
		.project-table td {
			padding: 12px 15px;
			text-align: center;
			position: sticky;
			top: 0;
			background: inherit;
		}
		
		/* 表格内容区域 */
		.project-table tbody {
			overflow-y: auto;
		}
		.project-table tbody tr:hover {
			background-color: #f5f7fa;
		}

		/* 当前项目标识 */
		.current-badge {
			background: #0066cc;
			color: white;
			padding: 2px 8px;
			border-radius: 12px;
			font-size: 0.8em;
			margin-left: 8px;
		}

		/* 操作按钮容器 */
		.project-actions {
			display: flex;
			gap: 8px;
			justify-content: center;
		}

		.project-actions button {
			padding: 4px 12px;
			font-size: 0.85em;
			border-radius: 4px;
			transition: all 0.2s;
		}

		.project-actions button:first-child {
			background: #28a745;
			color: white;
		}

		.project-actions button:last-child {
			background: #ffc107;
			color: #333;
		}

		/* 状态标识 */
		.status-active { color: #28a745; }
		-status-archived { color: #6c757d; }


		.form-group {
			margin-bottom: 15px;
		}
		.form-actions{
			position:absolute;
			bottom:10px;
			right:10px;
		}
		
		.form-input {
			width: 100%;
			padding: 8px;
			border: 1px solid #ddd;
			border-radius: 4px;
		}
		.form-select{
			appearance: none;
			background-image: url("data:image/svg+xml;charset=UTF-8,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='%23666'%3e%3cpath d='M7 10l5 5 5-5z'/%3e%3c/svg%3e");
			background-repeat: no-repeat;
			background-position: right 8px center;
			background-size: 16px;
			padding-right: 32px;
		}
		.disabled{
			background-color:#eee !important;
			color:#333 !important;
		}
		
    </style>
</head>
<body>

    <!-- Tabs -->
    <div class="tabs">
        <div class="tab active" data-tab="file-tab">文件管理</div>
        <div class="tab" data-tab="text-tab">文本框</div>
		
		 <div class="menu-icon" onclick='showProjects()'>
			<span></span>
			<span></span>
			<span></span>
		</div>
		
	
    </div>

    <!-- Tab Content -->
    <div id="file-tab" class="tab-content active">
        <!-- File upload and list -->
        <div id="upload-area">点击上传文件或拖拽文件到此区域</div>
        <input type="file" id="file-upload" multiple style="display:none;"/>
        <ul id="file-list"></ul>
    </div>

    <div id="text-tab" class="tab-content">
        <!-- Textbox -->
        <textarea id="text-box" rows="20" cols="50" placeholder="在此输入内容..."></textarea>
        <button onclick='saveText()'>保存</button>
    </div>
	<div id="toast" class="toast" >
        <span class="icon"></span>
        <span id="toast-message"></span>
    </div>
	
    <div id="preview-modal" class="modal">
		<div class="modal-panel">
			<div class="modal-title">
                <span>文件预览</span> <!-- 新增标题 -->
                <span class="modal-close" onclick='closePreview()'>&times;</span>
            </div>
			
			<div class="modal-content">
				<div id="file-content"></div>
			</div>
			<div class="modal-actions" id="modal-actions"></div>
        </div>
	</div>
	<div id="projects-modal" class="modal">
		<div class="modal-panel">
			<div class="modal-title">
				<span>项目管理</span>
				<span class="modal-close" onclick='closeProjectsModal()'>&times;</span>
			</div>
			<div class="modal-content">
				<table class="project-table">
					<thead>
						<tr>
							<th>项目名称</th>
							<th>创建时间</th>
							<th>完成时间</th>
							<th>状态</th>
							<th>操作</th>
						</tr>
					</thead>
					<tbody id="projects-list"></tbody>
				</table>
			</div>
			
			<div class="modal-actions">
				<button class="btn-primary" onclick='openCreateModal()'>新建项目</button>
				<button class="btn-secondary" onclick='closeProjectsModal()'>取消</button>
			</div>
		</div>
	</div>
	<div id="edit-project-modal" class="modal">
		<div class="modal-panel">
			<div class="modal-title">
				<span>编辑项目</span>
				<span class="modal-close" onclick='closeEditModal()'>&times;</span>
			</div>
			<div class="modal-content" >
				<form id="edit-project-form" >
					<input type="hidden" id="edit-project-id">
					<div class="form-group">
						<label>项目名称：</label>
						<input type="text" id="edit-project-name" required class="form-input">
					</div>
					<div class="form-group">
						<label>状态：</label>
						<select id="edit-project-status" class="form-input form-select">
							<option value="active">进行中</option>
							<option value="archived">已归档</option>
						</select>
					</div>
					<div class="form-group">
						<label>完成时间：</label>
						<input type="datetime-local" id="edit-project-endtime" class="form-input">
					</div>
					<div class="form-group">
						<label>备注：</label>
						<textarea rows=5 id="edit-project-description" class="form-input"></textarea>
					</div>
				</form>
			</div>
			<div class="modal-actions">
				<button type="button" class="btn-primary" onclick='submitProjectEdit()'>保存</button>
				<button type="button" onclick='closeEditModal()'>取消</button>
			</div>
		</div>
	</div>

	<div id="create-project-modal" class="modal">
		<div class="modal-panel">
			<div class="modal-title">
				<span>新建项目</span>
				<span class="modal-close" onclick='closeCreateModal()'>&times;</span>
			</div>
			<div class="modal-content" >
				<form id="create-project-form" >
					<input type="hidden" id="create-project-id">
					<div class="form-group">
						<label>项目名称：</label>
						<input type="text" id="create-project-name" required class="form-input">
					</div>
					
					<div class="form-group">
						<label>备注：</label>
						<textarea rows=5 id="create-project-description" class="form-input"></textarea>
					</div>
					
					
				</form>
			</div>
			<div class="modal-actions">
				<button type="button" class="btn-primary" onclick='submitProjectCreate()'>保存</button>
				<button type="button" onclick='closeCreateModal()'>取消</button>
			</div>
		</div>
	</div>

	
    <script>
		class ModalManager {
			constructor() {
				this.modalStack = [];
				this.initGlobalHandlers();
			}

			// 初始化全局事件监听
			initGlobalHandlers() {
				// ESC键关闭
				document.addEventListener('keydown', (e) => {
					if (e.key === 'Escape') {
						this.closeTopModal();
					}
				});

				// 点击外部关闭
				document.addEventListener('click', (e) => {
					if (e.target.classList.contains('modal')) {
						this.closeTopModal();
					}
				});
			}

			// 打开弹窗
			openModal(modalId, size = 'medium') {
				const modal = document.getElementById(modalId);
				if (!modal) return;

				// 关闭现有弹窗的激活状态
				this.modalStack.forEach(m => m.classList.remove('modal-active'));

				// 设置尺寸类
				modal.className = `modal modal-${size}`;
				modal.style.display = 'flex';
			
				// 压入堆栈
				this.modalStack.push(modal);
			}

			// 关闭顶层弹窗
			closeTopModal() {
				if (this.modalStack.length === 0) return;

				const modal = this.modalStack.pop();
				//modal.classList.remove('modal-active');
				
				modal.style.display = 'none';
				
				
				
			}

			// 关闭指定弹窗
			closeModal(modalId) {
				const index = this.modalStack.findIndex(m => m.id === modalId);
				if (index === -1) return;
				this.modalStack.splice(index, 1);
				document.getElementById(modalId).style.display = 'none';
			}
		}


		// 初始化管理器
		const modalManager = new ModalManager();

        // Function to show the correct tab content
        function showTab(tabId) {
            document.querySelectorAll('.tab-content').forEach(function (tab) {
                tab.classList.remove('active');
            });

            document.querySelectorAll('.tab').forEach(function (tab) {
                tab.classList.remove('active');
            });

            document.getElementById(tabId).classList.add('active');
            const clickedTab = document.querySelector(`.tabs .tab[data-tab="${tabId}"]`);
            clickedTab.classList.add('active');
        }
	
	// 前端JavaScript修改部分（script标签内）：
	function handleResponse(response) {
		return response.json().catch(err => {
			return { success: false, message: "响应解析失败" };
		}).then(data => {
				if (!response.ok || !data.success) {
					throw new Error(data.message || "请求失败");
				}
				return data;
			})
		};

        // Show toast notification
        function showToast(message,status) {
			const toast = document.getElementById('toast');
            const messageElement = document.getElementById('toast-message');
			
			// 移除旧状态
			toast.removeAttribute('data-status');
			
			// 设置新状态
			toast.dataset.status = status;
			
			const iconMap = {
				success: '✓', // 更简洁的符号
				error: '✕',
				warning: '⚠',
				info: 'ℹ',
				wait: '⏳'
			};
			
           
			messageElement.textContent = message;
			toast.classList.add('show');
			toast.querySelector('.icon').textContent = iconMap[status] || '';
			if(status=='wait'){
				return;
			}
					
            setTimeout(() => {
                toast.classList.remove('show');
            }, 2000);  // 2.5 seconds
        }


        // Add event listener to tabs
        const tabs = document.querySelectorAll('.tab');
        tabs.forEach(function(tab) {
            tab.addEventListener('click', function() {
                const tabId = tab.getAttribute('data-tab');
                showTab(tabId);
            });
        });

        const uploadArea = document.getElementById('upload-area');
        const fileInput = document.getElementById('file-upload');
        const fileList = document.getElementById('file-list');

        // Trigger file input when upload area is clicked
        uploadArea.addEventListener('click', () => {
            fileInput.click();
        });

        // File upload event when files are selected
        fileInput.addEventListener('change', (e) => {
            const files = e.target.files;
            if (files.length > 0) {
                uploadFiles(files);
            }
        });

        // File upload event when dragging over the area
        uploadArea.addEventListener('dragover', (e) => {
            e.preventDefault();
            uploadArea.style.borderColor = '#000';
        });

        uploadArea.addEventListener('dragleave', () => {
            uploadArea.style.borderColor = '#ccc';
        });

        uploadArea.addEventListener('drop', (e) => {
            e.preventDefault();
            uploadArea.style.borderColor = '#ccc';
            const files = e.dataTransfer.files;
            if (files.length > 0) {
                uploadFiles(files);
            }
        });
	
		//复制文件路径函数
		function copyFilePath(filename) {
			const baseUrl = window.location.origin;
			const fileUrl = `${baseUrl}/download?file=${encodeURIComponent(filename)}`;
			
			navigator.clipboard.writeText(fileUrl).then(() => {
				showToast('链接已复制到剪贴板', 'success');
			}).catch(err => {
				console.error('复制失败:', err);
				showToast('复制失败，请手动复制', 'error');
			});
		}

        function uploadFiles(files) {
			const projectId = localStorage.getItem('current_project_id');
			if (!projectId) {
				showToast('请先选择项目', 'error');
				return;
			}

			const formData = new FormData();
			formData.append('project_id', projectId);
			
			for (let i = 0; i < files.length; i++) {
				formData.append('file', files[i]);
			}

			const xhr = new XMLHttpRequest();
			xhr.open('POST', '/upload', true);

			xhr.upload.onprogress = function (e) {
				if (e.lengthComputable) {
					const percent = Math.round((e.loaded / e.total) * 100);
					showToast(`上传进度: ${percent}%`, 'wait');
				}
			};

			xhr.onload = function () {
				if (xhr.status === 200) {
					showToast('上传成功', 'success');
					loadFileList();
				} else {
					showToast('上传失败', 'error');
				}
			};

			xhr.send(formData);
		}

        // Load file list
		// 修改后的loadFileList函数
        function loadFileList() {
			const projectId = localStorage.getItem('current_project_id');
			if (!projectId) {
				showToast('请先选择项目', 'error');
				return;
			}

            fetch(`/fileList?project_id=${projectId}`)
                .then(response => response.json())
                .then(data => {
                    fileList.innerHTML = '';

					data.forEach(file => {
						const li = createFileItem(file);
						fileList.appendChild(li);
					});
                });
        }

     

        function createFileItem(file) {
            const li = document.createElement('li');
            const previewDisabled = file.previewable ? '' : 'style="display:none"';
            const copyDisabled = file.copyable ? '' : 'style="display:none"';
            li.innerHTML = `
                <span>
                    <strong onclick=\"copyFilePath('${file.name.replace(/'/g, "\\'")}')\">
                        ${file.name}
                    </strong> 
                    - ${file.size} - ${file.created}
                </span>
                <span>
                    <button ${previewDisabled} onclick=\"previewFile('${file.name}')\">查看</button>
                    <button ${copyDisabled} onclick=\"copyImageToClipboard('${file.name}')\">复制</button>
                    <button onclick=\"downloadFile('${file.name}')\">下载</button>
                </span>`;
            return li;
        }

        function toggleGroup(header) {
            const group = header.parentElement;
            const sublist = header.nextElementSibling;
            const arrow = header.querySelector('.arrow');
            
            if (sublist.style.display === 'none') {
                sublist.style.display = 'block';
                arrow.style.transform = 'rotate(90deg)';
                group.classList.add('expanded');
            } else {
                sublist.style.display = 'none';
                arrow.style.transform = '';
                group.classList.remove('expanded');
            }
        }

        // Download file
        function downloadFile(filename) {
			const projectId = localStorage.getItem('current_project_id');
			if (!projectId) {
				showToast('请先选择项目', 'error');
				return;
			}
            const url = `/download?project_id=${projectId}&file=${filename}`;
            window.location.href = url;
        }
		
		// Load saved text
        function loadText() {
			const projectId = localStorage.getItem('current_project_id');
			if (!projectId) {
				showToast('请先选择项目', 'error');
				return;
			}

			
            fetch(`/loadText?project_id=${projectId}`)
                .then(response => response.json())
                .then(data => {
                    if (data.success && data.text) {
                        document.getElementById('text-box').value = data.text;
                    } else {
                        document.getElementById('text-box').value = '';
                    }
                });
        }
		

        // Save text content to server
        function saveText(id) {
			
			const projectId = localStorage.getItem('current_project_id');
			if (!projectId) {
				showToast('请先选择项目', 'error');
				return;
			}

			const content = document.getElementById(`text-box`).value;
			
			fetch(`/saveText?project_id=${projectId}`, {
				method: 'POST',
				headers: {'Content-Type': 'application/json'},
				body: JSON.stringify({ id: id, content: content })
			})
			.then(handleResponse)
			.then(() => showToast('保存成功', 'success'))
			.catch(err => showToast(err.message, 'error'));
		}



		async function copyImageToClipboard(filename) {
			  try {
				const projectId = localStorage.getItem('current_project_id');
				if (!projectId) {
					showToast('请先选择项目', 'error');
					return;
				}
				const res = await fetch(`/view?project_id=${projectId}&file=${encodeURIComponent(filename)}`);
				if (!res.ok) throw new Error('加载失败');
				
				const blob = await res.blob();

				const item = new ClipboardItem({ 'image/png': blob });
				await navigator.clipboard.write([item]);
				showToast('图片已复制，请到Word中按 Ctrl+V 粘贴', 'success');
			  } catch (err) {
				console.error('复制失败:', err);
				showToast('复制失败，请检查浏览器权限', 'error');
			  }
		}


		// 新增预览功能
		async function previewFile(filename) {
			const modal = document.getElementById('preview-modal');
			const content = document.getElementById('file-content');
			const actions = document.getElementById('modal-actions');
			const modalContent = document.querySelector('.modal-content');
			
			let isImage = false;

				const projectId = localStorage.getItem('current_project_id');
				if (!projectId) {
					showToast('请先选择项目', 'error');
					return;
				}
		
				const res = await fetch(`/view?project_id=${projectId}&file=${encodeURIComponent(filename)}`);
				if (!res.ok) throw new Error('加载失败');
				
				const blob = await res.blob();
				const url = URL.createObjectURL(blob);
				
				// 清空旧内容
				actions.innerHTML = '';
				content.innerHTML = '';

				// 判断内容类型
				isImage = res.headers.get('Content-Type').startsWith('image/');

				if (isImage) {
					modalContent.style.display = 'flex';
					content.innerHTML = `<img src="${url}" style=\"display: block;\" onclick=\"handleImageClick(this)\" onload=\"initImageSize(this)\">`;
				} else {
					const text = await blob.text();
					content.innerHTML = `<pre style=\"white-space: pre-wrap\">${text}</pre>`;
				}

				// 动态生成操作按钮
				const downloadBtn = document.createElement('button');
				downloadBtn.className = 'download-btn';
				downloadBtn.innerHTML = '下载文件';
				downloadBtn.onclick = () => downloadFile(filename);

				actions.appendChild(downloadBtn);

				if (isImage) {
					const copyBtn = document.createElement('button');
					copyBtn.className = 'copy-btn';
					copyBtn.innerHTML = '复制图片';
					copyBtn.onclick = () => copyImageToClipboard(filename);
					actions.appendChild(copyBtn);
				}

				modalManager.openModal('preview-modal', 'large');

		}
		
		function initImageSize(img) {
			const modalContent = document.querySelector('.modal-content');
			const maxHeight = modalContent.clientHeight -40;
			
			// 初始状态：限制高度不超过弹窗
			if (img.naturalHeight > maxHeight) {
				img.style.maxHeight = maxHeight + 'px';
				img.style.width = 'auto';
				img.dataset.originalHeight = maxHeight;
			} else {
				img.style.maxHeight = 'none';
				img.style.width = 'auto';
				img.dataset.originalHeight = img.naturalHeight;
			}
			
			img.dataset.zoomed = 'false';
		}

		function handleImageClick(img) {
			const modalContent = document.querySelector('.modal-content');
			const maxWidth = modalContent.clientWidth-80;
			
			if (img.dataset.zoomed === 'true') {
				// 恢复初始尺寸
				img.style.maxWidth = 'none';
				img.style.width = 'auto';
				img.style.maxHeight = img.dataset.originalHeight + 'px';
				img.dataset.zoomed = 'false';
			} else {
				// 放大到弹窗宽度
				img.style.maxWidth = maxWidth + 'px';
				img.style.width = maxWidth + 'px';
				img.style.maxHeight = 'none';
				img.dataset.zoomed = 'true';
			}
		}
		
				
		// 编辑项目模态框控制
		let editingProjectId = null;

		function openEditModal(projectId) {
			const project = projects.find(p => p.id == projectId);
			if (!project) return;

			editingProjectId = projectId;
			document.getElementById('edit-project-id').value = projectId;
			document.getElementById('edit-project-name').value = project.name;
			document.getElementById('edit-project-status').value = project.status;
			document.getElementById('edit-project-endtime').value =	project.ended ? project.ended.substring(0,16) : '';
			document.getElementById('edit-project-description').value =	project.description ? project.description : '';

			modalManager.openModal('edit-project-modal', 'small');
		}
	
		function closeEditModal() {
			modalManager.closeModal('edit-project-modal');
		}

		function submitProjectEdit() {
			const project = {
				id: editingProjectId,
				name: document.getElementById('edit-project-name').value,
				status: document.getElementById('edit-project-status').value,
				description: document.getElementById('edit-project-description').value,
				end_time: document.getElementById('edit-project-endtime').value
			};

			fetch(`/api/projects/${editingProjectId}`, {
				method: 'PUT',
				headers: {'Content-Type': 'application/json'},
				body: JSON.stringify(project)
			})
			.then(handleResponse)
			.then(() => {
				showToast('项目更新成功', 'success');
				closeEditModal();
				showProjects(); // 刷新列表
			})
			.catch(err => showToast(err.message, 'error'));
			return false; // 阻止表单默认提交
		}
		
		
		function openCreateModal() {
			modalManager.openModal('create-project-modal', 'mini');
		}
		function closeCreateModal() {
			modalManager.closeModal('create-project-modal');
			document.getElementById('create-project-name').value = '';
            document.getElementById('create-project-description').value = '';
		}
		function submitProjectCreate() {
			const project = {
				name: document.getElementById('create-project-name').value,
				description: document.getElementById('create-project-description').value
			};
			
			fetch('/api/projects', {
				method: 'POST',
				headers: {'Content-Type': 'application/json'},
				body: JSON.stringify(project)
			})
			.then(handleResponse)
			.then((res) => {
				showToast(res.message, 'success');
				closeCreateModal();
				showProjects();
			})
			.catch(err => {
				showToast(err.message, 'error');
			});
			
			return false;
		}

		function handleDirectoryConflict(project) {
			return new Promise((resolve, reject) => {
				// 显示自定义弹窗（替代原生confirm）
				const confirmed = window.confirm('目录已存在，是否清空原目录内容？\n\n选择"确定"清空，选择"取消"保留');
				
				if (confirmed) {
					fetch('/api/projects', {
						method: 'POST',
						headers: {'Content-Type': 'application/json'},
						body: JSON.stringify({...project, overwrite: true})
					})
					.then(response => {
						if (!response.ok) throw new Error('覆盖操作失败');
						resolve();
					})
					.catch(reject);
				} else {
					reject(new Error('已保留原有目录内容'));
				}
			});
		}

		function closeProjectsModal() {
			modalManager.closeModal('projects-modal');
            document.getElementById('create-project-name').value = '';
            document.getElementById('create-project-description').value = '';
		}

        // 关闭弹窗
		function closePreview() {
			modalManager.closeModal('preview-modal');
			document.getElementById('file-content').innerHTML = '';
			document.getElementById('modal-actions').innerHTML = ''; // 清空按钮
		}

        // 点击外部关闭
        window.onclick = function(event) {
            const modal = document.getElementById('preview-modal');
            if (event.target === modal) {
                closePreview();
            }
        }

		// 项目管理功能
		let projects = [];
		function showProjects() {
			const currentId = localStorage.getItem('current_project_id');
			fetch('/api/projects')
				.then(res => res.json())
				.then(data => {
					projects = data;
					const tbody = document.getElementById('projects-list');
					tbody.innerHTML = projects.map(p => `
						<tr class="project-row ${p.id == currentId ? 'current-project' : ''}">
							<td>
								${p.name}
							</td>
							<td>${p.created || '-'}</td>
							<td>${p.ended || '进行中'}</td>
							<td class="status-${p.status}">${getStatusText(p.status)}</td>
							<td>
								<div class="project-actions">
									${p.id != currentId ? 
										`<button onclick=\"selectProject(${p.id})\">选中</button>` : '<button disabled  class=\"disabled\">选中</button>'
									}
									<button onclick=\"openEditModal(${p.id})\">编辑</button>
									<button onclick=\"exportProject(${p.id})\">导出</button>
								</div>
							</td>
						</tr>
					`).join('');
					modalManager.openModal('projects-modal', 'medium');
				});
		}
		// 状态显示文本转换
		function getStatusText(status) {
			return {
				active: '进行中',
				archived: '已归档'
			}[status] || status;
		}


		
		function selectProject(id) {
			fetch('/api/projects/current', {
				method: 'POST',
				headers: {'Content-Type': 'application/json'},
				body: JSON.stringify({id: id})
			})
			.then(handleResponse)
			.then(() => {
				localStorage.setItem('current_project_id', id);
				showToast("已切换", 'success')
				closeProjectsModal();
				loadCurrentProject();
				//location.reload(); // 刷新页面加载新项目数据
			}).catch(err => showToast(err.message, 'error'));

		}

		// 初始化加载当前项目
		function loadProject() {
			fetch('/api/projects')
				.then(res => res.json())
				.then(data => {
					if (data.length > 0) {
						selectProject(data[0].id);
					}
				});
		}
		
		// 初始化加载当前项目
		function loadCurrentProject() {
			let currentProject = null;
			fetch('/api/projects/current')
				.then(res => res.json())
				.then(data => {
					if (data.id && data.id !== currentProject?.id) {
						currentProject = data;
						localStorage.setItem('current_project_id', data.id);
						//updateProjectDisplay(data);
						loadText();
						loadFileList();
					}else{
						showToast('请先选择项目!', 'error');
					}
				});
		}
		function exportProject(projectId) {
			showToast('正在生成导出文件...', 'wait');
			fetch(`/api/projects/export/${projectId}`)
				.then(response => {
					if (!response.ok) throw new Error('导出失败');
					return response.blob();
				})
				.then(blob => {
					const url = window.URL.createObjectURL(blob);
					const a = document.createElement('a');
					a.href = url;
					a.download = `project_${projectId}_export.zip`;
					document.body.appendChild(a);
					a.click();
					window.URL.revokeObjectURL(url);
					showToast('导出成功', 'success');
				})
				.catch(err => showToast(err.message, 'error'));
		}

        // Page load actions
        window.onload = function() {
			loadCurrentProject();
        };
    </script>
</body>
</html>

)";

// 新增图标处理函数
void handle_favicon(const httplib::Request& req, httplib::Response& res) {
    // 设置长期缓存（1年）
    res.set_header("Cache-Control", "public, max-age=31536000");
    res.set_header("Content-Type", "image/x-icon");
    res.set_content(
        reinterpret_cast<const char*>(favourite_ico), 
        favourite_ico_len, 
        "image/x-icon"
    );
}

// 主页处理函数
void handle_index(const httplib::Request& req, httplib::Response& res) {
    res.set_content(html_page, "text/html");
}

int main() {
    init_database(); // 新增数据库初始化

	httplib::SSLServer svr(
        "./server.crt", // 证书路径
        "./server.key"  // 私钥路径
    );
	
    svr.Get("/", handle_index); // 返回前端 HTML 页面
    svr.Post("/upload", handle_upload);  // 上传文件的路由
    svr.Get("/fileList", handle_file_list);  // 列出文件的路由
    svr.Get("/download", handle_download);  // 下载文件的路由
    svr.Post("/saveText", handle_save_text);  // 保存文本的路由
    svr.Get("/loadText", handle_load_text);  // 加载文本的路由
    svr.Get("/view", handle_view);  // 新增预览路由
    svr.Get("/favicon.ico", handle_favicon); // 新增图标路由
	svr.Put(R"(/api/projects/(\d+))", handle_update_project);
	svr.Get("/api/projects", handle_get_projects);
    svr.Post("/api/projects", handle_create_project);
    svr.Post("/api/projects/current", handle_set_current_project);
    svr.Get("/api/projects/current", handle_get_current_project);
	svr.Get(R"(/api/projects/export/(\d+))", handle_export_project);

    svr.listen("0.0.0.0", 443);

    return 0;
}