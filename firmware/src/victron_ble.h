#pragma once
#include <Arduino.h>
#include <stdint.h>

struct VictronDevice {
    const char* name;
    const char* mac;     // lowercase "aa:bb:cc:dd:ee:ff"
    const char* keyHex;  // 32 hex chars
};

enum class VictronRecordType : uint8_t {
    TestRecord       = 0x00,
    SolarCharger     = 0x01,
    BatteryMonitor   = 0x02,
    Inverter         = 0x03,
    DcDcConverter    = 0x04,
    SmartLithium     = 0x05,
    InverterRS       = 0x06,
    GxDevice         = 0x07,
    AcCharger        = 0x08,
    SmartBatteryProtect = 0x09,
    LynxSmartBMS     = 0x0A,
    MultiRS          = 0x0B,
    VeBus            = 0x0C,
    DcEnergyMeter    = 0x0D,
    OrionXS          = 0x0F,
    Unknown          = 0xFF,
};

// Decoded sample. Only fields valid for the record type are populated;
// the rest stay NAN / INT32_MIN to signal "not present".
struct VictronSample {
    const char*  name;
    VictronRecordType type;
    uint16_t     modelId;
    // Common-ish fields (in SI units; NAN when not applicable / invalid):
    float        batteryVoltage;     // V
    float        batteryCurrent;     // A (signed: + charging, - discharging)
    float        soc;                // %
    float        consumedAh;         // Ah
    int32_t      timeToGoMin;        // minutes, INT32_MIN if invalid / infinite
    float        auxVoltage;         // V (BMV aux input, if configured as voltage)
    float        temperatureC;       // C
    float        midpointVoltage;    // V
    // Solar charger:
    uint8_t      chargerState;       // 0xFF if N/A
    uint8_t      chargerError;       // 0xFF if N/A
    float        solarPowerW;        // W
    float        yieldTodayWh;       // Wh
    float        loadCurrentA;       // A, NAN if not present
    // Inverter:
    float        acApparentPowerVA;  // VA
    float        acVoltage;          // V
    float        acCurrent;          // A
};

class VictronDecoder {
public:
    // Try to decode a Victron BLE advertisement.
    // `manufData` is the raw payload that follows the manufacturer ID 0x02E1.
    // Returns true on success and fills `out`. Returns false if the data is
    // not a Victron Instant Readout, the device MAC is not configured, or
    // the encryption key prefix does not match.
    static bool decode(const uint8_t* manufData, size_t len,
                       const char* macLower,
                       const VictronDevice* devices, size_t deviceCount,
                       VictronSample& out);

private:
    static bool hexToBytes(const char* hex, uint8_t* out, size_t outLen);
    static bool aesCtrDecrypt(const uint8_t key[16], uint16_t nonceCounter,
                              const uint8_t* in, uint8_t* out, size_t len);
    static void parseBatteryMonitor(const uint8_t* p, size_t len, VictronSample& s);
    static void parseSolarCharger(const uint8_t* p, size_t len, VictronSample& s);
    static void parseInverter(const uint8_t* p, size_t len, VictronSample& s);
    static void parseDcDcConverter(const uint8_t* p, size_t len, VictronSample& s);
};
