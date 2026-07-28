#pragma once

struct gpiod_chip;
struct gpiod_line;

enum class ButtonEvent { none, falling_edge, error };

class GpioControl {
public:
    GpioControl() = default;
    ~GpioControl();

    GpioControl(const GpioControl&) = delete;
    GpioControl& operator=(const GpioControl&) = delete;

    bool open();
    void close();
    int event_fd() const;
    ButtonEvent consume_event();
    void set_led(bool on) const;
    void disable_led_trigger() const;

private:
    gpiod_chip* chip_ = nullptr;
    gpiod_line* button_ = nullptr;
};
