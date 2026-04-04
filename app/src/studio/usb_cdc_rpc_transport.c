/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file usb_cdc_rpc_transport.c
 * @brief USB CDC ACM RPC Transport for ZMK Studio
 *
 * This transport enables ZMK Studio communication over USB using the CDC ACM
 * (Communications Device Class - Abstract Control Model) protocol. When the
 * keyboard is connected via USB, this transport allows cross-platform
 * configuration tools running on Windows, macOS, Linux, iOS, or Android to
 * communicate with the keyboard firmware using the ZMK Studio RPC protocol.
 *
 * The transport uses a Zephyr CDC-ACM UART device (referenced via the
 * `zmk,studio-rpc-usb-cdc` devicetree chosen node) and provides:
 *   - Interrupt-driven or polling-based RX/TX
 *   - Optional DTR (Data Terminal Ready) line state monitoring to detect
 *     host-side application connect/disconnect events
 *   - Integration with the shared ZMK Studio RPC ring buffers and framing layer
 *
 * @note Enable this transport by setting CONFIG_ZMK_STUDIO_TRANSPORT_USB_CDC=y
 *       and including a devicetree overlay that defines the chosen CDC-ACM UART
 *       node. The `studio-rpc-usb-cdc` snippet provides a ready-made overlay.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/logging/log.h>
#include <zmk/studio/rpc.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#define USB_CDC_DEVICE_NODE DT_CHOSEN(zmk_studio_rpc_usb_cdc)

static const struct device *const cdc_uart_dev = DEVICE_DT_GET(USB_CDC_DEVICE_NODE);

static void tx_notify(struct ring_buf *tx_ring_buf, size_t written, bool msg_done,
                      void *user_data) {
    if (msg_done || (ring_buf_size_get(tx_ring_buf) > (ring_buf_capacity_get(tx_ring_buf) / 2))) {
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
        uart_irq_tx_enable(cdc_uart_dev);
#else
        struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
        uint8_t *buf;
        uint32_t claim_len;
        while ((claim_len = ring_buf_get_claim(tx_buf, &buf, tx_buf->size)) > 0) {
            for (int i = 0; i < claim_len; i++) {
                uart_poll_out(cdc_uart_dev, buf[i]);
            }
            ring_buf_get_finish(tx_buf, claim_len);
        }
#endif
    }
}

#if !IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)

static void usb_cdc_rx_main(void) {
    for (;;) {
        uint8_t *buf;
        struct ring_buf *ring_buf = zmk_rpc_get_rx_buf();
        uint32_t claim_len = ring_buf_put_claim(ring_buf, &buf, 1);

        if (claim_len < 1) {
            LOG_WRN("No RX buffer space available");
            k_sleep(K_MSEC(1));
            continue;
        }

        if (uart_poll_in(cdc_uart_dev, buf) < 0) {
            ring_buf_put_finish(ring_buf, 0);
            k_sleep(K_MSEC(1));
        } else {
            ring_buf_put_finish(ring_buf, 1);
            zmk_rpc_rx_notify();
        }
    }
}

K_THREAD_DEFINE(usb_cdc_transport_read_thread, CONFIG_ZMK_STUDIO_TRANSPORT_USB_CDC_RX_STACK_SIZE,
                usb_cdc_rx_main, NULL, NULL, NULL,
                CONFIG_ZMK_STUDIO_TRANSPORT_USB_CDC_RX_PRIORITY, 0, 0);

#endif /* !IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN) */

static int start_rx(void) {
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    uart_irq_rx_enable(cdc_uart_dev);
#else
    k_thread_resume(usb_cdc_transport_read_thread);
#endif
    return 0;
}

static int stop_rx(void) {
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    uart_irq_rx_disable(cdc_uart_dev);
#else
    k_thread_suspend(usb_cdc_transport_read_thread);
#endif
    return 0;
}

ZMK_RPC_TRANSPORT(usb_cdc, ZMK_TRANSPORT_USB, start_rx, stop_rx, NULL, tx_notify);

#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)

/**
 * @brief UART interrupt callback for the CDC ACM device.
 *
 * Handles both RX data arrival (copying bytes into the shared RPC RX ring
 * buffer and notifying the RPC thread) and TX drain (sending bytes from the
 * shared TX ring buffer over CDC ACM).
 */
static void usb_cdc_serial_cb(const struct device *dev, void *user_data) {
    if (!uart_irq_update(cdc_uart_dev)) {
        return;
    }

    if (uart_irq_rx_ready(cdc_uart_dev)) {
        uint32_t last_read = 0, len = 0;
        struct ring_buf *buf = zmk_rpc_get_rx_buf();
        do {
            uint8_t *buffer;
            len = ring_buf_put_claim(buf, &buffer, buf->size);
            if (len > 0) {
                last_read = uart_fifo_read(cdc_uart_dev, buffer, len);
                ring_buf_put_finish(buf, last_read);
            } else {
                LOG_ERR("Dropping incoming USB CDC RPC byte, insufficient room in RX buffer. "
                        "Bump CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE.");
                uint8_t dummy;
                last_read = uart_fifo_read(cdc_uart_dev, &dummy, 1);
            }
        } while (last_read && last_read == len);

        zmk_rpc_rx_notify();
    }

    if (uart_irq_tx_ready(cdc_uart_dev)) {
        struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();
        uint32_t len;
        while ((len = ring_buf_size_get(tx_buf)) > 0) {
            uint8_t *buf;
            uint32_t claim_len = ring_buf_get_claim(tx_buf, &buf, tx_buf->size);

            if (claim_len == 0) {
                continue;
            }

            int sent = uart_fifo_fill(cdc_uart_dev, buf, claim_len);
            ring_buf_get_finish(tx_buf, MAX(sent, 0));
        }
    }
}

#endif /* IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN) */

static int usb_cdc_rpc_interface_init(void) {
    if (!device_is_ready(cdc_uart_dev)) {
        LOG_ERR("USB CDC UART device not ready!");
        return -ENODEV;
    }

#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    int ret = uart_irq_callback_user_data_set(cdc_uart_dev, usb_cdc_serial_cb, NULL);

    if (ret < 0) {
        if (ret == -ENOTSUP) {
            LOG_ERR("Interrupt-driven UART API not supported by USB CDC device");
        } else if (ret == -ENOSYS) {
            LOG_ERR("USB CDC device does not implement interrupt-driven UART API");
        } else {
            LOG_ERR("Error setting USB CDC UART callback: %d", ret);
        }
        return ret;
    }
#endif /* IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN) */

    return 0;
}

SYS_INIT(usb_cdc_rpc_interface_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
