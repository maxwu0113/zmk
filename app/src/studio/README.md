# ZMK Studio — Firmware Architecture & RPC Transport Guide

ZMK Studio provides runtime keymap configuration for ZMK-powered keyboards without
reflashing firmware.  This document describes the firmware-side architecture, the RPC
transport layer, and how to enable USB CDC ACM or BLE GATT communication for
cross-platform clients (Windows, macOS, Linux, iOS, Android).

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│                  ZMK Studio Client                       │
│  (zmk.studio Web App / Native App / Custom Tool)         │
└────────────┬─────────────────────┬───────────────────────┘
             │ USB CDC ACM          │ BLE GATT (Indicate)
             ▼                     ▼
┌────────────────────┐  ┌──────────────────────────────────┐
│  usb_cdc_rpc_      │  │  gatt_rpc_transport.c            │
│  transport.c       │  │  (ZMK_STUDIO_TRANSPORT_BLE)      │
│  (ZMK_STUDIO_      │  └────────────┬─────────────────────┘
│  TRANSPORT_USB_CDC)│               │
└────────────┬───────┘               │
             │                       │
             └──────────┬────────────┘
                        │  ZMK_TRANSPORT_USB / ZMK_TRANSPORT_BLE
                        ▼
             ┌──────────────────────┐
             │  rpc.c               │
             │  RX ring buf         │
             │  TX ring buf         │
             │  msg_framing.c       │  ← SOF / ESC / EOF framing
             │  protobuf (nanopb)   │
             └──────────┬───────────┘
                        │  zmk_studio_Request / zmk_studio_Response
                        ▼
         ┌──────────────────────────────┐
         │  RPC Subsystem Dispatch      │
         │  core_subsystem.c            │
         │  keymap_subsystem.c          │
         │  behavior_subsystem.c        │
         └──────────────────────────────┘
                        │
                        ▼
         ┌──────────────────────────────┐
         │  ZMK Keymap / Behavior API   │
         │  zmk_keymap_set_layer_       │
         │    binding_at_idx()          │
         │  zmk_keymap_save_changes()   │
         │  zmk_keymap_add_layer()      │
         │  zmk_keymap_move_layer()     │
         └──────────────────────────────┘
```

---

## Transport Modules

### USB CDC ACM Transport (`usb_cdc_rpc_transport.c`)

Registers with the RPC core as `ZMK_TRANSPORT_USB`.  When the keyboard is
connected via USB, this transport receives protobuf-framed RPC messages from a
CDC ACM virtual serial port and sends responses back over the same port.

**Device tree chosen node:** `zmk,studio-rpc-usb-cdc`

**Quick start:** add the `studio-rpc-usb-cdc` snippet to your build.

```yaml title="build.yaml"
include:
  - board: nice_nano_v2
    shield: corne_left
    snippet: studio-rpc-usb-cdc
    cmake-args: -DCONFIG_ZMK_STUDIO=y
  - board: nice_nano_v2
    shield: corne_right
```

**Manual config** (if not using the snippet):

```kconfig title="prj.conf"
CONFIG_ZMK_USB=y
CONFIG_USB_DEVICE_STACK=y
CONFIG_USB_CDC_ACM=y
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_UART_LINE_CTRL=y
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_STUDIO_TRANSPORT_USB_CDC=y
```

```dts title="board.overlay"
/ {
    chosen {
        zmk,studio-rpc-usb-cdc = &cdc_acm_uart0;
    };
};

&zephyr_udc0 {
    cdc_acm_uart0: cdc_acm_uart0 {
        compatible = "zephyr,cdc-acm-uart";
    };
};
```

### UART Serial Transport (`uart_rpc_transport.c`)

Also registers as `ZMK_TRANSPORT_USB`.  Uses the devicetree chosen node
`zmk,studio-rpc-uart`, which can point to either a physical hardware UART or a
CDC ACM node (via the `studio-rpc-usb-uart` snippet).  Enable with
`CONFIG_ZMK_STUDIO_TRANSPORT_UART=y`.

### BLE GATT Transport (`gatt_rpc_transport.c`)

Registers as `ZMK_TRANSPORT_BLE`.  Uses a GATT service with a custom
128-bit UUID.  The client writes RPC requests to the RPC characteristic and
subscribes for indications to receive responses.  Enable with
`CONFIG_ZMK_STUDIO_TRANSPORT_BLE=y` (requires `CONFIG_ZMK_BLE=y`).

---

## Message Framing (`msg_framing.c` / `msg_framing.h`)

All bytes transmitted over any transport are wrapped in a simple framing layer
to delineate message boundaries and escape control bytes:

| Byte value | Meaning            |
|------------|--------------------|
| `0xAB`     | Start-of-Frame (SOF) |
| `0xAC`     | Escape (ESC)       |
| `0xAD`     | End-of-Frame (EOF) |

A data byte equal to SOF, ESC, or EOF is prefixed by an ESC byte before
transmission.  The `studio_framing_process_byte()` function drives a state
machine that strips framing bytes and returns only payload bytes to the
protobuf decoder.

---

## RPC Protocol

Requests and responses are encoded with [nanopb](https://jpa.kapsi.fi/nanopb/)
using the message definitions from
[zmkfirmware/zmk-studio-messages](https://github.com/zmkfirmware/zmk-studio-messages).

The top-level types are `zmk_studio_Request` and `zmk_studio_Response`.
Each carries a `subsystem` field that routes the message to the appropriate
subsystem handler:

| Subsystem     | Source file              | Handles                                    |
|---------------|--------------------------|--------------------------------------------|
| `core`        | `core_subsystem.c`       | Lock/unlock, reset, version info           |
| `keymap`      | `keymap_subsystem.c`     | Read/write key bindings, layers, names     |
| `behavior`    | `behavior_subsystem.c`   | Enumerate available behaviors & metadata   |

---

## Keymap API Reference

The following public API (declared in `app/include/zmk/keymap.h`) is exposed
via the RPC keymap subsystem and can be used directly in firmware modules:

| Function | Description |
|----------|-------------|
| `zmk_keymap_get_layer_binding_at_idx(layer, idx)` | Read a key binding |
| `zmk_keymap_set_layer_binding_at_idx(layer, idx, binding)` | Write a key binding |
| `zmk_keymap_save_changes()` | Persist changes to settings storage |
| `zmk_keymap_discard_changes()` | Discard unsaved changes |
| `zmk_keymap_reset_settings()` | Restore default keymap |
| `zmk_keymap_check_unsaved_changes()` | Query whether unsaved changes exist |
| `zmk_keymap_add_layer()` | Add a new layer (requires `CONFIG_ZMK_KEYMAP_LAYER_REORDERING`) |
| `zmk_keymap_remove_layer(idx)` | Remove a layer |
| `zmk_keymap_move_layer(from, to)` | Reorder layers |
| `zmk_keymap_set_layer_name(idx, name)` | Rename a layer |

---

## Adding a New RPC Handler

1. Add a handler function with signature `zmk_studio_Response my_handler(const zmk_studio_Request *req)`.
2. Register it with `ZMK_RPC_SUBSYSTEM_HANDLER(subsystem_name, my_handler, ZMK_STUDIO_RPC_HANDLER_SECURED)`.
3. Ensure the corresponding protobuf message definition exists in the
   zmk-studio-messages schema.

---

## Cross-Platform Client Notes

| Platform | USB CDC ACM | BLE GATT |
|----------|-------------|----------|
| Windows  | ✅ Built-in CDC driver (no driver install needed on Windows 10+) | ✅ via Web Bluetooth (Chrome/Edge) or native app |
| macOS    | ✅ Built-in | ✅ via Web Bluetooth or native app |
| Linux    | ✅ `/dev/ttyACM*` (may require `dialout` group membership) | ✅ via Web Bluetooth (Chrome) or native app |
| iOS      | ✅ via WebUSB-compatible browser or dedicated app | ✅ via Web Bluetooth |
| Android  | ✅ via WebUSB (Chrome for Android) or dedicated app | ✅ via Web Bluetooth |

For USB CDC ACM on Linux you may need:
```bash
sudo usermod -aG dialout $USER
# log out and back in for the change to take effect
```

---

## Further Reading

- [ZMK Studio capabilities](https://zmk.dev/docs/features/studio)
- [zmk-studio-messages protocol definitions](https://github.com/zmkfirmware/zmk-studio-messages)
- [Zephyr USB CDC ACM documentation](https://docs.zephyrproject.org/latest/connectivity/usb/device/usb_device.html)
