// This UART0 driver uses minimal synchronization to obtain better performance.
// It is caller's responsibility to ensure all required synchronization.
// FreeRTOS task notification system is used to provide lightweigt semaphores.
//
// It is zero-copy and does most of the work inside the interrupt handler itself.
// For this to work, the receive and transmit tasks must be realtime and fast
// enough to keep the driver supplied with fresh buffers, otherwise it may start
// dropping received bytes or delay transmitted bytes.
//
// Copyright (C) 2023 Prusa Research a.s - www.prusa3d.com
// SPDX-License-Identifier: GPL-3.0-or-later

#include "uart0_driver.h"

// #ifdef CONFIG_IDF_TARGET_ESP8266 
// #define USE_CUSTOM_UART_DRIVER
// #endif

#include "driver/uart.h"
#ifdef CONFIG_IDF_TARGET_ESP8266
#include "esp8266/pin_mux_register.h"
#include "esp8266/uart_register.h"
#include "esp8266/uart_struct.h"
#include "projdefs.h"
#endif
#ifdef CONFIG_IDF_TARGET_ESP32
#include "soc/uart_reg.h"
#include "soc/uart_struct.h"
#include "soc/io_mux_reg.h"
#define uart0 UART0
#endif
#include "esp_attr.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <arpa/inet.h>

const uint32_t baud_rate = 4608000;
// const uint32_t baud_rate = 115200;

static const char *TAG = "uart_driver";

static uint8_t intron[8];

QueueHandle_t empty_buffer_queue;
QueueHandle_t full_buffer_queue;

static atomic_bool pending_frm_err = false;
static atomic_bool pending_parity_err = false;
static atomic_bool pending_rxfifo_ovf = false;

void IRAM_ATTR uart0_set_intron(uint8_t new_intron[8]) {
    memcpy(intron, new_intron, sizeof(intron));
}

void IRAM_ATTR uart0_rx_release_buffer(RxBuffer *buffer) {
    xQueueSendToBack( empty_buffer_queue, &buffer, portMAX_DELAY );
}

RxBuffer* IRAM_ATTR uart0_rx_get_buffer() {
    RxBuffer *buffer;
    xQueueReceive( full_buffer_queue, &buffer, portMAX_DELAY );
    // uart1.fifo.rw_byte = '*';

    // Report problems that emerged during the transfer.
    if(pending_frm_err) {
        pending_frm_err = false;
        ESP_LOGE(TAG, "UART0 frame error");
    }
    if (pending_parity_err) {
        pending_parity_err = false;
        ESP_LOGE(TAG, "UART0 parity error");
    }
    if(pending_rxfifo_ovf) {
        pending_rxfifo_ovf = false;
        ESP_LOGE(TAG, "UART0 RX FIFO overflow");
    }

    return buffer;
}

static void setup_queues() {
     // Rx, Tx queues
    empty_buffer_queue = xQueueCreate( 10, sizeof(RxBuffer*));
    assert(empty_buffer_queue);
    full_buffer_queue = xQueueCreate( 10, sizeof(RxBuffer*));
    assert(full_buffer_queue);

    // Allocate Tx buffers
    for(int _ = 0; _ < 3; _++) {
        RxBuffer *buffer = malloc(sizeof(RxBuffer));
        assert(buffer);
        xQueueSendToBack( empty_buffer_queue, &buffer, portMAX_DELAY );
    }
}

#ifdef USE_CUSTOM_UART_DRIVER

const uint32_t rxfifo_full_thresh = 96;
const uint32_t rx_timeout_thresh = 16;
const uint32_t txfifo_empty_thresh = 32;

static TaskHandle_t tx_task_handle; // task to be notified after uart0_isr() finishes transmit
static uint8_t *tx_data;
static size_t tx_size;

#define FORCE_INLINE inline __attribute__((always_inline))

static FORCE_INLINE uint8_t *uart0_rx_fifo(uint8_t* data, size_t size) {
    for (const uint8_t* end = data + size; data != end; ++data) {
        *data = uart0.fifo.rw_byte; // read from register to drain hardware RX FIFO
    }
    return data;
}

static FORCE_INLINE uint8_t *uart0_tx_fifo(uint8_t* data, size_t size) {
    for (const uint8_t* end = data + size; data != end; ++data) {
        uart0.fifo.rw_byte = *data; // write to register to fill hardware TX FIFO
    }
    return data;
}

static void read_fifo();

static void IRAM_ATTR uart0_isr(void *param) {
    // This ISR handles all UART0 conditions:
    //   * TX FIFO empty - happens when TX FIFO has less than `txfifo_empty_thresh` bytes
    //   * RX FIFO full  - happens when RX FIFO has more than `rxfifo_full_thresh` bytes
    //   * RX timeout    - happens when no byte is received for the duration of time it
    //                     would normally take to receive `rx_timeout_thresh` bytes
    //   * Frame error   - is only logged TODO: Some action should be taken
    //   * Parity error  - is only logged TODO: Some action should be taken
    //   * RX FIFO ovf   - is only logged TODO: Some action should be taken
    // The handler is structured as a loop, to account for new conditions arising while
    // processing previous conditions.
    BaseType_t task_woken = pdFALSE;
    for (;;) {
        const uint32_t uart_intr_status = uart0.int_st.val;

        if (uart_intr_status == 0) {
            // No more conditions on UART0, optionally unblock waiting tasks and we are done.
            if (task_woken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
            return;
        }

        if (uart_intr_status & UART_TXFIFO_EMPTY_INT_ST_M) {
            // Handle TX FIFO empty condition.

            const uint8_t tx_remain_fifo_cnt = UART_FIFO_LEN - uart0.status.txfifo_cnt;
            if (tx_size > tx_remain_fifo_cnt) {
                // Send partial data to fifo, do some bookkeeping and keep interrupt enabled.
                tx_data = uart0_tx_fifo(tx_data, tx_remain_fifo_cnt);
                tx_size -= tx_remain_fifo_cnt;
            } else {
                // Send complete data to fifo. Note that we do not need any more bookkeeping.
                (void)uart0_tx_fifo(tx_data, tx_size);

                // Disable more interrupts and unblock uart0_tx_bytes().
                uart0.int_ena.txfifo_empty = 0;
                vTaskNotifyGiveFromISR(tx_task_handle, &task_woken);
            }
            uart0.int_clr.txfifo_empty = 1;
            continue;
        }

        if (uart_intr_status & (UART_RXFIFO_FULL_INT_ST_M|UART_RXFIFO_TOUT_INT_ST_M)) {
            read_fifo();
            uart0.int_clr.rxfifo_full = 1;
            uart0.int_clr.rxfifo_tout = 1;
            continue;
        }

        if (uart_intr_status & UART_FRM_ERR_INT_ST_M) {
            pending_frm_err = true;
            uart0.int_clr.frm_err = 1;
            continue;
        }

        if (uart_intr_status & UART_PARITY_ERR_INT_ST_M) {
            pending_parity_err = true;
            uart0.int_clr.parity_err = 1;
            continue;
        }

        if (uart_intr_status & UART_RXFIFO_OVF_INT_ST_M) {
            pending_rxfifo_ovf = true;
            uart0.int_clr.rxfifo_ovf = 1;
            continue;
        }

        // Note: This should never happen in current setup, other interrupts are not enabled.
        //       But just to be sure and to prevent infinite loop, let's clear the status bit.
        uart0.int_clr.val = uart_intr_status;
    }
}

void IRAM_ATTR uart0_tx_bytes(uint8_t *data, size_t size) {
    const uint8_t tx_remain_fifo_cnt = UART_FIFO_LEN - uart0.status.txfifo_cnt;
    if (size > tx_remain_fifo_cnt) {
        // send partial data to fifo
        tx_data = uart0_tx_fifo(data, tx_remain_fifo_cnt);
        tx_size = size - tx_remain_fifo_cnt;

        // delegate rest of the work to interrupt handler
        custom_taskENTER_CRITICAL(&critical_mutex);
        uart0.int_ena.txfifo_empty = 1;
        custom_taskEXIT_CRITICAL(&critical_mutex);

        // wait for interrupt handler and we are done
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    } else {
        // send complete data to fifo and we are done
        (void)uart0_tx_fifo(data, size);
    }
}

BaseType_t IRAM_ATTR handle_fifo_byte(const char c) {
    static RxBuffer *rx_buffer = NULL;
    static size_t rx_buffer_pos = 0;
    static size_t rx_buffer_size = 0;
    static uint8_t intron_pos = 0;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Obtain buffer, skip if none available
    if(!rx_buffer && !xQueueReceiveFromISR( empty_buffer_queue, &rx_buffer, &xHigherPriorityTaskWoken)) {
        return xHigherPriorityTaskWoken;
    }

    // Handle intron (TODO: Use Trie)
    if(intron_pos < sizeof(intron)) {
        if(c == intron[intron_pos]) {
            intron_pos++;
        } else {
            intron_pos = 0;
        }

        // Move to next state
        if(intron_pos == sizeof(intron)) {
            rx_buffer_pos = 0;
            rx_buffer_size = sizeof(struct Header);
        }
        return xHigherPriorityTaskWoken;
    }

    // Read byte into buffer
    ((char*)rx_buffer)[rx_buffer_pos++] = c;

    // Process size from header when available
    if(rx_buffer_pos == sizeof(struct Header)) {
        size_t size = ntohs(rx_buffer->header.size);
        if(size < MAX_PACKET_SIZE) {
            rx_buffer_size += size; // Schedule message body read
        } else {
            intron_pos = 0; // Skip this message
        }
    }

    // Dispatch buffer and reset state when full
    if(rx_buffer_pos == rx_buffer_size) {
        if(xQueueSendToBackFromISR(full_buffer_queue, &rx_buffer, &xHigherPriorityTaskWoken)) {
            rx_buffer = NULL;
        }
        intron_pos = 0;
    }

    return xHigherPriorityTaskWoken;
}

void IRAM_ATTR read_fifo() {
    while(uart0.status.rxfifo_cnt) {
        BaseType_t xHigherPriorityTaskWoken = handle_fifo_byte(uart0.fifo.rw_byte);
        if( xHigherPriorityTaskWoken ) {
            portYIELD_FROM_ISR ();
        }
    }
}

static uint32_t round_div(uint32_t a, uint32_t b) {
    return (a + b/2) / b;
}

esp_err_t uart0_driver_install(TaskHandle_t uart0_tx_task_handle) {
#ifdef CONFIG_IDF_TARGET_ESP32
    uart_set_pin(UART_NUM_0, 1, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#endif

    if (uart_is_driver_installed(UART_NUM_0)) {
        return ESP_FAIL;
    }

    setup_queues();

    custom_taskENTER_CRITICAL(&critical_mutex);
#ifdef CONFIG_IDF_TARGET_ESP8266
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0TXD_U);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_U0RXD);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);
#endif
#ifdef CONFIG_IDF_TARGET_ESP32
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0TXD_U);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_U0RXD_U0RXD);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD_U0TXD);
#endif
    uart0.clk_div.val = round_div(UART_CLK_FREQ, baud_rate) & 0xFFFFF;
    uart0.conf0.bit_num = UART_DATA_8_BITS;
    uart0.conf0.parity = (UART_PARITY_DISABLE & 0x1);
    uart0.conf0.parity_en = ((UART_PARITY_DISABLE >> 1) & 0x1);
    uart0.conf0.stop_bit_num = UART_STOP_BITS_1;
    uart0.conf0.tx_flow_en = 0;
    uart0.conf1.rx_flow_en = 0;
    uart0.conf1.rx_tout_en = 1;
    uart0.conf1.rx_tout_thrhd = (rx_timeout_thresh & 0x7f);
    uart0.conf1.rxfifo_full_thrhd = rxfifo_full_thresh;
    uart0.conf1.txfifo_empty_thrhd = txfifo_empty_thresh;
    custom_taskEXIT_CRITICAL(&critical_mutex);

    // Note: We plug into original ISR in order to keep UART_NUM_1 working.
    #if CONFIG_IDF_TARGET_ESP32
    if (uart_isr_register(UART_NUM_0, uart0_isr, NULL, 0, NULL) != ESP_OK) {
    #endif
    #if CONFIG_IDF_TARGET_ESP8266
    if (uart_isr_register(UART_NUM_0, uart0_isr, NULL) != ESP_OK) {
    #endif
        return ESP_FAIL;
    }

    tx_task_handle = uart0_tx_task_handle;
    xTaskNotifyGive(tx_task_handle);

    custom_taskENTER_CRITICAL(&critical_mutex);
    uart0.int_ena.rxfifo_full = 1;
    uart0.int_ena.rxfifo_tout = 1;
    uart0.int_ena.parity_err = 1;
    uart0.int_ena.frm_err = 1;
    uart0.int_ena.rxfifo_ovf = 1;
    custom_taskEXIT_CRITICAL(&critical_mutex);


    return ESP_OK;
}

#else // Simplified implementation without custom UART driver

static void IRAM_ATTR wait_for_intron() {
    ESP_LOGI(TAG, "Waiting for intron");
    uint pos = 0;
    bool failure = false;
    while(pos < sizeof(intron)) {
        uint8_t c;
        int read = uart_read_bytes(UART_NUM_0, &c, sizeof(c), portMAX_DELAY);
        if(read == sizeof(c)) {
            if (c == intron[pos]) {
                pos++;
            } else {
                // ESP_LOGI(TAG, "Invalid: %c, val: %d\n", c, (int)c);
                pos = 0;
                failure = true;
            }
        } else {
            ESP_LOGI(TAG, "Timeout!!!");
        }
    }
    if(failure) {
        ESP_LOGI(TAG, "Intron found after failure");
    }
}

static void IRAM_ATTR rx_task(void *arg) {
    for(;;) {
        RxBuffer *rx_buffer = NULL;
 
        // Obtain buffer
        ESP_LOGI(TAG, "Waiting for buffer");
        xQueueReceive(empty_buffer_queue, &rx_buffer, portMAX_DELAY);
        ESP_LOGI(TAG, "Got buffer");
        assert(rx_buffer);

        // Wait for intron
        wait_for_intron();

        // Read header
        uart_read_bytes(UART_NUM_0, (uint8_t*)rx_buffer, sizeof(struct Header), portMAX_DELAY);
        const size_t size = ntohs(rx_buffer->header.size);
        if(size < MAX_PACKET_SIZE) {
            // Read body
            uart_read_bytes(UART_NUM_0, (uint8_t*)rx_buffer + sizeof(struct Header), size, portMAX_DELAY);

            // Dispatch buffer
            ESP_LOGI(TAG, "Dispatching buffer");
            xQueueSendToBack( full_buffer_queue, &rx_buffer, portMAX_DELAY );
        } else {
            // Skip this message
            ESP_LOGI(TAG, "Skipping message");
            xQueueSendToBack( empty_buffer_queue, &rx_buffer, portMAX_DELAY );
        }
    }
}

esp_err_t uart0_driver_install(TaskHandle_t uart0_tx_task_handle) {
#ifdef CONFIG_IDF_TARGET_ESP32
    uart_set_pin(UART_NUM_0, 1, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#endif

    // Configure parameters of an UART driver,
    // communication pins and install the driver
    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_NUM_0, 16384, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_config);

    setup_queues();

    // Start UART reader thread
    if (xTaskCreate(rx_task, "uart_rx", 2048, NULL, 16, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (uart_rx)");
        abort();
    }
 
    // Report driver is installed
    xTaskNotifyGive(uart0_tx_task_handle);

    return ESP_OK;
}

void uart0_tx_bytes(uint8_t *data, size_t size) {
    uart_write_bytes(UART_NUM_0, (const char *)data, size);
}

#endif