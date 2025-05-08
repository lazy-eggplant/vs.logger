
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

static std::string escape_json(const std::string &s) {
    std::string result;
    for (auto c : s) {
        switch(c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b";  break;
            case '\f': result += "\\f";  break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:
                result.push_back(c);
        }
    }
    return result;
}

void Logger::writeToFS(const log_entry_t& entry, const std::string &message) {
    if (!logFileStream.is_open()) {
        std::cerr << "Log file not open!" << std::endl;
        return;
    }

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
    // Build JSON payload.
    std::string payload = std::format(
        "{{\"timestamp\":{},\"type\":\"{}\",\"severity\":\"{}\",\"activity_uuid\":\"{}\","
        "\"seq_id\":{},\"parent_uuid\":\"{}\",\"message\":\"{}\"}}",
        entry.timestamp,
        to_string(entry.type),
        to_string(entry.sev),
        entry.activity_uuid,
        entry.seq_id,
        entry.parent_uuid,
        escape_json(message)
    );

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
                return R"raw(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Advanced Live Log Viewer</title>
  <style>
    html, body {
      height: 100%;
      margin: 0;
    }
    /* Base layout & colors */
    body {
      font-family: Arial, sans-serif;
      background-color: var(--bg-color);
      color: var(--text-color);
      display: flex;
      flex-direction: column;
    }
    header {
      background-color: var(--header-bg);
      border-bottom: 1px solid var(--border-color);
      display: flex;
      align-items: center;
      padding: 5px 10px;
    }
    header .connStatus {
      font-size: 1.5em;
      margin-right: 10px;
    }
    header h1 {
      flex: 1;
      margin: 0;
      font-size: 1.5em;
    }
    header button {
      margin-left: 10px;
    }
    .controls {
      background-color: var(--header-bg);
      border-bottom: 1px solid var(--border-color);
      padding: 10px;
    }
    .controls label {
      margin-right: 5px;
    }
    .controls input,
    .controls select {
      margin-right: 15px;
      vertical-align: middle;
    }
    .controls button, .controls label.auto-scroll {
      margin-right: 15px;
      vertical-align: middle;
    }
    /* Light/dark theme variables */
    :root {
      --bg-color: #ffffff;
      --text-color: #000000;
      --header-bg: #f0f0f0;
      --border-color: #ccc;
      --table-header-bg: #e9e9e9;
      --table-row-bg: #f8f8f8;
    }
    [data-theme="dark"] {
      --bg-color: #1e1e1e;
      --text-color: #e0e0e0;
      --header-bg: #333;
      --border-color: #555;
      --table-header-bg: #444;
      --table-row-bg: #262626;
    }
    /* Log panel styling: fill remaining screen space */
    #logPanel {
      flex: 1;
      overflow-y: auto;
      border-top: 1px solid var(--border-color);
    }
    table {
      border-collapse: collapse;
      width: 100%;
    }
    thead {
      background-color: var(--table-header-bg);
      position: sticky;
      top: 0;
      z-index: 1;
    }
    th, td {
      padding: 8px;
      border: 1px solid var(--border-color);
      text-align: left;
      font-family: monospace;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    /* Set fixed widths for all columns except message */
    th.timestamp, td.timestamp { width: 110px; }
    th.type, td.type { width: 60px; text-align: center; }
    th.severity, td.severity { width: 80px; }
    th.parent, td.parent { width: 200px; cursor: pointer; }
    th.activity, td.activity { width: 200px; cursor: pointer; }
    th.seq, td.seq { width: 60px; }
    th.message, td.message { width: auto; }
  </style>
</head>
<body data-theme="light">
  <header>
    <!-- Connection Status Icon: chain when connected, broken chain when disconnected -->
    <div class="connStatus" id="connStatus" title="Connection Status">⛓️</div>
    <h1>Advanced Live Log Viewer</h1>
    <button id="toggleTheme">Toggle Dark/Light</button>
    <button id="muteAudioBtn">Mute Panic Audio</button>
  </header>
  <div class="controls">
    <!-- Filter & Export Controls -->
    <label title="Type: OK = Successful, INFO = Information, WARNING = Warning, ERROR = Error, PANIC = Panic">Type</label>
    <select id="filterType">
      <option value="">(All)</option>
      <option value="OK">✔️ OK – Successful</option>
      <option value="INFO">ℹ️ INFO – Information</option>
      <option value="WARNING">⚠️ WARNING – Warning</option>
      <option value="ERROR">❌ ERROR – Error</option>
      <option value="PANIC">⛔ PANIC – Panic</option>
    </select>

    <label title="Severity: NONE, LOW, MID, HIGH">Severity</label>
    <select id="filterSeverity">
      <option value="">(All)</option>
      <option value="NONE">NONE</option>
      <option value="LOW">LOW</option>
      <option value="MID">MID</option>
      <option value="HIGH">HIGH</option>
    </select>

    <label title="Activity UUID">Activity 🆔</label>
    <input type="text" id="filterActivity" placeholder="Activity UUID">

    <label title="Parent UUID">Parent 🔗</label>
    <input type="text" id="filterParent" placeholder="Parent UUID">

    <label title="Keyword Search">Search 🔍</label>
    <input type="text" id="searchText" placeholder="Search text...">
    
    <button id="exportBtn">Export CSV</button>
    <button id="clearLogsBtn">Clear Logs</button>
    
    <label class="auto-scroll" title="Toggle auto-scroll on/off">
      <input type="checkbox" id="autoScroll" checked> Auto-scroll
    </label>
  </div>
  <div id="logPanel">
    <table id="logTable">
      <thead>
        <tr>
          <th class="timestamp">Timestamp</th>
          <th class="type">Type</th>
          <th class="severity">Severity</th>
          <th class="parent">Parent UUID</th>
          <th class="activity">Activity UUID</th>
          <th class="seq">Seq ID</th>
          <th class="message">Message</th>
        </tr>
      </thead>
      <tbody>
      </tbody>
    </table>
  </div>

  <!-- Audio element for PANIC logs -->
  <audio id="panicSound" preload="auto">
    <source src="data:audio/wav;base64,UklGRhwMAABXQVZFZm10IBAAAAABAAEAgD4AAIA+AAABAAgAZGF0Ya4LAACAgICAgICAgICAgICAgICAgICAgICAgICAf3hxeH+AfXZ1eHx6dnR5fYGFgoOKi42aloubq6GOjI2Op7ythXJ0eYF5aV1AOFFib32HmZSHhpCalIiYi4SRkZaLfnhxaWptb21qaWBea2BRYmZTVmFgWFNXVVVhaGdbYGhZbXh1gXZ1goeIlot1k6yxtKaOkaWhq7KonKCZoaCjoKWuqqmurK6ztrO7tbTAvru/vb68vbW6vLGqsLOfm5yal5KKhoyBeHt2dXBnbmljVlJWUEBBPDw9Mi4zKRwhIBYaGRQcHBURGB0XFxwhGxocJSstMjg6PTc6PUxVV1lWV2JqaXN0coCHhIyPjpOenqWppK6xu72yxMu9us7Pw83Wy9nY29ve6OPr6uvs6ezu6ejk6erm3uPj3dbT1sjBzdDFuMHAt7m1r7W6qaCupJOTkpWPgHqAd3JrbGlnY1peX1hTUk9PTFRKR0RFQkRBRUVEQkdBPjs9Pzo6NT04Njs+PTxAPzo/Ojk6PEA5PUJAQD04PkRCREZLUk1KT1BRUVdXU1VRV1tZV1xgXltcXF9hXl9eY2VmZmlna3J0b3F3eHyBfX+JgIWJiouTlZCTmpybnqSgnqyrqrO3srK2uL2/u7jAwMLFxsfEv8XLzcrIy83JzcrP0s3M0dTP0drY1dPR1dzc19za19XX2dnU1NjU0dXPzdHQy8rMysfGxMLBvLu3ta+sraeioJ2YlI+MioeFfX55cnJsaWVjXVlbVE5RTktHRUVAPDw3NC8uLyknKSIiJiUdHiEeGx4eHRwZHB8cHiAfHh8eHSEhISMoJyMnKisrLCszNy8yOTg9QEJFRUVITVFOTlJVWltaXmNfX2ZqZ21xb3R3eHqAhoeJkZKTlZmhpJ6kqKeur6yxtLW1trW4t6+us7axrbK2tLa6ury7u7u9u7vCwb+/vr7Ev7y9v8G8vby6vru4uLq+tri8ubi5t7W4uLW5uLKxs7G0tLGwt7Wvs7avr7O0tLW4trS4uLO1trW1trm1tLm0r7Kyr66wramsqaKlp52bmpeWl5KQkImEhIB8fXh3eHJrbW5mYGNcWFhUUE1LRENDQUI9ODcxLy8vMCsqLCgoKCgpKScoKCYoKygpKyssLi0sLi0uMDIwMTIuLzQ0Njg4Njc8ODlBQ0A/RUdGSU5RUVFUV1pdXWFjZGdpbG1vcXJ2eXh6fICAgIWIio2OkJGSlJWanJqbnZ2cn6Kkp6enq62srbCysrO1uLy4uL+/vL7CwMHAvb/Cvbq9vLm5uba2t7Sysq+urqyqqaalpqShoJ+enZuamZqXlZWTkpGSkpCNjpCMioqLioiHhoeGhYSGg4GDhoKDg4GBg4GBgoGBgoOChISChISChIWDg4WEgoSEgYODgYGCgYGAgICAgX99f398fX18e3p6e3t7enp7fHx4e3x6e3x7fHx9fX59fn1+fX19fH19fnx9fn19fX18fHx7fHx6fH18fXx8fHx7fH1+fXx+f319fn19fn1+gH9+f4B/fn+AgICAgH+AgICAgIGAgICAgH9+f4B+f35+fn58e3t8e3p5eXh4d3Z1dHRzcXBvb21sbmxqaWhlZmVjYmFfX2BfXV1cXFxaWVlaWVlYV1hYV1hYWVhZWFlaWllbXFpbXV5fX15fYWJhYmNiYWJhYWJjZGVmZ2hqbG1ub3Fxc3V3dnd6e3t8e3x+f3+AgICAgoGBgoKDhISFh4aHiYqKi4uMjYyOj4+QkZKUlZWXmJmbm52enqCioqSlpqeoqaqrrK2ur7CxsrGys7O0tbW2tba3t7i3uLe4t7a3t7i3tre2tba1tLSzsrKysbCvrq2sq6qop6alo6OioJ+dnJqZmJeWlJKSkI+OjoyLioiIh4WEg4GBgH9+fXt6eXh3d3V0c3JxcG9ubWxsamppaWhnZmVlZGRjYmNiYWBhYGBfYF9fXl5fXl1dXVxdXF1dXF1cXF1cXF1dXV5dXV5fXl9eX19gYGFgYWJhYmFiY2NiY2RjZGNkZWRlZGVmZmVmZmVmZ2dmZ2hnaGhnaGloZ2hpaWhpamlqaWpqa2pra2xtbGxtbm1ubm5vcG9wcXBxcnFycnN0c3N0dXV2d3d4eHh5ent6e3x9fn5/f4CAgIGCg4SEhYaGh4iIiYqLi4uMjY2Oj5CQkZGSk5OUlJWWlpeYl5iZmZqbm5ybnJ2cnZ6en56fn6ChoKChoqGio6KjpKOko6SjpKWkpaSkpKSlpKWkpaSlpKSlpKOkpKOko6KioaKhoaCfoJ+enp2dnJybmpmZmJeXlpWUk5STkZGQj4+OjYyLioqJh4eGhYSEgoKBgIB/fn59fHt7enl5eHd3dnZ1dHRzc3JycXBxcG9vbm5tbWxrbGxraWppaWhpaGdnZ2dmZ2ZlZmVmZWRlZGVkY2RjZGNkZGRkZGRkZGRkZGRjZGRkY2RjZGNkZWRlZGVmZWZmZ2ZnZ2doaWhpaWpra2xsbW5tbm9ub29wcXFycnNzdHV1dXZ2d3d4eXl6enp7fHx9fX5+f4CAgIGAgYGCgoOEhISFhoWGhoeIh4iJiImKiYqLiouLjI2MjI2OjY6Pj46PkI+QkZCRkJGQkZGSkZKRkpGSkZGRkZKRkpKRkpGSkZKRkpGSkZKRkpGSkZCRkZCRkI+Qj5CPkI+Pjo+OjY6Njo2MjYyLjIuMi4qLioqJiomJiImIh4iHh4aHhoaFhoWFhIWEg4SDg4KDgoKBgoGAgYCBgICAgICAf4CAf39+f35/fn1+fX59fHx9fH18e3x7fHt6e3p7ent6e3p5enl6enl6eXp5eXl4eXh5eHl4eXh5eHl4eXh5eHh3eHh4d3h4d3h3d3h4d3l4eHd4d3h3eHd4d3h3eHh4eXh5eHl4eHl4eXh5enl6eXp5enl6eXp5ent6ent6e3x7fHx9fH18fX19fn1+fX5/fn9+f4B/gH+Af4CAgICAgIGAgYCBgoGCgYKCgoKDgoOEg4OEg4SFhIWEhYSFhoWGhYaHhoeHhoeGh4iHiIiHiImIiImKiYqJiYqJiouKi4qLiouKi4qLiouKi4qLiouKi4qLi4qLiouKi4qLiomJiomIiYiJiImIh4iIh4iHhoeGhYWGhYaFhIWEg4OEg4KDgoOCgYKBgIGAgICAgH+Af39+f359fn18fX19fHx8e3t6e3p7enl6eXp5enl6enl5eXh5eHh5eHl4eXh5eHl4eHd5eHd3eHl4d3h3eHd4d3h3eHh4d3h4d3h3d3h5eHl4eXh5eHl5eXp5enl6eXp7ent6e3p7e3t7fHt8e3x8fHx9fH1+fX59fn9+f35/gH+AgICAgICAgYGAgYKBgoGCgoKDgoOEg4SEhIWFhIWFhoWGhYaGhoaHhoeGh4aHhoeIh4iHiIeHiIeIh4iHiIeIiIiHiIeIh4iHiIiHiIeIh4iHiIeIh4eIh4eIh4aHh4aHhoeGh4aHhoWGhYaFhoWFhIWEhYSFhIWEhISDhIOEg4OCg4OCg4KDgYKCgYKCgYCBgIGAgYCBgICAgICAgICAf4B/f4B/gH+Af35/fn9+f35/fn1+fn19fn1+fX59fn19fX19fH18fXx9fH18fXx9fH18fXx8fHt8e3x7fHt8e3x7fHt8e3x7fHt8e3x7fHt8e3x7fHt8e3x8e3x7fHt8e3x7fHx8fXx9fH18fX5+fX59fn9+f35+f35/gH+Af4B/gICAgICAgICAgICAgYCBgIGAgIGAgYGBgoGCgYKBgoGCgYKBgoGCgoKDgoOCg4KDgoOCg4KDgoOCg4KDgoOCg4KDgoOCg4KDgoOCg4KDgoOCg4KDgoOCg4KDgoOCg4KDgoOCg4KCgoGCgYKBgoGCgYKBgoGCgYKBgoGCgYKBgoGCgYKBgoGCgYKBgoGCgYKBgoGBgYCBgIGAgYCBgIGAgYCBgIGAgYCBgIGAgYCBgIGAgYCAgICBgIGAgYCBgIGAgYCBgIGAgYCBgExJU1RCAAAASU5GT0lDUkQMAAAAMjAwOC0wOS0yMQAASUVORwMAAAAgAAABSVNGVBYAAABTb255IFNvdW5kIEZvcmdlIDguMAAA" />
    Your browser does not support the audio element.
  </audio>

  <script>
    // Utility function to save settings to localStorage
    function saveSettings() {
      const settings = {
        filterType: document.getElementById("filterType").value,
        filterSeverity: document.getElementById("filterSeverity").value,
        filterActivity: document.getElementById("filterActivity").value,
        filterParent: document.getElementById("filterParent").value,
        searchText: document.getElementById("searchText").value,
        autoScroll: document.getElementById("autoScroll").checked,
        theme: document.body.getAttribute("data-theme"),
        panicAudioMuted: document.getElementById("panicSound").muted
      };
      localStorage.setItem("logViewerSettings", JSON.stringify(settings));
    }

    // Utility function to load settings from localStorage
    function loadSettings() {
      const settingsStr = localStorage.getItem("logViewerSettings");
      if (settingsStr) {
        const settings = JSON.parse(settingsStr);
        document.getElementById("filterType").value = settings.filterType || "";
        document.getElementById("filterSeverity").value = settings.filterSeverity || "";
        document.getElementById("filterActivity").value = settings.filterActivity || "";
        document.getElementById("filterParent").value = settings.filterParent || "";
        document.getElementById("searchText").value = settings.searchText || "";
        document.getElementById("autoScroll").checked = (settings.autoScroll === undefined) ? true : settings.autoScroll;
        document.body.setAttribute("data-theme", settings.theme || "light");
        document.getElementById("panicSound").muted = settings.panicAudioMuted || false;
        updateMuteButton();
      }
    }

    // Update mute button's label
    function updateMuteButton() {
      const muteBtn = document.getElementById("muteAudioBtn");
      const panicAudioMuted = document.getElementById("panicSound").muted;
      muteBtn.textContent = panicAudioMuted ? "Unmute Panic Audio" : "Mute Panic Audio";
    }

    // Data store for logs
    let logStore = [];

    // Colors for type icons only (the cell showing the unicode icon)
    const typeColors = {
      "OK": "#008000",       // green
      "INFO": "#0000FF",     // blue
      "WARNING": "#FFA500",  // orange
      "ERROR": "#FF0000",    // red
      "PANIC": "#800080"     // purple
    };

    // Unicode mapping for type filtering icons and tooltips.
    const typeIcons = {
      "OK": { icon: "✔️", title: "OK – Successful message" },
      "INFO": { icon: "ℹ️", title: "INFO – Information message" },
      "WARNING": { icon: "⚠️", title: "WARNING – Warning message" },
      "ERROR": { icon: "❌", title: "ERROR – Error message" },
      "PANIC": { icon: "⛔", title: "PANIC – Panic message" }
    };

    // WebSocket setup & auto-reconnect
    const wsUrl = "ws://" + location.host + "/ws";
    let ws;
    let reconnectInterval = 5000; // milliseconds

    function setConnectionStatus(connected) {
      const connStatusEl = document.getElementById("connStatus");
      if (connected) {
        connStatusEl.textContent = "⛓️"; // chain icon
        connStatusEl.title = "Connected";
      } else {
        connStatusEl.textContent = "🔌"; // unplug icon
        connStatusEl.title = "Disconnected";
      }
    }

    function connectWebSocket() {
      ws = new WebSocket(wsUrl);
      ws.onopen = () => {
        setConnectionStatus(true);
      };
      ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          // Only add if seq_id not already present.
          if (!logStore.some(log => log.seq_id === data.seq_id)) {
            logStore.push(data);
            // Sort logStore by seq_id.
            logStore.sort((a, b) => a.seq_id - b.seq_id);
            // Play sound for PANIC logs (if not muted).
            if (data.type === "PANIC") {
              const panicSound = document.getElementById("panicSound");
              panicSound.play().catch(e => console.error(e));
            }
            renderLogs();
          }
        } catch (e) {
          console.error("Error parsing message", e);
        }
      };
      ws.onclose = () => {
        setConnectionStatus(false);
        setTimeout(connectWebSocket, reconnectInterval);
      };
      ws.onerror = (err) => {
        console.error("WebSocket error", err);
        ws.close();
      };
    }
    connectWebSocket();

    // Render table rows for logs
    function renderLogs() {
      const filterType = document.getElementById("filterType").value;
      const filterSeverity = document.getElementById("filterSeverity").value;
      const filterActivity = document.getElementById("filterActivity").value.trim().toLowerCase();
      const filterParent = document.getElementById("filterParent").value.trim().toLowerCase();
      const searchText = document.getElementById("searchText").value.toLowerCase();
      const tbody = document.getElementById("logTable").querySelector("tbody");
      tbody.innerHTML = "";
      
      // Filter logic: allow filtering by type, severity, activity_uuid, parent_uuid, and message content.
      const filteredLogs = logStore.filter(log => {
        const matchType = filterType ? log.type === filterType : true;
        const matchSeverity = filterSeverity ? log.severity === filterSeverity : true;
        const matchActivity = filterActivity ? (log.activity_uuid && log.activity_uuid.toLowerCase().includes(filterActivity)) : true;
        const matchParent = filterParent ? (log.parent_uuid && log.parent_uuid.toLowerCase().includes(filterParent)) : true;
        const matchText = searchText ? ((log.message && log.message.toLowerCase().includes(searchText)) ||
                                        (log.activity_uuid && log.activity_uuid.toLowerCase().includes(searchText)) ||
                                        (log.parent_uuid && log.parent_uuid.toLowerCase().includes(searchText))) : true;
        return matchType && matchSeverity && matchActivity && matchParent && matchText;
      });
      
      // Create a row for each filtered log.
      for (const log of filteredLogs) {
        const tr = document.createElement("tr");
        
        // Timestamp cell.
        const tdTimestamp = document.createElement("td");
        tdTimestamp.className = "timestamp";
        tdTimestamp.textContent = log.timestamp;
        tr.appendChild(tdTimestamp);

        // Type cell with icon, color, and tooltip.
        const tdType = document.createElement("td");
        tdType.className = "type";
        if (typeIcons[log.type]) {
          tdType.textContent = typeIcons[log.type].icon;
          tdType.title = typeIcons[log.type].title;
          tdType.style.color = typeColors[log.type];
        } else {
          tdType.textContent = log.type;
        }
        tr.appendChild(tdType);
        
        // Severity cell.
        const tdSeverity = document.createElement("td");
        tdSeverity.className = "severity";
        tdSeverity.textContent = log.severity;
        tr.appendChild(tdSeverity);
        
        // Parent UUID cell (clickable)
        const tdParent = document.createElement("td");
        tdParent.className = "parent";
        tdParent.textContent = log.parent_uuid;
        tdParent.title = "Click to filter by Parent UUID";
        tdParent.addEventListener("click", () => {
          document.getElementById("filterParent").value = log.parent_uuid;
          renderLogs();
          saveSettings();
        });
        tr.appendChild(tdParent);
        
        // Activity UUID cell (clickable)
        const tdActivity = document.createElement("td");
        tdActivity.className = "activity";
        tdActivity.textContent = log.activity_uuid;
        tdActivity.title = "Click to filter by Activity UUID";
        tdActivity.addEventListener("click", () => {
          document.getElementById("filterActivity").value = log.activity_uuid;
          renderLogs();
          saveSettings();
        });
        tr.appendChild(tdActivity);
        
        // Sequence ID cell.
        const tdSeq = document.createElement("td");
        tdSeq.className = "seq";
        tdSeq.textContent = log.seq_id;
        tr.appendChild(tdSeq);
        
        // Message cell.
        const tdMessage = document.createElement("td");
        tdMessage.className = "message";
        tdMessage.textContent = log.message;
        tr.appendChild(tdMessage);
        
        tbody.appendChild(tr);
      }

      // Auto-scroll if enabled.
      if (document.getElementById("autoScroll").checked) {
        const panel = document.getElementById("logPanel");
        panel.scrollTop = panel.scrollHeight;
      }
    }

    // Attach filtering events and save changes.
    document.getElementById("filterType").addEventListener("change", () => { renderLogs(); saveSettings(); });
    document.getElementById("filterSeverity").addEventListener("change", () => { renderLogs(); saveSettings(); });
    document.getElementById("filterActivity").addEventListener("input", () => { renderLogs(); saveSettings(); });
    document.getElementById("filterParent").addEventListener("input", () => { renderLogs(); saveSettings(); });
    document.getElementById("searchText").addEventListener("input", () => { renderLogs(); saveSettings(); });
    document.getElementById("autoScroll").addEventListener("change", saveSettings);

    // CSV export functionality.
    function escapeCSV(value) {
      if (typeof value === 'string' && (value.includes(',') || value.includes('"') || value.includes('\n'))) {
        return '"' + value.replace(/"/g, '""') + '"';
      }
      return value;
    }
    document.getElementById("exportBtn").addEventListener("click", () => {
      const filterType = document.getElementById("filterType").value;
      const filterSeverity = document.getElementById("filterSeverity").value;
      const filterActivity = document.getElementById("filterActivity").value.trim().toLowerCase();
      const filterParent = document.getElementById("filterParent").value.trim().toLowerCase();
      const searchText = document.getElementById("searchText").value.toLowerCase();

      const filteredLogs = logStore.filter(log => {
        const matchType = filterType ? log.type === filterType : true;
        const matchSeverity = filterSeverity ? log.severity === filterSeverity: true;
        const matchActivity = filterActivity ? (log.activity_uuid && log.activity_uuid.toLowerCase().includes(filterActivity)) : true;
        const matchParent = filterParent ? (log.parent_uuid && log.parent_uuid.toLowerCase().includes(filterParent)) : true;
        const matchText = searchText ? ((log.message && log.message.toLowerCase().includes(searchText)) ||
                                        (log.activity_uuid && log.activity_uuid.toLowerCase().includes(searchText)) ||
                                        (log.parent_uuid && log.parent_uuid.toLowerCase().includes(searchText))) : true;
        return matchType && matchSeverity && matchActivity && matchParent && matchText;
      });
      
      let csvContent = "timestamp,type,severity,parent_uuid,activity_uuid,seq_id,message\n";
      filteredLogs.forEach(log => {
        csvContent += [
          escapeCSV(log.timestamp),
          escapeCSV(log.type),
          escapeCSV(log.severity),
          escapeCSV(log.parent_uuid),
          escapeCSV(log.activity_uuid),
          log.seq_id,
          escapeCSV(log.message)
        ].join(",") + "\n";
      });
      
      const blob = new Blob([csvContent], { type: "text/csv;charset=utf-8;" });
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = "logs.csv";
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
    });

    // Clear logs functionality.
    document.getElementById("clearLogsBtn").addEventListener("click", () => {
      logStore = [];
      renderLogs();
    });

    // Toggle theme.
    document.getElementById("toggleTheme").addEventListener("click", () => {
      const bodyEl = document.body;
      bodyEl.setAttribute("data-theme", bodyEl.getAttribute("data-theme") === "light" ? "dark" : "light");
      saveSettings();
    });

    // Mute/unmute panic audio.
    document.getElementById("muteAudioBtn").addEventListener("click", () => {
      const panicAudio = document.getElementById("panicSound");
      panicAudio.muted = !panicAudio.muted;
      updateMuteButton();
      saveSettings();
    });

    // Load settings on page load.
    loadSettings();
    renderLogs();
  </script>
</body>
</html>
)raw";
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
