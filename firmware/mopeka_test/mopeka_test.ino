// Quick test: listen for EyeGas Pro / Mopeka Pro Check BLE adverts and print readings.
// Board: any ESP32. Arduino IDE -> Tools -> Board -> ESP32 Dev Module. Serial Monitor at 115200.

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

BLEScan *scan;

// Converts the raw time-of-flight value to mm in LPG (temperature-compensated).
// Coefficients from the open-source Home Assistant Mopeka parser.
float rawToMm(uint16_t raw, uint8_t rawTemp) {
  const float c0 = 0.573045f, c1 = -0.002822f, c2 = -0.00000535f;
  return raw * (c0 + c1 * rawTemp + c2 * rawTemp * rawTemp);
}

class Callbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    if (!dev.haveManufacturerData()) return;
    auto md = dev.getManufacturerData();          // first 2 bytes = company ID
    if (md.length() != 12) return;
    if ((uint8_t)md[0] != 0x59 || (uint8_t)md[1] != 0x00) return;  // Nordic / Mopeka

    const uint8_t *d = (const uint8_t *)md.c_str() + 2;  // 10 bytes of sensor data

    uint8_t hwId     = d[0] & 0x7F;
    float   voltage  = (d[1] & 0x7F) / 32.0f;
    float   battPct  = constrain((voltage - 2.2f) / 0.65f * 100.0f, 0.0f, 100.0f);
    uint8_t rawTemp  = d[2] & 0x7F;
    int     tempC    = (int)rawTemp - 40;
    bool    syncBtn  = d[2] & 0x80;
    uint16_t rawLvl  = ((d[4] << 8) | d[3]) & 0x3FFF;
    uint8_t quality  = d[4] >> 6;                 // 0 = no reading ... 3 = best
    float   levelMm  = rawToMm(rawLvl, rawTemp);

    Serial.printf("%s | hw 0x%02X | level %.0f mm (raw %u, quality %u/3) | temp %d C | batt %.2f V (%.0f%%) | RSSI %d%s\n",
                  dev.getAddress().toString().c_str(), hwId, levelMm, rawLvl, quality,
                  tempC, voltage, battPct, dev.getRSSI(), syncBtn ? " | SYNC PRESSED" : "");
  }
};

void setup() {
  Serial.begin(115200);
  delay(3000);   // give the S3's native USB time to connect before printing
  Serial.println("Scanning for Mopeka / EyeGas sensors...");
  BLEDevice::init("");
  scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new Callbacks(), true);  // true = report duplicates
  scan->setActiveScan(false);   // passive listening is enough
  scan->setInterval(100);
  scan->setWindow(99);
}

void loop() {
  Serial.println("--- scanning 5 s ---");   // heartbeat: proves Serial works even if no sensor is found
  scan->start(5, false);
  scan->clearResults();
}
