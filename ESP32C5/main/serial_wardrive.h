#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_console.h"
struct cJSON;
typedef int (*sw_job_operation_handler_t)(const struct cJSON *request, const char *token);
typedef int (*sw_job_operation_stop_t)(const char *token);
typedef bool (*sw_radio_busy_probe_t)(void);
bool sw_active(void);
bool sw_hs_mode(void);
bool sw_wifi_only_mode(void);
bool sw_ble_only_mode(void);
bool sw_stop(void);
void sw_ble(const uint8_t *addr, uint8_t type, int rssi, uint8_t event, const uint8_t *data, size_t len);
void sw_register(void);
esp_err_t sw_register_command(const esp_console_cmd_t *cmd);
/* Implemented by main.c; invoked only by the serial session owner. */
bool sw_radio_start(void);
void sw_radio_stop(void);
void sw_radio_hop(unsigned index);

bool sw_prepare(void);
/* Extension seam for operation-specific wrappers that share the v3 owner. */
bool sw_register_job_operation(const char *operation, sw_job_operation_handler_t handler);
bool sw_register_job_operation_ex(const char *operation, sw_job_operation_handler_t start_handler, sw_job_operation_stop_t stop_handler);
void sw_register_radio_busy_probe(sw_radio_busy_probe_t probe);
const char *sw_job_token(void);
bool sw_job_owned(void);
bool sw_job_owner_matches(const char *token);
bool sw_job_lease_valid(void);
bool sw_job_set_radio_plan(const uint8_t *channels, size_t count, unsigned dwell_ms);
bool sw_job_set_filters(const uint8_t targets[][6], size_t target_count,
                        const uint8_t exclusions[][6], size_t exclusion_count);
bool sw_job_radio_hop(unsigned index, uint8_t *applied_channel);
bool sw_job_apply_channel(uint8_t channel);
bool sw_job_channel_allowed(uint8_t channel);
size_t sw_job_copy_channels(uint8_t *channels, size_t capacity);
unsigned sw_job_dwell_ms(void);
/* json_fields is a comma-separated sequence of JSON properties without braces. */
void sw_job_emit_data(const char *data_kind, const char *json_fields);
void sw_job_emit_legacy_line(const char *line);
void sw_job_emit_lifecycle(const char *kind, const char *state, const char *json_fields);
void sw_job_remember_terminal(void);
/* Account a managed-operation capacity drop in the v3 losses object. */
void sw_job_note_saturation(void);
