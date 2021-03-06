/*
 * H.264 encoding using the x264 library
 * Copyright (C) 2005  Mans Rullgard <mans@mansr.com>
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

#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/stereo3d.h"
#include "avcodec.h"
#include "internal.h"

#if defined(_MSC_VER)
#define X264_API_IMPORTS 1
#endif

#include <x264.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct X264Context {
    AVClass        *class;
    x264_param_t    params;
    x264_t         *enc;
    x264_picture_t  pic;
    uint8_t        *sei;
    int             sei_size;
    char *preset;
    char *tune;
    char *profile;
    int fastfirstpass;
    float crf;
    float crf_max;
    int cqp;
    int aq_mode;
    float aq_strength;
    char *psy_rd;
    int psy;
    int rc_lookahead;
    int weightp;
    int weightb;
    int ssim;
    int intra_refresh;
    int bluray_compat;
    int b_bias;
    int b_pyramid;
    int mixed_refs;
    int dct8x8;
    int fast_pskip;
    int aud;
    int mbtree;
    char *deblock;
    float cplxblur;
    char *partitions;
    int direct_pred;
    int slice_max_size;
    char *stats;
    int nal_hrd;
    char *x264_params;
} X264Context;

static void X264_log(void *p, int level, const char *fmt, va_list args)
{
    static const int level_map[] = {
        [X264_LOG_ERROR]   = AV_LOG_ERROR,
        [X264_LOG_WARNING] = AV_LOG_WARNING,
        [X264_LOG_INFO]    = AV_LOG_INFO,
        [X264_LOG_DEBUG]   = AV_LOG_DEBUG
    };

    if (level < 0 || level > X264_LOG_DEBUG)
        return;

    av_vlog(p, level_map[level], fmt, args);
}


static int encode_nals(AVCodecContext *ctx, AVPacket *pkt,
                       x264_nal_t *nals, int nnal)
{
    X264Context *x4 = ctx->priv_data;
    uint8_t *p;
    int i, size = x4->sei_size, ret;

    if (!nnal)
        return 0;

    for (i = 0; i < nnal; i++)
        size += nals[i].i_payload;

    if ((ret = ff_alloc_packet(pkt, size)) < 0)
        return ret;

    p = pkt->data;

    /* Write the SEI as part of the first frame. */
    if (x4->sei_size > 0 && nnal > 0) {
        memcpy(p, x4->sei, x4->sei_size);
        p += x4->sei_size;
        x4->sei_size = 0;
    }

    for (i = 0; i < nnal; i++){
        memcpy(p, nals[i].p_payload, nals[i].i_payload);
        p += nals[i].i_payload;
    }

    return 1;
}

static void reconfig_encoder(AVCodecContext *ctx, const AVFrame *frame)
{
    X264Context *x4 = ctx->priv_data;
    AVFrameSideData *side_data;


    if (x4->params.b_tff != frame->top_field_first) {
        x4->params.b_tff = frame->top_field_first;
        x264_encoder_reconfig(x4->enc, &x4->params);
    }
    if (x4->params.vui.i_sar_height != ctx->sample_aspect_ratio.den ||
        x4->params.vui.i_sar_width  != ctx->sample_aspect_ratio.num) {
        x4->params.vui.i_sar_height = ctx->sample_aspect_ratio.den;
        x4->params.vui.i_sar_width  = ctx->sample_aspect_ratio.num;
        x264_encoder_reconfig(x4->enc, &x4->params);
    }

    if (x4->params.rc.i_vbv_buffer_size != ctx->rc_buffer_size / 1000 ||
        x4->params.rc.i_vbv_max_bitrate != ctx->rc_max_rate    / 1000) {
        x4->params.rc.i_vbv_buffer_size = ctx->rc_buffer_size / 1000;
        x4->params.rc.i_vbv_max_bitrate = ctx->rc_max_rate    / 1000;
        x264_encoder_reconfig(x4->enc, &x4->params);
    }

    if (x4->params.rc.i_rc_method == X264_RC_ABR &&
        x4->params.rc.i_bitrate != ctx->bit_rate / 1000) {
        x4->params.rc.i_bitrate = ctx->bit_rate / 1000;
        x264_encoder_reconfig(x4->enc, &x4->params);
    }

    if (x4->crf >= 0 &&
        x4->params.rc.i_rc_method == X264_RC_CRF &&
        x4->params.rc.f_rf_constant != x4->crf) {
        x4->params.rc.f_rf_constant = x4->crf;
        x264_encoder_reconfig(x4->enc, &x4->params);
    }

    if (x4->params.rc.i_rc_method == X264_RC_CQP &&
        x4->params.rc.i_qp_constant != x4->cqp) {
        x4->params.rc.i_qp_constant = x4->cqp;
        x264_encoder_reconfig(x4->enc, &x4->params);
    }

    if (x4->crf_max >= 0 &&
        x4->params.rc.f_rf_constant_max != x4->crf_max) {
        x4->params.rc.f_rf_constant_max = x4->crf_max;
        x264_encoder_reconfig(x4->enc, &x4->params);
    }

    side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_STEREO3D);
    if (side_data) {
        AVStereo3D *stereo = (AVStereo3D *)side_data->data;
        int fpa_type;

        switch (stereo->type) {
        case AV_STEREO3D_CHECKERBOARD:
            fpa_type = 0;
            break;
        case AV_STEREO3D_COLUMNS:
            fpa_type = 1;
            break;
        case AV_STEREO3D_LINES:
            fpa_type = 2;
            break;
        case AV_STEREO3D_SIDEBYSIDE:
            fpa_type = 3;
            break;
        case AV_STEREO3D_TOPBOTTOM:
            fpa_type = 4;
            break;
        case AV_STEREO3D_FRAMESEQUENCE:
            fpa_type = 5;
            break;
        default:
            fpa_type = -1;
            break;
        }

        if (fpa_type != x4->params.i_frame_packing) {
            x4->params.i_frame_packing = fpa_type;
            x264_encoder_reconfig(x4->enc, &x4->params);
        }
    }
}

static int X264_frame(AVCodecContext *ctx, AVPacket *pkt, const AVFrame *frame,
                      int *got_packet)
{
    X264Context *x4 = ctx->priv_data;
    x264_nal_t *nal;
    int nnal, i, ret;
    x264_picture_t pic_out;

    x264_picture_init( &x4->pic );
    x4->pic.img.i_csp   = x4->params.i_csp;
    if (x264_bit_depth > 8)
        x4->pic.img.i_csp |= X264_CSP_HIGH_DEPTH;
    x4->pic.img.i_plane = 3;

    if (frame) {
        for (i = 0; i < 3; i++) {
            x4->pic.img.plane[i]    = frame->data[i];
            x4->pic.img.i_stride[i] = frame->linesize[i];
        }

        x4->pic.i_pts  = frame->pts;
        x4->pic.i_type =
            frame->pict_type == AV_PICTURE_TYPE_I ? X264_TYPE_KEYFRAME :
            frame->pict_type == AV_PICTURE_TYPE_P ? X264_TYPE_P :
            frame->pict_type == AV_PICTURE_TYPE_B ? X264_TYPE_B :
                                            X264_TYPE_AUTO;
        reconfig_encoder(ctx, frame);
    }
    do {
        if (x264_encoder_encode(x4->enc, &nal, &nnal, frame? &x4->pic: NULL, &pic_out) < 0)
            return AVERROR_UNKNOWN;

        ret = encode_nals(ctx, pkt, nal, nnal);
        if (ret < 0)
            return ret;
    } while (!ret && !frame && x264_encoder_delayed_frames(x4->enc));

    pkt->pts = pic_out.i_pts;
    pkt->dts = pic_out.i_dts;

#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
    switch (pic_out.i_type) {
    case X264_TYPE_IDR:
    case X264_TYPE_I:
        ctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;
        break;
    case X264_TYPE_P:
        ctx->coded_frame->pict_type = AV_PICTURE_TYPE_P;
        break;
    case X264_TYPE_B:
    case X264_TYPE_BREF:
        ctx->coded_frame->pict_type = AV_PICTURE_TYPE_B;
        break;
    }
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    pkt->flags |= AV_PKT_FLAG_KEY*pic_out.b_keyframe;
    if (ret) {
        uint8_t *sd = av_packet_new_side_data(pkt, AV_PKT_DATA_QUALITY_FACTOR,
                                              sizeof(int));
        if (!sd)
            return AVERROR(ENOMEM);
        *(int *)sd = (pic_out.i_qpplus1 - 1) * FF_QP2LAMBDA;

#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
        ctx->coded_frame->quality = (pic_out.i_qpplus1 - 1) * FF_QP2LAMBDA;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    }

    *got_packet = ret;
    return 0;
}

static av_cold int X264_close(AVCodecContext *avctx)
{
    X264Context *x4 = avctx->priv_data;

    av_freep(&avctx->extradata);
    av_freep(&x4->sei);

    if (x4->enc) {
        x264_encoder_close(x4->enc);
        x4->enc = NULL;
    }

    return 0;
}

static int convert_pix_fmt(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV420P9:
    case AV_PIX_FMT_YUV420P10: return X264_CSP_I420;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUV422P10: return X264_CSP_I422;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUV444P9:
    case AV_PIX_FMT_YUV444P10: return X264_CSP_I444;
    case AV_PIX_FMT_NV12:      return X264_CSP_NV12;
    case AV_PIX_FMT_NV16:
    case AV_PIX_FMT_NV20:      return X264_CSP_NV16;
    };
    return 0;
}

#define PARSE_X264_OPT(name, var)\
    if (x4->var && x264_param_parse(&x4->params, name, x4->var) < 0) {\
        av_log(avctx, AV_LOG_ERROR, "Error parsing option '%s' with value '%s'.\n", name, x4->var);\
        return AVERROR(EINVAL);\
    }

static av_cold int X264_init(AVCodecContext *avctx)
{
    X264Context *x4 = avctx->priv_data;

#if CONFIG_LIBX262_ENCODER
    if (avctx->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        x4->params.b_mpeg2 = 1;
        x264_param_default_mpeg2(&x4->params);
    } else
#else
    x264_param_default(&x4->params);
#endif

    x4->params.b_deblocking_filter         = avctx->flags & CODEC_FLAG_LOOP_FILTER;

    if (x4->preset || x4->tune)
        if (x264_param_default_preset(&x4->params, x4->preset, x4->tune) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Error setting preset/tune %s/%s.\n", x4->preset, x4->tune);
            return AVERROR(EINVAL);
        }

    if (avctx->level > 0)
        x4->params.i_level_idc = avctx->level;

    x4->params.pf_log               = X264_log;
    x4->params.p_log_private        = avctx;
    x4->params.i_log_level          = X264_LOG_DEBUG;
    x4->params.i_csp                = convert_pix_fmt(avctx->pix_fmt);

    if (avctx->bit_rate) {
        x4->params.rc.i_bitrate   = avctx->bit_rate / 1000;
        x4->params.rc.i_rc_method = X264_RC_ABR;
    }
    x4->params.rc.i_vbv_buffer_size = avctx->rc_buffer_size / 1000;
    x4->params.rc.i_vbv_max_bitrate = avctx->rc_max_rate    / 1000;
    x4->params.rc.b_stat_write      = avctx->flags & CODEC_FLAG_PASS1;
    if (avctx->flags & CODEC_FLAG_PASS2) {
        x4->params.rc.b_stat_read = 1;
    } else {
        if (x4->crf >= 0) {
            x4->params.rc.i_rc_method   = X264_RC_CRF;
            x4->params.rc.f_rf_constant = x4->crf;
        } else if (x4->cqp >= 0) {
            x4->params.rc.i_rc_method   = X264_RC_CQP;
            x4->params.rc.i_qp_constant = x4->cqp;
        }

        if (x4->crf_max >= 0)
            x4->params.rc.f_rf_constant_max = x4->crf_max;
    }

    if (avctx->rc_buffer_size && avctx->rc_initial_buffer_occupancy > 0 &&
        (avctx->rc_initial_buffer_occupancy <= avctx->rc_buffer_size)) {
        x4->params.rc.f_vbv_buffer_init =
            (float)avctx->rc_initial_buffer_occupancy / avctx->rc_buffer_size;
    }

    if (avctx->i_quant_factor > 0)
        x4->params.rc.f_ip_factor         = 1 / fabs(avctx->i_quant_factor);
    x4->params.rc.f_pb_factor             = avctx->b_quant_factor;
    x4->params.analyse.i_chroma_qp_offset = avctx->chromaoffset;

    if (avctx->me_method == ME_EPZS)
        x4->params.analyse.i_me_method = X264_ME_DIA;
    else if (avctx->me_method == ME_HEX)
        x4->params.analyse.i_me_method = X264_ME_HEX;
    else if (avctx->me_method == ME_UMH)
        x4->params.analyse.i_me_method = X264_ME_UMH;
    else if (avctx->me_method == ME_FULL)
        x4->params.analyse.i_me_method = X264_ME_ESA;
    else if (avctx->me_method == ME_TESA)
        x4->params.analyse.i_me_method = X264_ME_TESA;

    if (avctx->gop_size >= 0)
        x4->params.i_keyint_max         = avctx->gop_size;
    if (avctx->max_b_frames >= 0)
        x4->params.i_bframe             = avctx->max_b_frames;
    if (avctx->scenechange_threshold >= 0)
        x4->params.i_scenecut_threshold = avctx->scenechange_threshold;
    if (avctx->qmin >= 0)
        x4->params.rc.i_qp_min          = avctx->qmin;
    if (avctx->qmax >= 0)
        x4->params.rc.i_qp_max          = avctx->qmax;
    if (avctx->max_qdiff >= 0)
        x4->params.rc.i_qp_step         = avctx->max_qdiff;
    if (avctx->qblur >= 0)
        x4->params.rc.f_qblur           = avctx->qblur;     /* temporally blur quants */
    if (avctx->qcompress >= 0)
        x4->params.rc.f_qcompress       = avctx->qcompress; /* 0.0 => cbr, 1.0 => constant qp */
    if (avctx->refs >= 0)
        x4->params.i_frame_reference    = avctx->refs;
    if (avctx->trellis >= 0)
        x4->params.analyse.i_trellis    = avctx->trellis;
    if (avctx->me_range >= 0)
        x4->params.analyse.i_me_range   = avctx->me_range;
    if (avctx->noise_reduction >= 0)
        x4->params.analyse.i_noise_reduction = avctx->noise_reduction;
    if (avctx->me_subpel_quality >= 0)
        x4->params.analyse.i_subpel_refine   = avctx->me_subpel_quality;
    if (avctx->b_frame_strategy >= 0)
        x4->params.i_bframe_adaptive = avctx->b_frame_strategy;
    if (avctx->keyint_min >= 0)
        x4->params.i_keyint_min = avctx->keyint_min;
    if (avctx->coder_type >= 0)
        x4->params.b_cabac = avctx->coder_type == FF_CODER_TYPE_AC;
    if (avctx->me_cmp >= 0)
        x4->params.analyse.b_chroma_me = avctx->me_cmp & FF_CMP_CHROMA;

    if (x4->aq_mode >= 0)
        x4->params.rc.i_aq_mode = x4->aq_mode;
    if (x4->aq_strength >= 0)
        x4->params.rc.f_aq_strength = x4->aq_strength;
    PARSE_X264_OPT("psy-rd", psy_rd);
    PARSE_X264_OPT("deblock", deblock);
    PARSE_X264_OPT("partitions", partitions);
    PARSE_X264_OPT("stats", stats);
    if (x4->psy >= 0)
        x4->params.analyse.b_psy  = x4->psy;
    if (x4->rc_lookahead >= 0)
        x4->params.rc.i_lookahead = x4->rc_lookahead;
    if (x4->weightp >= 0)
        x4->params.analyse.i_weighted_pred = x4->weightp;
    if (x4->weightb >= 0)
        x4->params.analyse.b_weighted_bipred = x4->weightb;
    if (x4->cplxblur >= 0)
        x4->params.rc.f_complexity_blur = x4->cplxblur;

    if (x4->ssim >= 0)
        x4->params.analyse.b_ssim = x4->ssim;
    if (x4->intra_refresh >= 0)
        x4->params.b_intra_refresh = x4->intra_refresh;
    if (x4->bluray_compat >= 0) {
        x4->params.b_bluray_compat = x4->bluray_compat;
        x4->params.b_vfr_input = 0;
    }
    if (x4->b_bias != INT_MIN)
        x4->params.i_bframe_bias              = x4->b_bias;
    if (x4->b_pyramid >= 0)
        x4->params.i_bframe_pyramid = x4->b_pyramid;
    if (x4->mixed_refs >= 0)
        x4->params.analyse.b_mixed_references = x4->mixed_refs;
    if (x4->dct8x8 >= 0)
        x4->params.analyse.b_transform_8x8    = x4->dct8x8;
    if (x4->fast_pskip >= 0)
        x4->params.analyse.b_fast_pskip       = x4->fast_pskip;
    if (x4->aud >= 0)
        x4->params.b_aud                      = x4->aud;
    if (x4->mbtree >= 0)
        x4->params.rc.b_mb_tree               = x4->mbtree;
    if (x4->direct_pred >= 0)
        x4->params.analyse.i_direct_mv_pred   = x4->direct_pred;

    if (x4->slice_max_size >= 0)
        x4->params.i_slice_max_size =  x4->slice_max_size;

    if (x4->fastfirstpass)
        x264_param_apply_fastfirstpass(&x4->params);

    if (x4->nal_hrd >= 0)
        x4->params.i_nal_hrd = x4->nal_hrd;

    if (x4->profile)
        if (x264_param_apply_profile(&x4->params, x4->profile) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Error setting profile %s.\n", x4->profile);
            return AVERROR(EINVAL);
        }

    x4->params.i_width          = avctx->width;
    x4->params.i_height         = avctx->height;
    x4->params.vui.i_sar_width  = avctx->sample_aspect_ratio.num;
    x4->params.vui.i_sar_height = avctx->sample_aspect_ratio.den;
    x4->params.i_fps_num = x4->params.i_timebase_den = avctx->time_base.den;
    x4->params.i_fps_den = x4->params.i_timebase_num = avctx->time_base.num;

    x4->params.analyse.b_psnr = avctx->flags & CODEC_FLAG_PSNR;

    x4->params.i_threads      = avctx->thread_count;
    if (avctx->thread_type)
        x4->params.b_sliced_threads = avctx->thread_type == FF_THREAD_SLICE;

    x4->params.b_interlaced   = avctx->flags & CODEC_FLAG_INTERLACED_DCT;

    x4->params.b_open_gop     = !(avctx->flags & CODEC_FLAG_CLOSED_GOP);

    x4->params.i_slice_count  = avctx->slices;

    x4->params.vui.b_fullrange = avctx->pix_fmt == AV_PIX_FMT_YUVJ420P ||
                                 avctx->pix_fmt == AV_PIX_FMT_YUVJ422P ||
                                 avctx->pix_fmt == AV_PIX_FMT_YUVJ444P ||
                                 avctx->color_range == AVCOL_RANGE_JPEG;

    // x264 validates the values internally
    x4->params.vui.i_colorprim = avctx->color_primaries;
    x4->params.vui.i_transfer  = avctx->color_trc;
    x4->params.vui.i_colmatrix = avctx->colorspace;

    if (avctx->flags & CODEC_FLAG_GLOBAL_HEADER)
        x4->params.b_repeat_headers = 0;

    if (x4->x264_params) {
        AVDictionary *dict    = NULL;
        AVDictionaryEntry *en = NULL;

        if (!av_dict_parse_string(&dict, x4->x264_params, "=", ":", 0)) {
            while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX))) {
                if (x264_param_parse(&x4->params, en->key, en->value) < 0)
                    av_log(avctx, AV_LOG_WARNING,
                           "Error parsing option '%s = %s'.\n",
                            en->key, en->value);
            }

            av_dict_free(&dict);
        }
    }

    // update AVCodecContext with x264 parameters
    avctx->has_b_frames = x4->params.i_bframe ?
        x4->params.i_bframe_pyramid ? 2 : 1 : 0;
    if (avctx->max_b_frames < 0)
        avctx->max_b_frames = 0;

    avctx->bit_rate = x4->params.rc.i_bitrate*1000;

    x4->enc = x264_encoder_open(&x4->params);
    if (!x4->enc)
        return AVERROR_UNKNOWN;

    if (avctx->flags & CODEC_FLAG_GLOBAL_HEADER) {
        x264_nal_t *nal;
        uint8_t *p;
        int nnal, s, i;

        s = x264_encoder_headers(x4->enc, &nal, &nnal);
        avctx->extradata = p = av_malloc(s);
        if (!p)
            return AVERROR(ENOMEM);

        for (i = 0; i < nnal; i++) {
            /* Don't put the SEI in extradata. */
            if (nal[i].i_type == NAL_SEI) {
                av_log(avctx, AV_LOG_INFO, "%s\n", nal[i].p_payload+25);
                x4->sei_size = nal[i].i_payload;
                x4->sei      = av_malloc(x4->sei_size);
                if (!x4->sei)
                    return AVERROR(ENOMEM);
                memcpy(x4->sei, nal[i].p_payload, nal[i].i_payload);
                continue;
            }
            memcpy(p, nal[i].p_payload, nal[i].i_payload);
            p += nal[i].i_payload;
        }
        avctx->extradata_size = p - avctx->extradata;
    }

    return 0;
}

static const enum AVPixelFormat pix_fmts_8bit[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_NV16,
    AV_PIX_FMT_NONE
};
static const enum AVPixelFormat pix_fmts_9bit[] = {
    AV_PIX_FMT_YUV420P9,
    AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_NONE
};
static const enum AVPixelFormat pix_fmts_10bit[] = {
    AV_PIX_FMT_YUV420P10,
    AV_PIX_FMT_YUV422P10,
    AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_NV20,
    AV_PIX_FMT_NONE
};

static av_cold void X264_init_static(AVCodec *codec)
{
    if (x264_bit_depth == 8)
        codec->pix_fmts = pix_fmts_8bit;
    else if (x264_bit_depth == 9)
        codec->pix_fmts = pix_fmts_9bit;
    else if (x264_bit_depth == 10)
        codec->pix_fmts = pix_fmts_10bit;
}

#define OFFSET(x) offsetof(X264Context, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "preset",        "Set the encoding preset (cf. x264 --fullhelp)",   OFFSET(preset),        AV_OPT_TYPE_STRING, { .str = "medium" }, 0, 0, VE},
    { "tune",          "Tune the encoding params (cf. x264 --fullhelp)",  OFFSET(tune),          AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE},
    { "profile",       "Set profile restrictions (cf. x264 --fullhelp) ", OFFSET(profile),       AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE},
    { "fastfirstpass", "Use fast settings when encoding first pass",      OFFSET(fastfirstpass), AV_OPT_TYPE_INT,    { .i64 = 1 }, 0, 1, VE},
    { "crf",           "Select the quality for constant quality mode",    OFFSET(crf),           AV_OPT_TYPE_FLOAT,  {.dbl = -1 }, -1, FLT_MAX, VE },
    { "crf_max",       "In CRF mode, prevents VBV from lowering quality beyond this point.",OFFSET(crf_max), AV_OPT_TYPE_FLOAT, {.dbl = -1 }, -1, FLT_MAX, VE },
    { "qp",            "Constant quantization parameter rate control method",OFFSET(cqp),        AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX, VE },
    { "aq-mode",       "AQ method",                                       OFFSET(aq_mode),       AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX, VE, "aq_mode"},
    { "none",          NULL,                              0, AV_OPT_TYPE_CONST, {.i64 = X264_AQ_NONE},         INT_MIN, INT_MAX, VE, "aq_mode" },
    { "variance",      "Variance AQ (complexity mask)",   0, AV_OPT_TYPE_CONST, {.i64 = X264_AQ_VARIANCE},     INT_MIN, INT_MAX, VE, "aq_mode" },
    { "autovariance",  "Auto-variance AQ (experimental)", 0, AV_OPT_TYPE_CONST, {.i64 = X264_AQ_AUTOVARIANCE}, INT_MIN, INT_MAX, VE, "aq_mode" },
    { "aq-strength",   "AQ strength. Reduces blocking and blurring in flat and textured areas.", OFFSET(aq_strength), AV_OPT_TYPE_FLOAT, {.dbl = -1}, -1, FLT_MAX, VE},
    { "psy",           "Use psychovisual optimizations.",                 OFFSET(psy),           AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 1, VE },
    { "psy-rd",        "Strength of psychovisual optimization, in <psy-rd>:<psy-trellis> format.", OFFSET(psy_rd), AV_OPT_TYPE_STRING,  {0 }, 0, 0, VE},
    { "rc-lookahead",  "Number of frames to look ahead for frametype and ratecontrol", OFFSET(rc_lookahead), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, VE },
    { "weightb",       "Weighted prediction for B-frames.",               OFFSET(weightb),       AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 1, VE },
    { "weightp",       "Weighted prediction analysis method.",            OFFSET(weightp),       AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX, VE, "weightp" },
    { "none",          NULL, 0, AV_OPT_TYPE_CONST, {.i64 = X264_WEIGHTP_NONE},   INT_MIN, INT_MAX, VE, "weightp" },
    { "simple",        NULL, 0, AV_OPT_TYPE_CONST, {.i64 = X264_WEIGHTP_SIMPLE}, INT_MIN, INT_MAX, VE, "weightp" },
    { "smart",         NULL, 0, AV_OPT_TYPE_CONST, {.i64 = X264_WEIGHTP_SMART},  INT_MIN, INT_MAX, VE, "weightp" },
    { "ssim",          "Calculate and print SSIM stats.",                 OFFSET(ssim),          AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 1, VE },
    { "intra-refresh", "Use Periodic Intra Refresh instead of IDR frames.",OFFSET(intra_refresh),AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 1, VE },
    { "bluray-compat", "Bluray compatibility workarounds.",               OFFSET(bluray_compat) ,AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 1, VE },
    { "b-bias",        "Influences how often B-frames are used",          OFFSET(b_bias),        AV_OPT_TYPE_INT,    { .i64 = INT_MIN}, INT_MIN, INT_MAX, VE },
    { "b-pyramid",     "Keep some B-frames as references.",               OFFSET(b_pyramid),     AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX, VE, "b_pyramid" },
    { "none",          NULL,                                  0, AV_OPT_TYPE_CONST, {.i64 = X264_B_PYRAMID_NONE},   INT_MIN, INT_MAX, VE, "b_pyramid" },
    { "strict",        "Strictly hierarchical pyramid",       0, AV_OPT_TYPE_CONST, {.i64 = X264_B_PYRAMID_STRICT}, INT_MIN, INT_MAX, VE, "b_pyramid" },
    { "normal",        "Non-strict (not Blu-ray compatible)", 0, AV_OPT_TYPE_CONST, {.i64 = X264_B_PYRAMID_NORMAL}, INT_MIN, INT_MAX, VE, "b_pyramid" },
    { "mixed-refs",    "One reference per partition, as opposed to one reference per macroblock", OFFSET(mixed_refs), AV_OPT_TYPE_INT, { .i64 = -1}, -1, 1, VE },
    { "8x8dct",        "High profile 8x8 transform.",                     OFFSET(dct8x8),        AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 1, VE},
    { "fast-pskip",    NULL,                                              OFFSET(fast_pskip),    AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 1, VE},
    { "aud",           "Use access unit delimiters.",                     OFFSET(aud),           AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 1, VE},
    { "mbtree",        "Use macroblock tree ratecontrol.",                OFFSET(mbtree),        AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, 1, VE},
    { "deblock",       "Loop filter parameters, in <alpha:beta> form.",   OFFSET(deblock),       AV_OPT_TYPE_STRING, { 0 },  0, 0, VE},
    { "cplxblur",      "Reduce fluctuations in QP (before curve compression)", OFFSET(cplxblur), AV_OPT_TYPE_FLOAT,  {.dbl = -1 }, -1, FLT_MAX, VE},
    { "partitions",    "A comma-separated list of partitions to consider. "
                       "Possible values: p8x8, p4x4, b8x8, i8x8, i4x4, none, all", OFFSET(partitions), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE},
    { "direct-pred",   "Direct MV prediction mode",                       OFFSET(direct_pred),   AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX, VE, "direct-pred" },
    { "none",          NULL,      0,    AV_OPT_TYPE_CONST, { .i64 = X264_DIRECT_PRED_NONE },     0, 0, VE, "direct-pred" },
    { "spatial",       NULL,      0,    AV_OPT_TYPE_CONST, { .i64 = X264_DIRECT_PRED_SPATIAL },  0, 0, VE, "direct-pred" },
    { "temporal",      NULL,      0,    AV_OPT_TYPE_CONST, { .i64 = X264_DIRECT_PRED_TEMPORAL }, 0, 0, VE, "direct-pred" },
    { "auto",          NULL,      0,    AV_OPT_TYPE_CONST, { .i64 = X264_DIRECT_PRED_AUTO },     0, 0, VE, "direct-pred" },
    { "slice-max-size","Limit the size of each slice in bytes",           OFFSET(slice_max_size),AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX, VE },
    { "stats",         "Filename for 2 pass stats",                       OFFSET(stats),         AV_OPT_TYPE_STRING, { 0 },  0,       0, VE },
    { "nal-hrd",       "Signal HRD information (requires vbv-bufsize; "
                       "cbr not allowed in .mp4)",                        OFFSET(nal_hrd),       AV_OPT_TYPE_INT,    { .i64 = -1 }, -1, INT_MAX, VE, "nal-hrd" },
    { "none",          NULL, 0, AV_OPT_TYPE_CONST, {.i64 = X264_NAL_HRD_NONE}, INT_MIN, INT_MAX, VE, "nal-hrd" },
    { "vbr",           NULL, 0, AV_OPT_TYPE_CONST, {.i64 = X264_NAL_HRD_VBR},  INT_MIN, INT_MAX, VE, "nal-hrd" },
    { "cbr",           NULL, 0, AV_OPT_TYPE_CONST, {.i64 = X264_NAL_HRD_CBR},  INT_MIN, INT_MAX, VE, "nal-hrd" },
    { "x264-params",  "Override the x264 configuration using a :-separated list of key=value parameters", OFFSET(x264_params), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },
    { NULL },
};

static const AVCodecDefault x264_defaults[] = {
    { "b",                "0" },
    { "bf",               "-1" },
    { "g",                "-1" },
    { "i_qfactor",        "-1" },
    { "qmin",             "-1" },
    { "qmax",             "-1" },
    { "qdiff",            "-1" },
    { "qblur",            "-1" },
    { "qcomp",            "-1" },
    { "refs",             "-1" },
    { "sc_threshold",     "-1" },
    { "trellis",          "-1" },
    { "nr",               "-1" },
    { "me_range",         "-1" },
    { "me_method",        "-1" },
    { "subq",             "-1" },
    { "b_strategy",       "-1" },
    { "keyint_min",       "-1" },
    { "coder",            "-1" },
    { "cmp",              "-1" },
    { "threads",          AV_STRINGIFY(X264_THREADS_AUTO) },
    { "thread_type",      "0" },
    { "flags",            "+cgop" },
    { "rc_init_occupancy","-1" },
    { NULL },
};

#if CONFIG_LIBX264_ENCODER
static const AVClass class = {
    .class_name = "libx264",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libx264_encoder = {
    .name             = "libx264",
    .long_name        = NULL_IF_CONFIG_SMALL("libx264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_H264,
    .priv_data_size   = sizeof(X264Context),
    .init             = X264_init,
    .encode2          = X264_frame,
    .close            = X264_close,
    .capabilities     = CODEC_CAP_DELAY | CODEC_CAP_AUTO_THREADS,
    .priv_class       = &class,
    .defaults         = x264_defaults,
    .init_static_data = X264_init_static,
    .caps_internal    = FF_CODEC_CAP_INIT_THREADSAFE |
                        FF_CODEC_CAP_INIT_CLEANUP,
};
#endif

#if CONFIG_LIBX262_ENCODER
static const AVClass X262_class = {
    .class_name = "libx262",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libx262_encoder = {
    .name             = "libx262",
    .long_name        = NULL_IF_CONFIG_SMALL("libx262 MPEG2VIDEO"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_MPEG2VIDEO,
    .priv_data_size   = sizeof(X264Context),
    .init             = X264_init,
    .encode2          = X264_frame,
    .close            = X264_close,
    .capabilities     = CODEC_CAP_DELAY | CODEC_CAP_AUTO_THREADS,
    .priv_class       = &X262_class,
    .defaults         = x264_defaults,
    .pix_fmts         = pix_fmts_8bit,
    .caps_internal    = FF_CODEC_CAP_INIT_THREADSAFE |
                        FF_CODEC_CAP_INIT_CLEANUP,
};
#endif
