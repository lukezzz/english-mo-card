#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <cstring>
#include "config.h"
#include "epd_gdey042z98.h"

// Keep existing local config.h files compatible with the explicit MISO setting.
#ifndef EPD_PIN_MISO
#define EPD_PIN_MISO 9 // XIAO D9; no EPD wire is needed (display is write-only).
#endif

namespace
{
    constexpr size_t kFrameBytes = 400U * 300U / 4U;
    constexpr unsigned long kReconnectInterval = 10000;
    WebServer server(80);
    uint8_t frame[kFrameBytes];
    size_t received = 0;
    bool uploadOk = false;
    bool uploadAuthorized = false;
    bool panelSelected = false;
    bool serverStarted = false;
    unsigned long lastReconnect = 0;
    wl_status_t lastWifiStatus = WL_IDLE_STATUS;

    bool authorized()
    {
        if (strlen(EPD_API_TOKEN) == 0)
            return true;
        return server.header("Authorization") == String("Bearer ") + EPD_API_TOKEN;
    }
    void reply(int status, const char *message)
    {
        server.send(status, "application/json", message);
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
        Serial.println("[EPD] Panel selected: gdey042z98 / tricolor");
        reply(200, "{\"ok\":true}");
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
        Serial.println("[EPD] Refresh started");
        unsigned long started = millis();
        if (!GDEY042Z98_refreshLogicalFrame(frame, EPDColorMode::Tricolor))
        {
            Serial.println("[EPD] Refresh failed: see BUSY diagnostics above");
            reply(500, "{\"error\":\"EPD refresh failed; check serial BUSY diagnostics\"}");
            return;
        }
        Serial.printf("[EPD] Refresh finished in %lu ms\n", millis() - started);
        reply(200, "{\"ok\":true}");
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n[BOOT] Flash card EPD starting");
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
    server.on("/api/health", HTTP_GET, []
              { reply(200, "{\"ok\":true}"); });
    server.onNotFound([]
                      { Serial.printf("[HTTP] Not found: %s\n", server.uri().c_str()); reply(404, "{\"error\":\"not found\"}"); });
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastReconnect = millis();
    Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
}
void loop()
{
    wl_status_t status = WiFi.status();
    if (status != lastWifiStatus)
    {
        lastWifiStatus = status;
        if (status == WL_CONNECTED)
            Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
        else
            Serial.printf("[WiFi] Disconnected/status: %d\n", static_cast<int>(status));
    }
    if (status == WL_CONNECTED)
    {
        // Opening a socket before WiFi initializes lwIP causes Invalid mbox.
        if (!serverStarted)
        {
            server.begin();
            serverStarted = true;
            Serial.printf("[HTTP] Listening at http://%s:80\n", WiFi.localIP().toString().c_str());
        }
        server.handleClient();
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
            Serial.println("[WiFi] Reconnecting...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
    }
    delay(1);
}
