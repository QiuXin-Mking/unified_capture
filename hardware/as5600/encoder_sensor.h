#pragma once
/*
 * encoder_sensor.h — AS5600Sensor: 磁编码器 I2C 读取
 *
 * 通过 extern "C" 调用 hardware/as5600 下的驱动.
 * 采样间隔 10ms (100Hz), 输出 CSV 带 CLOCK_MONOTONIC 时间戳.
 */

#include "sensor.h"

extern "C" {
#include "hardware/as5600/as5600.h"
}

// elapsed_us() / g_t0 定义在 sensor.h

class EncoderSensor : public Sensor {
public:
    EncoderSensor(const std::string& i2c_path,
                  uint8_t i2c_addr,
                  const std::string& session_dir,
                  int session_num,
                  const std::string& session_ts,
                  int interval_us,
                  std::atomic<bool>& running)
        : Sensor("as5600", running)
        , i2c_path_(i2c_path)
        , i2c_addr_(i2c_addr)
        , session_dir_(session_dir)
        , session_num_(session_num)
        , session_ts_(session_ts)
        , interval_us_(interval_us) {}

protected:
    void setup() override {
        dev_ = as5600_open(i2c_path_.c_str(), i2c_addr_);
        if (!dev_) {
            fprintf(stderr, "[as5600] open %s failed\n", i2c_path_.c_str());
            return;
        }
        if (as5600_probe(dev_) < 0) {
            fprintf(stderr, "[as5600] probe failed at 0x%02x on %s\n",
                    i2c_addr_, i2c_path_.c_str());
            as5600_close(dev_);
            dev_ = nullptr;
            return;
        }

        char path[256];
        snprintf(path, sizeof(path), "%s/encoder-%s.jsonl",
                 session_dir_.c_str(), session_ts_.c_str());
        fp_ = fopen(path, "w");

        printf("[as5600] setup OK (bus=%s, addr=0x%02x, interval=%dus)\n",
               i2c_path_.c_str(), i2c_addr_, interval_us_);
        initialized_ = true;
    }

    void collect() override {
        if (!initialized_) return;

        while (running_) {
            uint64_t ts_now = elapsed_us();
            uint16_t raw = as5600_get_raw_angle(dev_);
            float deg = as5600_convert_raw_angle_to_degrees(raw);
            uint8_t magnet = as5600_detect_magnet(dev_);

            if (fp_) {
                fprintf(fp_,
                        "{\"ts_us\":%llu,\"raw_angle\":%u,\"degrees\":%.3f,\"magnet_detected\":%u}\n",
                        (unsigned long long)ts_now, raw, deg, magnet);
                fflush(fp_);
            }
            usleep(interval_us_);
        }
    }

    void teardown() override {
        if (dev_) { as5600_close(dev_); dev_ = nullptr; }
        if (fp_)  { fclose(fp_); fp_ = nullptr; }
        printf("[as5600] teardown OK\n");
    }

private:
    std::string i2c_path_;
    uint8_t i2c_addr_;
    std::string session_dir_;
    int session_num_;
    std::string session_ts_;
    int interval_us_;
    as5600_dev_t dev_ = nullptr;
    FILE* fp_ = nullptr;
    bool initialized_ = false;
};
