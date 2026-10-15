/* Guard the full shipped C5 device_info catalog against its 1000-byte bound. */
#include "../../ESP32C5/main/serial_wardrive.c"

bool uota_busy(void) { return false; }
bool sw_prepare(void) { return true; }
bool sw_radio_start(void) { return true; }
bool sw_radio_stop(void) { return true; }
void sw_radio_hop(unsigned index) { (void)index; }

static int operation_start(const cJSON *request, const char *token) {
    (void)request; (void)token; return SW_JOB_START_READY;
}
static int operation_stop(const char *token) { (void)token; return 0; }

static size_t record_length(void) {
    const char *record = strstr(output_capture, "WDG:");
    assert(record);
    return strcspn(record + 4, "\r\n");
}

int main(void) {
    static const char *const shipped_operations[] = {
        "handshake", "deauth", "sae", "blackout", "evil_twin",
        "portal", "sniffer", "bt_track", "airtag_scan"
    };
    char expected[512] = "\"wardrive\",\"wifi_scan\",\"ble_scan\",\"hs_sniff\"";
    size_t used = strlen(expected);

    sw_register();
    assert(ensure_identity());
    for (size_t i = 0; i < sizeof(shipped_operations) / sizeof(shipped_operations[0]); i++) {
        assert(sw_register_job_operation_ex(shipped_operations[i], operation_start, operation_stop));
        int n = snprintf(expected + used, sizeof(expected) - used, ",\"%s\"", shipped_operations[i]);
        assert(n > 0 && (size_t)n < sizeof(expected) - used);
        used += (size_t)n;
    }

    sw_set_boot_state(false, "initializing", false);
    output_capture[0] = 0;
    assert(device_info(1, NULL) == 0);
    assert(strstr(output_capture, "\"boot_state\":\"degraded\"") &&
           strstr(output_capture, "\"boot_reason\":\"initializing\"") &&
           strstr(output_capture, "\"capabilities\":{\"c5_jobs_v1\":false}") &&
           strstr(output_capture, "\"operations\":[]") && strstr(output_capture, "\"channels\":[]") &&
           strstr(output_capture, "\"limits\":{\"targets\":0,\"exclusions\":0,\"batch\":0,\"aps\":0,\"clients\":0,\"probes\":0}"));
    assert(record_length() <= 1000);
    output_capture[0] = 0;
    assert(capabilities(1, NULL) == 0 && strstr(output_capture, "\"c5_jobs_v1\":false") &&
           !strstr(output_capture, "wardrive_serial_v1"));
    char *blocked_job[] = {"job_start", "init-gate", "invalid"};
    assert(dispatch(3, blocked_job) == 1 && !sw_job_owned() && !sw_active());
    assert(dispatch(0, NULL) == 1);

    sw_set_boot_state(true, "normal", false);
    output_capture[0] = 0;
    assert(device_info(1, NULL) == 0);
    assert(strstr(output_capture, "\"kind\":\"device_info\""));
    assert(strstr(output_capture, "\"build_id\":\"0123456789abcdef\""));
#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    assert(strstr(output_capture, "\"board\":\"waveshare-zero\""));
    assert(strstr(output_capture, "\"psram_bytes\":0"));
#else
    assert(strstr(output_capture, "\"board\":\"xiao\""));
    assert(strstr(output_capture, "\"psram_bytes\":8388608"));
#endif
    assert(strstr(output_capture, expected));
    assert(record_length() <= 1000);

#if CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO
    /* Zero has only three spare operation slots; long names force overflow. */
    static const char *const overflow_operations[] = {
        "extra_device_operation1", "extra_device_operation2", "extra_device_operation3"
    };
    for (size_t i = 0; i < sizeof(overflow_operations) / sizeof(overflow_operations[0]); i++)
        assert(sw_register_job_operation_ex(overflow_operations[i], operation_start, operation_stop));
    output_capture[0] = 0;
    assert(device_info(1, NULL) == 1);
    assert(strstr(output_capture, "\"kind\":\"error\""));
    assert(strstr(output_capture, "\"error\":\"device_info_too_large\""));
    assert(record_length() <= 1000);
#endif
    return 0;
}
