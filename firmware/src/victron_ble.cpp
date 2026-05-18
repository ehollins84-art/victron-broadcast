// Decoder for Victron "Instant Readout" BLE advertisements.
//
// Wire format (after the BLE manufacturer id 0x02E1):
//
//   offset  size  field
//   0       1     0x10 (prefix / record header)
//   1       2     model id (LE)
//   3       1     record type (see VictronRecordType)
//   4       2     nonce / IV counter (LE) -- used as low 16 bits of AES-CTR IV
//   6       1     first byte of the bind key (key-match check)
//   7       N     AES-128-CTR encrypted payload
//
// The encrypted payload is decoded per record type as a little-endian,
// LSB-first bit stream. See Victron's "Extra Manufacturer Data" PDF for
// the per-record bitfield layouts that are implemented below.

#include "victron_ble.h"
#include <math.h>
#include <string.h>
#include "mbedtls/aes.h"

namespace {

class BitReader {
public:
    BitReader(const uint8_t* buf, size_t len) : buf_(buf), bitLen_(len * 8), pos_(0) {}

    // Read `n` bits (n <= 32), LSB-first within each byte, little-endian across bytes.
    uint32_t read(uint8_t n) {
        uint32_t v = 0;
        for (uint8_t i = 0; i < n; ++i) {
            if (pos_ >= bitLen_) return v;
            uint32_t bit = (buf_[pos_ >> 3] >> (pos_ & 7)) & 1u;
            v |= (bit << i);
            ++pos_;
        }
        return v;
    }

    int32_t readSigned(uint8_t n) {
        uint32_t u = read(n);
        // sign-extend from n bits
        uint32_t signBit = 1u << (n - 1);
        if (u & signBit) {
            return (int32_t)(u | (~((1u << n) - 1)));
        }
        return (int32_t)u;
    }

    void skip(uint16_t n) { pos_ += n; }

private:
    const uint8_t* buf_;
    size_t bitLen_;
    size_t pos_;
};

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

bool macEqualsLower(const char* a, const char* b) {
    // both expected lowercase, but be lenient
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

} // namespace

bool VictronDecoder::hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
    for (size_t i = 0; i < outLen; ++i) {
        int hi = hexNibble(hex[2 * i]);
        int lo = hexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool VictronDecoder::aesCtrDecrypt(const uint8_t key[16], uint16_t nonceCounter,
                                   const uint8_t* in, uint8_t* out, size_t len) {
    // AES-128-CTR with IV = nonceCounter (LE, 2 bytes) followed by 14 zero bytes.
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    uint8_t nonceCounterBuf[16] = {0};
    nonceCounterBuf[0] = (uint8_t)(nonceCounter & 0xFF);
    nonceCounterBuf[1] = (uint8_t)((nonceCounter >> 8) & 0xFF);
    uint8_t streamBlock[16] = {0};
    size_t ncOff = 0;
    int rc = mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, nonceCounterBuf, streamBlock, in, out);
    mbedtls_aes_free(&ctx);
    return rc == 0;
}

static void resetSample(VictronSample& s) {
    s.modelId = 0;
    s.type = VictronRecordType::Unknown;
    s.batteryVoltage = NAN;
    s.batteryCurrent = NAN;
    s.soc = NAN;
    s.consumedAh = NAN;
    s.timeToGoMin = INT32_MIN;
    s.auxVoltage = NAN;
    s.temperatureC = NAN;
    s.midpointVoltage = NAN;
    s.chargerState = 0xFF;
    s.chargerError = 0xFF;
    s.solarPowerW = NAN;
    s.yieldTodayWh = NAN;
    s.loadCurrentA = NAN;
    s.acApparentPowerVA = NAN;
    s.acVoltage = NAN;
    s.acCurrent = NAN;
}

void VictronDecoder::parseBatteryMonitor(const uint8_t* p, size_t len, VictronSample& s) {
    // 16-byte payload, 128 bits, layout (LSB-first):
    //   16 bits  TTG (minutes), 0xFFFF = infinite/invalid
    //   16 bits  voltage,   signed, 0.01 V, 0x7FFF = invalid
    //   16 bits  alarm reason
    //   16 bits  aux,       signed, meaning depends on aux mode; 0x7FFF invalid
    //   22 bits  current,   signed, 0.001 A, 0x3FFFFF = invalid
    //   20 bits  consumed Ah, 0.1 Ah, 0xFFFFF = invalid
    //   10 bits  SoC, 0.1 %, 0x3FF = invalid
    //    3 bits  aux input mode (0=starter V, 1=midpoint V, 2=temperature, 3=disabled)
    //    9 bits  padding
    if (len < 16) return;
    BitReader br(p, len);
    uint16_t ttg     = br.read(16);
    int32_t  volt    = br.readSigned(16);
    /*uint16_t alarm =*/ br.read(16);
    int32_t  aux     = br.readSigned(16);
    int32_t  current = br.readSigned(22);
    uint32_t consAh  = br.read(20);
    uint16_t soc     = br.read(10);
    uint8_t  auxMode = (uint8_t)br.read(3);

    if (ttg != 0xFFFF) s.timeToGoMin = ttg;
    if ((volt & 0xFFFF) != 0x7FFF) s.batteryVoltage = volt * 0.01f;
    if (((uint32_t)current & 0x3FFFFF) != 0x3FFFFF) s.batteryCurrent = current * 0.001f;
    if (consAh != 0xFFFFF) s.consumedAh = consAh * -0.1f; // reported as positive Ah consumed
    if (soc != 0x3FF) s.soc = soc * 0.1f;
    if ((aux & 0xFFFF) != 0x7FFF) {
        switch (auxMode) {
            case 0: s.auxVoltage = aux * 0.01f; break;       // starter battery voltage
            case 1: s.midpointVoltage = aux * 0.01f; break;
            case 2: s.temperatureC = (aux / 100.0f) - 273.15f; break; // K -> C
            default: break;
        }
    }
}

void VictronDecoder::parseSolarCharger(const uint8_t* p, size_t len, VictronSample& s) {
    // ~12-byte payload, LSB-first:
    //    8 bits  device state
    //    8 bits  charger error
    //   16 bits  battery voltage, signed, 0.01 V
    //   16 bits  battery current, signed, 0.1 A
    //   16 bits  yield today, 0.01 kWh (= 10 Wh)
    //   16 bits  PV power, 1 W
    //    9 bits  load current, 0.1 A, 0x1FF = N/A
    if (len < 11) return;
    BitReader br(p, len);
    s.chargerState = (uint8_t)br.read(8);
    s.chargerError = (uint8_t)br.read(8);
    int32_t volt = br.readSigned(16);
    int32_t cur  = br.readSigned(16);
    uint16_t yield = br.read(16);
    uint16_t pv    = br.read(16);
    uint16_t load  = br.read(9);

    if ((volt & 0xFFFF) != 0x7FFF) s.batteryVoltage = volt * 0.01f;
    if ((cur  & 0xFFFF) != 0x7FFF) s.batteryCurrent = cur * 0.1f;
    if (yield != 0xFFFF) s.yieldTodayWh = yield * 10.0f;
    if (pv != 0xFFFF) s.solarPowerW = pv;
    if (load != 0x1FF) s.loadCurrentA = load * 0.1f;
}

void VictronDecoder::parseInverter(const uint8_t* p, size_t len, VictronSample& s) {
    //    8 bits  device state
    //   16 bits  alarm reason
    //   16 bits  battery voltage, signed, 0.01 V
    //   16 bits  AC apparent power, VA
    //   16 bits  AC voltage, signed, 0.01 V
    //   16 bits  AC current, 0.1 A
    if (len < 11) return;
    BitReader br(p, len);
    s.chargerState = (uint8_t)br.read(8);
    /*alarm=*/ br.read(16);
    int32_t batV = br.readSigned(16);
    uint16_t va  = br.read(16);
    int32_t acV  = br.readSigned(16);
    uint16_t acI = br.read(16);
    if ((batV & 0xFFFF) != 0x7FFF) s.batteryVoltage = batV * 0.01f;
    if (va != 0xFFFF) s.acApparentPowerVA = va;
    if ((acV & 0xFFFF) != 0x7FFF) s.acVoltage = acV * 0.01f;
    if (acI != 0xFFFF) s.acCurrent = acI * 0.1f;
}

void VictronDecoder::parseDcDcConverter(const uint8_t* p, size_t len, VictronSample& s) {
    //    8 bits  device state
    //    8 bits  charger error
    //   16 bits  input voltage, 0.01 V    -> stored as solar/PV-style "input"
    //   16 bits  output voltage, signed, 0.01 V -> battery voltage
    //   32 bits  off reason
    if (len < 10) return;
    BitReader br(p, len);
    s.chargerState = (uint8_t)br.read(8);
    s.chargerError = (uint8_t)br.read(8);
    /*int32_t inV =*/ br.readSigned(16);
    int32_t outV = br.readSigned(16);
    if ((outV & 0xFFFF) != 0x7FFF) s.batteryVoltage = outV * 0.01f;
}

bool VictronDecoder::decode(const uint8_t* manufData, size_t len,
                            const char* macLower,
                            const std::vector<VictronDevice>& devices,
                            VictronSample& out) {
    if (len < 8) return false;
    if (manufData[0] != 0x10) return false; // not an Instant Readout record

    // Match by MAC against configured devices.
    const VictronDevice* dev = nullptr;
    for (const auto& d : devices) {
        if (macEqualsLower(macLower, d.mac.c_str())) { dev = &d; break; }
    }
    if (!dev) return false;

    uint8_t key[16];
    if (!hexToBytes(dev->key.c_str(), key, 16)) return false;

    // Verify key prefix byte.
    if (manufData[6] != key[0]) return false;

    uint16_t modelId = manufData[1] | ((uint16_t)manufData[2] << 8);
    uint8_t  rtype   = manufData[3];
    uint16_t nonce   = manufData[4] | ((uint16_t)manufData[5] << 8);
    const uint8_t* enc = manufData + 7;
    size_t encLen = len - 7;
    if (encLen == 0 || encLen > 64) return false;

    uint8_t plain[64] = {0};
    if (!aesCtrDecrypt(key, nonce, enc, plain, encLen)) return false;

    resetSample(out);
    out.name = dev->name.c_str();
    out.modelId = modelId;
    out.type = (VictronRecordType)rtype;

    switch (out.type) {
        case VictronRecordType::BatteryMonitor:  parseBatteryMonitor(plain, encLen, out); break;
        case VictronRecordType::SolarCharger:    parseSolarCharger(plain, encLen, out);   break;
        case VictronRecordType::Inverter:        parseInverter(plain, encLen, out);       break;
        case VictronRecordType::DcDcConverter:   parseDcDcConverter(plain, encLen, out);  break;
        default:
            // Unsupported record type — still report that we saw the device.
            break;
    }
    return true;
}
