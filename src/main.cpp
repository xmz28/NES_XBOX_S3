#include <Arduino.h>
#include <string.h>

#include "display_panel.h"
#include "game_menu.h"
#include "game_ui.h"
#include "nes_runtime.h"
#include "touch_input.h"
#include "usb_host_manager.h"
#include "usb_xbox_controller.h"

namespace {

bool gMenuActive = true;
uint16_t gLatestXboxButtons = 0;
bool gWaitForMenuButtonRelease = false;

struct GameTouchGesture {
  bool active = false;
  bool returnCandidate = false;
  uint16_t startY = 0;
  uint16_t lastY = 0;
};

GameTouchGesture gGameTouchGesture;

void processGameTouch() {
  constexpr uint16_t kGestureLeft = 144;
  constexpr uint16_t kGestureRight = 656;
  constexpr uint16_t kGestureStartY = 400;
  constexpr int kReturnSwipeDistance = 100;

  touch_input::State touch = {};
  while (touch_input::takeLatest(touch)) {
    if (!touch.valid) continue;
    if (touch.pressed) {
      if (!gGameTouchGesture.active) {
        gGameTouchGesture.active = true;
        gGameTouchGesture.returnCandidate =
            touch.x >= kGestureLeft && touch.x < kGestureRight &&
            touch.y >= kGestureStartY;
        gGameTouchGesture.startY = touch.y;
        gGameTouchGesture.lastY = touch.y;
      }
      if (gGameTouchGesture.returnCandidate) {
        gGameTouchGesture.lastY = touch.y;
      } else {
        game_ui::handleTouch(touch);
      }
      continue;
    }

    if (!gGameTouchGesture.active) continue;
    if (!gGameTouchGesture.returnCandidate) {
      game_ui::handleTouch(touch);
    } else if (static_cast<int>(gGameTouchGesture.startY) -
                   static_cast<int>(gGameTouchGesture.lastY) >=
               kReturnSwipeDistance) {
      if (nes_runtime::requestReturnToMenu()) {
        Serial.println("MENU: bottom swipe up; requesting game exit");
      }
    }
    gGameTouchGesture = {};
  }
}

uint16_t toNesButtons(uint16_t xboxButtons) {
  uint16_t buttons = 0;
  if ((xboxButtons & USB_XBOX_A) != 0U) buttons |= nes_runtime::kButtonA;
  if ((xboxButtons & (USB_XBOX_X | USB_XBOX_B)) != 0U) {
    buttons |= nes_runtime::kButtonB;
  }
  if ((xboxButtons & USB_XBOX_VIEW) != 0U) {
    buttons |= nes_runtime::kButtonSelect;
  }
  if ((xboxButtons & USB_XBOX_MENU) != 0U) {
    buttons |= nes_runtime::kButtonStart;
  }
  if ((xboxButtons & USB_XBOX_UP) != 0U) buttons |= nes_runtime::kButtonUp;
  if ((xboxButtons & USB_XBOX_DOWN) != 0U) {
    buttons |= nes_runtime::kButtonDown;
  }
  if ((xboxButtons & USB_XBOX_LEFT) != 0U) {
    buttons |= nes_runtime::kButtonLeft;
  }
  if ((xboxButtons & USB_XBOX_RIGHT) != 0U) {
    buttons |= nes_runtime::kButtonRight;
  }
  return buttons;
}

void processXboxEvents() {
  UsbXboxEvent event = {};
  while (usbXboxControllerTakeEvent(event)) {
    switch (event.type) {
      case UsbXboxEventType::CONNECTED:
        gLatestXboxButtons = 0;
        gWaitForMenuButtonRelease = false;
        nes_runtime::setButtons(0);
        game_menu::setButtons(0);
        Serial.printf(
            "XBOX: connected %04X:%04X; D-pad + A/NES-A + X/B/NES-B + View/Select + "
            "Menu/Start\n",
            event.vendorId, event.productId);
        break;
      case UsbXboxEventType::DISCONNECTED:
        gLatestXboxButtons = 0;
        gWaitForMenuButtonRelease = false;
        nes_runtime::setButtons(0);
        game_menu::setButtons(0);
        Serial.println("XBOX: disconnected; NES buttons released");
        break;
      case UsbXboxEventType::INPUT_UPDATED:
        gLatestXboxButtons = event.buttons;
        if (gMenuActive) {
          if (gWaitForMenuButtonRelease) {
            game_menu::setButtons(0);
            if (event.buttons == 0U) {
              gWaitForMenuButtonRelease = false;
              Serial.println("MENU: controller released; menu input enabled");
            }
          } else {
            game_menu::setButtons(toNesButtons(event.buttons));
          }
        } else {
          nes_runtime::setButtons(toNesButtons(event.buttons));
          if ((event.buttons & (USB_XBOX_MENU | USB_XBOX_VIEW)) ==
              (USB_XBOX_MENU | USB_XBOX_VIEW)) {
            if (nes_runtime::requestReturnToMenu()) {
              Serial.println("MENU: View+Menu pressed; requesting game exit");
            }
          }
        }
        break;
      case UsbXboxEventType::ERROR:
        Serial.printf("XBOX: USB error=%d\n", event.error);
        break;
    }
  }
}

void renderInitialUi(uint16_t *frameBuffer, uint16_t width, uint16_t height,
                     void *) {
  if (frameBuffer == nullptr) return;
  memset(frameBuffer, 0, static_cast<size_t>(width) * height * sizeof(uint16_t));
  game_ui::render(frameBuffer, width, height);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("BOOT: NES_XBOX_S3 starting");
  Serial.printf("BOOT: flash=%u psram=%u freeHeap=%u freePsram=%u\n",
                ESP.getFlashChipSize(), ESP.getPsramSize(), ESP.getFreeHeap(),
                ESP.getFreePsram());

  game_ui::begin();
  if (!display_panel::beginBacklight(game_ui::brightnessPercent())) {
    Serial.println("BOOT: GPIO42 backlight PWM initialization failed");
  }
  if (!display_panel::begin()) {
    Serial.println("BOOT: RGB display initialization failed");
    return;
  }
  display_panel::renderAndPresentFrame(renderInitialUi, nullptr);

  if (!touch_input::begin()) {
    Serial.println("BOOT: GT911 unavailable; side sliders disabled");
  }

  if (!usbHostManagerBegin()) {
    Serial.println("BOOT: USB Host initialization failed");
  } else if (!usbXboxControllerBegin()) {
    Serial.println("BOOT: Xbox XGIP client initialization failed");
  } else {
    Serial.println("BOOT: USB Host ready for Xbox 045E:0B12");
  }

  if (!game_menu::begin()) {
    Serial.println("BOOT: ROM catalog unavailable; flash the generated roms.bin");
    return;
  }
  for (uint8_t index = 0; index < 3; ++index) {
    game_menu::renderIfNeeded(true);
  }
}

void loop() {
  processXboxEvents();
  if (gMenuActive) {
    game_menu::processTouch();
    game_menu::tick();
    game_menu::renderIfNeeded();
    uint16_t gameIndex = 0;
    if (game_menu::takeLaunchRequest(gameIndex)) {
      // The menu occupies the whole framebuffer while the emulator only
      // updates the center 512 pixels. Clear all three buffers first so menu
      // labels do not remain behind the side brightness/volume controls.
      for (uint8_t index = 0; index < 3; ++index) {
        display_panel::renderAndPresentFrame(renderInitialUi, nullptr);
      }
      if (nes_runtime::begin(gameIndex)) {
        gMenuActive = false;
        gGameTouchGesture = {};
        Serial.printf("MENU: launching game #%u\n", gameIndex + 1);
      } else {
        Serial.println("MENU: failed to launch selected game");
        game_menu::renderIfNeeded(true);
      }
    }
  } else {
    processGameTouch();
    if (nes_runtime::takeReturnCompleted()) {
      gMenuActive = true;
      gWaitForMenuButtonRelease = gLatestXboxButtons != 0U;
      gGameTouchGesture = {};
      game_menu::setButtons(0);
      Serial.println("MENU: emulator stopped; returning without reboot");
      for (uint8_t index = 0; index < 3; ++index) {
        game_menu::renderIfNeeded(true);
      }
    }
  }
  delay(2);
}
