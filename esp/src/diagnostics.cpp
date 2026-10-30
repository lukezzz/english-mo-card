#include "diagnostics.h"
#include "config.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <cstdarg>
#include <esp_attr.h>

#ifndef EPD_PERSIST_DEBUG
#define EPD_PERSIST_DEBUG 1
#endif

namespace {
constexpr uint32_t kMagic = 0x45504401;
constexpr size_t kEntries = 24;
struct History {
    uint32_t magic, boots, next, count;
    char lines[kEntries][192];
};
History history = {};
Preferences storage;
bool storageReady = false, dirty = false;
unsigned long lastSave = 0, lastHeartbeat = 0;
char serialCommand[16] = {};
size_t commandLength = 0;
unsigned long lastSerialByte = 0;
struct Breadcrumb { uint32_t magic, uptime, progress; char stage[48]; };
RTC_NOINIT_ATTR Breadcrumb breadcrumb;
void serialDispatch() {
    serialCommand[commandLength] = '\0';
    if (!strcmp(serialCommand, "logs")) Serial.println(diagnosticsJson());
    else if (!strcmp(serialCommand, "status")) Serial.println(diagnosticsStatus());
    else if (commandLength) Serial.println("[DEBUG] Commands: logs, status");
    commandLength = 0;
}
const char *resetName() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    default: return "other";
    }
}
void save() {
    if (storageReady && dirty) {
        if (storage.putBytes("history", &history, sizeof(history)) == sizeof(history)) dirty = false;
        else Serial.println("[DEBUG] Flash log write failed");
    }
    lastSave = millis();
}
String escaped(const char *value) {
    String result;
    for (; *value; ++value) {
        if (*value == '"' || *value == '\\') result += '\\';
        if (static_cast<unsigned char>(*value) >= 32) result += *value;
    }
    return result;
}
}
void diagnosticsStage(const char *stage, uint32_t progress) {
    breadcrumb.magic = 0;
    breadcrumb.uptime = millis()/1000;
    breadcrumb.progress = progress;
    snprintf(breadcrumb.stage, sizeof(breadcrumb.stage), "%s", stage);
    breadcrumb.magic = kMagic;
}
void diagnosticLog(const char *format, ...) {
    char message[140];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    snprintf(history.lines[history.next], sizeof(history.lines[0]), "boot=%lu up=%lus %s",
             static_cast<unsigned long>(history.boots), millis()/1000, message);
    Serial.println(history.lines[history.next]);
    history.next = (history.next + 1) % kEntries;
    if (history.count < kEntries) ++history.count;
    dirty = true;
}
void diagnosticsBegin() {
    storageReady = EPD_PERSIST_DEBUG && storage.begin("epd-debug", false);
    if (storageReady && storage.getBytesLength("history") == sizeof(history))
        storage.getBytes("history", &history, sizeof(history));
    if (history.magic != kMagic || history.next >= kEntries || history.count > kEntries) {
        history = {};
        history.magic = kMagic;
    }
    // Even after a corrupt/incomplete record, all log strings remain bounded.
    for (auto &line : history.lines) line[sizeof(line)-1] = '\0';
    ++history.boots;
    diagnosticLog("BOOT reset=%s(%d) flash_logs=%s heap=%u", resetName(),
                  static_cast<int>(esp_reset_reason()), storageReady ? "on" : "off", ESP.getFreeHeap());
    if (esp_reset_reason() != ESP_RST_POWERON && esp_reset_reason() != ESP_RST_BROWNOUT && breadcrumb.magic == kMagic) {
        breadcrumb.stage[sizeof(breadcrumb.stage)-1] = '\0';
        diagnosticLog("PREVIOUS stage=%s up=%lus progress=%lu", breadcrumb.stage,
                      static_cast<unsigned long>(breadcrumb.uptime), static_cast<unsigned long>(breadcrumb.progress));
    }
    diagnosticsStage("boot");
    save();
}
String diagnosticsStatus() {
    return String("{\"boot\":") + history.boots + ",\"reset_reason\":\"" + resetName() +
        "\",\"uptime_seconds\":" + millis()/1000 + ",\"wifi_status\":" + static_cast<int>(WiFi.status()) +
        ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi_dbm\":" + WiFi.RSSI() +
        ",\"heap_free\":" + ESP.getFreeHeap() + ",\"heap_min\":" + ESP.getMinFreeHeap() +
        ",\"heap_largest\":" + ESP.getMaxAllocHeap() + ",\"stage\":\"" + breadcrumb.stage + "\",\"flash_logs\":" +
        (storageReady ? "true" : "false") + "}";
}
String diagnosticsJson() {
    String result = "{\"status\":" + diagnosticsStatus() + ",\"logs\":[";
    for (uint32_t i=0; i<history.count; ++i) {
        if (i) result += ',';
        result += '"';
        result += escaped(history.lines[(history.next+kEntries-history.count+i)%kEntries]);
        result += '"';
    }
    return result + "]}";
}
void diagnosticsTick() {
    if (millis()-lastHeartbeat >= 300000) {
        lastHeartbeat = millis();
        diagnosticLog("HEARTBEAT wifi=%d rssi=%d heap=%u min=%u largest=%u", static_cast<int>(WiFi.status()),
                      WiFi.RSSI(), ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    }
    // Bounded flash writes; the most recent minute may be lost on power failure.
    if (dirty && millis()-lastSave >= 60000) save();
    while (Serial.available()) {
        char c = Serial.read();
        lastSerialByte = millis();
        if (c == '\r' || c == '\n') {
            serialDispatch();
        } else if (commandLength < sizeof(serialCommand)-1) serialCommand[commandLength++] = c;
    }
    if (commandLength && millis()-lastSerialByte >= 250) serialDispatch();
}
