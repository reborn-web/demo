#include <array>
#include <atomic>
#include <vector>
#include <thread>
#include <iostream>
#include <chrono>
#include <random>
#include <mutex>
#include <climits>
#include <semaphore.h>
#include <fcntl.h>
#include <unistd.h>

const int BUFFER_SIZE = 16;
const int NUM_CONSUMERS = 3;

enum class ConsumerType {
    NORMAL,
    SLOW,
    FAST,
    UNSTABLE
};

ConsumerType consumer_types[NUM_CONSUMERS] = {ConsumerType::FAST, ConsumerType::SLOW, ConsumerType::UNSTABLE};

struct Frame {
    unsigned char data[1024];
    int frame_id;
    std::atomic<int> consumers_processed{0};
};

std::array<Frame, BUFFER_SIZE> buffer;        
std::atomic<int> in{0};
std::atomic<int> out[NUM_CONSUMERS];
std::atomic<bool> running{true};
std::atomic<bool> producer_finished{false};
std::atomic<int> total_frames_produced{0};
std::atomic<int> total_frames_consumed{0};
std::mutex cout_mutex;

sem_t* empty_slots;
sem_t* filled_slots_0;
sem_t* filled_slots_1;
sem_t* filled_slots_2;
sem_t* filled_slots[NUM_CONSUMERS];

void safe_print(const std::string& message) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << message << std::endl;
}

void generate_frame(unsigned char* data) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 255);
    
    for (int i = 0; i < 1024; ++i) {
        data[i] = static_cast<unsigned char>(dis(gen));
    }
}

void process(const unsigned char* data, int frame_id, int consumer_id) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    int processing_time;
    std::string consumer_type_name;
    
    switch (consumer_types[consumer_id]) {
        case ConsumerType::FAST:
            processing_time = std::uniform_int_distribution<>(5, 15)(gen);
            consumer_type_name = "FAST";
            break;
        case ConsumerType::SLOW:
            processing_time = std::uniform_int_distribution<>(100, 200)(gen);
            consumer_type_name = "SLOW";
            break;
        case ConsumerType::UNSTABLE:
            if (std::uniform_int_distribution<>(0, 1)(gen)) {
                processing_time = std::uniform_int_distribution<>(5, 20)(gen);
            } else {
                processing_time = std::uniform_int_distribution<>(80, 150)(gen);
            }
            consumer_type_name = "UNSTABLE";
            break;
        default:
            processing_time = std::uniform_int_distribution<>(20, 50)(gen);
            consumer_type_name = "NORMAL";
            break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(processing_time));

    int checksum = 0;
    for (int i = 0; i < 1024; ++i) {
        checksum += static_cast<int>(data[i]);
    }
    
    total_frames_consumed.fetch_add(1);
    
    safe_print("Consumer " + std::to_string(consumer_id) + " (" + consumer_type_name + 
               ") processed frame " + std::to_string(frame_id) + 
               " in " + std::to_string(processing_time) + "ms, checksum: " + std::to_string(checksum));
}

void producer() {
    int frame_id = 0;
    safe_print("Producer started");
    
    while (running) {
        if (!running) {
            break;
        }

        auto start_time = std::chrono::steady_clock::now();
        sem_wait(empty_slots);
        auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        
        if (wait_time > 0) {
            safe_print("Producer waited " + std::to_string(wait_time) + "ms for empty slot (buffer pressure!)");
        }

        if (!running) {
            sem_post(empty_slots);
            break;
        }

        int idx = in.load() % BUFFER_SIZE;
        generate_frame(buffer[idx].data);
        buffer[idx].frame_id = frame_id++;
        buffer[idx].consumers_processed.store(0);

        total_frames_produced.fetch_add(1);

        safe_print("Producer generated frame " + std::to_string(frame_id - 1) + 
                  " at buffer[" + std::to_string(idx) + "]");

        in.fetch_add(1);

        for (int i = 0; i < NUM_CONSUMERS; ++i) {
            sem_post(filled_slots[i]);
        }

        int production_delay;
        if (frame_id % 10 == 0) {
            production_delay = 10;
        } else if (frame_id % 5 == 0) {
            production_delay = 15;
        } else {
            production_delay = 20;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(production_delay));
    }

    producer_finished = true;
    safe_print("Producer finished, total frames produced: " + std::to_string(total_frames_produced.load()));
}

bool has_unfinished_frames(int consumer_id) {
    if (!producer_finished.load()) {
        return true;
    }

    int total_produced = total_frames_produced.load();
    int current_consumer_position = out[consumer_id].load();

    if (current_consumer_position < total_produced) {
        return true;
    }
    
    return false;
}

void consumer(int id) {
    std::string type_name;
    switch (consumer_types[id]) {
        case ConsumerType::FAST: type_name = "FAST"; break;
        case ConsumerType::SLOW: type_name = "SLOW"; break;
        case ConsumerType::UNSTABLE: type_name = "UNSTABLE"; break;
        default: type_name = "NORMAL"; break;
    }
    
    safe_print("Consumer " + std::to_string(id) + " (" + type_name + ") started");

    while (running || has_unfinished_frames(id)) {
        auto start_time = std::chrono::steady_clock::now();
        bool acquired = false;
        auto timeout_start = std::chrono::steady_clock::now();
        while (!acquired) {
            if (sem_trywait(filled_slots[id]) == 0) {
                acquired = true;
                break;
            }
            
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - timeout_start).count();
            if (elapsed >= 10) {
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        if (!acquired) {
            if (!running && producer_finished && !has_unfinished_frames(id)) {
                break;
            }
            continue;
        }
        
        auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        if (wait_time > 100) {
            safe_print("Consumer " + std::to_string(id) + " waited " + std::to_string(wait_time) + "ms for work");
        }

        int idx = out[id].load() % BUFFER_SIZE;
        process(buffer[idx].data, buffer[idx].frame_id, id);

        out[id].fetch_add(1);

        int processed = buffer[idx].consumers_processed.fetch_add(1) + 1;
        if (processed == NUM_CONSUMERS) {
            sem_post(empty_slots);
            safe_print("All consumers finished frame " + std::to_string(buffer[idx].frame_id) + 
                      " at buffer[" + std::to_string(idx) + "], slot released");
        }
    }
    
    safe_print("Consumer " + std::to_string(id) + " (" + type_name + ") stopped");
}

int main() {
    empty_slots = sem_open("empty_slots", O_CREAT | O_EXCL, 0644, BUFFER_SIZE);
    if (empty_slots == SEM_FAILED) {
        perror("sem_open empty_slots");
        return 1;
    }

    filled_slots_0 = sem_open("filled_slots_0", O_CREAT | O_EXCL, 0644, 0);
    if (filled_slots_0 == SEM_FAILED) {
        perror("sem_open filled_slots_0");
        return 1;
    }

    filled_slots_1 = sem_open("filled_slots_1", O_CREAT | O_EXCL, 0644, 0);
    if (filled_slots_1 == SEM_FAILED) {
        perror("sem_open filled_slots_1");
        return 1;
    }

    filled_slots_2 = sem_open("filled_slots_2", O_CREAT | O_EXCL, 0644, 0);
    if (filled_slots_2 == SEM_FAILED) {
        perror("sem_open filled_slots_2");
        return 1;
    }

    filled_slots[0] = filled_slots_0;
    filled_slots[1] = filled_slots_1;
    filled_slots[2] = filled_slots_2;

    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        out[i].store(0);
    }

    std::thread producer_thread(producer);
    std::vector<std::thread> consumer_threads;
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        consumer_threads.emplace_back(consumer, i);
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));

    safe_print("=== STOPPING PRODUCTION ===");
    running = false;
    producer_thread.join();
    safe_print("Producer joined successfully");

    safe_print("Waiting for consumers to finish remaining frames...");
    for (auto& t : consumer_threads) {
        t.join();
    }
    safe_print("All consumers joined successfully");

    sem_close(empty_slots);
    sem_close(filled_slots_0);
    sem_close(filled_slots_1);
    sem_close(filled_slots_2);
    sem_unlink("empty_slots");
    sem_unlink("filled_slots_0");
    sem_unlink("filled_slots_1");
    sem_unlink("filled_slots_2");

    safe_print("Total frames produced: " + std::to_string(total_frames_produced.load()));
    safe_print("Total frames consumed: " + std::to_string(total_frames_consumed.load()));
    return 0;
}