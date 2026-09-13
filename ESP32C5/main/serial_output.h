#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "sdkconfig.h"

/* The IDF console installs a 256-byte USB TX ring. write_bytes queues the
 * WHOLE request or fails; a larger JSON line can never fit. Keep each write
 * small, lock for the complete line, and bound waiting even with no USB host. */
static bool serial_output(const char *data, unsigned length, unsigned timeout_ms) {
    int64_t end=esp_timer_get_time()/1000+timeout_ms;
    while(ftrylockfile(stdout)!=0) {
        if(esp_timer_get_time()/1000>=end) return false;
        vTaskDelay(1);
    }
    unsigned sent=0;
    while(sent<length) {
        int64_t left=end-esp_timer_get_time()/1000;
        if(left<=0) break;
        unsigned chunk=length-sent;
        if(chunk>64) chunk=64;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        int n=usb_serial_jtag_write_bytes(data+sent,chunk,pdMS_TO_TICKS(left));
#else
        int n=uart_tx_chars(CONFIG_ESP_CONSOLE_UART_NUM,data+sent,chunk);
#endif
        if(n>0) sent+=(unsigned)n;
        else vTaskDelay(1);
    }
    funlockfile(stdout);
    return sent==length;
}
