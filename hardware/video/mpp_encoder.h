#pragma once
/*
 * mpp_encoder.h — MPP H.265 硬件编码器
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_err.h>

struct MppEncoder {
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppBufferGroup buf_group = nullptr;
    uint32_t frame_size = 0;
    uint32_t width = 0, height = 0;

    bool init(uint32_t w, uint32_t h, int bps, int fps, int gop) {
        width = w; height = h;
        frame_size = w * h * 3 / 2;
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
        prep.hor_stride  = w;
        prep.ver_stride  = h;
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

        // Buffer group — pre-allocate enough buffers for 4 concurrent
        // 4K-class encoders; default internal group size is too small.
        ret = mpp_buffer_group_get(&buf_group, MPP_BUFFER_TYPE_DRM,
                                   MPP_BUFFER_INTERNAL, "he", NULL);
        if (ret != MPP_OK) { fprintf(stderr, "mpp_buffer_group_get failed\n"); return false; }
        ret = mpp_buffer_group_limit_config(buf_group, frame_size, 8);
        if (ret != MPP_OK) { fprintf(stderr, "mpp_buffer_group_limit_config failed\n"); return false; }

        return true;
    }

    // 编码一帧 NV12 → H.265 NAL, 直接写 FILE*
    size_t put(uint8_t* nv12, FILE* fp) {
        MppFrame frame;
        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, width);
        mpp_frame_set_height(frame, height);
        mpp_frame_set_hor_stride(frame, width);
        mpp_frame_set_ver_stride(frame, height);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_eos(frame, 0);

        MppBuffer buf = nullptr;
        MPP_RET ret = mpp_buffer_get(buf_group, &buf, frame_size);
        if (ret != MPP_OK || !buf) {
            fprintf(stderr, "[MPP] mpp_buffer_get failed ret=%d buf=%p\n", ret, (void*)buf);
            mpp_frame_deinit(&frame);
            return 0;
        }

        void* ptr = mpp_buffer_get_ptr(buf);
        if (!ptr) {
            fprintf(stderr, "[MPP] mpp_buffer_get_ptr returned NULL\n");
            mpp_frame_deinit(&frame);
            return 0;
        }

        memcpy(ptr, nv12, frame_size);

        mpp_frame_set_buffer(frame, buf);

        ret = mpi->encode_put_frame(ctx, frame);
        mpp_frame_deinit(&frame);

        // 取编码结果
        MppPacket pkt = nullptr;
        size_t written = 0;
        MPP_RET pkt_ret = mpi->encode_get_packet(ctx, &pkt);
        if (!pkt_ret && pkt && mpp_packet_get_length(pkt) > 0) {
            written = mpp_packet_get_length(pkt);
            fwrite(mpp_packet_get_data(pkt), 1, written, fp);
        }
        if (pkt) mpp_packet_deinit(&pkt);
        return written;
    }

    // Flush 编码器 (发送 EOS, 排空残余帧)
    size_t flush(FILE* fp) {
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
