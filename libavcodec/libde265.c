/*
 * H.265 decoder
 *
 * Copyright (c) 2013, Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * H.265 decoder based on libde265
 */

#include <libde265/de265.h>

#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "avcodec.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"


#define DE265_MAX_PTS_QUEUE 256

typedef struct DE265DecoderContext {
    de265_decoder_context* decoder;

    int64_t pts_queue[DE265_MAX_PTS_QUEUE];
    int pts_queue_len;
    int pts_min_queue_len;
} DE265Context;



static int de265_decode(AVCodecContext *avctx,
                        void *data, int *got_frame, AVPacket *avpkt)
{
    DE265Context *ctx = avctx->priv_data;
    AVFrame *picture = data;
    const struct de265_image *img;
    de265_error err;
    int ret;

    const uint8_t* src[4];
    int stride[4];

    // insert input packet PTS into sorted queue
    if (ctx->pts_queue_len < DE265_MAX_PTS_QUEUE) {
        int pos=0;
        while (ctx->pts_queue[pos] < avpkt->pts &&
            pos<ctx->pts_queue_len) {
            pos++;
        }

        if (pos < ctx->pts_queue_len) {
            memmove(&ctx->pts_queue[pos+1], &ctx->pts_queue[pos],
                sizeof(int64_t) * (ctx->pts_queue_len - pos));
        }

        ctx->pts_queue[pos] = avpkt->pts;
        ctx->pts_queue_len++;
        if (ctx->pts_min_queue_len < ctx->pts_queue_len) {
            ctx->pts_min_queue_len = ctx->pts_queue_len;
        }
    }

    // replace 4-byte length fields with NAL start codes
    uint8_t* avpkt_data = avpkt->data;
    uint8_t* avpkt_end = avpkt->data + avpkt->size;
    while (avpkt_data + 4 < avpkt_end) {
        int nal_size = AV_RB32(avpkt_data);
        AV_WB32(avpkt_data, 0x00000001);
        avpkt_data += 4 + nal_size;
    }

    err = de265_decode_data(ctx->decoder, avpkt->data, avpkt->size);
    if (err != DE265_OK) {
        const char *error  = de265_get_error_text(err);

        av_log(avctx, AV_LOG_ERROR, "Failed to decode frame: %s\n", error);
        return AVERROR_INVALIDDATA;
    }

    if (img = de265_get_next_picture(ctx->decoder)) {
      int width  = de265_get_image_width(img,0);
      int height = de265_get_image_height(img,0);
      if (width != avctx->width || height != avctx->height) {
            if (avctx->width != 0)
                av_log(avctx, AV_LOG_INFO, "dimension change! %dx%d -> %dx%d\n",
                       avctx->width, avctx->height, width, height);

            if (av_image_check_size(width, height, 0, avctx))
                return AVERROR_INVALIDDATA;

            avcodec_set_dimensions(avctx, width, height);
        }
        if (ctx->pts_queue_len < ctx->pts_min_queue_len) {
            // fill pts queue to ensure reordering works
            return avpkt->size;
        }
        if ((ret = ff_get_buffer(avctx, picture, 0)) < 0)
            return ret;

        for (int i=0;i<4;i++) {
            src[i] = de265_get_image_plane(img,i, &stride[i]);
        }

        av_image_copy(picture->data, picture->linesize, src, stride,
                      avctx->pix_fmt, width, height);
        *got_frame = 1;

        // assign next PTS from queue
        if (ctx->pts_queue_len > 0) {
            picture->pkt_pts = ctx->pts_queue[0];

            if (ctx->pts_queue_len>1) {
                memmove(&ctx->pts_queue[0], &ctx->pts_queue[1],
                    sizeof(int64_t) * (ctx->pts_queue_len-1));
            }

            ctx->pts_queue_len--;
        }
    }

    return avpkt->size;
}


static av_cold int de265_free(AVCodecContext *avctx)
{
    DE265Context *ctx = avctx->priv_data;
    de265_free_decoder(ctx->decoder);
    return 0;
}


static av_cold void de265_flush(AVCodecContext *avctx)
{
    DE265Context *ctx = avctx->priv_data;
    ctx->pts_queue_len = 0;
}


static av_cold void de265_static_init(struct AVCodec *codec)
{
    de265_init();
}


#if CONFIG_HEVC_DECODER
static av_cold int de265_ctx_init(AVCodecContext *avctx)
{
    int threads = avctx->thread_count;
    DE265Context *ctx = avctx->priv_data;
    ctx->decoder = de265_new_decoder();
    if (threads <= 0) {
        threads = av_cpu_count();
    }
    if (threads > 0) {
        de265_start_worker_threads(ctx->decoder, threads);
    }
    ctx->pts_queue_len = 0;
    ctx->pts_min_queue_len = 0;

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    return 0;
}


AVCodec ff_hevc_decoder = {
    .name           = "libde265",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .priv_data_size = sizeof(DE265Context),
    .init_static_data = de265_static_init,
    .init           = de265_ctx_init,
    .close          = de265_free,
    .decode         = de265_decode,
    .flush          = de265_flush,
    .capabilities   = CODEC_CAP_DELAY | CODEC_CAP_AUTO_THREADS | CODEC_CAP_DR1 |
                      CODEC_CAP_SLICE_THREADS | CODEC_CAP_FRAME_THREADS,
    .long_name      = NULL_IF_CONFIG_SMALL("libde265 H.265/HEVC decoder"),
};
#endif /* CONFIG_HEVC_DECODER */

