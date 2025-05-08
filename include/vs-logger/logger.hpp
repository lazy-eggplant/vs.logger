#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <mutex>
#include <fstream>
#include <sys/un.h>
#include <thread>

class Logger {
public:
    enum struct type_t {
        OK, INFO, WARNING, ERROR, PANIC
    };

    enum struct severity_t {
        NONE, LOW, MID, HIGH
    };

    struct log_entry_t {
        type_t type;
        severity_t sev;
        uint64_t timestamp;
        uint64_t activity_uuid;  // Group related logs by activity.
        uint64_t seq_id;         // Sequence number.
        uint64_t parent_uuid;    // Zero if not relevant.

        size_t offset;           // Offset in the log file.
        size_t length;           // Message length.
    };

    Logger(std::optional<std::filesystem::path> logFilePath = std::nullopt,
           std::optional<std::filesystem::path> udsPath = std::nullopt);
    ~Logger();

    // Delete copy semantics.
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Log a message with the given metadata.
    void log(type_t type,
             severity_t sev,
             const std::string &message,
             uint64_t activity_uuid = 0,
             uint64_t parent_uuid = 0);

    // Initialize the logger (open file, start web server, etc.).
    #ifndef LOG_HEADLESS
    void start_server(uint16_t port = 18080);
    #endif

private:
    // Write the log entry with message to the file.
    void writeToFS(const log_entry_t& entry, const std::string &message);
    // Send a notification (JSON payload) over UDS.
    void writeToWS(const log_entry_t &entry, const std::string &message);
    // Helper to get the current timestamp in microseconds.
    static uint64_t getTimestamp();

    std::optional<std::filesystem::path> logFilePath;
    std::optional<std::filesystem::path> udsPath;

    std::ofstream logFileStream;
    int udsSock=-1;
    sockaddr_un udsAddr;

    // Sequence number for log entries.
    uint64_t seq_id = 0;

    std::mutex writeMutex;


    #ifndef LOG_HEADLESS
    // Background thread for the web server.
    std::thread webServerThread;
    #endif
};

