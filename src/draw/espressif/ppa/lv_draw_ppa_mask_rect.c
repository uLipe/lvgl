/**
 * @file lv_draw_ppa_mask_rect.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

#if LV_USE_PPA_MASK_RECT

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void enqueue_clear(lv_draw_ppa_unit_t * u, lv_draw_buf_t * draw_buf,
                          const lv_area_t * area, const lv_area_t * buf_area);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/* Sharp-corner mask rectangle: with radius == 0 the operation reduces to
 * clearing the area complement (keep_outside == 0) or the area itself
 * (keep_outside == 1) on the layer's alpha channel. Both cases are
 * implemented as PPA fills with a fully-transparent color, which writes
 * (R=0, G=0, B=0, A=0) into the destination buffer.
 *
 * Anti-aliased rounded corners need a per-pixel alpha multiply that is not
 * exposed as a single PPA primitive (the engine offers ALPHA_SCALE per
 * transaction, ALPHA_INVERT per pixel and Porter-Duff OVER blending, none of
 * which yield A_dst *= mask in one op). The rounded path will land in Phase 2
 * via the tile composer, which prepares the corner alpha map in PSRAM and
 * applies it through hardware blend; until then radius != 0 stays on SW. */
void LV_ATTRIBUTE_FAST_MEM lv_draw_ppa_mask_rect(lv_draw_task_t * t, const lv_draw_mask_rect_dsc_t * dsc)
{
    if(dsc->radius != 0) return;

    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)t->draw_unit;
    lv_layer_t * layer = t->target_layer;
    lv_draw_buf_t * draw_buf = layer->draw_buf;
    const lv_area_t * buf_area = &layer->buf_area;

    lv_area_t draw_area;
    if(!lv_area_intersect(&draw_area, &dsc->area, &t->clip_area)) return;

    if(dsc->keep_outside) {
        /* Erase the masked area itself, leaving the outside intact. */
        enqueue_clear(u, draw_buf, &draw_area, buf_area);
        return;
    }

    /* keep_outside == 0: erase everything in the clip rectangle that lies
     * outside dsc->area. Decompose into four axis-aligned strips so each
     * cleared region can be submitted as a single PPA fill. */
    lv_area_t strip;

    /* Top strip: clip rows above dsc->area. */
    lv_area_set(&strip, t->clip_area.x1, t->clip_area.y1, t->clip_area.x2, dsc->area.y1 - 1);
    if(strip.y2 >= strip.y1) enqueue_clear(u, draw_buf, &strip, buf_area);

    /* Bottom strip: clip rows below dsc->area. */
    lv_area_set(&strip, t->clip_area.x1, dsc->area.y2 + 1, t->clip_area.x2, t->clip_area.y2);
    if(strip.y2 >= strip.y1) enqueue_clear(u, draw_buf, &strip, buf_area);

    /* Left and right strips: limited to the vertical band of dsc->area so
     * they do not overlap the top/bottom strips already cleared. */
    int32_t side_y1 = LV_MAX(t->clip_area.y1, dsc->area.y1);
    int32_t side_y2 = LV_MIN(t->clip_area.y2, dsc->area.y2);
    if(side_y2 < side_y1) return;

    lv_area_set(&strip, t->clip_area.x1, side_y1, dsc->area.x1 - 1, side_y2);
    if(strip.x2 >= strip.x1) enqueue_clear(u, draw_buf, &strip, buf_area);

    lv_area_set(&strip, dsc->area.x2 + 1, side_y1, t->clip_area.x2, side_y2);
    if(strip.x2 >= strip.x1) enqueue_clear(u, draw_buf, &strip, buf_area);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void LV_ATTRIBUTE_FAST_MEM enqueue_clear(lv_draw_ppa_unit_t * u, lv_draw_buf_t * draw_buf,
                                                const lv_area_t * area, const lv_area_t * buf_area)
{
    lv_area_t rel;
    lv_area_copy(&rel, area);
    lv_area_move(&rel, -buf_area->x1, -buf_area->y1);
    if(rel.x2 < rel.x1 || rel.y2 < rel.y1) return;

    ppa_fill_oper_config_t cfg = {0};
    /* Fully transparent: A=0, R=G=B=0. On RGB-only destinations the alpha
     * field is dropped by the engine and the strip ends up black, which is
     * fine because mask rectangles are only meaningful on ARGB layers. */
    cfg.fill_argb_color.val = 0;
    cfg.out.block_offset_x  = rel.x1;
    cfg.out.block_offset_y  = rel.y1;
    cfg.out.fill_cm         = lv_color_format_to_ppa_fill(draw_buf->header.cf);
    cfg.fill_block_w        = lv_area_get_width(&rel);
    cfg.fill_block_h        = lv_area_get_height(&rel);
    cfg.out.buffer          = draw_buf->data;
    cfg.out.buffer_size     = draw_buf->data_size;
    cfg.out.pic_w           = draw_buf->header.w;
    cfg.out.pic_h           = draw_buf->header.h;
    cfg.mode                = LV_PPA_TRANS_MODE;
    cfg.user_data           = u;

    lv_draw_ppa_begin_op(u);
    esp_err_t ret = ppa_do_fill(u->fill_client, &cfg);
    if(ret != ESP_OK) {
        lv_draw_ppa_cancel_op(u);
        LV_LOG_ERROR("PPA mask_rect clear failed: %d", ret);
    }
}

#endif /* LV_USE_PPA_MASK_RECT */
