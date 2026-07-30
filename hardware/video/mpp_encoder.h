#pragma once
/*
 * mpp_encoder.h — MPP H.265 硬件编码器
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unistd.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>

struct MppPutResult {
    bool ok = false;
    size_t bytes = 0;

    // Keeps the legacy synchronous sensors buildable until Task 6 replaces
    // their collect loops with VideoFrameProcessor.
    operator size_t() const { return bytes; }
};

struct MppEncoder {
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppBufferGroup buf_group = nullptr;
    uint32_t frame_size = 0;
    uint32_t width = 0, height = 0;
    uint32_t hor_stride = 0, ver_stride = 0;
    mutable std::mutex mutex_;

    bool init(uint32_t w, uint32_t h, int bps, int fps, int gop) {
        width = w; height = h;
        // MPP requires horizontal stride aligned to 64 bytes for NV12.
        hor_stride = (w + 63) & ~63U;
        ver_stride = h;
        frame_size = hor_stride * h * 3 / 2;
        MPP_RET ret;

        ret = mpp_create(&ctx, &mpi);
        if (ret != MPP_OK) { fprintf(stderr, "mpp_create failed\n"); return false; }

        ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingHEVC);
        if (ret != MPP_OK) { fprintf(stderr, "mpp_init failed\n"); return false; }

        // Prep 配置
        MppEncPrepCfg prep;
        memset(&prep, 0, sizeof(prep));
        prep.change      = 0xFFFFFFFF;
        prep.width       = w;
        prep.height      = h;
        prep.hor_stride  = hor_stride;
        prep.ver_stride  = ver_stride;
        prep.format      = MPP_FMT_YUV420SP;
        ret = mpi->control(ctx, MPP_ENC_SET_PREP_CFG, &prep);
        if (ret != MPP_OK) { fprintf(stderr, "MPP_ENC_SET_PREP_CFG failed\n"); return false; }

        // RC 配置 (CBR)
        MppEncRcCfg rc;
        memset(&rc, 0, sizeof(rc));
        rc.change       = 0xFFFFFFFF;
        rc.rc_mode      = MPP_ENC_RC_MODE_CBR;
        rc.bps_target   = bps;
        rc.bps_max      = bps * 3 / 2;
        rc.bps_min      = bps / 8;
        rc.fps_in_num   = fps;
        rc.fps_in_denorm  = 1;
        rc.fps_out_num  = fps;
        rc.fps_out_denorm = 1;
        rc.gop          = gop;
        ret = mpi->control(ctx, MPP_ENC_SET_RC_CFG, &rc);
        if (ret != MPP_OK) { fprintf(stderr, "MPP_ENC_SET_RC_CFG failed\n"); return false; }

        // Codec 配置
        MppEncCodecCfg codec;
        memset(&codec, 0, sizeof(codec));
        codec.coding = MPP_VIDEO_CodingHEVC;
        codec.h265.change = 0xFFFFFFFF;
        ret = mpi->control(ctx, MPP_ENC_SET_CODEC_CFG, &codec);
        if (ret != MPP_OK) { fprintf(stderr, "MPP_ENC_SET_CODEC_CFG failed\n"); return false; }

        // Buffer group
        ret = mpp_buffer_group_get(&buf_group, MPP_BUFFER_TYPE_DRM,
                                   MPP_BUFFER_INTERNAL, "he", NULL);
        if (ret != MPP_OK) { fprintf(stderr, "mpp_buffer_group_get failed\n"); return false; }

        return true;
    }

    // 编码一帧 NV12 → H.265 NAL, 直接写 FILE*
    MppPutResult put(const uint8_t* nv12, FILE* fp) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ctx || !mpi || !nv12 || !fp) {
            return {};
        }

        MppFrame frame = nullptr;
        MPP_RET ret = mpp_frame_init(&frame);
        if (ret != MPP_OK || !frame) {
            fprintf(stderr, "[MPP] mpp_frame_init failed ret=%d\n", ret);
            return {};
        }
        mpp_frame_set_width(frame, width);
        mpp_frame_set_height(frame, height);
        mpp_frame_set_hor_stride(frame, hor_stride);
        mpp_frame_set_ver_stride(frame, ver_stride);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_eos(frame, 0);

        MppBuffer buf = nullptr;
        ret = mpp_buffer_get(buf_group, &buf, frame_size);
        if (ret != MPP_OK || !buf) {
            // Buffer temporarily exhausted; wait a bit and retry once.
            usleep(5000);
            ret = mpp_buffer_get(buf_group, &buf, frame_size);
        }
        if (ret != MPP_OK || !buf) {
            fprintf(stderr, "[MPP] mpp_buffer_get failed ret=%d buf=%p\n", ret, (void*)buf);
            mpp_frame_deinit(&frame);
            return {};
        }

        void* ptr = mpp_buffer_get_ptr(buf);
        if (!ptr) {
            fprintf(stderr, "[MPP] mpp_buffer_get_ptr returned NULL\n");
            mpp_buffer_put(buf);
            mpp_frame_deinit(&frame);
            return {};
        }

        memcpy(ptr, nv12, frame_size);

        mpp_frame_set_buffer(frame, buf);

        ret = mpi->encode_put_frame(ctx, frame);
        mpp_frame_deinit(&frame);
        mpp_buffer_put(buf);
        if (ret != MPP_OK) {
            fprintf(stderr, "[MPP] encode_put_frame failed ret=%d\n", ret);
            return {};
        }

        MppPutResult result{true, 0};
        MppPacket pkt = nullptr;
        ret = mpi->encode_get_packet(ctx, &pkt);
        if (ret != MPP_OK) {
            fprintf(stderr, "[MPP] encode_get_packet failed ret=%d\n", ret);
            return {};
        }
        if (pkt) {
            size_t len = mpp_packet_get_length(pkt);
            if (len > 0) {
                const size_t written =
                    fwrite(mpp_packet_get_data(pkt), 1, len, fp);
                if (written != len || ferror(fp)) {
                    fprintf(stderr,
                            "[MPP] H.265 FIFO write failed (%zu/%zu)\n",
                            written, len);
                    mpp_packet_deinit(&pkt);
                    return {};
                }
                result.bytes = written;
            }
            mpp_packet_deinit(&pkt);
        }
        return result;
    }

    // Flush 编码器 (发送 EOS, 排空残余帧)
    size_t flush(FILE* fp) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        MppFrame eos;
        mpp_frame_init(&eos);
        mpp_frame_set_eos(eos, 1);
        mpi->encode_put_frame(ctx, eos);
        mpp_frame_deinit(&eos);

        for (int i = 0; i < 20; i++) {
            MppPacket pkt = nullptr;
            if (mpi->encode_get_packet(ctx, &pkt) || !pkt) break;
            size_t len = mpp_packet_get_length(pkt);
            if (len) { fwrite(mpp_packet_get_data(pkt), 1, len, fp); total += len; }
            mpp_packet_deinit(&pkt);
        }
        return total;
    }

    void destroy() {
        if (ctx) {
            mpi->reset(ctx);
            mpp_destroy(ctx);
            ctx = nullptr;
            mpi = nullptr;
        }
        // buf_group 是 MPP_BUFFER_INTERNAL, 由 context 管理, 不需要手动释放
    }
};
