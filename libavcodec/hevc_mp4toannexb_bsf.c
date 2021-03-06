/*
 * HEVC MP4 to Annex B byte stream format filter
 * copyright (c) 2015 Anton Khirnov
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "bytestream.h"
#include "hevc.h"

#define MIN_HEVCC_LENGTH 23

typedef struct HEVCBSFContext {
    uint8_t  length_size;
    int      extradata_parsed;

    int logged_nonmp4_warning;
} HEVCBSFContext;

static int hevc_extradata_to_annexb(AVCodecContext *avctx)
{
    GetByteContext gb;
    int length_size, num_arrays, i, j;
    int ret = 0;

    uint8_t *new_extradata = NULL;
    size_t   new_extradata_size = 0;;

    bytestream2_init(&gb, avctx->extradata, avctx->extradata_size);

    bytestream2_skip(&gb, 21);
    length_size = (bytestream2_get_byte(&gb) & 3) + 1;
    num_arrays  = bytestream2_get_byte(&gb);

    for (i = 0; i < num_arrays; i++) {
        int type = bytestream2_get_byte(&gb) & 0x3f;
        int cnt  = bytestream2_get_be16(&gb);

        if (!(type == NAL_VPS || type == NAL_SPS || type == NAL_PPS ||
              type == NAL_SEI_PREFIX || type == NAL_SEI_SUFFIX)) {
            av_log(avctx, AV_LOG_ERROR, "Invalid NAL unit type in extradata: %d\n",
                   type);
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        for (j = 0; j < cnt; j++) {
            int nalu_len = bytestream2_get_be16(&gb);

            if (4 + FF_INPUT_BUFFER_PADDING_SIZE + nalu_len > SIZE_MAX - new_extradata_size) {
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            ret = av_reallocp(&new_extradata, new_extradata_size + nalu_len + 4 + FF_INPUT_BUFFER_PADDING_SIZE);
            if (ret < 0)
                goto fail;

            AV_WB32(new_extradata + new_extradata_size, 1); // add the startcode
            bytestream2_get_buffer(&gb, new_extradata + new_extradata_size + 4, nalu_len);
            new_extradata_size += 4 + nalu_len;
            memset(new_extradata + new_extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
        }
    }

    av_freep(&avctx->extradata);
    avctx->extradata      = new_extradata;
    avctx->extradata_size = new_extradata_size;

    if (!new_extradata_size)
        av_log(avctx, AV_LOG_WARNING, "No parameter sets in the extradata\n");

    return length_size;
fail:
    av_freep(&new_extradata);
    return ret;
}

static int hevc_mp4toannexb_filter(AVBitStreamFilterContext *bsfc,
                                   AVCodecContext *avctx, const char *args,
                                   uint8_t **poutbuf, int *poutbuf_size,
                                   const uint8_t *buf, int buf_size,
                                   int keyframe)
{
    HEVCBSFContext *ctx = bsfc->priv_data;
    GetByteContext gb;

    uint8_t *out = NULL;
    size_t   out_size = 0;
    int got_irap = 0;
    int i, ret = 0;

    if (!ctx->extradata_parsed) {
        if (avctx->extradata_size < MIN_HEVCC_LENGTH ||
            AV_RB24(avctx->extradata) == 1           ||
            AV_RB32(avctx->extradata) == 1) {
            if (!ctx->logged_nonmp4_warning) {
                av_log(avctx, AV_LOG_VERBOSE,
                       "The input looks like it is Annex B already\n");
                ctx->logged_nonmp4_warning = 1;
            }
            *poutbuf      = buf;
            *poutbuf_size = buf_size;
            return 0;
        }

        ret = hevc_extradata_to_annexb(avctx);
        if (ret < 0)
            return ret;
        ctx->length_size      = ret;
        ctx->extradata_parsed = 1;
    }

    *poutbuf_size = 0;
    *poutbuf      = NULL;

    bytestream2_init(&gb, buf, buf_size);

    while (bytestream2_get_bytes_left(&gb)) {
        uint32_t nalu_size = 0;
        int      nalu_type;
        int is_irap, add_extradata, extra_size;

        for (i = 0; i < ctx->length_size; i++)
            nalu_size = (nalu_size << 8) | bytestream2_get_byte(&gb);

        nalu_type = (bytestream2_peek_byte(&gb) >> 1) & 0x3f;

        /* prepend extradata to IRAP frames */
        is_irap       = nalu_type >= 16 && nalu_type <= 23;
        add_extradata = is_irap && !got_irap;
        extra_size    = add_extradata * avctx->extradata_size;
        got_irap     |= is_irap;

        if (SIZE_MAX - out_size < 4             ||
            SIZE_MAX - out_size - 4 < nalu_size ||
            SIZE_MAX - out_size - 4 - nalu_size < extra_size) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        ret = av_reallocp(&out, out_size + 4 + nalu_size + extra_size);
        if (ret < 0)
            goto fail;

        if (add_extradata)
            memcpy(out + out_size, avctx->extradata, extra_size);
        AV_WB32(out + out_size + extra_size, 1);
        bytestream2_get_buffer(&gb, out + out_size + 4 + extra_size, nalu_size);
        out_size += 4 + nalu_size + extra_size;
    }

    *poutbuf      = out;
    *poutbuf_size = out_size;

    return 1;

fail:
    av_freep(&out);
    return ret;
}

AVBitStreamFilter ff_hevc_mp4toannexb_bsf = {
    "hevc_mp4toannexb",
    sizeof(HEVCBSFContext),
    hevc_mp4toannexb_filter,
};
