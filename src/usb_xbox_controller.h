#pragma once

#include <cstdint>

enum class UsbXboxEventType : uint8_t {
    CONNECTED = 0,
    DISCONNECTED,
    INPUT_UPDATED,
    ERROR
};

constexpr uint16_t USB_XBOX_A = 1u << 0;
constexpr uint16_t USB_XBOX_B = 1u << 1;
constexpr uint16_t USB_XBOX_X = 1u << 2;
constexpr uint16_t USB_XBOX_Y = 1u << 3;
constexpr uint16_t USB_XBOX_UP = 1u << 4;
constexpr uint16_t USB_XBOX_DOWN = 1u << 5;
constexpr uint16_t USB_XBOX_LEFT = 1u << 6;
constexpr uint16_t USB_XBOX_RIGHT = 1u << 7;
constexpr uint16_t USB_XBOX_MENU = 1u << 8;
constexpr uint16_t USB_XBOX_VIEW = 1u << 9;

struct UsbXboxEvent {
    UsbXboxEventType type;
    uint16_t vendorId;
    uint16_t productId;
    int error;
    uint16_t buttons;
};

// usbHostManagerBegin() must succeed before this is called.
bool usbXboxControllerBegin();
bool usbXboxControllerTakeEvent(UsbXboxEvent& event);
