#pragma once

#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <memory>
#include <functional>
#include <openssl/sha.h>

namespace fs = std::filesystem;

// ============================================================================
// 配置常量
// ============================================================================
namespace config {
    // 哈希块大小
    constexpr size_t HEAD_HASH_SIZE = 4096;           // 头部哈希读取大小
    constexpr size_t SAMPLE_BLOCK_SIZE = 4096;         // 采样块大小
    constexpr size_t SAMPLE_THRESHOLD = 100 * 1024 * 1024; // 100MB 开始采样
    constexpr size_t SAMPLE_COUNT = 5;                  // 采样点数量

    // 缓冲区大小
    constexpr size_t BUFFER_SIZE_SMALL = 4 * 1024;      // < 1MB
    constexpr size_t BUFFER_SIZE_MEDIUM = 64 * 1024;    // 1MB - 100MB
    constexpr size_t BUFFER_SIZE_LARGE = 1024 * 1024;  // > 100MB
    constexpr size_t MMAP_THRESHOLD = 500 * 1024 * 1024; // 500MB 使用内存映射

    // 线程默认使用 CPU 核心数 - 1
    constexpr int DEFAULT_THREADS = 0;

    // 默认保留策略
    constexpr const wchar_t* DEFAULT_KEEP_METHOD = L"oldest";
}

// ============================================================================
// 轻量级数据结构
// ============================================================================

// 文件条目（紧凑结构）
struct FileEntry {
    uint64_t size;           // 文件大小
    uint64_t mtime;          // 修改时间
    uint32_t path_index;     // 路径在字符串池中的索引

    FileEntry() : size(0), mtime(0), path_index(0) {}
    FileEntry(uint64_t s, uint64_t mt, uint32_t pi) : size(s), mtime(mt), path_index(pi) {}
};

// 哈希结果
struct HashResult {
    uint64_t xxhash64;       // xxHash64 快速哈希

    bool operator==(const HashResult& other) const {
        return xxhash64 == other.xxhash64;
    }
};

// 重复文件组
struct DuplicateGroup {
    uint64_t file_size;
    std::vector<uint32_t> file_indices;  // 文件索引列表
};

// ============================================================================
// 字符串池（减少内存分配）
// ============================================================================
class StringPool {
public:
    uint32_t add(const std::wstring& str) {
        auto it = index_map_.find(str);
        if (it != index_map_.end()) return it->second;

        uint32_t idx = static_cast<uint32_t>(strings_.size());
        strings_.push_back(str);
        index_map_[str] = idx;
        return idx;
    }

    const std::wstring& get(uint32_t index) const {
        return strings_[index];
    }

    size_t size() const { return strings_.size(); }

private:
    std::vector<std::wstring> strings_;
    std::unordered_map<std::wstring, uint32_t> index_map_;
};

// ============================================================================
// 命令行参数
// ============================================================================
struct CommandLineArgs {
    std::wstring directory;

    // 扫描控制
    std::vector<std::wstring> extensions;      // 扩展名过滤
    uint64_t min_size = 0;                      // 最小文件大小 (bytes)
    uint64_t max_size = 0;                      // 最大文件大小 (0=不限制)

    // 哈希策略
    bool quick_mode = false;                     // 快速模式 (只用大小+头部哈希)
    size_t sample_size = config::SAMPLE_BLOCK_SIZE;

    // 输出控制
    bool show_all = false;                      // 显示所有重复组
    std::wstring output_file;                   // JSON 输出文件
    bool output_json = false;

    // 删除操作
    bool delete_files = false;                  // 自动删除
    std::wstring keep_method = config::DEFAULT_KEEP_METHOD; // 保留策略
    bool preview_only = false;                  // 仅预览
    bool interactive = false;                   // 交互式确认

    // 性能优化
    int thread_count = config::DEFAULT_THREADS; // 线程数
    bool use_cache = false;                     // 增量缓存

    // 其他
    bool verbose = false;
};

// ============================================================================
// 扫描结果
// ============================================================================
struct ScanResult {
    uint64_t total_files = 0;
    uint64_t duplicate_groups = 0;
    uint64_t duplicate_files = 0;
    uint64_t potential_savings = 0;
    double scan_duration = 0;
    std::vector<DuplicateGroup> groups;
};

// ============================================================================
// 核心类声明
// ============================================================================

// xxHash64 实现（内联）
class XXHash64 {
public:
    static uint64_t hash(const void* data, size_t len, uint64_t seed = 0);

    // 流式哈希
    class Stream {
    public:
        Stream(uint64_t seed = 0);
        void update(const void* data, size_t len);
        uint64_t digest() const;

    private:
        void process_batch();

        uint64_t seed_;
        uint64_t acc_[4];
        uint64_t total_len_;
        mutable uint32_t buf_pos_;
        char buf_[32];
    };
};

// SHA-256 上下文 (简化版)
struct SHA256Context {
    SHA256_CTX ctx;
    unsigned char hash[32];
};

void sha256_init(SHA256Context* ctx);
void sha256_update(SHA256Context* ctx, const void* data, size_t len);
void sha256_final(SHA256Context* ctx);

// 文件读取器（支持内存映射）
class FileReader {
public:
    explicit FileReader(const std::wstring& path);
    ~FileReader();

    // 读取指定范围
    size_t read(void* buffer, size_t offset, size_t size) const;

    // 读取头部
    size_t read_head(void* buffer, size_t size) const;

    // 读取采样块
    std::vector<std::pair<size_t, size_t>> get_sample_ranges(size_t block_size, size_t count) const;

    uint64_t size() const { return size_; }
    bool is_open() const { return file_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE file_;
    HANDLE mapping_;
    void* mapped_data_;
    uint64_t size_;
    bool use_mmap_;
};

// 线程池
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    template<typename Func>
    void enqueue(Func&& task);

    void wait();

    size_t size() const { return threads_.size(); }

private:
    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;
    std::atomic<size_t> active_tasks_;
};

// 重复文件扫描器
class DuplicateScanner {
public:
    explicit DuplicateScanner(const CommandLineArgs& args);

    ScanResult scan();
    void export_json(const std::wstring& path) const;
    void delete_duplicates(const CommandLineArgs& args, ScanResult& result);

    // 获取文件列表用于输出
    const std::vector<FileEntry>& get_files() const { return files_; }
    const StringPool& get_path_pool() const { return path_pool_; }

private:
    // 阶段1: 遍历目录
    void phase1_scan_directory();

    // 阶段2: 按大小分组
    void phase2_group_by_size();

    // 阶段3: 并行哈希比较
    void phase3_compare_hash();

    // 计算哈希
    HashResult compute_hash(const FileReader& reader, bool full_hash) const;

    // 过滤扩展名
    bool matches_extension(const std::wstring& path) const;

    // 获取要保留的文件索引
    uint32_t get_keep_index(const DuplicateGroup& group) const;

    const CommandLineArgs& args_;
    StringPool path_pool_;
    std::vector<FileEntry> files_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> size_groups_;
    ScanResult result_;
    std::mutex result_mutex_;
};

// ============================================================================
// CLI 和输出
// ============================================================================

// 命令行解析
CommandLineArgs parse_arguments(int argc, wchar_t** argv);

// 格式化输出
void print_banner();
void print_help();
void print_result(const ScanResult& result, const std::vector<FileEntry>& files, const StringPool& pool, const CommandLineArgs& args);

// 工具函数
std::wstring format_size(uint64_t bytes);
std::string to_utf8(const std::wstring& wstr);
std::wstring to_wstring(const std::string& str);
