/*
 * HEVC video decoder
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2012 - 2013 Gildas Cocherel
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
 
#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/stereo3d.h"
#include "libavutil/thread.h"
#include "libavutil/timestamp.h"

#include "container_fifo.h"
#include "decode.h"
#include "hevc.h"
#include "hevcdec.h"
#include "progressframe.h"
#include "refstruct.h"

typedef struct HEVCOutputFrameConstructionContext {
    // Thread Data Access/Synchronization.
    AVMutex mutex;

    // DPB Output Tracking.
    uint64_t dpb_counter;
    int dpb_poc;
    uint64_t dpb_poc_ooorder_counter;

    // Collect the First Field.
    int have_first_field;
    int first_field_poc;
    int first_field_sei_pic_struct;
    AVFrame *first_field;

    uint64_t orphaned_field_pictures;

    // Reconstructed Interlaced Frames From Field Pictures for Output.
    AVFrame *constructed_frame;

    // Output Frame Tracking.
    uint64_t output_counter;
    int output_poc;
    uint64_t output_poc_ooorder_counter;
} HEVCOutputFrameConstructionContext;

static void hevc_output_frame_construction_ctx_free(FFRefStructOpaque opaque, void *obj)
{
    HEVCOutputFrameConstructionContext * ctx = (HEVCOutputFrameConstructionContext *)obj;

    if (!ctx)
        return;

    av_frame_free(&ctx->first_field);
    av_frame_free(&ctx->constructed_frame);
    av_assert0(ff_mutex_destroy(&ctx->mutex) == 0);
}

int ff_hevc_output_frame_construction_ctx_alloc(HEVCContext *s)
{
    if (s->output_frame_construction_ctx) {
        av_log(s->avctx, AV_LOG_ERROR,
               "s->output_frame_construction_ctx is already set.\n");
        return AVERROR_INVALIDDATA;
    }

    s->output_frame_construction_ctx =
        ff_refstruct_alloc_ext(sizeof(*(s->output_frame_construction_ctx)),
                               0, NULL, hevc_output_frame_construction_ctx_free);
    if (!s->output_frame_construction_ctx)
        return AVERROR(ENOMEM);

    av_assert0(ff_mutex_init(&s->output_frame_construction_ctx->mutex, NULL) == 0);

    return 0;
}

void ff_hevc_output_frame_construction_ctx_replace(HEVCContext *dst, HEVCContext *src)
{
    ff_refstruct_replace(&dst->output_frame_construction_ctx,
                         src->output_frame_construction_ctx);
}

void ff_hevc_output_frame_construction_ctx_unref(HEVCContext *s)
{
    if (s->output_frame_construction_ctx &&
        ff_refstruct_exclusive(s->output_frame_construction_ctx)) {

        HEVCOutputFrameConstructionContext * ctx = s->output_frame_construction_ctx;

        av_assert0(ff_mutex_lock(&ctx->mutex) == 0);

       if (ctx->dpb_counter) {
           av_log(s->avctx, AV_LOG_ERROR,
                  "[HEVCOutputFrameConstructionContext @ 0x%p]:\n"
                  "      DPB:    Counter=%" PRIu64 " POCOutOfOrder=%" PRIu64 " Orphaned=%" PRIu64 "\n"
                  "      Output: Counter=%" PRIu64 " POCOutOfOrder=%" PRIu64 "\n"
                  "%s",
                  ctx,
                  ctx->dpb_counter,
                  ctx->dpb_poc_ooorder_counter,
                  ctx->orphaned_field_pictures,
                  ctx->output_counter,
                  ctx->output_poc_ooorder_counter,
                  "");
       }

       av_assert0(ff_mutex_unlock(&ctx->mutex) == 0);
    }

    ff_refstruct_unref(&s->output_frame_construction_ctx);
}

void ff_hevc_unref_frame(HEVCFrame *frame, int flags)
{
    frame->flags &= ~flags;
    if (!frame->flags) {
        ff_progress_frame_unref(&frame->tf);
        av_frame_unref(frame->frame_grain);
        frame->needs_fg = 0;

        ff_refstruct_unref(&frame->pps);
        ff_refstruct_unref(&frame->tab_mvf);

        ff_refstruct_unref(&frame->rpl);
        frame->nb_rpl_elems = 0;
        ff_refstruct_unref(&frame->rpl_tab);
        frame->refPicList = NULL;

        ff_refstruct_unref(&frame->hwaccel_picture_private);
    }
}

const RefPicList *ff_hevc_get_ref_list(const HEVCFrame *ref, int x0, int y0)
{
    const HEVCSPS *sps = ref->pps->sps;
    int x_cb         = x0 >> sps->log2_ctb_size;
    int y_cb         = y0 >> sps->log2_ctb_size;
    int pic_width_cb = sps->ctb_width;
    int ctb_addr_ts  = ref->pps->ctb_addr_rs_to_ts[y_cb * pic_width_cb + x_cb];
    return &ref->rpl_tab[ctb_addr_ts]->refPicList[0];
}

void ff_hevc_clear_refs(HEVCLayerContext *l)
{
    int i;
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++)
        ff_hevc_unref_frame(&l->DPB[i],
                            HEVC_FRAME_FLAG_SHORT_REF |
                            HEVC_FRAME_FLAG_LONG_REF);
}

void ff_hevc_flush_dpb(HEVCContext *s)
{
    for (int layer = 0; layer < FF_ARRAY_ELEMS(s->layers); layer++) {
        HEVCLayerContext *l = &s->layers[layer];
        for (int i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++)
            ff_hevc_unref_frame(&l->DPB[i], ~0);
    }
}

static HEVCFrame *alloc_frame(HEVCContext *s, HEVCLayerContext *l)
{
    const HEVCVPS *vps = l->sps->vps;
    const int  view_id = vps->view_id[s->cur_layer];
    int i, j, ret;
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
        HEVCFrame *frame = &l->DPB[i];
        if (frame->f)
            continue;

        ret = ff_progress_frame_alloc(s->avctx, &frame->tf);
        if (ret < 0)
            return NULL;

        // Add LCEVC SEI metadata here, as it's needed in get_buffer()
        if (s->sei.common.lcevc.info) {
            HEVCSEILCEVC *lcevc = &s->sei.common.lcevc;
            ret = ff_frame_new_side_data_from_buf(s->avctx, frame->tf.f,
                                                  AV_FRAME_DATA_LCEVC, &lcevc->info);
            if (ret < 0)
                goto fail;
        }

        // add view ID side data if it's nontrivial
        if (vps->nb_layers > 1 || view_id) {
            HEVCSEITDRDI *tdrdi = &s->sei.tdrdi;
            AVFrameSideData *sd = av_frame_side_data_new(&frame->f->side_data,
                                                         &frame->f->nb_side_data,
                                                         AV_FRAME_DATA_VIEW_ID,
                                                         sizeof(int), 0);
            if (!sd)
                goto fail;
            *(int*)sd->data = view_id;

            if (tdrdi->num_ref_displays) {
                AVStereo3D *stereo_3d;

                stereo_3d = av_stereo3d_create_side_data(frame->f);
                if (!stereo_3d)
                    goto fail;

                stereo_3d->type = AV_STEREO3D_FRAMESEQUENCE;
                if (tdrdi->left_view_id[0] == view_id)
                    stereo_3d->view = AV_STEREO3D_VIEW_LEFT;
                else if (tdrdi->right_view_id[0] == view_id)
                    stereo_3d->view = AV_STEREO3D_VIEW_RIGHT;
                else
                    stereo_3d->view = AV_STEREO3D_VIEW_UNSPEC;
            }
        }

        ret = ff_progress_frame_get_buffer(s->avctx, &frame->tf,
                                           AV_GET_BUFFER_FLAG_REF);
        if (ret < 0)
            return NULL;

        frame->rpl = ff_refstruct_allocz(s->pkt.nb_nals * sizeof(*frame->rpl));
        if (!frame->rpl)
            goto fail;
        frame->nb_rpl_elems = s->pkt.nb_nals;

        frame->tab_mvf = ff_refstruct_pool_get(l->tab_mvf_pool);
        if (!frame->tab_mvf)
            goto fail;

        frame->rpl_tab = ff_refstruct_pool_get(l->rpl_tab_pool);
        if (!frame->rpl_tab)
            goto fail;
        frame->ctb_count = l->sps->ctb_width * l->sps->ctb_height;
        for (j = 0; j < frame->ctb_count; j++)
            frame->rpl_tab[j] = frame->rpl;

        frame->sei_pic_struct = s->sei.picture_timing.picture_struct;
        if (ff_hevc_sei_pic_struct_is_interlaced(frame->sei_pic_struct)) {
            frame->f->flags |= AV_FRAME_FLAG_INTERLACED;
            if (ff_hevc_sei_pic_struct_is_tff(frame->sei_pic_struct))
                frame->f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
        }
        if (frame->sei_pic_struct == HEVC_SEI_PIC_STRUCT_FRAME_TFBFTF ||
            frame->sei_pic_struct == HEVC_SEI_PIC_STRUCT_FRAME_BFTFBF)
            frame->f->repeat_pict = 1;
        else if (frame->sei_pic_struct == HEVC_SEI_PIC_STRUCT_FRAME_DOUBLING)
            frame->f->repeat_pict = 2;
        else if (frame->sei_pic_struct == HEVC_SEI_PIC_STRUCT_FRAME_TRIPLING)
            frame->f->repeat_pict = 3;

        ret = ff_hwaccel_frame_priv_alloc(s->avctx, &frame->hwaccel_picture_private);
        if (ret < 0)
            goto fail;

        frame->pps = ff_refstruct_ref_c(s->pps);

        return frame;
fail:
        ff_hevc_unref_frame(frame, ~0);
        return NULL;
    }
    av_log(s->avctx, AV_LOG_ERROR, "Error allocating frame, DPB full.\n");
    return NULL;
}

int ff_hevc_set_new_ref(HEVCContext *s, HEVCLayerContext *l, int poc)
{
    HEVCFrame *ref;
    int i;

    /* check that this POC doesn't already exist */
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
        HEVCFrame *frame = &l->DPB[i];

        if (frame->f && frame->poc == poc) {
            av_log(s->avctx, AV_LOG_ERROR, "Duplicate POC in a sequence: %d.\n",
                   poc);
            return AVERROR_INVALIDDATA;
        }
    }

    ref = alloc_frame(s, l);
    if (!ref)
        return AVERROR(ENOMEM);

    s->cur_frame = ref;
    l->cur_frame = ref;
    s->collocated_ref = NULL;

    ref->base_layer_frame = (l != &s->layers[0] && s->layers[0].cur_frame) ?
                            s->layers[0].cur_frame - s->layers[0].DPB : -1;

    if (s->sh.pic_output_flag)
        ref->flags = HEVC_FRAME_FLAG_OUTPUT | HEVC_FRAME_FLAG_SHORT_REF;
    else
        ref->flags = HEVC_FRAME_FLAG_SHORT_REF;

    ref->poc      = poc;
    ref->f->crop_left   = l->sps->output_window.left_offset;
    ref->f->crop_right  = l->sps->output_window.right_offset;
    ref->f->crop_top    = l->sps->output_window.top_offset;
    ref->f->crop_bottom = l->sps->output_window.bottom_offset;

    return 0;
}

static void unref_missing_refs(HEVCLayerContext *l)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
         HEVCFrame *frame = &l->DPB[i];
         if (frame->flags & HEVC_FRAME_FLAG_UNAVAILABLE) {
             ff_hevc_unref_frame(frame, ~0);
         }
    }
}

static void copy_field2(AVFrame *_dst, const AVFrame *_src)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(_src->format);
    int i, j, planes_nb = 0;
    for (i = 0; i < desc->nb_components; i++)
        planes_nb = FFMAX(planes_nb, desc->comp[i].plane + 1);
    for (i = 0; i < planes_nb; i++) {
        int h = _src->height;
        uint8_t *dst = _dst->data[i] + (_dst->linesize[i] / 2);
        uint8_t *src = _src->data[i];
        if (i == 1 || i == 2) {
            h = FF_CEIL_RSHIFT(_src->height, desc->log2_chroma_h);
        }
        for (j = 0; j < h; j++) {
            memcpy(dst, src, _src->linesize[i]);
            dst += _dst->linesize[i];
            src += _src->linesize[i];
        }
    }
}

static int interlaced_frame_from_fields(AVFrame *dst,
                                        const AVFrame *field1,
                                        const AVFrame *field2)
{
    int i, ret = 0;

    av_frame_unref(dst);

    dst->format         = field1->format;
    dst->width          = field1->width;
    dst->height         = field1->height * 2;
    dst->nb_samples     = field1->nb_samples;
    ret = av_channel_layout_copy(&dst->ch_layout, &field1->ch_layout);
    if (ret < 0)
        return ret;

    ret = av_frame_copy_props(dst, field1);
    if (ret < 0)
        return ret;
    if (field1->duration > 0 && field1->duration != AV_NOPTS_VALUE)
        dst->duration = field2->duration * 2;
    else if (field2->duration > 0 && field2->duration != AV_NOPTS_VALUE)
        dst->duration = field2->duration * 2;

    for (i = 0; i < field2->nb_side_data; i++) {
        const AVFrameSideData *sd_src = field2->side_data[i];
        AVFrameSideData *sd_dst;
        AVBufferRef *ref = av_buffer_ref(sd_src->buf);
        sd_dst = av_frame_new_side_data_from_buf(dst, sd_src->type, ref);
        if (!sd_dst) {
            av_buffer_unref(&ref);
            return AVERROR(ENOMEM);
        }
    }

    for (i = 0; i < AV_NUM_DATA_POINTERS; i++)
        dst->linesize[i] = field1->linesize[i]*2;

    ret = av_frame_get_buffer(dst, 0);
    if (ret < 0)
        return ret;

    ret = av_frame_copy(dst, field1);
    if (ret < 0)
        av_frame_unref(dst);

    copy_field2(dst, field2);

    for (i = 0; i < AV_NUM_DATA_POINTERS; i++)
        dst->linesize[i] = field1->linesize[i];

    return ret;
}

int ff_hevc_output_frames(HEVCContext *s,
                          unsigned layers_active_decode, unsigned layers_active_output,
                          unsigned max_output, unsigned max_dpb, int discard)
{
    while (1) {
        int nb_dpb[HEVC_VPS_MAX_LAYERS] = { 0 };
        int nb_output = 0;
        int min_poc   = INT_MAX;
        int min_layer = -1;
        int min_idx, ret = 0;

        for (int layer = 0; layer < FF_ARRAY_ELEMS(s->layers); layer++) {
            HEVCLayerContext *l = &s->layers[layer];

            if (!(layers_active_decode & (1 << layer)))
                continue;

            for (int i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
                HEVCFrame *frame = &l->DPB[i];
                if (frame->flags & HEVC_FRAME_FLAG_OUTPUT) {
                    // nb_output counts AUs with an output-pending frame
                    // in at least one layer
                    if (!(frame->base_layer_frame >= 0 &&
                          (s->layers[0].DPB[frame->base_layer_frame].flags & HEVC_FRAME_FLAG_OUTPUT)))
                        nb_output++;
                    if (min_layer < 0 || frame->poc < min_poc) {
                        min_poc = frame->poc;
                        min_idx = i;
                        min_layer = layer;
                    }
                }
                nb_dpb[layer] += !!frame->flags;
            }
        }

        if (nb_output > max_output ||
            (nb_output &&
             (nb_dpb[0] > max_dpb || nb_dpb[1] > max_dpb))) {
            HEVCFrame *frame = &s->layers[min_layer].DPB[min_idx];
            AVFrame *f = frame->needs_fg ? frame->frame_grain : frame->f;
            int output = !discard && (layers_active_output & (1 << min_layer));

            if (ff_hevc_sei_pict_struct_is_field_picture(frame->sei_pic_struct)) {
                // Skip the extra work if the stream contains frame pictures.
                // NOTE: This also fixes the final frame output for the fate test streams.
                if (frame->poc != s->poc) {
                    if (s->avctx->active_thread_type == FF_THREAD_FRAME)
                    {
                        // Wait for other thread to finish decoding this frame/field picture.
                        // Otherwise I have seen image corruption for some streams..
                        av_log(s->avctx, AV_LOG_DEBUG,
                               "Waiting on Frame POC: %d.\n",
                               frame->poc);
                        ff_progress_frame_await(&frame->tf, INT_MAX);
                    }
                } else {
                    // This is the Context currently decoding..
                    // Skip it to ensure that this frame is completely decoded and finalized.
                    // This will allow the next context to process it
                    // Otherwise I have seen image corruption for some streams.
                    av_log(s->avctx, AV_LOG_DEBUG,
                           "Schedule Frame for Next Pass POC: %d.\n",
                           frame->poc);
                    return 0;
                }
            }

            av_assert0(s->output_frame_construction_ctx);
            av_assert0(ff_mutex_lock(&s->output_frame_construction_ctx->mutex) == 0);

            if (output) {
                const int dpb_poc = frame->poc;
                const int dpb_sei_pic_struct = frame->sei_pic_struct;
                AVFrame *output_frame = f;
                int output_poc = dpb_poc;
                int output_sei_pic_struct = dpb_sei_pic_struct;

                s->output_frame_construction_ctx->dpb_counter++;
                if (s->output_frame_construction_ctx->dpb_counter > 1 &&
                        dpb_poc < s->output_frame_construction_ctx->dpb_poc &&
                        dpb_poc > 0) {
                    s->output_frame_construction_ctx->dpb_poc_ooorder_counter++;
                    av_log(s->avctx, AV_LOG_ERROR,
                           "DPB POC Out of Order POC %d < PrevPOC %d "
                           ": Counter=%" PRIu64 " OORCounter=%" PRIu64 ".\n",
                           dpb_poc,
                           s->output_frame_construction_ctx->dpb_poc,
                           s->output_frame_construction_ctx->dpb_counter,
                           s->output_frame_construction_ctx->dpb_poc_ooorder_counter);
                }
                s->output_frame_construction_ctx->dpb_poc = dpb_poc;

                if (ff_hevc_sei_pict_struct_is_field_picture(dpb_sei_pic_struct)) {
                    const int have_first_field = s->output_frame_construction_ctx->have_first_field;
                    const int is_first_field =
                        (ff_hevc_sei_pic_struct_is_tff(dpb_sei_pic_struct) &&
                            ff_hevc_sei_pic_struct_is_tf(dpb_sei_pic_struct)) ||
                        (ff_hevc_sei_pic_struct_is_bff(dpb_sei_pic_struct) &&
                            ff_hevc_sei_pic_struct_is_bf(dpb_sei_pic_struct)) ||
                        (!s->output_frame_construction_ctx->have_first_field &&
                            (dpb_poc % 2) == 0) ||
                        (s->output_frame_construction_ctx->have_first_field &&
                            s->output_frame_construction_ctx->first_field_sei_pic_struct == dpb_sei_pic_struct &&
                            (dpb_poc % 2) == 0 &&
                            dpb_poc > s->output_frame_construction_ctx->first_field_poc);

                    output_frame = NULL;

                    if (!s->output_frame_construction_ctx->first_field)
                    {
                        s->output_frame_construction_ctx->first_field = av_frame_alloc();
                        if (!s->output_frame_construction_ctx->first_field) {
                            av_log(s->avctx, AV_LOG_ERROR, "AVERROR(ENOMEM)");
                            ret = AVERROR(ENOMEM);
                            goto unref_frame_and_check_ret;
                        }
                    }
                    if (!s->output_frame_construction_ctx->constructed_frame) {
                        s->output_frame_construction_ctx->constructed_frame = av_frame_alloc();
                        if (!s->output_frame_construction_ctx->constructed_frame) {
                            av_log(s->avctx, AV_LOG_ERROR, "AVERROR(ENOMEM)");
                            ret = AVERROR(ENOMEM);
                            goto unref_frame_and_check_ret;
                        }
                    }

                    if (is_first_field) {
                        // This is a first field picture.
                        av_log(s->avctx, AV_LOG_DEBUG,
                               "Found first field picture POC %d.\n",
                               dpb_poc);
                        if (s->output_frame_construction_ctx->have_first_field) {
                            // We were waiting for a second field, but got another frist
                            // field instead.
                            av_log(s->avctx, AV_LOG_ERROR,
                                   "Discarded Orphaned First Field with POC %d.\n",
                                   s->output_frame_construction_ctx->first_field_poc);
                        }
                        s->output_frame_construction_ctx->have_first_field = 1;
                        s->output_frame_construction_ctx->first_field_sei_pic_struct = dpb_sei_pic_struct;
                        s->output_frame_construction_ctx->first_field_poc = dpb_poc;
                        av_frame_unref(s->output_frame_construction_ctx->first_field);
                        ret = av_frame_ref(s->output_frame_construction_ctx->first_field, f);
                        if (ret < 0) {
                            av_log(s->avctx, AV_LOG_ERROR,
                                   "Failure updating first Field picture POC %d.\n",
                                   dpb_poc);
                            s->output_frame_construction_ctx->have_first_field = 0;
                            s->output_frame_construction_ctx->orphaned_field_pictures++;
                            goto unref_frame_and_check_ret;
                        }
                    } else if (have_first_field) {
                        // We Found the next field.
                        if (f->width == s->output_frame_construction_ctx->first_field->width &&
                            f->height == s->output_frame_construction_ctx->first_field->height) {
                            // Combine the top and bottom fields into one frame for output.
                            AVFrame *constructed_frame = s->output_frame_construction_ctx->constructed_frame;
                            AVFrame *top_field;
                            AVFrame *bottom_field;
                            int tfPoc, bfPoc;
                            if (ff_hevc_sei_pic_struct_is_tf(dpb_sei_pic_struct)) {
                                top_field = f;
                                tfPoc = dpb_poc;
                                bottom_field = s->output_frame_construction_ctx->first_field;
                                bfPoc = s->output_frame_construction_ctx->first_field_poc;
                            } else {
                                top_field = s->output_frame_construction_ctx->first_field;
                                tfPoc = s->output_frame_construction_ctx->first_field_poc;
                                bottom_field = f;
                                bfPoc = dpb_poc;
                            }
                            av_frame_unref(constructed_frame);
                            ret = interlaced_frame_from_fields(constructed_frame, top_field, bottom_field);
                            if (ret >= 0) {
                                output_frame = constructed_frame;
                                output_poc = s->output_frame_construction_ctx->first_field_poc;
                                output_sei_pic_struct = s->output_frame_construction_ctx->first_field_sei_pic_struct;
                                output_frame->flags |= AV_FRAME_FLAG_INTERLACED;
                                if (!ff_hevc_sei_pic_struct_is_bf(output_sei_pic_struct)) {
                                    output_frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
                                } else {
                                    output_frame->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
                                }
                            } else {
                                av_log(s->avctx, AV_LOG_ERROR,
                                       "Interlaced Frame Construction Failure POCs: %d %d.\n",
                                       tfPoc, bfPoc);
                                s->output_frame_construction_ctx->orphaned_field_pictures += 2;
                            }
                        } else if ((dpb_poc % 2) == 0) {
                            av_log(s->avctx, AV_LOG_ERROR,
                                   "Discarded orphaned first field pictures POC: %d.\n",
                                   s->output_frame_construction_ctx->first_field_poc);
                            s->output_frame_construction_ctx->orphaned_field_pictures++;
                            // This may be the next first field.
                            s->output_frame_construction_ctx->have_first_field = 0;
                            av_assert0(ff_mutex_unlock(&s->output_frame_construction_ctx->mutex) == 0);
                            continue;
                        } else {
                            av_log(s->avctx, AV_LOG_ERROR,
                                   "Discarded mismatched field pictures POCs: %d %d.\n",
                                   s->output_frame_construction_ctx->first_field_poc,
                                   dpb_poc);
                            s->output_frame_construction_ctx->orphaned_field_pictures++;
                        }
                        // Find the next first field.
                        s->output_frame_construction_ctx->have_first_field = 0;
                    } else {
                        // We have a second field without a first field.
                        av_log(s->avctx, AV_LOG_ERROR,
                               "Discarded orphaned second field picture with POC %d.\n",
                               dpb_poc);
                        s->output_frame_construction_ctx->orphaned_field_pictures++;
                    }
                } else if (s->output_frame_construction_ctx->have_first_field) {
                     av_log(s->avctx, AV_LOG_ERROR,
                            "Discarded orphaned first field pictures POC: %d.\n",
                            s->output_frame_construction_ctx->first_field_poc);
                     s->output_frame_construction_ctx->orphaned_field_pictures++;
                     // Find the next first field.
                     s->output_frame_construction_ctx->have_first_field = 0;
                }

                if (output_frame) {
                    output_frame->pkt_dts = s->pkt_dts;

                    //av_log(s->avctx, AV_LOG_ERROR,
                    av_log(s->avctx, AV_LOG_DEBUG,
                           "s=0x%" PRIx64 " s->avctx=0x%" PRIx64 "\n"
                           "  ====Output: FrameType:%s\n"
                           "  === POC=%d PKTDTS=%s PTS=%s Duration=%s\n"
                           "  === SEIPic=%d Interlaced=%s TFF=%s PictType='%c' Key=%s\n"
                           "  === WxH=%dx%d SAR=%dx%d\n"
                           "%s",
                           (uint64_t)s, (uint64_t)s->avctx,
                           (output_frame->flags & AV_FRAME_FLAG_INTERLACED) ? "Interlaced" : "Progressive",
                           output_poc,
                           av_ts2str(output_frame->pkt_dts),
                           av_ts2str(output_frame->pts),
                           av_ts2str(output_frame->duration),
                           output_sei_pic_struct,
                           (output_frame->flags & AV_FRAME_FLAG_INTERLACED) ? "Yes" : "No",
                           (output_frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? "Yes" : "No",
                           av_get_picture_type_char(output_frame->pict_type),
                           (output_frame->flags & AV_FRAME_FLAG_KEY) ? "Yes" : "No",
                           output_frame->width, output_frame->height,
                           (int)output_frame->sample_aspect_ratio.num,
                           (int)output_frame->sample_aspect_ratio.den,
                           "");

                    s->output_frame_construction_ctx->output_counter++;
                    if (s->output_frame_construction_ctx->output_counter > 1 &&
                            output_poc < s->output_frame_construction_ctx->output_poc &&
                            output_poc > 0) {
                        s->output_frame_construction_ctx->output_poc_ooorder_counter++;
                        av_log(s->avctx, AV_LOG_ERROR,
                               "Output POC Out of Order POC %d < PrevPOC %d "
                               ": Counter=%" PRIu64 " OORCounter=%" PRIu64 ".\n",
                               output_poc,
                               s->output_frame_construction_ctx->output_poc,
                               s->output_frame_construction_ctx->output_counter,
                               s->output_frame_construction_ctx->output_poc_ooorder_counter);
                    }
                    s->output_frame_construction_ctx->output_poc = output_poc;

                    ret = ff_container_fifo_write(s->output_fifo, output_frame);
                }
            }

unref_frame_and_check_ret:

            av_assert0(ff_mutex_unlock(&s->output_frame_construction_ctx->mutex) == 0);

            ff_hevc_unref_frame(frame, HEVC_FRAME_FLAG_OUTPUT);
            if (ret < 0)
                return ret;

            av_log(s->avctx, AV_LOG_DEBUG, "%s frame with POC %d/%d.\n",
                   output ? "Output" : "Discarded", min_layer, frame->poc);
            continue;
        }
        return 0;
    }
}

static int init_slice_rpl(HEVCContext *s)
{
    HEVCFrame *frame = s->cur_frame;
    int ctb_count    = frame->ctb_count;
    int ctb_addr_ts  = s->pps->ctb_addr_rs_to_ts[s->sh.slice_segment_addr];
    int i;

    if (s->slice_idx >= frame->nb_rpl_elems)
        return AVERROR_INVALIDDATA;

    for (i = ctb_addr_ts; i < ctb_count; i++)
        frame->rpl_tab[i] = frame->rpl + s->slice_idx;

    frame->refPicList = (RefPicList *)frame->rpl_tab[ctb_addr_ts];

    return 0;
}

int ff_hevc_slice_rpl(HEVCContext *s)
{
    SliceHeader *sh = &s->sh;

    uint8_t nb_list = sh->slice_type == HEVC_SLICE_B ? 2 : 1;
    uint8_t list_idx;
    int i, j, ret;

    ret = init_slice_rpl(s);
    if (ret < 0)
        return ret;

    if (!(s->rps[ST_CURR_BEF].nb_refs + s->rps[ST_CURR_AFT].nb_refs +
          s->rps[LT_CURR].nb_refs +
          s->rps[INTER_LAYER0].nb_refs + s->rps[INTER_LAYER1].nb_refs) &&
        !s->pps->pps_curr_pic_ref_enabled_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "Zero refs in the frame RPS.\n");
        return AVERROR_INVALIDDATA;
    }

    for (list_idx = 0; list_idx < nb_list; list_idx++) {
        RefPicList  rpl_tmp = { { 0 } };
        RefPicList *rpl     = &s->cur_frame->refPicList[list_idx];

        /* The order of the elements is
         * ST_CURR_BEF - INTER_LAYER0 - ST_CURR_AFT - LT_CURR - INTER_LAYER1 for the L0 and
         * ST_CURR_AFT - INTER_LAYER1 - ST_CURR_BEF - LT_CURR - INTER_LAYER0 for the L1 */
        int cand_lists[] = { list_idx ? ST_CURR_AFT : ST_CURR_BEF,
                             list_idx ? INTER_LAYER1 : INTER_LAYER0,
                             list_idx ? ST_CURR_BEF : ST_CURR_AFT,
                             LT_CURR,
                             list_idx ? INTER_LAYER0 : INTER_LAYER1
        };

        /* concatenate the candidate lists for the current frame */
        while (rpl_tmp.nb_refs < sh->nb_refs[list_idx]) {
            for (i = 0; i < FF_ARRAY_ELEMS(cand_lists); i++) {
                RefPicList *rps = &s->rps[cand_lists[i]];
                for (j = 0; j < rps->nb_refs && rpl_tmp.nb_refs < HEVC_MAX_REFS; j++) {
                    rpl_tmp.list[rpl_tmp.nb_refs]       = rps->list[j];
                    rpl_tmp.ref[rpl_tmp.nb_refs]        = rps->ref[j];
                    // multiview inter-layer refs are treated as long-term here,
                    // cf. G.8.1.3
                    rpl_tmp.isLongTerm[rpl_tmp.nb_refs] = cand_lists[i] == LT_CURR ||
                                                          cand_lists[i] == INTER_LAYER0 ||
                                                          cand_lists[i] == INTER_LAYER1;
                    rpl_tmp.nb_refs++;
                }
            }
            // Construct RefPicList0, RefPicList1 (8-8, 8-10)
            if (s->pps->pps_curr_pic_ref_enabled_flag && rpl_tmp.nb_refs < HEVC_MAX_REFS) {
                rpl_tmp.list[rpl_tmp.nb_refs]           = s->cur_frame->poc;
                rpl_tmp.ref[rpl_tmp.nb_refs]            = s->cur_frame;
                rpl_tmp.isLongTerm[rpl_tmp.nb_refs]     = 1;
                rpl_tmp.nb_refs++;
            }
        }

        /* reorder the references if necessary */
        if (sh->rpl_modification_flag[list_idx]) {
            for (i = 0; i < sh->nb_refs[list_idx]; i++) {
                int idx = sh->list_entry_lx[list_idx][i];

                if (idx >= rpl_tmp.nb_refs) {
                    av_log(s->avctx, AV_LOG_ERROR, "Invalid reference index.\n");
                    return AVERROR_INVALIDDATA;
                }

                rpl->list[i]       = rpl_tmp.list[idx];
                rpl->ref[i]        = rpl_tmp.ref[idx];
                rpl->isLongTerm[i] = rpl_tmp.isLongTerm[idx];
                rpl->nb_refs++;
            }
        } else {
            memcpy(rpl, &rpl_tmp, sizeof(*rpl));
            rpl->nb_refs = FFMIN(rpl->nb_refs, sh->nb_refs[list_idx]);
        }

        // 8-9
        if (s->pps->pps_curr_pic_ref_enabled_flag &&
            !sh->rpl_modification_flag[list_idx] &&
            rpl_tmp.nb_refs > sh->nb_refs[L0]) {
            rpl->list[sh->nb_refs[L0] - 1] = s->cur_frame->poc;
            rpl->ref[sh->nb_refs[L0] - 1]  = s->cur_frame;
        }

        if (sh->collocated_list == list_idx &&
            sh->collocated_ref_idx < rpl->nb_refs)
            s->collocated_ref = rpl->ref[sh->collocated_ref_idx];
    }

    return 0;
}

static HEVCFrame *find_ref_idx(HEVCContext *s, HEVCLayerContext *l,
                               int poc, uint8_t use_msb)
{
    int mask = use_msb ? ~0 : (1 << l->sps->log2_max_poc_lsb) - 1;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
        HEVCFrame *ref = &l->DPB[i];
        if (ref->f) {
            if ((ref->poc & mask) == poc && (use_msb || ref->poc != s->poc))
                return ref;
        }
    }

    if (s->nal_unit_type != HEVC_NAL_CRA_NUT && !IS_BLA(s))
        av_log(s->avctx, AV_LOG_ERROR,
               "Could not find ref with POC %d\n", poc);
    return NULL;
}

static void mark_ref(HEVCFrame *frame, int flag)
{
    frame->flags &= ~(HEVC_FRAME_FLAG_LONG_REF | HEVC_FRAME_FLAG_SHORT_REF);
    frame->flags |= flag;
}

static HEVCFrame *generate_missing_ref(HEVCContext *s, HEVCLayerContext *l, int poc)
{
    HEVCFrame *frame;
    int i, y;

    frame = alloc_frame(s, l);
    if (!frame)
        return NULL;

    if (!s->avctx->hwaccel) {
        if (!l->sps->pixel_shift) {
            for (i = 0; frame->f->data[i]; i++)
                memset(frame->f->data[i], 1 << (l->sps->bit_depth - 1),
                       frame->f->linesize[i] * AV_CEIL_RSHIFT(l->sps->height, l->sps->vshift[i]));
        } else {
            for (i = 0; frame->f->data[i]; i++)
                for (y = 0; y < (l->sps->height >> l->sps->vshift[i]); y++) {
                    uint8_t *dst = frame->f->data[i] + y * frame->f->linesize[i];
                    AV_WN16(dst, 1 << (l->sps->bit_depth - 1));
                    av_memcpy_backptr(dst + 2, 2, 2*(l->sps->width >> l->sps->hshift[i]) - 2);
                }
        }
    }

    frame->poc      = poc;
    frame->flags    = HEVC_FRAME_FLAG_UNAVAILABLE;

    if (s->avctx->active_thread_type == FF_THREAD_FRAME)
        ff_progress_frame_report(&frame->tf, INT_MAX);

    return frame;
}

/* add a reference with the given poc to the list and mark it as used in DPB */
static int add_candidate_ref(HEVCContext *s, HEVCLayerContext *l,
                             RefPicList *list,
                             int poc, int ref_flag, uint8_t use_msb)
{
    HEVCFrame *ref = find_ref_idx(s, l, poc, use_msb);

    if (ref == s->cur_frame || list->nb_refs >= HEVC_MAX_REFS)
        return AVERROR_INVALIDDATA;

    if (!ref) {
        ref = generate_missing_ref(s, l, poc);
        if (!ref)
            return AVERROR(ENOMEM);
    }

    list->list[list->nb_refs] = ref->poc;
    list->ref[list->nb_refs]  = ref;
    list->nb_refs++;

    mark_ref(ref, ref_flag);
    return 0;
}

int ff_hevc_frame_rps(HEVCContext *s, HEVCLayerContext *l)
{
    const ShortTermRPS *short_rps = s->sh.short_term_rps;
    const LongTermRPS  *long_rps  = &s->sh.long_term_rps;
    RefPicList               *rps = s->rps;
    int i, ret = 0;

    unref_missing_refs(l);

    /* clear the reference flags on all frames except the current one */
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++) {
        HEVCFrame *frame = &l->DPB[i];

        if (frame == s->cur_frame)
            continue;

        mark_ref(frame, 0);
    }

    for (i = 0; i < NB_RPS_TYPE; i++)
        rps[i].nb_refs = 0;

    if (!short_rps)
        goto inter_layer;

    /* add the short refs */
    for (i = 0; i < short_rps->num_delta_pocs; i++) {
        int poc = s->poc + short_rps->delta_poc[i];
        int list;

        if (!(short_rps->used & (1 << i)))
            list = ST_FOLL;
        else if (i < short_rps->num_negative_pics)
            list = ST_CURR_BEF;
        else
            list = ST_CURR_AFT;

        ret = add_candidate_ref(s, l, &rps[list], poc,
                                HEVC_FRAME_FLAG_SHORT_REF, 1);
        if (ret < 0)
            goto fail;
    }

    /* add the long refs */
    for (i = 0; i < long_rps->nb_refs; i++) {
        int poc  = long_rps->poc[i];
        int list = long_rps->used[i] ? LT_CURR : LT_FOLL;

        ret = add_candidate_ref(s, l, &rps[list], poc,
                                HEVC_FRAME_FLAG_LONG_REF, long_rps->poc_msb_present[i]);
        if (ret < 0)
            goto fail;
    }

inter_layer:
    /* add inter-layer refs */
    if (s->sh.inter_layer_pred) {
        HEVCLayerContext *l0 = &s->layers[0];

        av_assert0(l != l0);

        /* Given the assumption of at most two layers, refPicSet0Flag is
         * always 1, so only RefPicSetInterLayer0 can ever contain a frame. */
        if (l0->cur_frame) {
            // inter-layer refs are treated as short-term here, cf. F.8.1.6
            ret = add_candidate_ref(s, l0, &rps[INTER_LAYER0], l0->cur_frame->poc,
                                    HEVC_FRAME_FLAG_SHORT_REF, 1);
            if (ret < 0)
                goto fail;
        }
    }

fail:
    /* release any frames that are now unused */
    for (i = 0; i < FF_ARRAY_ELEMS(l->DPB); i++)
        ff_hevc_unref_frame(&l->DPB[i], 0);

    return ret;
}

int ff_hevc_frame_nb_refs(const SliceHeader *sh, const HEVCPPS *pps,
                          unsigned layer_idx)
{
    int ret = 0;
    int i;
    const ShortTermRPS     *rps = sh->short_term_rps;
    const LongTermRPS *long_rps = &sh->long_term_rps;

    if (rps) {
        for (i = 0; i < rps->num_negative_pics; i++)
            ret += !!(rps->used & (1 << i));
        for (; i < rps->num_delta_pocs; i++)
            ret += !!(rps->used & (1 << i));
    }

    if (long_rps) {
        for (i = 0; i < long_rps->nb_refs; i++)
            ret += !!long_rps->used[i];
    }

    if (sh->inter_layer_pred) {
        av_assert0(pps->sps->vps->num_direct_ref_layers[layer_idx] < 2);
        ret++;
    }

    if (pps->pps_curr_pic_ref_enabled_flag)
        ret++;

    return ret;
}
