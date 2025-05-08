#include <chrono>
#include <filesystem>
#include <thread>
#include <iostream>
#include <format>

#include "vs-logger/logger.hpp"


int main() {
    Logger logger(std::filesystem::temp_directory_path()/"a.0", std::filesystem::temp_directory_path()/"a.0");
    logger.start_server();

    // Simulate generating log messages.
    for (int i = 0; i < 100; i++) {
        logger.log((Logger::type_t)(random()%5), (Logger::severity_t)(random()%4),
                   std::format("Test log message number {}",i + 1),
                   /* activity_uuid */ 12345);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    std::cout << "Press Ctrl+C to exit..." << std::endl;
    // Keep the main thread alive.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    return 0;
}