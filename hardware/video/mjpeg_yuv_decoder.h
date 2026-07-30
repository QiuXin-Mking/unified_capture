#pragma once

#include "hardware/video/yuv_to_nv12.h"

#include <turbojpeg.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class MjpegYuvDecoder {
public:
    MjpegYuvDecoder() : handle_(tjInitDecompress()) {
        if (!handle_) {
            error_ = "tjInitDecompress failed";
        }
    }

    ~MjpegYuvDecoder() {
        if (handle_) {
            tjDestroy(handle_);
        }
    }

    MjpegYuvDecoder(const MjpegYuvDecoder&) = delete;
    MjpegYuvDecoder& operator=(const MjpegYuvDecoder&) = delete;

    bool decode(const uint8_t* jpeg, size_t size, DecodedYuvFrame& output) {
        if (!handle_ || !jpeg || size == 0) {
            error_ = "invalid MJPEG decoder input";
            return false;
        }

        int width = 0;
        int height = 0;
        int subsamp = 0;
        int colorspace = 0;
        if (tjDecompressHeader3(handle_, jpeg, static_cast<unsigned long>(size),
                                &width, &height, &subsamp, &colorspace) != 0) {
            error_ = tjGetErrorStr2(handle_);
            return false;
        }
        if (width <= 0 || height <= 0 || !map_subsampling(subsamp)) {
            if (error_.empty()) {
                error_ = "invalid MJPEG dimensions";
            }
            return false;
        }

        const int plane_count = subsamp == TJSAMP_GRAY ? 1 : 3;
        std::array<unsigned char*, 3> destinations{};
        std::array<int, 3> strides{};
        for (int component = 0; component < plane_count; ++component) {
            const int plane_width = tjPlaneWidth(component, width, subsamp);
            const int plane_height = tjPlaneHeight(component, height, subsamp);
            if (plane_width <= 0 || plane_height <= 0) {
                error_ = "invalid TurboJPEG plane dimensions";
                return false;
            }
            plane_widths_[component] = plane_width;
            plane_heights_[component] = plane_height;
            strides[component] = plane_width;
            planes_[component].resize(
                static_cast<size_t>(plane_width) * plane_height);
            destinations[component] = planes_[component].data();
        }

        if (tjDecompressToYUVPlanes(
                handle_, jpeg, static_cast<unsigned long>(size),
                destinations.data(), width, strides.data(), height,
                TJFLAG_FASTDCT) != 0) {
            error_ = tjGetErrorStr2(handle_);
            return false;
        }

        output.width = width;
        output.height = height;
        output.subsampling = mapped_subsampling_;
        output.y = {planes_[0].data(), plane_widths_[0], plane_heights_[0],
                    plane_widths_[0]};
        output.u = {};
        output.v = {};
        if (plane_count == 3) {
            output.u = {planes_[1].data(), plane_widths_[1], plane_heights_[1],
                        plane_widths_[1]};
            output.v = {planes_[2].data(), plane_widths_[2], plane_heights_[2],
                        plane_widths_[2]};
        }
        error_.clear();
        return true;
    }

    const char* error() const {
        return error_.c_str();
    }

private:
    bool map_subsampling(int subsamp) {
        switch (subsamp) {
            case TJSAMP_420:
                mapped_subsampling_ = YuvSubsampling::yuv420;
                return true;
            case TJSAMP_422:
                mapped_subsampling_ = YuvSubsampling::yuv422;
                return true;
            case TJSAMP_444:
                mapped_subsampling_ = YuvSubsampling::yuv444;
                return true;
            case TJSAMP_GRAY:
                mapped_subsampling_ = YuvSubsampling::gray;
                return true;
            default:
                error_ = "unsupported MJPEG chroma subsampling";
                return false;
        }
    }

    tjhandle handle_ = nullptr;
    std::array<std::vector<uint8_t>, 3> planes_;
    std::array<int, 3> plane_widths_{};
    std::array<int, 3> plane_heights_{};
    YuvSubsampling mapped_subsampling_ = YuvSubsampling::gray;
    std::string error_;
};
