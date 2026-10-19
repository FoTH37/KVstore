#include "persist.h"
#include "BusinessHandler.h"
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <ctime>
#include <cstring>
#include <liburing.h>
#include "log.h"

// ==================== CRC32 工具函数 ====================
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;
static void initCrc32Table() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}
static uint32_t calculateCrc32(const std::string& data) {
    if (!crc32_table_initialized) initCrc32Table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < data.size(); i++) {
        uint8_t index = (crc ^ data[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32_table[index];
    }
    return crc ^ 0xFFFFFFFF;
}

Persist::Persist(const std::string& filename, Engine* engine, PersistMode mode)
    : filename_(filename), engine_(engine), mode_(mode) {
    rdb_filename_ = filename_;
    size_t pos = rdb_filename_.rfind('.');
    if (pos != std::string::npos) {
        rdb_filename_ = rdb_filename_.substr(0, pos) + ".rdb";
    } else {
        rdb_filename_ += ".rdb";
    }

    if (mode_ == PERSIST_MODE_AOF || mode_ == PERSIST_MODE_BOTH) {
        fd_ = open(filename_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    } else {
        fd_ = -1;
    }

    stop_ = false;
    last_rdb_time_ = time(nullptr);

    if (mode_ == PERSIST_MODE_AOF || mode_ == PERSIST_MODE_BOTH) {
        flush_thread_ = std::thread(&Persist::flushThread, this);
    }
}

Persist::~Persist() {
    if (rdb_dirty_ && (mode_ == PERSIST_MODE_RDB || mode_ == PERSIST_MODE_BOTH)) {
        saveSnapshot();
    }

    if (mode_ == PERSIST_MODE_AOF || mode_ == PERSIST_MODE_BOTH) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
    }

    if (fd_ != -1) {
        close(fd_);
    }
}

void Persist::saveSnapshot() {
    // ===== 新增：持久化模式独立保护 =====
    // 只有 RDB 或 BOTH 模式才允许生成 RDB 快照
    // aof 模式下调用 SAVE 不会生成 rdb 文件（哪怕 0 字节）
    if (mode_ != PERSIST_MODE_RDB && mode_ != PERSIST_MODE_BOTH) {
        LOG_WARN("Persist: saveSnapshot ignored, persist_mode is not RDB/BOTH");
        return;
    }
    if (fd_ != -1) {
        fsync(fd_);
    }
    if (!engine_) {
        std::cerr << "Persist: no engine for RDB snapshot" << std::endl;
        return;
    }
    std::string tmp_file = rdb_filename_ + ".tmp";
    // ===== 快照写出优化 v3（250 万 key 实测 SAVE 12.4s → 目标 ~3s）=====
    // v2 用 ofstream(1MB 缓冲) + 每 key 一个 std::string rec（reserve + 6 次 += + to_string×2），
    // 250 万次 string 分配/释放 + 500 万次 to_string 格式化 + 250 万次 ofstream.write 调用，
    // 序列化开销 ~5μs/key 成为大头。v3 改为裸 fd + thread_local 1MB 手写缓冲：
    //   - 零 per-key 分配（数字手写转十进制，memcpy 直写缓冲）
    //   - 每 1MB 一次 write() 系统调用
    int fd = open(tmp_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::cerr << "Persist: failed to open temp file " << tmp_file << std::endl;
        return;
    }
    // 注意：必须 thread_local——saveSnapshot 可能并发执行（flushThread 自动快照 +
    // SAVE 命令/全量同步），static 共享缓冲会导致数据竞争、内存踩踏（曾致崩溃）
    static thread_local char snapshot_buf[1024 * 1024];
    size_t blen = 0;
    auto flush_buf = [&]() {
        size_t off = 0;
        while (off < blen) {
            ssize_t n = write(fd, snapshot_buf + off, blen - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            off += (size_t)n;
        }
        blen = 0;
    };
    auto put = [&](const char* p, size_t n) {
        if (blen + n > sizeof(snapshot_buf)) flush_buf();
        memcpy(snapshot_buf + blen, p, n);
        blen += n;
    };
    auto put_u64 = [&](size_t v) {
        char tmp[24];
        int n = 0;
        if (v == 0) {
            tmp[n++] = '0';
        } else {
            char rev[24];
            int rn = 0;
            while (v) { rev[rn++] = (char)('0' + v % 10); v /= 10; }
            while (rn) tmp[n++] = rev[--rn];
        }
        put(tmp, (size_t)n);
    };

    int count = 0;
    engine_->scan([&](const std::string& key, const std::string& value) {
        // RDB 只保存 key/value，不存指令（SET）
        // 格式：key长度\nkey\nvalue长度\nvalue\n
        put_u64(key.size());   put("\n", 1);
        put(key.data(), key.size()); put("\n", 1);
        put_u64(value.size()); put("\n", 1);
        put(value.data(), value.size()); put("\n", 1);
        count++;
        return true;
    });
    flush_buf();
    close(fd);
    if (std::rename(tmp_file.c_str(), rdb_filename_.c_str()) != 0) {
        std::cerr << "Persist: rename failed for RDB snapshot" << std::endl;
        std::remove(tmp_file.c_str());
        return;
    }
}

void Persist::logCommand(const std::string& cmd) {
    if (mode_ == PERSIST_MODE_RDB) return;

    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(cmd);
    rdb_dirty_ = true;
    cv_.notify_one();
}

void Persist::setRdbInterval(int seconds) {
    rdb_interval_ = seconds;
    if (seconds > 0) {
        last_rdb_time_ = time(nullptr);
    }
}

void Persist::flushThread() {
    struct io_uring ring;
    int ret = io_uring_queue_init(32, &ring, 0);
    if (ret != 0) {
        LOG_ERROR("Persist flushThread: io_uring_queue_init failed");
        return;
    }

    std::vector<std::string> batch;
    char *write_buf = nullptr;
    size_t write_len = 0;

    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return !queue_.empty() || stop_;
            });

            if (stop_ && queue_.empty()) break;

            batch.assign(queue_.begin(), queue_.end());
            queue_.clear();
        }

        if (fd_ != -1 && !batch.empty()) {
            // 拼接数据
            std::string data;
            for (const auto& line : batch) {
                data += line;
            }
            write_len = data.size();

            // 用 io_uring 异步写入
            struct io_uring_sqe *sqe;
            struct io_uring_cqe *cqe;

            // 分配缓冲区（io_uring 要求写入期间缓冲区有效）
            write_buf = (char *)malloc(write_len);
            memcpy(write_buf, data.data(), write_len);

            // 提交 write 请求
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_write(sqe, fd_, write_buf, write_len, 0);
            io_uring_sqe_set_data(sqe, (void *)(uintptr_t)1);
            io_uring_submit(&ring);

            // 等待 write 完成
            ret = io_uring_wait_cqe(&ring, &cqe);
            if (ret == 0) {
                if (cqe->res < 0) {
                    LOG_ERROR("Persist: io_uring write failed, res=%d", cqe->res);
                }
                io_uring_cqe_seen(&ring, cqe);
            }

            free(write_buf);
            write_buf = nullptr;

            // 提交 fsync 请求
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_fsync(sqe, fd_, IORING_FSYNC_DATASYNC);
            io_uring_sqe_set_data(sqe, (void *)(uintptr_t)2);
            io_uring_submit(&ring);

            // 等待 fsync 完成
            ret = io_uring_wait_cqe(&ring, &cqe);
            if (ret == 0) {
                if (cqe->res < 0) {
                    LOG_ERROR("Persist: io_uring fsync failed, res=%d", cqe->res);
                }
                io_uring_cqe_seen(&ring, cqe);
            }

            // CRC32
            uint32_t crc = calculateCrc32(data);
            std::string crcLine = "#crc32=" + std::to_string(crc) + "\r\n";

            // 同步写 CRC（小数据，同步即可）
            ssize_t unused = write(fd_, crcLine.c_str(), crcLine.size());
            (void)unused;
            fsync(fd_);

            // AOF 轮转检查
            off_t cur = lseek(fd_, 0, SEEK_CUR);
            if (cur > (off_t)MAX_FILE_SIZE) {
                close(fd_);
                rotate(true);
                fd_ = open(filename_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            }
        }

        batch.clear();

        if ((mode_ == PERSIST_MODE_RDB || mode_ == PERSIST_MODE_BOTH) &&
            rdb_interval_ > 0 && rdb_dirty_) {
            time_t now = time(nullptr);
            if (now - last_rdb_time_ >= rdb_interval_) {
                saveSnapshot();
                last_rdb_time_ = now;
                rdb_dirty_ = false;
            }
        }
    }

    io_uring_queue_exit(&ring);
}

void Persist::loadSnapshot() {
    int fd = open(rdb_filename_.c_str(), O_RDONLY);
    if (fd == -1) return;

    struct stat st;
    fstat(fd, &st);
    size_t fileSize = st.st_size;
    if (fileSize == 0) { close(fd); return; }

    char *data = (char*)mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (data == MAP_FAILED) return;

    int count = 0;
    size_t offset = 0;
    while (offset < fileSize) {
        // 读 key 长度（到 \n）；容错：长度行非纯数字视为文件损坏，停止加载
        size_t pos = offset;
        while (pos < fileSize && data[pos] != '\n') pos++;
        if (pos >= fileSize) break;
        size_t keyLen = 0;
        {
            std::string lenStr(data + offset, pos - offset);
            if (lenStr.empty() ||
                lenStr.find_first_not_of("0123456789") != std::string::npos) break;
            keyLen = std::stoul(lenStr);
        }
        offset = pos + 1;  // 跳过 \n

        // 读 key
        if (offset + keyLen > fileSize) break;
        std::string key(data + offset, keyLen);
        offset += keyLen;

        // 跳过 key 后的 \n
        if (offset >= fileSize || data[offset] != '\n') break;
        offset++;

        // 读 value 长度（到 \n）
        pos = offset;
        while (pos < fileSize && data[pos] != '\n') pos++;
        if (pos >= fileSize) break;
        size_t valLen = 0;
        {
            std::string lenStr(data + offset, pos - offset);
            if (lenStr.empty() ||
                lenStr.find_first_not_of("0123456789") != std::string::npos) break;
            valLen = std::stoul(lenStr);
        }
        offset = pos + 1;  // 跳过 \n

        // 读 value
        if (offset + valLen > fileSize) break;
        std::string value(data + offset, valLen);
        offset += valLen;

        // 跳过 value 后的 \n
        if (offset >= fileSize || data[offset] != '\n') break;
        offset++;

        engine_->set(key, value);
        count++;
    }

    munmap(data, fileSize);
}

void Persist::recover() {
    if (mode_ == PERSIST_MODE_RDB || mode_ == PERSIST_MODE_BOTH) {
        loadSnapshot();
    }

    if (mode_ == PERSIST_MODE_AOF || mode_ == PERSIST_MODE_BOTH) {
        int fd = open(filename_.c_str(), O_RDONLY);
        if (fd == -1) return;

        struct stat st;
        fstat(fd, &st);
        size_t fileSize = st.st_size;
        if (fileSize == 0) { close(fd); return; }

        std::string data;
        data.resize(fileSize);
        ssize_t bytesRead = read(fd, &data[0], fileSize);
        close(fd);
        if (bytesRead <= 0) return;
        data.resize(bytesRead);

        // ===== 方案 B：按 #crc32= 行分段，每段独立校验 =====
        // flushThread 分批写入 AOF，每批末尾带一个 #crc32=<crc>\r\n
        // 这个 CRC 只覆盖该批命令内容，所以恢复时必须逐段校验，不能整文件校验
        std::string validData;       // 通过校验的命令拼接结果
        size_t pos = 0;              // 当前扫描位置
        int segmentCount = 0;        // 校验通过的段数
        int badSegmentCount = 0;     // 校验失败的段数
        bool foundAnyCrc = false;    // 是否找到过 CRC 行（用于区分旧格式）

        while (pos < data.size()) {
            // 找下一个 "\n#crc32=" 的位置
            size_t crcLinePos = data.find("\n#crc32=", pos);
            if (crcLinePos == std::string::npos) break;

            foundAnyCrc = true;
            size_t crcLineStart = crcLinePos + 1;    // 指向 '#'
            size_t crcLineEnd = data.find("\r\n", crcLineStart);
            if (crcLineEnd == std::string::npos) break;

            // 提取段尾 CRC 值
            std::string crcLine = data.substr(crcLineStart, crcLineEnd - crcLineStart);
            uint32_t storedCrc = 0;
            if (crcLine.size() > 7 && crcLine.substr(0, 7) == "#crc32=") {
                // 容错：值必须全数字，否则视为坏段跳过（曾因 kill -9 截断+续写粘连，
                // CRC 行含非数字字符导致 std::stoul 抛异常，整个服务启动崩溃）
                std::string crcVal = crcLine.substr(7);
                bool allDigit = !crcVal.empty() &&
                    crcVal.find_first_not_of("0123456789") == std::string::npos;
                if (allDigit) {
                    storedCrc = (uint32_t)std::stoul(crcVal);
                } else {
                    badSegmentCount++;
                    pos = crcLineEnd + 2;
                    continue;
                }
            } else {
                break;
            }

            // 段体 = [pos, crcLinePos+1)，包含命令末尾的 \r\n（与写入端 CRC 计算一致）
            // 写入端 flushThread 对 data（含末尾 \r\n）算 CRC，恢复端 body 必须一致
            std::string body;
            if (crcLinePos + 1 > pos) {
                body = data.substr(pos, crcLinePos + 1 - pos);
            }

            // 逐段校验 CRC
            uint32_t calcCrc = calculateCrc32(body);
            if (calcCrc == storedCrc) {
                validData += body;
                segmentCount++;
            } else {
                badSegmentCount++;
            }

            pos = crcLineEnd + 2;    // 跳过该 CRC 行的 \r\n
        }

        // 兼容旧格式：整个文件没有 CRC 行时，直接整体解析（不校验）
        if (!foundAnyCrc) {
            validData = data;
        }

        if (foundAnyCrc) {
            LOG_INFO("CRC32 check done, segments=%d (bad=%d), valid data size=%zu",
                     segmentCount, badSegmentCount, validData.size());
        }

        data = validData;
        // ===== 方案 B 结束 =====

        int count = 0;
        size_t offset = 0;
        while (offset < data.size()) {
            while (offset < data.size() && data[offset] != '*') offset++;
            if (offset >= data.size()) break;

            // 性能修复：原来 data.substr(offset) 每条命令复制整个剩余 AOF（O(n²)），
            // 48MB 恢复需数分钟；改为 parseRespCommandAt 游标解析，O(n) 秒级完成
            std::string cmd;
            std::vector<std::string> args;
            size_t endPos = 0;
            if (BusinessHandler::parseRespCommandAt(data, offset, cmd, args, endPos)) {
                if ((cmd == "SET" || cmd == "set") && args.size() >= 2) {
                    engine_->set(args[0], args[1]);
                    count++;
                } else if ((cmd == "DEL" || cmd == "del") && args.size() >= 1) {
                    engine_->del(args[0]);
                    count++;
                }
                offset = endPos;
            } else {
                offset++;
            }
        }
        if (count > 0) {
            LOG_INFO("AOF recovered %d commands", count);
        }
    }
}

void Persist::rotate(bool is_aof) {
    const std::string& base = is_aof ? filename_ : rdb_filename_;

    std::string oldest = base + "." + std::to_string(MAX_BACKUP);
    std::remove(oldest.c_str());

    for (int i = MAX_BACKUP - 1; i >= 1; i--) {
        std::string old_name = base + "." + std::to_string(i);
        std::string new_name = base + "." + std::to_string(i + 1);
        std::rename(old_name.c_str(), new_name.c_str());
    }

    std::rename(base.c_str(), (base + ".1").c_str());
}