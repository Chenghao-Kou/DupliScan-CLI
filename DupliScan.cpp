#include "DupliScan.h"
#include <shlwapi.h>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <io.h>
#include <fcntl.h>

#pragma comment(lib, "shlwapi.lib")

// ============================================================================
// 工具函数实现
// ============================================================================

void wprint(const std::wstring& str) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteConsoleW(hConsole, str.c_str(), static_cast<DWORD>(str.length()), &written, nullptr);
}

std::wstring format_size(uint64_t bytes) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double size = static_cast<double>(bytes);
    int unit_idx = 0;
    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        unit_idx++;
    }
    std::wostringstream ss;
    ss << std::fixed << std::setprecision(2) << size << L" " << units[unit_idx];
    return ss.str();
}

std::string to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &str[0], size_needed, nullptr, nullptr);
    return str;
}

std::wstring to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wstr[0], size_needed);
    return wstr;
}

// ============================================================================
// XXHash64 实现
// ============================================================================

// 质数
static const uint64_t PRIME64_1 = 11400714785074694791ULL;
static const uint64_t PRIME64_2 = 14029467366897019727ULL;
static const uint64_t PRIME64_3 = 1609587929392839161ULL;
static const uint64_t PRIME64_4 = 9650029242287828579ULL;
static const uint64_t PRIME64_5 = 2870177450012600261ULL;

static uint64_t rotl(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static uint64_t mix64(uint64_t h, uint64_t input, uint64_t mul) {
    input *= mul;
    input = rotl(input, 31);
    input *= mul;
    h ^= input;
    h = rotl(h, 27) * mul + PRIME64_4;
    return h;
}

uint64_t XXHash64::hash(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h;

    if (len >= 32) {
        uint64_t v1 = seed + PRIME64_1 + PRIME64_2;
        uint64_t v2 = seed + PRIME64_2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - PRIME64_1;

        const uint64_t* p64 = reinterpret_cast<const uint64_t*>(p);
        while (p64 + 4 <= reinterpret_cast<const uint64_t*>(p + len)) {
            v1 += *p64++ * PRIME64_2;
            v1 = rotl(v1, 31);
            v1 *= PRIME64_1;
            v2 += *p64++ * PRIME64_2;
            v2 = rotl(v2, 31);
            v2 *= PRIME64_1;
            v3 += *p64++ * PRIME64_2;
            v3 = rotl(v3, 31);
            v3 *= PRIME64_1;
            v4 += *p64++ * PRIME64_2;
            v4 = rotl(v4, 31);
            v4 *= PRIME64_1;
        }

        p = reinterpret_cast<const uint8_t*>(p64);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = mix64(h, v1, PRIME64_2);
        h = mix64(h, v2, PRIME64_2);
        h = mix64(h, v3, PRIME64_2);
        h = mix64(h, v4, PRIME64_2);
    } else {
        h = seed + PRIME64_5;
    }

    h += static_cast<uint64_t>(len);

    // 最后的处理
    while (p + 8 <= reinterpret_cast<const uint8_t*>(data) + len) {
        uint64_t k1 = *reinterpret_cast<const uint64_t*>(p);
        k1 *= PRIME64_2;
        k1 = rotl(k1, 31);
        k1 *= PRIME64_1;
        h ^= k1;
        h = rotl(h, 27) * PRIME64_1 + PRIME64_4;
        p += 8;
    }

    if (p + 4 <= reinterpret_cast<const uint8_t*>(data) + len) {
        h ^= static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(p)) * PRIME64_1;
        h = rotl(h, 23) * PRIME64_2 + PRIME64_3;
        p += 4;
    }

    while (p < reinterpret_cast<const uint8_t*>(data) + len) {
        h ^= static_cast<uint64_t>(*p) * PRIME64_5;
        h = rotl(h, 11) * PRIME64_1;
        ++p;
    }

    h ^= h >> 33;
    h *= PRIME64_2;
    h ^= h >> 29;
    h *= PRIME64_3;
    h ^= h >> 32;

    return h;
}

// XXHash64 Stream 实现
XXHash64::Stream::Stream(uint64_t seed) : seed_(seed), total_len_(0), buf_pos_(0) {
    acc_[0] = seed + PRIME64_1 + PRIME64_2;
    acc_[1] = seed + PRIME64_2;
    acc_[2] = seed;
    acc_[3] = seed - PRIME64_1;
}

void XXHash64::Stream::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_len_ += len;

    // 处理已有缓冲区
    if (buf_pos_ > 0) {
        size_t need = 32 - buf_pos_;
        if (len < need) {
            memcpy(buf_ + buf_pos_, p, len);
            buf_pos_ += len;
            return;
        }
        memcpy(buf_ + buf_pos_, p, need);
        process_batch();
        p += need;
        len -= need;
    }

    // 处理完整的 32 字节块
    while (len >= 32) {
        const uint64_t* p64 = reinterpret_cast<const uint64_t*>(p);
        acc_[0] += p64[0] * PRIME64_2;
        acc_[0] = rotl(acc_[0], 31);
        acc_[0] *= PRIME64_1;
        acc_[1] += p64[1] * PRIME64_2;
        acc_[1] = rotl(acc_[1], 31);
        acc_[1] *= PRIME64_1;
        acc_[2] += p64[2] * PRIME64_2;
        acc_[2] = rotl(acc_[2], 31);
        acc_[2] *= PRIME64_1;
        acc_[3] += p64[3] * PRIME64_2;
        acc_[3] = rotl(acc_[3], 31);
        acc_[3] *= PRIME64_1;
        p += 32;
        len -= 32;
    }

    // 保存剩余数据
    if (len > 0) {
        memcpy(buf_, p, len);
        buf_pos_ = len;
    }
}

void XXHash64::Stream::process_batch() {
    uint64_t* p64 = reinterpret_cast<uint64_t*>(buf_);
    acc_[0] += p64[0] * PRIME64_2;
    acc_[0] = rotl(acc_[0], 31);
    acc_[0] *= PRIME64_1;
    acc_[1] += p64[1] * PRIME64_2;
    acc_[1] = rotl(acc_[1], 31);
    acc_[1] *= PRIME64_1;
    acc_[2] += p64[2] * PRIME64_2;
    acc_[2] = rotl(acc_[2], 31);
    acc_[2] *= PRIME64_1;
    acc_[3] += p64[3] * PRIME64_2;
    acc_[3] = rotl(acc_[3], 31);
    acc_[3] *= PRIME64_1;
    buf_pos_ = 0;
}

uint64_t XXHash64::Stream::digest() const {
    uint64_t h;

    if (total_len_ >= 32) {
        h = rotl(acc_[0], 1) + rotl(acc_[1], 7) + rotl(acc_[2], 12) + rotl(acc_[3], 18);
        h = mix64(h, acc_[0], PRIME64_2);
        h = mix64(h, acc_[1], PRIME64_2);
        h = mix64(h, acc_[2], PRIME64_2);
        h = mix64(h, acc_[3], PRIME64_2);
    } else {
        h = seed_ + PRIME64_5;
    }

    h += static_cast<uint64_t>(total_len_);

    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf_);
    // 处理剩余的 8 字节块
    while (buf_pos_ >= 8) {
        uint64_t k1 = *reinterpret_cast<const uint64_t*>(p);
        k1 *= PRIME64_2;
        k1 = rotl(k1, 31);
        k1 *= PRIME64_1;
        h ^= k1;
        h = rotl(h, 27) * PRIME64_1 + PRIME64_4;
        p += 8;
        uint32_t temp = -8;
        buf_pos_ -= temp;
    }

    // 处理剩余字节
    if (buf_pos_ > 0) {
        h ^= static_cast<uint64_t>(*p) * PRIME64_5;
        h = rotl(h, 11) * PRIME64_1;
    }

    h ^= h >> 33;
    h *= PRIME64_2;
    h ^= h >> 29;
    h *= PRIME64_3;
    h ^= h >> 32;

    return h;
}

// ============================================================================
// SHA-256 包装器实现
// ============================================================================

void sha256_init(SHA256Context* ctx) {
    SHA256_Init(&ctx->ctx);
}

void sha256_update(SHA256Context* ctx, const void* data, size_t len) {
    SHA256_Update(&ctx->ctx, data, len);
}

void sha256_final(SHA256Context* ctx) {
    SHA256_Final(ctx->hash, &ctx->ctx);
}

// ============================================================================
// 文件读取器实现
// ============================================================================

std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &str[0], size_needed, nullptr, nullptr);
    return str;
}

FileReader::FileReader(const std::wstring& path)
    : file_(INVALID_HANDLE_VALUE), mapping_(INVALID_HANDLE_VALUE), mapped_data_(nullptr), size_(0), use_mmap_(false) {

    std::string path_utf8 = wstring_to_utf8(path);

    file_ = CreateFileA(path_utf8.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (file_ == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER li;
    GetFileSizeEx(file_, &li);
    size_ = li.QuadPart;

    // 大文件使用内存映射
    if (size_ >= config::MMAP_THRESHOLD) {
        mapping_ = CreateFileMapping(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ != INVALID_HANDLE_VALUE) {
            mapped_data_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
            if (mapped_data_) {
                use_mmap_ = true;
            }
        }
    }
}

FileReader::~FileReader() {
    if (mapped_data_) UnmapViewOfFile(mapped_data_);
    if (mapping_ != INVALID_HANDLE_VALUE) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

size_t FileReader::read(void* buffer, size_t offset, size_t size) const {
    if (!is_open() || offset >= size_) return 0;

    // 加上括号是为了阻止宏替换(也可以禁用Visual C++中的 min/max宏定义)
    size = (std::min)(size, static_cast<size_t>(size_ - offset));

    if (use_mmap_ && mapped_data_) {
        memcpy(buffer, static_cast<char*>(mapped_data_) + offset, size);
        return size;
    }

    OVERLAPPED ol = {};
    ol.Offset = static_cast<DWORD>(offset);
    ol.OffsetHigh = static_cast<DWORD>(offset >> 32);

    DWORD bytes_read = 0;
    if (ReadFile(file_, buffer, static_cast<DWORD>(size), &bytes_read, &ol)) {
        return bytes_read;
    }
    return 0;
}

size_t FileReader::read_head(void* buffer, size_t size) const {
    return read(buffer, 0, size);
}

std::vector<std::pair<size_t, size_t>> FileReader::get_sample_ranges(size_t block_size, size_t count) const {
    std::vector<std::pair<size_t, size_t>> ranges;

    if (size_ == 0 || block_size == 0 || count == 0) return ranges;

    // 采样点：开头、25%、50%、75%、结尾
    size_t total_sample_size = block_size * count;

    if (total_sample_size >= size_) {
        // 文件太小，只采样开头
        // 加上括号是为了阻止宏替换(也可以禁用Visual C++中的 min/max宏定义)
        ranges.emplace_back(0, (std::min)(block_size, static_cast<size_t>(size_)));
        return ranges;
    }

    // 计算采样间隔
    double step = static_cast<double>(size_ - total_sample_size) / (count - 1);

    for (size_t i = 0; i < count; ++i) {
        size_t offset = static_cast<size_t>(i * step);
        // 加上括号是为了阻止宏替换(也可以禁用Visual C++中的 min/max宏定义)
        offset = (std::min)(offset, size_ - block_size);
        ranges.emplace_back(offset, block_size);
    }

    return ranges;
}

// ============================================================================
// 线程池实现
// ============================================================================

ThreadPool::ThreadPool(size_t num_threads)
    : stop_(false), active_tasks_(0) {

    if (num_threads == 0) {
        // 加上括号是为了阻止宏替换(也可以禁用Visual C++中的 min/max宏定义)
        num_threads = (std::max)(1u, std::thread::hardware_concurrency() - 1);
    }

    threads_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        threads_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return stop_.load() || !tasks_.empty(); });
                    if (stop_.load() && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                    active_tasks_++;
                }
                task();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    active_tasks_--;
                }
                cv_.notify_all();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    stop_ = true;
    cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

template<typename Func>
void ThreadPool::enqueue(Func&& task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.emplace(std::forward<Func>(task));
    }
    cv_.notify_one();
}

void ThreadPool::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return tasks_.empty() && active_tasks_ == 0; });
}

// ============================================================================
// 命令行解析
// ============================================================================

CommandLineArgs parse_arguments(int argc, wchar_t** argv) {
    CommandLineArgs args;

    if (argc < 2) {
        return args;
    }

    args.directory = argv[1];

    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];

        if (arg == L"-h" || arg == L"--help") {
            args.directory = L"--help";
            break;
        }
        else if (arg.rfind(L"--ext=", 0) == 0) {
            std::wstring list = arg.substr(6);
            std::wstring token;
            std::wstringstream ss(list);
            while (std::getline(ss, token, L',')) {
                if (!token.empty()) {
                    if (token[0] != L'.') token = L"." + token;
                    args.extensions.push_back(token);
                }
            }
        }
        else if (arg.rfind(L"--min-size=", 0) == 0) {
            args.min_size = std::stoull(arg.substr(11)) * 1024;
        }
        else if (arg.rfind(L"--max-size=", 0) == 0) {
            args.max_size = std::stoull(arg.substr(11)) * 1024;
        }
        else if (arg == L"--quick") {
            args.quick_mode = true;
        }
        else if (arg == L"--show-all") {
            args.show_all = true;
        }
        else if (arg.rfind(L"--output=", 0) == 0) {
            args.output_file = arg.substr(9);
            args.output_json = true;
        }
        else if (arg == L"--delete") {
            args.delete_files = true;
        }
        else if (arg.rfind(L"--keep=", 0) == 0) {
            args.keep_method = arg.substr(7);
        }
        else if (arg == L"--preview") {
            args.preview_only = true;
        }
        else if (arg == L"--interactive") {
            args.interactive = true;
        }
        else if (arg.rfind(L"--threads=", 0) == 0) {
            args.thread_count = std::stoi(arg.substr(10));
        }
        else if (arg == L"--cache") {
            args.use_cache = true;
        }
        else if (arg == L"--verbose") {
            args.verbose = true;
        }
    }

    return args;
}

// ============================================================================
// DuplicateScanner 实现
// ============================================================================

DuplicateScanner::DuplicateScanner(const CommandLineArgs& args)
    : args_(args) {}

bool DuplicateScanner::matches_extension(const std::wstring& path) const {
    if (args_.extensions.empty()) return true;

    std::wstring ext = fs::path(path).extension().wstring();
    for (const auto& e : args_.extensions) {
        if (ext == e) return true;
    }
    return false;
}

void DuplicateScanner::phase1_scan_directory() {
    std::vector<std::wstring> dirs_to_scan = { args_.directory };

    while (!dirs_to_scan.empty()) {
        std::wstring current_dir = dirs_to_scan.back();
        dirs_to_scan.pop_back();

        try {
            for (const auto& entry : fs::recursive_directory_iterator(current_dir)) {
                if (!entry.is_regular_file()) continue;

                std::wstring path = entry.path().wstring();
                std::wstring filename = entry.path().filename().wstring();

                // 跳过隐藏文件
                if (!filename.empty() && filename[0] == L'.') continue;

                // 扩展名过滤
                if (!matches_extension(path)) continue;

                try {
                    uint64_t size = entry.file_size();
                    auto ftime = entry.last_write_time();
                    auto ftime_epoch = ftime.time_since_epoch();
                    uint64_t mtime = std::chrono::duration_cast<std::chrono::seconds>(ftime_epoch).count();

                    // 大小过滤
                    if (size < args_.min_size) continue;
                    if (args_.max_size > 0 && size > args_.max_size) continue;

                    uint32_t path_idx = path_pool_.add(path);
                    files_.emplace_back(size, mtime, path_idx);

                    result_.total_files++;
                }
                catch (...) {
                    // 跳过无法访问的文件
                }
            }
        }
        catch (...) {
            // 跳过无法访问的目录
        }
    }
}

void DuplicateScanner::phase2_group_by_size() {
    for (uint32_t i = 0; i < static_cast<uint32_t>(files_.size()); ++i) {
        const auto& file = files_[i];
        size_groups_[file.size].push_back(i);
    }

    // 移除只有一个文件的组（不可能重复）
    for (auto it = size_groups_.begin(); it != size_groups_.end();) {
        if (it->second.size() < 2) {
            it = size_groups_.erase(it);
        } else {
            ++it;
        }
    }
}

// 这个函数暂时保留，不再使用
// 实际哈希计算逻辑在 phase3_compare_hash 中直接处理

uint32_t DuplicateScanner::get_keep_index(const DuplicateGroup& group) const {
    if (group.file_indices.empty()) return 0;

    const auto& method = args_.keep_method;
    uint32_t best_idx = group.file_indices[0];
    const FileEntry& best_file = files_[best_idx];

    for (size_t i = 1; i < group.file_indices.size(); ++i) {
        const FileEntry& current = files_[group.file_indices[i]];

        if (method == L"oldest") {
            if (current.mtime < best_file.mtime) best_idx = group.file_indices[i];
        }
        else if (method == L"newest") {
            if (current.mtime > best_file.mtime) best_idx = group.file_indices[i];
        }
        else if (method == L"shortest_path") {
            const std::wstring& best_path = path_pool_.get(best_file.path_index);
            const std::wstring& current_path = path_pool_.get(current.path_index);
            if (current_path.length() < best_path.length()) best_idx = group.file_indices[i];
        }
        else if (method == L"alphabetical") {
            const std::wstring& best_name = path_pool_.get(best_file.path_index);
            const std::wstring& current_name = path_pool_.get(current.path_index);
            if (current_name < best_name) best_idx = group.file_indices[i];
        }
    }

    return best_idx;
}

void DuplicateScanner::phase3_compare_hash() {
    //                                                                  加上括号是为了阻止宏替换(也可以禁用Visual C++中的 min/max宏定义)
    size_t num_threads = args_.thread_count > 0 ? args_.thread_count : (std::max)(1u, std::thread::hardware_concurrency() - 1);
    ThreadPool pool(num_threads);

    // 对大小组排序，大组优先（更好的并行效率）
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> sorted_groups;
    sorted_groups.reserve(size_groups_.size());
    for (auto& pair : size_groups_) {
        if (pair.second.size() > 1) {
            sorted_groups.emplace_back(pair.first, std::move(pair.second));
        }
    }
    std::sort(sorted_groups.begin(), sorted_groups.end(),
        [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });

    // 并行处理每个大小组
    for (auto& group_data : sorted_groups) {
        pool.enqueue([this, &group_data]() {
            uint64_t file_size = group_data.first;
            auto& indices = group_data.second;

            // 计算每个文件的哈希
            std::unordered_map<uint64_t, std::vector<uint32_t>> head_hash_groups;

            // 第一轮：用头部哈希分组
            for (uint32_t idx : indices) {
                const auto& file = files_[idx];
                const std::wstring& path = path_pool_.get(file.path_index);

                FileReader reader(path);
                if (!reader.is_open()) continue;

                // 读取头部计算 xxHash64
                char buffer[config::HEAD_HASH_SIZE];
                size_t read = reader.read_head(buffer, config::HEAD_HASH_SIZE);
                uint64_t xxh = 0;
                if (read > 0) {
                    xxh = XXHash64::hash(buffer, read, 0);
                }
                head_hash_groups[xxh].push_back(idx);
            }

            // 对每个头部哈希组进一步处理
            std::vector<DuplicateGroup> local_groups;

            for (auto& hg : head_hash_groups) {
                if (hg.second.size() < 2) continue;

                // 如果是快速模式或文件数<=2，直接标记为重复
                if (args_.quick_mode || hg.second.size() <= 2) {
                    DuplicateGroup group;
                    group.file_size = file_size;
                    group.file_indices = std::move(hg.second);
                    local_groups.push_back(std::move(group));
                    continue;
                }

                // 非快速模式且超过2个文件：采样哈希进一步筛选
                if (file_size >= config::SAMPLE_THRESHOLD) {
                    std::unordered_map<uint64_t, std::vector<uint32_t>> sample_hash_groups;

                    for (uint32_t idx : hg.second) {
                        const auto& file = files_[idx];
                        const std::wstring& path = path_pool_.get(file.path_index);

                        FileReader reader(path);
                        if (!reader.is_open()) continue;

                        auto ranges = reader.get_sample_ranges(args_.sample_size, config::SAMPLE_COUNT);
                        XXHash64::Stream stream(0);
                        std::vector<char> sample_buf(args_.sample_size);

                        for (const auto& range : ranges) {
                            size_t r = reader.read(sample_buf.data(), range.first, range.second);
                            if (r > 0) stream.update(sample_buf.data(), r);
                        }

                        uint64_t sample_hash = stream.digest();
                        sample_hash_groups[sample_hash].push_back(idx);
                    }

                    // 收集采样哈希相同的
                    for (auto& shg : sample_hash_groups) {
                        if (shg.second.size() >= 2) {
                            DuplicateGroup group;
                            group.file_size = file_size;
                            group.file_indices = std::move(shg.second);
                            local_groups.push_back(std::move(group));
                        }
                    }
                } else {
                    // 小文件（< 100MB），直接用头部哈希分组
                    DuplicateGroup group;
                    group.file_size = file_size;
                    group.file_indices = std::move(hg.second);
                    local_groups.push_back(std::move(group));
                }
            }

            // 添加到结果
            if (!local_groups.empty()) {
                std::lock_guard<std::mutex> lock(result_mutex_);
                for (auto& g : local_groups) {
                    result_.duplicate_files += g.file_indices.size() - 1;
                    result_.potential_savings += g.file_size * (g.file_indices.size() - 1);
                    result_.groups.push_back(std::move(g));
                }
            }
        });
    }

    pool.wait();
    result_.duplicate_groups = static_cast<uint64_t>(result_.groups.size());
}

ScanResult DuplicateScanner::scan() {
    auto start_time = std::chrono::high_resolution_clock::now();

    // 阶段1: 遍历目录
    phase1_scan_directory();

    // 阶段2: 按大小分组
    phase2_group_by_size();

    // 阶段3: 并行哈希比较
    phase3_compare_hash();

    auto end_time = std::chrono::high_resolution_clock::now();
    result_.scan_duration = std::chrono::duration<double>(end_time - start_time).count();

    return result_;
}

void DuplicateScanner::export_json(const std::wstring& path) const {
    std::string output_path = to_utf8(path);
    std::ofstream ofs(output_path);

    ofs << "{\n";
    ofs << "  \"total_files\": " << result_.total_files << ",\n";
    ofs << "  \"duplicate_groups\": " << result_.duplicate_groups << ",\n";
    ofs << "  \"duplicate_files\": " << result_.duplicate_files << ",\n";
    ofs << "  \"potential_savings\": " << result_.potential_savings << ",\n";
    ofs << "  \"scan_duration\": " << std::fixed << std::setprecision(2) << result_.scan_duration << ",\n";
    ofs << "  \"duplicates\": [\n";

    for (size_t i = 0; i < result_.groups.size(); ++i) {
        const auto& group = result_.groups[i];
        ofs << "    {\n";
        ofs << "      \"size\": " << group.file_size << ",\n";
        ofs << "      \"files\": [\n";

        for (size_t j = 0; j < group.file_indices.size(); ++j) {
            const auto& file = files_[group.file_indices[j]];
            ofs << "        {\"path\": \"" << to_utf8(path_pool_.get(file.path_index)) << "\", \"status\": \""
                << (j == 0 ? "keep" : "delete") << "\"}";
            if (j < group.file_indices.size() - 1) ofs << ",";
            ofs << "\n";
        }

        ofs << "      ]\n";
        ofs << "    }";
        if (i < result_.groups.size() - 1) ofs << ",";
        ofs << "\n";
    }

    ofs << "  ]\n";
    ofs << "}\n";
    ofs.close();

    wprint(L"结果已导出到: " + path + L"\n");
}

void DuplicateScanner::delete_duplicates(const CommandLineArgs& args, ScanResult& result) {
    if (!args.delete_files) return;

    for (auto& group : result.groups) {
        uint32_t keep_idx = get_keep_index(group);

        for (uint32_t idx : group.file_indices) {
            if (idx == keep_idx) continue;

            const std::wstring& path = path_pool_.get(files_[idx].path_index);

            if (args.interactive) {
                wprint(L"Delete " + path + L"? (y/N): ");
                std::wstring response;
                std::getline(std::wcin, response);
                if (response != L"y" && response != L"Y") continue;
            }

            if (DeleteFileW(path.c_str())) {
                wprint(L"Deleted: " + path + L"\n");
            } else {
                wprint(L"Failed to delete: " + path + L"\n");
            }
        }
    }
}

// ============================================================================
// 输出函数
// ============================================================================

void print_banner() {
    wprint(L"                    DupliScan v3.0\n");
    wprint(L"           Fast Duplicate File Finder\n\n");
    std::cout.flush();
}

void print_help() {
    wprint(L"Usage: DupliScan <directory> [options]\n\n");
    wprint(L"Options:\n");
    wprint(L"  --ext=.jpg,.png       Filter by file extensions\n");
    wprint(L"  --min-size=<KB>      Minimum file size\n");
    wprint(L"  --max-size=<KB>      Maximum file size\n");
    wprint(L"  --quick              Quick mode (size + header hash only)\n");
    wprint(L"  --show-all           Show all duplicate groups\n");
    wprint(L"  --output=<file>      Export results to JSON\n");
    wprint(L"  --delete             Delete duplicate files\n");
    wprint(L"  --keep=<method>      Keep: oldest, newest, shortest_path, alphabetical\n");
    wprint(L"  --preview            Preview only, don't delete\n");
    wprint(L"  --interactive        Confirm each deletion\n");
    wprint(L"  --threads=<N>        Number of threads (0=auto)\n");
    wprint(L"  --cache              Enable incremental scanning\n");
    wprint(L"  --verbose            Verbose output\n");
    wprint(L"  --help               Show this help\n");
}

// 静态函数用于传递文件列表
void print_result(const ScanResult& result, const std::vector<FileEntry>& files, const StringPool& pool, const CommandLineArgs& args) {
    std::wcout << L"\n═══════════════════════════════════════════════════════════════\n\n";

    std::wcout << L"扫描结果:\n";
    std::wcout << L"  文件总数: " << result.total_files << L"\n";
    std::wcout << L"  重复组数: " << result.duplicate_groups << L"\n";
    std::wcout << L"  重复文件: " << result.duplicate_files << L"\n";
    std::wcout << L"  可释放空间: " << format_size(result.potential_savings) << L"\n";
    std::wcout << L"  扫描耗时: " << std::fixed << std::setprecision(2) << result.scan_duration << L" 秒\n\n";

    if (result.groups.empty()) {
        std::wcout << L"未找到重复文件！\n";
        return;
    }
    //                                                            加上括号是为了阻止宏替换(也可以禁用Visual C++中的 min/max宏定义)
    size_t display_count = args.show_all ? result.groups.size() : (std::min)(result.groups.size(), size_t(5));

    for (size_t i = 0; i < display_count; ++i) {
        const auto& group = result.groups[i];
        std::wcout << L"[" << (i + 1) << L"] 重复组 (" << format_size(group.file_size) << L")\n";

        for (size_t j = 0; j < group.file_indices.size(); ++j) {
            const auto& file = files[group.file_indices[j]];
            const std::wstring& path = pool.get(file.path_index);
            std::wcout << L"    " << (j == 0 ? L"[保留] " : L"[删除] ") << path << L"\n";
        }
        std::wcout << L"\n";
    }

    if (result.groups.size() > display_count) {
        std::wcout << L"... 还有 " << (result.groups.size() - display_count) << L" 组重复文件未显示\n";
        std::wcout << L"使用 --show-all 查看全部\n\n";
    }
    std::wcout.flush();
}

// ============================================================================
// 主函数
// ============================================================================

int wmain(int argc, wchar_t** argv) {
    // 设置控制台为 UTF-8
     SetConsoleOutputCP(CP_UTF8);

     // 设置 stdout 为 UTF-16 模式
     _setmode(_fileno(stdout), _O_U16TEXT);

     CommandLineArgs args = parse_arguments(argc, argv);

    if (args.directory.empty() || args.directory == L"--help") {
        print_banner();
        print_help();
        std::wcout.flush();
        return 0;
    }

    if (!fs::exists(args.directory) || !fs::is_directory(args.directory)) {
        std::wcerr << L"错误: 目录不存在或不是有效目录: " << args.directory << L"\n";
        return 1;
    }

    print_banner();

    DuplicateScanner scanner(args);
    ScanResult result = scanner.scan();

    // 输出结果
    print_result(result, scanner.get_files(), scanner.get_path_pool(), args);

    // 导出 JSON
    if (!args.output_file.empty()) {
        scanner.export_json(args.output_file);
    }

    // 删除文件
    if (args.delete_files && !args.preview_only) {
        if (args.interactive || args.preview_only) {
            std::wcout << L"\n预览模式: 不会实际删除文件\n";
        } else {
            std::wcout << L"\n确认删除?(yes 确认): ";
            std::wstring confirm;
            std::getline(std::wcin, confirm);
            if (confirm == L"yes") {
                scanner.delete_duplicates(args, result);
            }
        }
    }

    return 0;
}
