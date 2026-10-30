#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <cstring>
#include "config.h"
#include "epd_gdey042z98.h"
#include "diagnostics.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>

// Keep existing local config.h files compatible with the explicit MISO setting.
#ifndef EPD_PIN_MISO
#define EPD_PIN_MISO 9 // XIAO D9; no EPD wire is needed (display is write-only).
#endif

namespace
{
    constexpr size_t kFrameBytes = 400U * 300U / 4U;
    constexpr unsigned long kReconnectInterval = 30000;
    struct NetworkEvent { WiFiEvent_t type; uint8_t reason; };
    QueueHandle_t networkEvents = nullptr;
    unsigned int reconnectAttempts = 0;
    WebServer server(80);
    uint8_t frame[kFrameBytes];
    size_t received = 0;
    bool uploadOk = false;
    bool uploadAuthorized = false;
    bool panelSelected = false;
    bool serverStarted = false;
    unsigned long lastReconnect = 0;
    wl_status_t lastWifiStatus = WL_IDLE_STATUS;
    bool loopWatchdog = false;

    bool authorized()
    {
        if (strlen(EPD_API_TOKEN) == 0)
            return true;
        return server.header("Authorization") == String("Bearer ") + EPD_API_TOKEN;
    }
    void reply(int status, const char *message)
    {
        diagnosticsStage("http_reply", status);
        server.send(status, "application/json", message);
        diagnosticsStage("http_reply_done", status);
    }
    void handlePanel()
    {
        Serial.println("[HTTP] POST /api/panel");
        if (!authorized())
        {
            Serial.println("[HTTP] Unauthorized");
            reply(401, "{\"error\":\"unauthorized\"}");
            return;
        }
        if (server.arg("panelType") != "gdey042z98" || server.arg("colorMode") != "tricolor")
        {
            panelSelected = false;
            Serial.printf("[HTTP] Unsupported panel: %s / %s\n", server.arg("panelType").c_str(), server.arg("colorMode").c_str());
            reply(400, "{\"error\":\"unsupported panel\"}");
            return;
        }
        panelSelected = true;
        diagnosticLog("PANEL selected; sending response");
        reply(200, "{\"ok\":true}");
        diagnosticLog("PANEL response sent");
    }
    void handleUpload()
    {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START)
        {
            received = 0;
            uploadAuthorized = authorized();
            uploadOk = uploadAuthorized && panelSelected && upload.name == "frame";
            Serial.printf("[HTTP] Upload start: field=%s, file=%s, allowed=%s\n", upload.name.c_str(), upload.filename.c_str(), uploadOk ? "yes" : "no");
        }
        else if (upload.status == UPLOAD_FILE_WRITE)
        {
            Serial.printf("[HTTP] Received chunk: %u bytes, total %u/%u\n", static_cast<unsigned>(upload.currentSize), static_cast<unsigned>(received + upload.currentSize), static_cast<unsigned>(kFrameBytes));
            if (upload.currentSize > kFrameBytes - received)
                uploadOk = false;
            else if (uploadOk)
            {
                memcpy(frame + received, upload.buf, upload.currentSize);
                received += upload.currentSize;
            }
        }
        else if (upload.status == UPLOAD_FILE_END)
        {
            Serial.printf("[HTTP] Upload end: %u bytes; valid=%s\n", static_cast<unsigned>(received), uploadOk && received == kFrameBytes ? "yes" : "no");
        }
        else if (upload.status == UPLOAD_FILE_ABORTED)
        {
            uploadOk = false;
            Serial.println("[HTTP] Upload aborted");
        }
    }
    void handleFrame()
    {
        if (!authorized())
        {
            reply(401, "{\"error\":\"unauthorized\"}");
            return;
        }
        if (!uploadOk || received != kFrameBytes)
        {
            Serial.println("[HTTP] Frame rejected: missing, invalid or incomplete upload");
            reply(400, "{\"error\":\"invalid frame\"}");
            return;
        }
        uploadOk = false;
        diagnosticLog("REFRESH start heap=%u min=%u rssi=%d", ESP.getFreeHeap(), ESP.getMinFreeHeap(), WiFi.RSSI());
        diagnosticsStage("epd_refresh");
        diagnosticsTick();
        unsigned long started = millis();
        if (!GDEY042Z98_refreshLogicalFrame(frame, EPDColorMode::Tricolor))
        {
            diagnosticLog("REFRESH failed BUSY diagnostics; elapsed=%lums heap=%u", millis()-started, ESP.getFreeHeap());
            reply(500, "{\"error\":\"EPD refresh failed; check serial BUSY diagnostics\"}");
            return;
        }
        diagnosticLog("REFRESH done elapsed=%lums heap=%u min=%u wifi=%d", millis()-started,
                      ESP.getFreeHeap(), ESP.getMinFreeHeap(), static_cast<int>(WiFi.status()));
        reply(200, "{\"ok\":true}");
    }

    class RawFrameHandler : public RequestHandler {
        unsigned long startedAt = 0;
    public:
        bool canHandle(HTTPMethod method, String uri) override {
            return method == HTTP_POST && uri == "/api/frame/raw";
        }
        bool canRaw(String uri) override { return uri == "/api/frame/raw"; }
        void raw(WebServer &web, String, HTTPRaw &data) override {
            if (data.status == RAW_START) {
                received = 0;
                startedAt = millis();
                uploadOk = authorized() && panelSelected && web.clientContentLength() == kFrameBytes;
                // WiFiClient uses seconds here. Unlike multipart _uploadReadByte,
                // raw reads abort on read timeout instead of waiting forever.
                web.client().setTimeout(2);
                diagnosticsStage("raw_upload");
                diagnosticLog("RAW upload start length=%d allowed=%d", web.clientContentLength(), uploadOk);
                if (!uploadOk) web.client().stop();
            } else if (data.status == RAW_WRITE) {
                if (!uploadOk || data.currentSize > kFrameBytes-received || millis()-startedAt > 15000) {
                    uploadOk = false;
                    diagnosticLog("RAW upload rejected/timeout bytes=%u", static_cast<unsigned>(received));
                    web.client().stop();
                    return;
                }
                memcpy(frame+received, data.buf, data.currentSize);
                received += data.currentSize;
                diagnosticsStage("raw_upload", received);
                Serial.printf("[HTTP] Raw chunk: %u bytes, total %u/30000\n", static_cast<unsigned>(data.currentSize), static_cast<unsigned>(received));
            } else if (data.status == RAW_ABORTED) {
                uploadOk = false;
                diagnosticLog("RAW upload aborted/idle timeout bytes=%u", static_cast<unsigned>(received));
            } else if (data.status == RAW_END) {
                uploadOk = uploadOk && received == kFrameBytes;
                diagnosticLog("RAW upload end bytes=%u valid=%d", static_cast<unsigned>(received), uploadOk);
            }
        }
        bool handle(WebServer &, HTTPMethod, String) override { handleFrame(); return true; }
    };
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n[BOOT] Flash card EPD starting");
    diagnosticsBegin();
    if (esp_task_wdt_init(180, true) == ESP_OK && esp_task_wdt_add(nullptr) == ESP_OK) {
        loopWatchdog = true;
        diagnosticLog("Loop watchdog enabled: 180s");
    } else diagnosticLog("ERROR: loop watchdog could not be enabled");
    pinMode(EPD_PIN_CS, OUTPUT);
    digitalWrite(EPD_PIN_CS, HIGH);
    pinMode(EPD_PIN_DC, OUTPUT);
    pinMode(EPD_PIN_RST, OUTPUT);
    digitalWrite(EPD_PIN_RST, HIGH);
    pinMode(EPD_PIN_BUSY, INPUT);
    Serial.printf("[EPD] Pins: BUSY=%d RST=%d DC=%d CS=%d; initial BUSY=%d\n",
                  EPD_PIN_BUSY, EPD_PIN_RST, EPD_PIN_DC, EPD_PIN_CS, digitalRead(EPD_PIN_BUSY));
    SPI.begin(EPD_PIN_SCK, EPD_PIN_MISO, EPD_PIN_MOSI, EPD_PIN_CS);
    SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    Serial.printf("[SPI] Ready: SCK=%d, MOSI=%d, MISO=%d (unconnected), CS=%d\n",
                  EPD_PIN_SCK, EPD_PIN_MOSI, EPD_PIN_MISO, EPD_PIN_CS);
    const char *headers[] = {"Authorization"};
    server.collectHeaders(headers, 1);
    server.on("/api/panel", HTTP_POST, handlePanel);
    server.on("/api/frame", HTTP_POST, handleFrame, handleUpload);
    server.addHandler(new RawFrameHandler());
    server.on("/api/health", HTTP_GET, []
              { reply(200, "{\"ok\":true}"); });
    server.on("/api/status", HTTP_GET, [] {
        if (!authorized()) { reply(401, "{\"error\":\"unauthorized\"}"); return; }
        server.send(200, "application/json", diagnosticsStatus());
    });
    server.on("/api/debug", HTTP_GET, [] {
        if (!authorized()) { reply(401, "{\"error\":\"unauthorized\"}"); return; }
        server.send(200, "application/json", diagnosticsJson());
    });
    server.onNotFound([]
                      { Serial.printf("[HTTP] Not found: %s\n", server.uri().c_str()); reply(404, "{\"error\":\"not found\"}"); });
    networkEvents = xQueueCreate(16, sizeof(NetworkEvent));
    if (!networkEvents) diagnosticLog("WiFi event queue allocation failed");
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        // Arduino invokes this callback from another task. Only enqueue here;
        // HTTP state, logging and flash writes belong to the main loop.
        if (networkEvents && (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED ||
            event == ARDUINO_EVENT_WIFI_STA_LOST_IP || event == ARDUINO_EVENT_WIFI_STA_GOT_IP)) {
            NetworkEvent entry = {event, static_cast<uint8_t>(event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED
                                     ? info.wifi_sta_disconnected.reason : 0)};
            xQueueSend(networkEvents, &entry, 0);
        }
    });
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastReconnect = millis();
    Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
}
void loop()
{
    if (loopWatchdog) esp_task_wdt_reset();
    diagnosticsStage("loop");
    NetworkEvent event;
    while (networkEvents && xQueueReceive(networkEvents, &event, 0) == pdTRUE) {
        if (event.type == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
            diagnosticLog("WiFi DISCONNECTED reason=%u heap=%u", event.reason, ESP.getFreeHeap());
        else if (event.type == ARDUINO_EVENT_WIFI_STA_LOST_IP)
            diagnosticLog("WiFi LOST_IP");
        else
            diagnosticLog("WiFi GOT_IP ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        // Rebind even if a disconnect/reconnect happened entirely during refresh.
        if (serverStarted) { server.stop(); serverStarted = false; uploadOk = false; }
    }
    wl_status_t status = WiFi.status();
    if (status != lastWifiStatus)
    {
        lastWifiStatus = status;
        if (status == WL_CONNECTED)
            Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
        else
            Serial.printf("[WiFi] Disconnected/status: %d\n", static_cast<int>(status));
    }
    if (status == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0))
    {
        reconnectAttempts = 0;
        lastReconnect = millis();
        // Opening a socket before WiFi initializes lwIP causes Invalid mbox.
        if (!serverStarted)
        {
            server.begin();
            serverStarted = true;
            Serial.printf("[HTTP] Listening at http://%s:80\n", WiFi.localIP().toString().c_str());
        }
        diagnosticsStage("http_handle_client");
        server.handleClient();
        diagnosticsStage("http_handle_done");
    }
    else
    {
        if (serverStarted)
        {
            server.stop();
            serverStarted = false;
            uploadOk = false;
            Serial.println("[HTTP] Stopped while WiFi is disconnected");
        }
        if (millis() - lastReconnect >= kReconnectInterval)
        {
            lastReconnect = millis();
            ++reconnectAttempts;
            if (reconnectAttempts % 4 == 0) {
                diagnosticLog("WiFi recovery attempt=%u: restart radio", reconnectAttempts);
                WiFi.mode(WIFI_OFF);
                delay(100);
                WiFi.mode(WIFI_STA);
                WiFi.setSleep(false);
                WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            } else {
                diagnosticLog("WiFi reconnect attempt=%u status=%d", reconnectAttempts, static_cast<int>(status));
                WiFi.reconnect();
            }
        }
    }
    diagnosticsStage("diagnostics");
    diagnosticsTick();
    delay(1);
}
