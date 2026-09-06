#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { CLIMATE_NONE, CLIMATE_INTERNAL, CLIMATE_ZONE } climate_source_t;
typedef struct {
    bool valid;
    int32_t temperature, humidity;
    uint64_t sampled_ms, expires_ms;
} climate_sample_t;
typedef struct {
    climate_sample_t local, external;
    uint32_t revision, session, generation;
    uint64_t source_mac, last_epoch_ms;
    bool configured, enabled;
    uint8_t confirmations;
} climate_policy_t;

bool climate_aht30_decode(const uint8_t bytes[7], int32_t *temperature, int32_t *humidity);
bool climate_configure(climate_policy_t *p, uint32_t revision, bool enabled, uint64_t mac);
bool climate_external(climate_policy_t *p, uint32_t revision, uint32_t session,
                      uint32_t generation, uint64_t sampled_epoch_ms, int32_t temperature,
                      uint64_t now_epoch_ms, uint64_t now_ms, uint32_t freshness_ms);
climate_source_t climate_select(climate_policy_t *p, uint64_t now_ms);
bool climate_selftest(void);
