#include "heater_climate_policy.h"
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return false; } while (0)

bool climate_selftest(void)
{
    uint8_t frame[7] = {0x18, 0x80, 0, 0x06, 0, 0, 0}; /* 25 C / 50% */
    uint8_t crc = 0xff;
    for (unsigned i = 0; i < 6; ++i) {
        crc ^= frame[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    frame[6] = crc;
    int32_t t, h;
    CHECK(climate_aht30_decode(frame, &t, &h) && t == 2500 && h == 5000);
    frame[6] ^= 1;
    CHECK(!climate_aht30_decode(frame, &t, &h));
    frame[6] ^= 1; frame[0] |= 0x80;
    CHECK(!climate_aht30_decode(frame, &t, &h));
    frame[0] = 0;
    CHECK(!climate_aht30_decode(frame, &t, &h));

    climate_policy_t p = {0};
    CHECK(climate_select(&p, 100) == CLIMATE_NONE);
    p.local = (climate_sample_t){ .valid = true, .temperature = 2500, .expires_ms = 10000 };
    CHECK(climate_select(&p, 100) == CLIMATE_INTERNAL);
    CHECK(climate_select(&p, 10000) == CLIMATE_NONE);
    CHECK(!climate_configure(&p, 0, true, 0x1234));
    CHECK(!climate_configure(&p, 1, true, 0));
    CHECK(climate_configure(&p, 1, true, 0x1234));
    uint64_t wall = 1800000000000ULL;
    CHECK(climate_external(&p, 1, 8, 1, wall, 2200, wall, 100, 30000));
    CHECK(climate_select(&p, 100) == CLIMATE_INTERNAL);
    CHECK(!climate_external(&p, 1, 8, 1, wall, 2200, wall + 1, 101, 30000));
    CHECK(p.confirmations == 1 && p.external.expires_ms == 30100);
    CHECK(climate_external(&p, 1, 8, 2, wall + 2000, 2100, wall + 2000, 2100, 30000));
    CHECK(climate_select(&p, 2100) == CLIMATE_ZONE);
    CHECK(climate_configure(&p, 1, true, 0x1234) && p.confirmations == 2);
    CHECK(!climate_configure(&p, 1, true, 0x5678));
    CHECK(!climate_external(&p, 1, 9, 1, wall, 2200, wall + 2000, 2100, 30000));
    CHECK(climate_select(&p, 32100) == CLIMATE_NONE && p.confirmations == 0);
    CHECK(climate_external(&p, 1, 8, 3, wall + 33000, 2200, wall + 33000, 33100, 30000));
    CHECK(p.confirmations == 1);
    CHECK(!climate_external(&p, 1, 8, 4, wall + 33001, 9000, wall + 33001, 33101, 30000));
    CHECK(!p.external.valid && p.confirmations == 0);
    CHECK(!climate_external(&p, 1, 8, 5, wall + 33002, 2200, wall + 63002, 63102, 30000));
    CHECK(!climate_external(&p, 1, 8, 6, wall + 90000, 2200, wall + 63002, 63102, 30000));
    CHECK(climate_configure(&p, 2, false, 0));
    CHECK(!climate_configure(&p, 1, true, 0x1234));
    CHECK(!climate_external(&p, 1, 8, 7, wall + 70000, 2200, wall + 70000, 70100, 30000));
    p = (climate_policy_t){0};
    CHECK(climate_select(&p, 0) == CLIMATE_NONE && !p.enabled);
    return true;
}

#ifdef CLIMATE_HOST_TEST
#include <stdio.h>
int main(void)
{
    bool ok = climate_selftest();
    puts(ok ? "PASS climate CRC/freshness/fallback/replay/reboot" : "FAIL climate policy");
    return ok ? 0 : 1;
}
#endif
