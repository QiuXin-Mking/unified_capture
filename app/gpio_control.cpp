#include "app/gpio_control.h"

#include <fcntl.h>
#include <gpiod.h>
#include <unistd.h>

namespace {

constexpr const char* kGpioChip = "/dev/gpiochip2";
constexpr int kButtonLine = 8;
constexpr const char* kLedBrightness =
    "/sys/class/leds/sys_led/brightness";
constexpr const char* kLedTrigger = "/sys/class/leds/sys_led/trigger";

}  // namespace

GpioControl::~GpioControl() {
    close();
}

bool GpioControl::open() {
    if (button_) {
        return true;
    }

    close();
    chip_ = gpiod_chip_open(kGpioChip);
    if (!chip_) {
        return false;
    }

    button_ = gpiod_chip_get_line(chip_, kButtonLine);
    if (!button_ ||
        gpiod_line_request_both_edges_events(button_, "capture-btn") < 0) {
        close();
        return false;
    }
    return true;
}

void GpioControl::close() {
    if (button_) {
        gpiod_line_release(button_);
        button_ = nullptr;
    }
    if (chip_) {
        gpiod_chip_close(chip_);
        chip_ = nullptr;
    }
}

int GpioControl::event_fd() const {
    return button_ ? gpiod_line_event_get_fd(button_) : -1;
}

ButtonEvent GpioControl::consume_event() {
    if (!button_) {
        return ButtonEvent::error;
    }

    gpiod_line_event event {};
    if (gpiod_line_event_read(button_, &event) < 0) {
        return ButtonEvent::error;
    }
    if (event.event_type == GPIOD_LINE_EVENT_FALLING_EDGE) {
        return ButtonEvent::falling_edge;
    }
    return ButtonEvent::none;
}

void GpioControl::set_led(bool on) const {
    int fd = ::open(kLedBrightness, O_WRONLY);
    if (fd < 0) {
        return;
    }
    const char value = on ? '1' : '0';
    ssize_t ignored = write(fd, &value, 1);
    (void)ignored;
    ::close(fd);
}

void GpioControl::disable_led_trigger() const {
    int fd = ::open(kLedTrigger, O_WRONLY);
    if (fd < 0) {
        return;
    }
    ssize_t ignored = write(fd, "none", 4);
    (void)ignored;
    ::close(fd);
}
