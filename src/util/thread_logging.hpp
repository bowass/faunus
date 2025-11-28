#pragma once
#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <map>

namespace thread_log {
    enum LogLevel { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

    inline int& log_level() {
        static int level = LOG_INFO;
        return level;
    }
    inline void set_log_level(LogLevel level) { log_level() = level; }
    inline const char* level_to_string(LogLevel level) {
        switch (level) {
            case LOG_ERROR: return "ERR";
            case LOG_WARN:  return "WRN";
            case LOG_INFO:  return "INF";
            case LOG_DEBUG: return "DBG";
            default:        return "UNK";
        }
    }
    inline LogLevel log_level_from_string(const std::string& level_str) {
        if (level_str == "ERROR") return LOG_ERROR;
        if (level_str == "WARN" || level_str == "WARNING") return LOG_WARN;
        if (level_str == "INFO") return LOG_INFO;
        if (level_str == "DEBUG") return LOG_DEBUG;
        throw std::invalid_argument("Unknown log level: " + level_str);
    }

    // Per-thread log file streams
    inline std::map<std::thread::id, std::ofstream>& thread_log_streams() {
        static std::map<std::thread::id, std::ofstream> streams;
        return streams;
    }
    inline std::string log_dir = "thread_logs";
    inline void setup_log_dir() {
        namespace fs = std::filesystem;
        fs::path dir(log_dir);
        if (fs::exists(dir)) {
            for (auto& entry : fs::directory_iterator(dir)) {
                fs::remove_all(entry.path());
            }
        } else {
            fs::create_directories(dir);
        }
    }
    inline void setup_thread_log() {
        auto tid = std::this_thread::get_id();
        std::ostringstream fname;
        fname << log_dir << "/thread_" << tid << ".log";
        thread_log_streams()[tid] = std::ofstream(fname.str(), std::ios::out | std::ios::trunc);
    }
    inline void close_thread_log() {
        auto tid = std::this_thread::get_id();
        auto& streams = thread_log_streams();
        if (streams.count(tid)) streams[tid].close();
    }
    inline std::string current_timestamp() {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto itt = system_clock::to_time_t(now);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        std::ostringstream ss;
        ss << std::put_time(std::localtime(&itt), "%F %T") << '.' << std::setw(5) << std::setfill('0') << ms.count();
        return ss.str();
    }
    inline void log(LogLevel level, const std::string& message) {
        static std::mutex log_mutex;
        auto tid = std::this_thread::get_id();
        if (log_level() >= level) {
            std::lock_guard<std::mutex> lock(log_mutex);
            auto& streams = thread_log_streams();
            if (!streams.count(tid)) setup_thread_log();
            std::ofstream& ofs = streams[tid];
            ofs << "[" << current_timestamp() << "] [" << level_to_string(level) << "] [TID " << tid << "] " << message << std::endl;
        }
    }
    #define LOG(level, msg) do { \
        std::ostringstream oss; \
        oss << msg; \
        ::thread_log::log(level, oss.str()); \
    } while (0)
    #define LOG_ERROR(msg) LOG(::thread_log::LOG_ERROR, msg)
    #define LOG_WARN(msg)  LOG(::thread_log::LOG_WARN,  msg)
    #define LOG_INFO(msg)  LOG(::thread_log::LOG_INFO,  msg)
    #define LOG_DEBUG(msg) LOG(::thread_log::LOG_DEBUG, msg)
}
