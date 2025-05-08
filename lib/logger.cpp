
#include <chrono>
#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "vs-logger/logger.hpp"

#ifndef LOG_HEADLESS
#include "vs-logger/crow_all.h"
#endif

// Utility functions to convert enum values to strings.
std::string_view to_string(Logger::type_t type) {
    switch (type) {
        case Logger::type_t::OK:      return "OK";
        case Logger::type_t::INFO:    return "INFO";
        case Logger::type_t::WARNING: return "WARNING";
        case Logger::type_t::ERROR:   return "ERROR";
        case Logger::type_t::PANIC:   return "PANIC";
    }
    return "UNKNOWN";
}

std::string_view to_string(Logger::severity_t sev) {
    switch (sev) {
        case Logger::severity_t::NONE: return "NONE";
        case Logger::severity_t::LOW:  return "LOW";
        case Logger::severity_t::MID:  return "MID";
        case Logger::severity_t::HIGH: return "HIGH";
    }
    return "UNKNOWN";
}

Logger::Logger(std::optional<std::filesystem::path> logFilePath, std::optional<std::filesystem::path> udsPath)
    : logFilePath(logFilePath), udsPath(udsPath) {
    if(logFilePath.has_value()){
        auto& path = *logFilePath;
        // Open the log file (located in tmpfs under /tmp).
        logFileStream.open(path, std::ios::app);
        if (!logFileStream.is_open()) {
            std::cerr << "Failed to open log file: " << path << std::endl;
        }
    }

    if(udsPath.has_value()){
        // Create a temporary UDS socket (datagram) and send the JSON message.
        udsSock = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (udsSock < 0) {
            std::cerr << "notifySubscribers: Failed to create socket: " << strerror(errno) << std::endl;
            return;
        }

        memset(&udsAddr, 0, sizeof(udsAddr));
        udsAddr.sun_family = AF_UNIX;
        // Use the same address as the UDS listener (web server bridge).
        strncpy(udsAddr.sun_path, udsPath->c_str(), sizeof(udsAddr.sun_path) - 1);
    }
}

Logger::~Logger() {
    if (logFileStream.is_open())
        logFileStream.close();
    
    if (udsSock>=0)
        close(udsSock);
}

uint64_t Logger::getTimestamp() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void Logger::writeToFS(const log_entry_t& entry, const std::string &message) {
    if (!logFileStream.is_open()) {
        std::cerr << "Log file not open!" << std::endl;
        return;
    }

    //TODO: escaping
    // Format: timestamp [TYPE] {SEVERITY} Activity:<uuid> Seq:<seq> Parent:<parent> -- message
    logFileStream.seekp(0, std::ios::end);
    logFileStream << std::format("[{}], {{{}}}, Activity: {} Seq: {} Parent: {} -- {}\n",
                                    to_string(entry.type),
                                    to_string(entry.sev),
                                    entry.activity_uuid,
                                    entry.seq_id,
                                    entry.parent_uuid,
                                    message
                                );
    logFileStream.flush();
}

void Logger::writeToWS(const log_entry_t &entry, const std::string &message) {
    //TODO: escaping
    // Build JSON payload.
    std::stringstream ss;
    ss << "{"
       << "\"timestamp\":" << entry.timestamp << ","
       << "\"type\":\"" << to_string(entry.type) << "\","
       << "\"severity\":\"" << to_string(entry.sev) << "\","
       << "\"activity_uuid\":" << entry.activity_uuid << ","
       << "\"seq_id\":" << entry.seq_id << ","
       << "\"parent_uuid\":" << entry.parent_uuid << ","
       << "\"message\":\"" << message << "\""
       << "}";

    std::string payload = ss.str();
    ssize_t ret = sendto(udsSock, payload.c_str(), payload.size(), 0,
                         (struct sockaddr*)&udsAddr, sizeof(udsAddr));
    if (ret < 0) {
        std::cerr << "notifySubscribers: Failed to send payload: " << strerror(errno) << std::endl;
    }
}

void Logger::log(type_t type, severity_t sev, const std::string &message,
                 uint64_t activity_uuid, uint64_t parent_uuid) {
    std::lock_guard<std::mutex> lock(writeMutex);

    log_entry_t entry;
    entry.type = type;
    entry.sev = sev;
    entry.timestamp = getTimestamp();
    entry.activity_uuid = activity_uuid;
    entry.seq_id = ++seq_id;
    entry.parent_uuid = parent_uuid;
    // In this example, we skip recording offset; OR set it to 0.
    entry.offset = 0;
    entry.length = message.size();

    if(logFilePath.has_value())writeToFS(entry, message);
    if(udsPath.has_value())writeToWS(entry, message);
}

#ifndef LOG_HEADLESS

void Logger::start_server(uint16_t port) {
    if(udsPath.has_value()){
        // Start the web server (which will act as the UDS listener as well).
        webServerThread = std::thread([this, port]() {
            // Start Crow application.
            crow::SimpleApp app;

            // Container to hold active WebSocket connections.
            std::set<crow::websocket::connection*> wsConnections;
            std::mutex wsMutex;

            // Serve the webpage.
            CROW_ROUTE(app, "/")
            ([]() consteval -> const char* {
                return R"raw(<!DOCTYPE html>
    <html>
    <head>
    <meta charset="utf-8">
    <title>Log Viewer</title>
    </head>
    <body>
    <h1>Live Log Viewer</h1>
    <div>
        <label for="filter">Filter by type:</label>
        <select id="filter">
        <option value="">(All)</option>
        <option value="OK">OK</option>
        <option value="INFO">INFO</option>
        <option value="WARNING">WARNING</option>
        <option value="ERROR">ERROR</option>
        <option value="PANIC">PANIC</option>
        </select>
    </div>
    <pre id="logs" style="background-color:#f0f0f0;padding:10px;height:400px;overflow:auto;"></pre>
    <script>
        const ws = new WebSocket("ws://" + location.host + "/ws");
        ws.onmessage = (event) => {
        const data = JSON.parse(event.data);
        const filter = document.getElementById("filter").value;
        if(filter && data.type !== filter) return;
        const logs = document.getElementById("logs");
        logs.textContent += `[${data.timestamp}] ${data.type} (${data.severity}): ${data.message}\n`;
        logs.scrollTop = logs.scrollHeight;
        };
        document.getElementById("filter").addEventListener("change", () => {
        // Clear the log view when filter changes.
        document.getElementById("logs").textContent = "";
        });
    </script>
    </body>
    </html>)raw";
            });

        // WebSocket endpoint.
        CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([&wsConnections, &wsMutex](crow::websocket::connection& conn) {
            std::lock_guard<std::mutex> lock(wsMutex);
            wsConnections.insert(&conn);
            std::cout << "WebSocket connection opened" << std::endl;
        })
        .onclose([&wsConnections, &wsMutex](crow::websocket::connection& conn, const std::string& reason, uint16_t) {
            std::lock_guard<std::mutex> lock(wsMutex);
            wsConnections.erase(&conn);
            std::cout << "WebSocket connection closed: " << reason << std::endl;
        })
        .onmessage([](crow::websocket::connection& /*conn*/, const std::string& /*data*/, bool /*is_binary*/) {
            // Ignore client messages.
        });

        // Single UDS listener that uses the same udsPath_.
        // This listener reads UDP datagrams and broadcasts them to all WebSocket clients.
        std::thread udsBridgeThread([this, &wsConnections, &wsMutex]() {
            int sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
            if (sockfd < 0) {
                std::cerr << "UDS Bridge: Failed to create socket: " << strerror(errno) << std::endl;
                return;
            }
            // Remove any previous socket file and bind.
            unlink(udsPath->c_str());
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, udsPath->c_str(), sizeof(addr.sun_path) - 1);

            if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                std::cerr << "UDS Bridge: Failed to bind: " << strerror(errno) << std::endl;
                close(sockfd);
                return;
            }

            constexpr size_t BUFFER_SIZE = 1024;
            char buffer[BUFFER_SIZE];
            while (true) {
                ssize_t n = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
                if (n > 0) {
                    buffer[n] = '\0';
                    // Print received message for debugging.
                    std::cout << "UDS Bridge received: " << buffer << std::endl;
                    // Broadcast the JSON message to all connected WebSocket clients.
                    std::lock_guard<std::mutex> lock(wsMutex);
                    for (auto* conn : wsConnections) {
                        try {
                            conn->send_text(std::string(buffer));
                        } catch (const std::exception& ex) {
                            std::cerr << "Failed to send via websocket: " << ex.what() << std::endl;
                        }
                    }
                }
                else {
                    // Sleep briefly to avoid busy looping.
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            close(sockfd);
        });
        udsBridgeThread.detach();

        app.port(port)./*multithreaded().*/run();
    });

    // Detach the web server thread so it runs in the background.
    webServerThread.detach();
}
#endif

}
