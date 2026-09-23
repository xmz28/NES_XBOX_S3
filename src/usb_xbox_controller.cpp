#include "usb_xbox_controller.h"

#include <Arduino.h>

#include "esp_err.h"
#include "usb/usb_host.h"

#include <cstring>

namespace {

constexpr uint16_t MICROSOFT_VENDOR_ID = 0x045E;
constexpr uint16_t XBOX_SERIES_PRODUCT_ID = 0x0B12;
constexpr uint8_t XGIP_INTERFACE_NUMBER = 0;
constexpr uint8_t XGIP_ALTERNATE_SETTING = 0;
constexpr uint8_t XGIP_CLASS = 0xFF;
constexpr uint8_t XGIP_SUBCLASS = 0x47;
constexpr uint8_t XGIP_PROTOCOL = 0xD0;
constexpr size_t XGIP_PACKET_SIZE = 64;
constexpr size_t EVENT_QUEUE_LENGTH = 16;

constexpr uint8_t GIP_CMD_ACK = 0x01;
constexpr uint8_t GIP_CMD_POWER = 0x05;
constexpr uint8_t GIP_CMD_AUTHENTICATE = 0x06;
constexpr uint8_t GIP_CMD_VIRTUAL_KEY = 0x07;
constexpr uint8_t GIP_CMD_LED = 0x0A;
constexpr uint8_t GIP_CMD_INPUT = 0x20;
constexpr uint8_t GIP_OPT_INTERNAL = 0x20;
constexpr uint8_t GIP_OPT_ACK_INTERNAL = 0x30;
constexpr size_t XBOX_BUTTONS_1_OFFSET = 4;
constexpr size_t XBOX_BUTTONS_2_OFFSET = 5;

constexpr uint8_t INIT_POWER_ON[] = {
    GIP_CMD_POWER, GIP_OPT_INTERNAL, 0x00, 0x01, 0x00
};
constexpr uint8_t INIT_LED_ON[] = {
    GIP_CMD_LED, GIP_OPT_INTERNAL, 0x00, 0x03, 0x00, 0x01, 0x14
};
constexpr uint8_t INIT_AUTH_DONE[] = {
    GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL, 0x00, 0x02, 0x01, 0x00
};
constexpr const uint8_t* INIT_PACKETS[] = {
    INIT_POWER_ON,
    INIT_LED_ON,
    INIT_AUTH_DONE
};
constexpr size_t INIT_PACKET_LENGTHS[] = {
    sizeof(INIT_POWER_ON),
    sizeof(INIT_LED_ON),
    sizeof(INIT_AUTH_DONE)
};

QueueHandle_t eventQueue = nullptr;
usb_host_client_handle_t clientHandle = nullptr;
usb_device_handle_t deviceHandle = nullptr;
usb_transfer_t* inputTransfer = nullptr;
usb_transfer_t* outputTransfer = nullptr;

uint16_t activeVendorId = 0;
uint16_t activeProductId = 0;
uint8_t inputEndpoint = 0;
uint8_t outputEndpoint = 0;
uint8_t outputSequence = 0;
size_t initPacketIndex = 0;
uint8_t pendingAckSequence = 0;
uint16_t currentButtons = 0;

bool interfaceClaimed = false;
bool inputInFlight = false;
bool outputInFlight = false;
bool disconnectPending = false;
bool readyReported = false;
bool ackPending = false;

void queueEvent(UsbXboxEventType type, esp_err_t error = ESP_OK) {
    if (eventQueue == nullptr) {
        return;
    }
    const UsbXboxEvent event{
        type,
        activeVendorId,
        activeProductId,
        static_cast<int>(error),
        currentButtons
    };
    if (xQueueSend(eventQueue, &event, 0) != pdTRUE) {
        UsbXboxEvent discarded{};
        xQueueReceive(eventQueue, &discarded, 0);
        xQueueSend(eventQueue, &event, 0);
    }
}

bool findXgipEndpoints(
    const usb_config_desc_t* config,
    uint8_t& inEndpoint,
    uint8_t& outEndpoint
) {
    if (config == nullptr || config->wTotalLength < sizeof(usb_config_desc_t)) {
        return false;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(config);
    size_t offset = 0;
    bool targetInterface = false;
    while (offset + 2 <= config->wTotalLength) {
        const uint8_t length = bytes[offset];
        const uint8_t descriptorType = bytes[offset + 1];
        if (length < 2 || offset + length > config->wTotalLength) {
            break;
        }

        if (descriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE &&
            length >= sizeof(usb_intf_desc_t)) {
            const auto* interfaceDesc =
                reinterpret_cast<const usb_intf_desc_t*>(bytes + offset);
            targetInterface =
                interfaceDesc->bInterfaceNumber == XGIP_INTERFACE_NUMBER &&
                interfaceDesc->bAlternateSetting == XGIP_ALTERNATE_SETTING &&
                interfaceDesc->bInterfaceClass == XGIP_CLASS &&
                interfaceDesc->bInterfaceSubClass == XGIP_SUBCLASS &&
                interfaceDesc->bInterfaceProtocol == XGIP_PROTOCOL;
        } else if (
            targetInterface &&
            descriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
            length >= sizeof(usb_ep_desc_t)
        ) {
            const auto* endpointDesc =
                reinterpret_cast<const usb_ep_desc_t*>(bytes + offset);
            if (USB_EP_DESC_GET_XFERTYPE(endpointDesc) ==
                USB_TRANSFER_TYPE_INTR) {
                if ((endpointDesc->bEndpointAddress &
                     USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0) {
                    inEndpoint = endpointDesc->bEndpointAddress;
                } else {
                    outEndpoint = endpointDesc->bEndpointAddress;
                }
            }
        }
        offset += length;
    }
    return inEndpoint != 0 && outEndpoint != 0;
}

void configureTransfer(
    usb_transfer_t* transfer,
    uint8_t endpoint,
    usb_transfer_cb_t callback
) {
    transfer->device_handle = deviceHandle;
    transfer->bEndpointAddress = endpoint;
    transfer->callback = callback;
    transfer->context = nullptr;
    transfer->flags = 0;
    transfer->timeout_ms = 0;
}

void submitNextOutput();

void outputTransferCallback(usb_transfer_t* transfer) {
    outputInFlight = false;
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        disconnectPending = true;
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        queueEvent(UsbXboxEventType::ERROR, ESP_FAIL);
        return;
    }
    submitNextOutput();
}

void submitRawOutput(const uint8_t* data, size_t length, bool setSequence) {
    if (outputTransfer == nullptr || outputInFlight || data == nullptr ||
        length == 0 || length > outputTransfer->data_buffer_size) {
        return;
    }

    memcpy(outputTransfer->data_buffer, data, length);
    if (setSequence && length >= 3) {
        outputTransfer->data_buffer[2] = outputSequence++;
    }
    outputTransfer->num_bytes = static_cast<int>(length);
    const esp_err_t result = usb_host_transfer_submit(outputTransfer);
    if (result == ESP_OK) {
        outputInFlight = true;
    } else {
        queueEvent(UsbXboxEventType::ERROR, result);
    }
}

void submitNextOutput() {
    if (disconnectPending || outputInFlight) {
        return;
    }
    if (initPacketIndex < sizeof(INIT_PACKETS) / sizeof(INIT_PACKETS[0])) {
        const size_t index = initPacketIndex++;
        submitRawOutput(
            INIT_PACKETS[index],
            INIT_PACKET_LENGTHS[index],
            true
        );
        return;
    }
    if (!readyReported) {
        readyReported = true;
        queueEvent(UsbXboxEventType::CONNECTED);
    }
    if (ackPending) {
        const uint8_t ack[] = {
            GIP_CMD_ACK, GIP_OPT_INTERNAL, pendingAckSequence, 0x09,
            0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x02,
            0x00, 0x00, 0x00, 0x00, 0x00
        };
        ackPending = false;
        submitRawOutput(ack, sizeof(ack), false);
    }
}

void inputTransferCallback(usb_transfer_t* transfer) {
    inputInFlight = false;
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        disconnectPending = true;
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        queueEvent(UsbXboxEventType::ERROR, ESP_FAIL);
    } else if (transfer->actual_num_bytes >= 6) {
        const uint8_t* data = transfer->data_buffer;
        if (data[0] == GIP_CMD_INPUT) {
            const uint8_t buttons1 = data[XBOX_BUTTONS_1_OFFSET];
            const uint8_t buttons2 = data[XBOX_BUTTONS_2_OFFSET];
            currentButtons = 0;
            if ((buttons1 & 0x10U) != 0U) currentButtons |= USB_XBOX_A;
            if ((buttons1 & 0x20U) != 0U) currentButtons |= USB_XBOX_B;
            if ((buttons1 & 0x40U) != 0U) currentButtons |= USB_XBOX_X;
            if ((buttons1 & 0x80U) != 0U) currentButtons |= USB_XBOX_Y;
            if ((buttons1 & 0x04U) != 0U) currentButtons |= USB_XBOX_MENU;
            if ((buttons1 & 0x08U) != 0U) currentButtons |= USB_XBOX_VIEW;
            if ((buttons2 & 0x01U) != 0U) currentButtons |= USB_XBOX_UP;
            if ((buttons2 & 0x02U) != 0U) currentButtons |= USB_XBOX_DOWN;
            if ((buttons2 & 0x04U) != 0U) currentButtons |= USB_XBOX_LEFT;
            if ((buttons2 & 0x08U) != 0U) currentButtons |= USB_XBOX_RIGHT;
            queueEvent(UsbXboxEventType::INPUT_UPDATED);
        } else if (
            data[0] == GIP_CMD_VIRTUAL_KEY &&
            data[1] == GIP_OPT_ACK_INTERNAL
        ) {
            pendingAckSequence = data[2];
            ackPending = true;
            submitNextOutput();
        }
    }

    if (!disconnectPending) {
        transfer->num_bytes = XGIP_PACKET_SIZE;
        const esp_err_t result = usb_host_transfer_submit(transfer);
        if (result == ESP_OK) {
            inputInFlight = true;
        } else {
            queueEvent(UsbXboxEventType::ERROR, result);
        }
    }
}

void resetDeviceState() {
    activeVendorId = 0;
    activeProductId = 0;
    inputEndpoint = 0;
    outputEndpoint = 0;
    outputSequence = 0;
    initPacketIndex = 0;
    pendingAckSequence = 0;
    currentButtons = 0;
    interfaceClaimed = false;
    inputInFlight = false;
    outputInFlight = false;
    disconnectPending = false;
    readyReported = false;
    ackPending = false;
}

void closeController() {
    const uint16_t disconnectedVendor = activeVendorId;
    const uint16_t disconnectedProduct = activeProductId;

    if (inputTransfer != nullptr) {
        usb_host_transfer_free(inputTransfer);
        inputTransfer = nullptr;
    }
    if (outputTransfer != nullptr) {
        usb_host_transfer_free(outputTransfer);
        outputTransfer = nullptr;
    }

    if (interfaceClaimed && deviceHandle != nullptr) {
        usb_host_interface_release(
            clientHandle,
            deviceHandle,
            XGIP_INTERFACE_NUMBER
        );
    }
    if (deviceHandle != nullptr) {
        usb_host_device_close(clientHandle, deviceHandle);
        deviceHandle = nullptr;
    }

    const bool reportDisconnect = disconnectedVendor != 0;
    resetDeviceState();
    if (reportDisconnect) {
        activeVendorId = disconnectedVendor;
        activeProductId = disconnectedProduct;
        queueEvent(UsbXboxEventType::DISCONNECTED);
        activeVendorId = 0;
        activeProductId = 0;
    }
}

void openController(uint8_t address) {
    if (deviceHandle != nullptr) {
        return;
    }

    usb_device_handle_t openedDevice = nullptr;
    esp_err_t result =
        usb_host_device_open(clientHandle, address, &openedDevice);
    if (result != ESP_OK) {
        return;
    }

    const usb_device_desc_t* deviceDesc = nullptr;
    result = usb_host_get_device_descriptor(openedDevice, &deviceDesc);
    if (result != ESP_OK || deviceDesc == nullptr ||
        deviceDesc->idVendor != MICROSOFT_VENDOR_ID ||
        deviceDesc->idProduct != XBOX_SERIES_PRODUCT_ID) {
        usb_host_device_close(clientHandle, openedDevice);
        return;
    }

    activeVendorId = deviceDesc->idVendor;
    activeProductId = deviceDesc->idProduct;

    const usb_config_desc_t* configDesc = nullptr;
    result = usb_host_get_active_config_descriptor(openedDevice, &configDesc);
    if (result != ESP_OK ||
        !findXgipEndpoints(configDesc, inputEndpoint, outputEndpoint)) {
        queueEvent(
            UsbXboxEventType::ERROR,
            result == ESP_OK ? ESP_ERR_NOT_FOUND : result
        );
        usb_host_device_close(clientHandle, openedDevice);
        resetDeviceState();
        return;
    }

    result = usb_host_interface_claim(
        clientHandle,
        openedDevice,
        XGIP_INTERFACE_NUMBER,
        XGIP_ALTERNATE_SETTING
    );
    if (result != ESP_OK) {
        queueEvent(UsbXboxEventType::ERROR, result);
        usb_host_device_close(clientHandle, openedDevice);
        resetDeviceState();
        return;
    }

    deviceHandle = openedDevice;
    interfaceClaimed = true;
    if (usb_host_transfer_alloc(XGIP_PACKET_SIZE, 0, &inputTransfer) != ESP_OK ||
        usb_host_transfer_alloc(XGIP_PACKET_SIZE, 0, &outputTransfer) != ESP_OK) {
        queueEvent(UsbXboxEventType::ERROR, ESP_ERR_NO_MEM);
        closeController();
        return;
    }

    configureTransfer(inputTransfer, inputEndpoint, inputTransferCallback);
    configureTransfer(outputTransfer, outputEndpoint, outputTransferCallback);

    inputTransfer->num_bytes = XGIP_PACKET_SIZE;
    result = usb_host_transfer_submit(inputTransfer);
    if (result != ESP_OK) {
        queueEvent(UsbXboxEventType::ERROR, result);
        closeController();
        return;
    }
    inputInFlight = true;
    submitNextOutput();
}

void clientEventCallback(
    const usb_host_client_event_msg_t* event,
    void*
) {
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        openController(event->new_dev.address);
    } else if (
        event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
        event->dev_gone.dev_hdl == deviceHandle
    ) {
        disconnectPending = true;
    }
}

void xboxClientTask(void*) {
    const usb_host_client_config_t config{
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = clientEventCallback,
            .callback_arg = nullptr
        }
    };
    const esp_err_t result =
        usb_host_client_register(&config, &clientHandle);
    if (result != ESP_OK) {
        queueEvent(UsbXboxEventType::ERROR, result);
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        const esp_err_t eventResult =
            usb_host_client_handle_events(
                clientHandle,
                pdMS_TO_TICKS(50)
            );
        if (eventResult != ESP_OK && eventResult != ESP_ERR_TIMEOUT) {
            queueEvent(UsbXboxEventType::ERROR, eventResult);
        }
        if (disconnectPending && !inputInFlight && !outputInFlight) {
            closeController();
        }
    }
}

}  // namespace

bool usbXboxControllerBegin() {
    eventQueue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(UsbXboxEvent));
    if (eventQueue == nullptr) {
        return false;
    }
    return xTaskCreatePinnedToCore(
        xboxClientTask,
        "usb_xbox_xgip",
        6144,
        nullptr,
        6,
        nullptr,
        0
    ) == pdPASS;
}

bool usbXboxControllerTakeEvent(UsbXboxEvent& event) {
    return eventQueue != nullptr &&
        xQueueReceive(eventQueue, &event, 0) == pdTRUE;
}
