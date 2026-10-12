#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static char projectzero_job_operation_name[24];
typedef void *TaskHandle_t;
static TaskHandle_t sniffer_channel_task_handle;
static bool sniffer_active;
static bool owner_matches = true;
static int stop_result;
static int export_result;
static unsigned stop_calls, export_calls, close_calls;
static bool resource_owned;
static unsigned delay_calls, exit_on_delay_call;
static bool sniffer_channel_wait_quiescent(void);

#define pdMS_TO_TICKS(ms) (ms)
#define ZERO_STACK_NOINLINE
static void vTaskDelay(unsigned ticks) {
    assert(ticks == 50);
    delay_calls++;
    if (exit_on_delay_call && delay_calls >= exit_on_delay_call)
        sniffer_channel_task_handle = NULL;
}

static bool sw_job_owner_matches(const char *token) {
    assert(token && strcmp(token, "job-token") == 0);
    return owner_matches;
}

static int stop_operations(bool from_command) {
    assert(!from_command);
    stop_calls++;
    if (strcmp(projectzero_job_operation_name, "sniffer") == 0) {
        sniffer_active = false;
        if (sniffer_channel_task_handle != NULL &&
            !sniffer_channel_wait_quiescent()) return 1;
    }
    return stop_result;
}

static int cmd_show_sniffer_intel(int argc, char **argv) {
    assert(sniffer_channel_task_handle == NULL);
    assert(!sniffer_active);
    assert(resource_owned && close_calls == 0 && stop_calls > 0);
    assert(argc == 2);
    assert(strcmp(argv[0], "show_sniffer_intel") == 0);
    assert(strcmp(argv[1], "job-token") == 0);
    export_calls++;
    return export_result;
}

static void job_resources_close(void) {
    assert(export_calls > 0 || strcmp(projectzero_job_operation_name, "sniffer") != 0);
    close_calls++;
    resource_owned = false;
}

#include "sniffer_stop_under_test.inc"

static void reset_case(const char *operation) {
    snprintf(projectzero_job_operation_name,
             sizeof(projectzero_job_operation_name), "%s", operation);
    owner_matches = true;
    stop_result = 0;
    export_result = 0;
    stop_calls = export_calls = close_calls = 0;
    resource_owned = true;
    sniffer_channel_task_handle = NULL;
    sniffer_active = false;
    delay_calls = exit_on_delay_call = 0;
}

int main(void) {
    reset_case("sniffer");
    owner_matches = false;
    assert(zero_job_stop_operation("job-token") != 0);
    assert(stop_calls == 0 && export_calls == 0 && close_calls == 0);
    assert(resource_owned && strcmp(projectzero_job_operation_name, "sniffer") == 0);

    /* A live producer times out without export or release. Its own completion
       signal clears the handle, and retry then exports and closes resources. */
    reset_case("sniffer");
    sniffer_channel_task_handle = (TaskHandle_t)1;
    assert(zero_job_stop_operation("job-token") != 0);
    assert(stop_calls == 1 && delay_calls == 30);
    assert(sniffer_channel_task_handle == (TaskHandle_t)1);
    assert(export_calls == 0 && close_calls == 0 && resource_owned);
    exit_on_delay_call = delay_calls + 2;
    assert(zero_job_stop_operation("job-token") == 0);
    assert(stop_calls == 2 && export_calls == 1 && close_calls == 1);
    assert(sniffer_channel_task_handle == NULL && !resource_owned);

    reset_case("sniffer");
    export_result = 1;
    assert(zero_job_stop_operation("job-token") != 0);
    assert(stop_calls == 1 && export_calls == 1 && close_calls == 0);
    assert(resource_owned && strcmp(projectzero_job_operation_name, "sniffer") == 0);

    /* A retry re-runs the idempotent stop, re-exports with the same token,
       then releases the owner only after the complete export succeeds. */
    export_result = 0;
    assert(zero_job_stop_operation("job-token") == 0);
    assert(stop_calls == 2 && export_calls == 2 && close_calls == 1);
    assert(!resource_owned && projectzero_job_operation_name[0] == '\0');

    reset_case("wardrive");
    assert(zero_job_stop_operation("job-token") == 0);
    assert(stop_calls == 1 && export_calls == 0 && close_calls == 1);
    assert(!resource_owned && projectzero_job_operation_name[0] == '\0');

    reset_case("sniffer");
    stop_result = 1;
    assert(zero_job_stop_operation("job-token") != 0);
    assert(stop_calls == 1 && export_calls == 0 && close_calls == 0);
    assert(resource_owned && strcmp(projectzero_job_operation_name, "sniffer") == 0);

    puts("PASS: managed sniffer export token/order, retry, and resource ownership");
    return 0;
}
