#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define KHEATER_LEGACY_REPLY_MAX 4
#define KHEATER_LEGACY_REPLY_LEN 32
#define KHEATER_COMMAND_RESULT_LEN 160

typedef struct {
	esp_err_t error;
	size_t reply_count;
	char replies[KHEATER_LEGACY_REPLY_MAX][KHEATER_LEGACY_REPLY_LEN];
	char result[KHEATER_COMMAND_RESULT_LEN];
} kheater_command_result_t;

bool legacy_execute_command(const char *text, kheater_command_result_t *result);
bool legacy_handle_command(const char *text);
void legacy_handle_text(const char *text);
void legacy_format_status_token(char *out, size_t out_size);
void legacy_format_schedule_meta_token(char *out, size_t out_size);
void legacy_format_schedule_diagnostic_token(char *out, size_t out_size);
