// 包含必要的头文件
#include <array>          // 固定大小数组容器
#include <atomic>         // 原子操作，保证线程安全
#include <vector>         // 动态数组容器
#include <thread>         // 线程支持
#include <iostream>       // 输入输出流
#include <chrono>         // 时间相关功能
#include <random>         // 随机数生成
#include <mutex>          // 互斥锁
#include <climits>        // 整数类型限制
#include <string>         // 字符串支持
#include <sstream>        // 字符串流
#include <iomanip>        // 输入输出操作符
#include <ctime>          // C时间函数
#include <windows.h>      // Windows API

// 系统配置常量
const int BUFFER_SIZE = 16;        // 环形缓冲区大小
const int NUM_CONSUMERS = 3;       // 消费者线程数量

// 消费者类型枚举，定义不同的处理速度特征
enum class ConsumerType {
    NORMAL,     // 正常速度
    SLOW,       // 慢速处理
    FAST,       // 快速处理
    UNSTABLE    // 不稳定速度（有时快有时慢）
};

// 为每个消费者分配类型：FAST、SLOW、UNSTABLE
ConsumerType consumer_types[NUM_CONSUMERS] = {ConsumerType::FAST, ConsumerType::SLOW, ConsumerType::UNSTABLE};

// 帧数据结构，包含数据和元信息
struct Frame {
    unsigned char data[1024];                    // 帧数据（1KB）
    int frame_id;                               // 帧ID，用于标识
    std::atomic<int> consumers_processed{0};    // 已处理此帧的消费者数量（原子操作）
};

// 全局变量声明
std::array<Frame, BUFFER_SIZE> buffer;                    // 环形缓冲区，存储帧数据
std::atomic<int> latest_frame_index{0};                  // 生产者维护的最新帧指针（原子操作）
std::atomic<int> out[NUM_CONSUMERS];                     // 每个消费者已处理的帧ID（原子操作）
std::atomic<bool> running{true};                         // 程序运行标志（原子操作）
std::atomic<bool> producer_finished{false};              // 生产者完成标志（原子操作）
std::atomic<int> total_frames_produced{0};               // 总生产帧数（原子操作）
std::atomic<int> frames_consumed_per_consumer[NUM_CONSUMERS]; // 每个消费者处理的帧数（原子操作）
std::mutex cout_mutex;                                   // 控制台输出互斥锁，防止输出混乱

// Windows信号量句柄，用于线程同步
HANDLE filled_slots_0;                                  // 消费者0的信号量
HANDLE filled_slots_1;                                  // 消费者1的信号量
HANDLE filled_slots_2;                                  // 消费者2的信号量
HANDLE filled_slots[NUM_CONSUMERS];                     // 信号量数组，方便遍历

/**
 * 获取当前时间戳字符串
 * @return 格式化的时间戳字符串
 */
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

/**
 * 线程安全的打印函数
 * 使用互斥锁确保多个线程的输出不会混乱
 * @param message 要打印的消息
 */
void safe_print(const std::string& message) {
    std::lock_guard<std::mutex> lock(cout_mutex);  // 自动加锁，函数结束时自动解锁
    std::cout << "[" << get_timestamp() << "] " << message << std::endl;
}

/**
 * 生成随机帧数据
 * 使用Mersenne Twister生成器产生高质量的随机数
 * @param data 指向数据缓冲区的指针
 */
void generate_frame(unsigned char* data) {
    static std::random_device rd;                           // 随机设备，用于种子
    static std::mt19937 gen(rd());                          // Mersenne Twister随机数生成器
    static std::uniform_int_distribution<> dis(0, 255);    // 均匀分布，范围0-255
    
    // 生成1024字节的随机数据
    for (int i = 0; i < 1024; ++i) {
        data[i] = static_cast<unsigned char>(dis(gen));
    }
}

/**
 * 处理帧数据的函数
 * 根据消费者类型模拟不同的处理时间，并计算数据校验和
 * @param data 帧数据指针
 * @param frame_id 帧ID
 * @param consumer_id 消费者ID
 */
void process(const unsigned char* data, int frame_id, int consumer_id) {
    static std::random_device rd;                           // 随机设备
    static std::mt19937 gen(rd());                          // 随机数生成器
    int processing_time;                                    // 处理时间
    std::string consumer_type_name;                         // 消费者类型名称
    
    // 根据消费者类型确定处理时间
    switch (consumer_types[consumer_id]) {
        case ConsumerType::FAST:
            processing_time = std::uniform_int_distribution<>(3, 5)(gen);    // 快速：3-5ms
            consumer_type_name = "FAST";
            break;
        case ConsumerType::SLOW:
            processing_time = std::uniform_int_distribution<>(10, 20)(gen);  // 慢速：10-20ms
            consumer_type_name = "SLOW";
            break;
        case ConsumerType::UNSTABLE:
            // 不稳定：80%概率快速，20%概率慢速
            if (std::uniform_int_distribution<>(1, 100)(gen) <= 80) {
                processing_time = std::uniform_int_distribution<>(3, 5)(gen);
            } else {
                processing_time = std::uniform_int_distribution<>(10, 20)(gen);
            }
            consumer_type_name = "UNSTABLE";
            break;
        default:
            processing_time = std::uniform_int_distribution<>(20, 50)(gen);  // 默认：20-50ms
            consumer_type_name = "NORMAL";
            break;
    }

    // 模拟处理时间
    std::this_thread::sleep_for(std::chrono::milliseconds(processing_time));

    // 计算数据校验和，验证数据完整性
    int checksum = 0;
    for (int i = 0; i < 1024; ++i) {
        checksum += static_cast<int>(data[i]);
    }

    // 更新该消费者处理的帧数统计
    frames_consumed_per_consumer[consumer_id].fetch_add(1);
    
    // 输出处理结果
    if (consumer_id == 2) {
        // 计算当前帧在buffer中的位置
        int buffer_position = frame_id % BUFFER_SIZE;
        safe_print("C " + std::to_string(consumer_id) + " (" + consumer_type_name + 
                ") proce frame " + std::to_string(frame_id) + 
                " buffer[" + std::to_string(buffer_position) + "]" +
                " in " + std::to_string(processing_time) + "ms, checksum: " + std::to_string(checksum));
    }
}

/**
 * 生产者线程函数
 * 不断生成新帧数据，写入环形缓冲区，覆盖旧数据
 */
void producer() {
    int frame_id = 0;  // 帧ID计数器
    safe_print("Producer started");
    
    while (running) {  // 主循环，直到收到停止信号
        if (!running) {
            break;
        }

        // 直接按顺序写入，不等待空槽位，覆盖旧数据
        int idx = frame_id % BUFFER_SIZE;  // 计算缓冲区索引，实现环形覆盖
        
        // 填充数据到缓冲区
        generate_frame(buffer[idx].data);
        buffer[idx].frame_id = frame_id;
        buffer[idx].consumers_processed.store(0);  // 重置处理计数

        // 数据填充完成后，更新最新帧指针（原子操作）
        latest_frame_index.store(idx);
        total_frames_produced.fetch_add(1);  // 增加总生产帧数

        // 输出生产信息
        safe_print("P generate frame " + std::to_string(frame_id) + 
                  " buffer[" + std::to_string(idx) + "]");

        frame_id++;  // 准备下一帧

        // 通知所有消费者有新数据可处理
        // 为每个消费者释放信号量，唤醒等待的消费者
        for (int i = 0; i < NUM_CONSUMERS; ++i) {
            ReleaseSemaphore(filled_slots[i], 1, NULL);
        }

        // 生产延迟，控制生产速度
        int production_delay = 10;  // 10ms
        std::this_thread::sleep_for(std::chrono::milliseconds(production_delay));
    }

    // 生产者完成，设置标志
    producer_finished = true;
    safe_print("Producer finished, total frames produced: " + std::to_string(total_frames_produced.load()));
}

/**
 * 检查指定消费者是否还有未完成的帧
 * @param consumer_id 消费者ID
 * @return true表示还有未完成的帧，false表示所有帧都处理完成
 */
bool has_unfinished_frames(int consumer_id) {
    // 如果生产者还在运行，肯定还有帧
    if (!producer_finished.load()) {
        return true;
    }

    // 检查该消费者是否处理了所有已生产的帧
    int total_produced = total_frames_produced.load();
    int latest_processed_frame_id = out[consumer_id].load();

    if ((latest_processed_frame_id + 1) < total_produced) {
        return true;
    }

    return false;
}

/**
 * 消费者线程函数
 * 不断从缓冲区读取最新帧进行处理
 * @param id 消费者ID
 */
void consumer(int id) {
    // 获取消费者类型名称，用于日志输出
    std::string type_name;
    switch (consumer_types[id]) {
        case ConsumerType::FAST: type_name = "FAST"; break;
        case ConsumerType::SLOW: type_name = "SLOW"; break;
        case ConsumerType::UNSTABLE: type_name = "UNSTABLE"; break;
        default: type_name = "NORMAL"; break;
    }
    
    safe_print("C " + std::to_string(id) + " (" + type_name + ") started");

    // 主循环：运行中或有未完成帧时继续
    while (running || has_unfinished_frames(id)) {
        auto start_time = std::chrono::steady_clock::now();  // 记录等待开始时间

        // 等待工作信号，超时时间10ms
        DWORD wait_result = WaitForSingleObject(filled_slots[id], 10);
        if (wait_result != WAIT_OBJECT_0) {  // 等待失败或超时
            if (wait_result == WAIT_TIMEOUT) {  // 超时情况
                // 如果程序停止且生产者完成且没有未完成帧，则退出
                if (!running && producer_finished && !has_unfinished_frames(id)) {
                    break;
                }
                continue;  // 继续等待
            } else {  // 其他错误
                safe_print("C " + std::to_string(id) + " failed to acquire work semaphore");
                break;
            }
        }
        
        // 计算等待时间，用于性能监控
        auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        
        // 如果等待时间过长，输出警告信息
        if (wait_time > 100) {
            safe_print("C " + std::to_string(id) + " waited " + std::to_string(wait_time) + "ms for work");
        }

        // 直接处理最新帧，跳过未处理的帧
        int latest_idx = latest_frame_index.load();  // 获取最新帧的缓冲区索引
        int latest_frame_id = buffer[latest_idx].frame_id;  // 获取最新帧的ID
        
        // 检查这个帧是否已经被处理过
        if (latest_frame_id > out[id].load()) {
            // 处理新帧
            if (id == 2) {
                if (latest_frame_id - out[id].load() > 1) {
                    safe_print("C " + std::to_string(id) + " skip " + std::to_string(latest_frame_id - out[id].load() - 1) + " frames");
                }
            }
            process(buffer[latest_idx].data, latest_frame_id, id);
            out[id].store(latest_frame_id);  // 更新已处理帧ID
        } else {
            if (id == 2) {
                safe_print("C " + std::to_string(id) + " " +
                        std::to_string(latest_frame_id) + " (already processed)" + ", out[" + std::to_string(id) + "]=" + std::to_string(out[id].load()));
            }
        }
    }
    
    safe_print("C " + std::to_string(id) + " (" + type_name + ") stopped");
}

/**
 * 主函数
 * 初始化系统，创建线程，等待完成，清理资源
 */
int main() {
    // 创建信号量，初始计数为0，最大计数为BUFFER_SIZE
    // 初始为0表示开始时没有工作可做
    filled_slots_0 = CreateSemaphore(NULL, 0, BUFFER_SIZE, NULL);
    filled_slots_1 = CreateSemaphore(NULL, 0, BUFFER_SIZE, NULL);
    filled_slots_2 = CreateSemaphore(NULL, 0, BUFFER_SIZE, NULL);
    
    // 检查信号量创建是否成功
    if (!filled_slots_0 || !filled_slots_1 || !filled_slots_2) {
        std::cerr << "Failed to create semaphores" << std::endl;
        return 1;
    }
    
    // 将信号量句柄放入数组，方便遍历
    filled_slots[0] = filled_slots_0;
    filled_slots[1] = filled_slots_1;
    filled_slots[2] = filled_slots_2;

    // 初始化消费者状态
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        out[i].store(-1);                                    // 已处理帧ID初始化为-1
        frames_consumed_per_consumer[i].store(0);           // 处理帧数初始化为0
    }

    // 创建并启动生产者线程
    std::thread producer_thread(producer);
    
    // 创建并启动消费者线程
    std::vector<std::thread> consumer_threads;
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        consumer_threads.emplace_back(consumer, i);
    }
    
    // 让系统运行3秒
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 停止生产
    safe_print("=== STOPPING PRODUCTION ===");
    running = false;
    
    // 等待生产者线程完成
    producer_thread.join();
    safe_print("Producer joined successfully");

    // 等待所有消费者处理完剩余帧
    safe_print("Waiting for consumers to finish remaining frames...");
    for (auto& t : consumer_threads) {
        t.join();
    }
    safe_print("All consumers joined successfully");

    // 输出统计信息
    safe_print("Total frames produced: " + std::to_string(total_frames_produced.load()));
    
    // 输出每个消费者的处理统计
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        std::string type_name;
        switch (consumer_types[i]) {
            case ConsumerType::FAST: type_name = "FAST"; break;
            case ConsumerType::SLOW: type_name = "SLOW"; break;
            case ConsumerType::UNSTABLE: type_name = "UNSTABLE"; break;
            default: type_name = "NORMAL"; break;
        }
        safe_print("C " + std::to_string(i) + " (" + type_name + ") consumed: " + 
                  std::to_string(frames_consumed_per_consumer[i].load()) + " frames");
    }

    // 清理资源：释放信号量句柄
    CloseHandle(filled_slots_0);
    CloseHandle(filled_slots_1);
    CloseHandle(filled_slots_2);
    
    return 0;
}
