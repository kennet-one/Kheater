#include "heater_climate_policy.h"
#include <string.h>

bool climate_aht30_decode(const uint8_t b[7], int32_t *t, int32_t *h)
{
    if (!b || !t || !h || (b[0] & 0x80) || (b[0] & 0x18) != 0x18) return false;
    uint8_t crc = 0xff;
    for (unsigned i = 0; i < 6; ++i) {
        crc ^= b[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    if (crc != b[6]) return false;
    uint32_t rh = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4);
    uint32_t raw_t = ((uint32_t)(b[3] & 15) << 16) | ((uint32_t)b[4] << 8) | b[5];
    *h = (int32_t)(((uint64_t)rh * 10000 + 524288) >> 20);
    *t = (int32_t)(((uint64_t)raw_t * 20000 + 524288) >> 20) - 5000;
    return *h >= 0 && *h <= 10000 && *t >= -4000 && *t <= 8000;
}

bool climate_configure(climate_policy_t *p, uint32_t rev, bool enabled, uint64_t mac)
{
    if (!p || !rev || (enabled && (!mac || mac > 0xffffffffffffULL))) return false;
    if (p->configured && rev == p->revision)
        return p->enabled == enabled && p->source_mac == mac;
    if (p->configured && (int32_t)(rev - p->revision) <= 0) return false;
    memset(&p->external, 0, sizeof(p->external));
    p->configured = true;
    p->revision = rev;
    p->enabled = enabled;
    p->source_mac = mac;
    p->confirmations = 0;
    p->session = p->generation = 0;
    p->last_epoch_ms = 0;
    return true;
}

bool climate_external(climate_policy_t *p, uint32_t rev, uint32_t session,
                      uint32_t gen, uint64_t epoch, int32_t t,
                      uint64_t wall, uint64_t now, uint32_t freshness)
{
    if (!p || !p->configured || !p->enabled || rev != p->revision || !session) return false;
    /* Replays never refresh deadlines, including after a source-session change. */
    if (epoch <= p->last_epoch_ms ||
        (session == p->session && (int32_t)(gen - p->generation) <= 0)) return false;
    if (wall < 1700000000000ULL || epoch > wall || wall - epoch >= freshness ||
        t < -4000 || t > 8000) {
        p->external.valid = false;
        p->confirmations = 0;
        return false;
    }
    if (session != p->session || !p->external.valid || now >= p->external.expires_ms)
        p->confirmations = 0;
    p->session = session;
    p->generation = gen;
    p->last_epoch_ms = epoch;
    p->external = (climate_sample_t){ .valid = true, .temperature = t,
        .sampled_ms = now >= wall - epoch ? now - (wall - epoch) : 0,
        .expires_ms = now + freshness - (wall - epoch) };
    if (p->confirmations < 2) ++p->confirmations;
    return true;
}

climate_source_t climate_select(climate_policy_t *p, uint64_t now)
{
    if (!p) return CLIMATE_NONE;
    if (p->external.valid && now >= p->external.expires_ms) {
        p->external.valid = false;
        p->confirmations = 0;
    }
    if (p->enabled && p->external.valid && p->confirmations >= 2) return CLIMATE_ZONE;
    if (p->local.valid && now < p->local.expires_ms) return CLIMATE_INTERNAL;
    return CLIMATE_NONE;
}
