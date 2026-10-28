#include "epd_gdey042z98.h"
#include "Display_EPD_W21_spi.h"

namespace
{
    constexpr uint16_t kWidth = 400;
    constexpr uint16_t kHeight = 300;
    constexpr size_t kPlaneBytes = kWidth * kHeight / 8U;

    constexpr uint8_t kLogicalRed = 0x02;
    constexpr uint8_t kLogicalBlack = 0x03;

    bool waitBusy(const char *phase)
    {
        unsigned long startedAt = millis();
        Serial.printf("[EPD42Z98] %s: BUSY=%d\n", phase, isEPD_W21_BUSY);
        // 4.2" tri-color is 1=BUSY, 0=IDLE
        while (isEPD_W21_BUSY == HIGH)
        {
            delay(1);
            if (millis() - startedAt > 35000)
            {
                Serial.printf("[EPD42Z98] %s: BUSY timeout (>35s)! Check wiring.\n", phase);
                return false;
            }
        }
        Serial.printf("[EPD42Z98] %s: idle after %lu ms\n", phase, millis() - startedAt);
        return true;
    }

    bool initFull()
    {
        Serial.println("[EPD42Z98] Init full refresh...");
        EPD_W21_RST_0;
        delay(10);
        EPD_W21_RST_1;
        delay(10);

        if (!waitBusy("hardware reset")) return false;
        EPD_W21_WriteCMD(0x12); // SWRESET
        if (!waitBusy("software reset")) return false;

        EPD_W21_WriteCMD(0x01); // Driver output control
        EPD_W21_WriteDATA((kHeight - 1) % 256);
        EPD_W21_WriteDATA((kHeight - 1) / 256);
        EPD_W21_WriteDATA(0x00);

        EPD_W21_WriteCMD(0x11); // data entry mode
        EPD_W21_WriteDATA(0x01);

        EPD_W21_WriteCMD(0x44); // set Ram-X address start/end position
        EPD_W21_WriteDATA(0x00);
        EPD_W21_WriteDATA(kWidth / 8 - 1);

        EPD_W21_WriteCMD(0x45); // set Ram-Y address start/end position
        EPD_W21_WriteDATA((kHeight - 1) % 256);
        EPD_W21_WriteDATA((kHeight - 1) / 256);
        EPD_W21_WriteDATA(0x00);
        EPD_W21_WriteDATA(0x00);

        EPD_W21_WriteCMD(0x3C); // BorderWavefrom
        EPD_W21_WriteDATA(0x05);

        EPD_W21_WriteCMD(0x18); // Read built-in temperature sensor
        EPD_W21_WriteDATA(0x80);

        EPD_W21_WriteCMD(0x4E); // set RAM x address count to 0;
        EPD_W21_WriteDATA(0x00);
        EPD_W21_WriteCMD(0x4F); // set RAM y address count to 0;
        EPD_W21_WriteDATA((kHeight - 1) % 256);
        EPD_W21_WriteDATA((kHeight - 1) / 256);
        if (!waitBusy("RAM setup")) return false;
        return true;
    }

    bool updateDisplay()
    {
        EPD_W21_WriteCMD(0x22);
        EPD_W21_WriteDATA(0xF7);
        EPD_W21_WriteCMD(0x20);
        // A disconnected/misassigned BUSY input can stay LOW forever. Confirm
        // refresh actually starts before treating LOW as completion or sleeping.
        const unsigned long startedAt = millis();
        while (isEPD_W21_BUSY != HIGH)
        {
            if (millis() - startedAt >= 2000)
            {
                Serial.println("[EPD42Z98] Refresh did not assert BUSY within 2s; check pins, power and SPI wiring.");
                return false;
            }
            delay(1);
        }
        Serial.printf("[EPD42Z98] Refresh BUSY asserted after %lu ms\n", millis() - startedAt);
        return waitBusy("display refresh");
    }

    void deepSleep()
    {
        Serial.println("[EPD42Z98] Deep sleep...");
        EPD_W21_WriteCMD(0x10);
        EPD_W21_WriteDATA(0x01);
        delay(100);
    }

    uint8_t logicalCodeAt(const uint8_t *logicalFrame, size_t pixelIndex)
    {
        // 修正：将 pixelIndex 转换为坐标 (x, y)，然后进行上下翻转
        uint16_t x = pixelIndex % kWidth;
        uint16_t y = pixelIndex / kWidth;

        // 上下翻转：y' = (height - 1) - y
        uint16_t flippedY = (kHeight - 1) - y;
        size_t flippedIndex = flippedY * kWidth + x;

        const uint8_t packedByte = logicalFrame[flippedIndex / 4U];
        const uint8_t shift = 6U - static_cast<uint8_t>((flippedIndex % 4U) * 2U);
        return (packedByte >> shift) & 0x03U;
    }
} // namespace

bool GDEY042Z98_refreshLogicalFrame(const uint8_t *logicalFrame, EPDColorMode mode)
{
    (void)mode;
    if (!initFull()) return false;

    // BW RAM (0x24): 0=Black, 1=White/Red
    EPD_W21_WriteCMD(0x24);
    for (size_t byteIndex = 0; byteIndex < kPlaneBytes; ++byteIndex)
    {
        uint8_t bwByte = 0;
        for (uint8_t bit = 0; bit < 8; ++bit)
        {
            const size_t pixelIndex = byteIndex * 8U + bit;
            const uint8_t code = logicalCodeAt(logicalFrame, pixelIndex);
            // If code is black (0x03), set bit to 0. Else set to 1.
            if (code != kLogicalBlack)
            {
                bwByte |= static_cast<uint8_t>(0x80U >> bit);
            }
        }
        EPD_W21_WriteDATA(bwByte);
        if (byteIndex % 100 == 0)
            Sys_run();
    }

    // RED RAM (0x26): 1=Red, 0=Non-Red
    EPD_W21_WriteCMD(0x26);
    for (size_t byteIndex = 0; byteIndex < kPlaneBytes; ++byteIndex)
    {
        uint8_t redByte = 0;
        for (uint8_t bit = 0; bit < 8; ++bit)
        {
            const size_t pixelIndex = byteIndex * 8U + bit;
            const uint8_t code = logicalCodeAt(logicalFrame, pixelIndex);
            // If code is red (0x02), set bit to 1. Else set to 0.
            if (code == kLogicalRed)
            {
                redByte |= static_cast<uint8_t>(0x80U >> bit);
            }
        }
        EPD_W21_WriteDATA(redByte);
        if (byteIndex % 100 == 0)
            Sys_run();
    }

    if (!updateDisplay()) return false;
    deepSleep();
    return true;
}
