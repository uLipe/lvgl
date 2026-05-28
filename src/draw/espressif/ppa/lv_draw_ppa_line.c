/**
 * @file lv_draw_ppa_line.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

#if LV_USE_PPA_LINE

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void enqueue_segment(lv_draw_ppa_unit_t * u, lv_draw_buf_t * draw_buf,
                            const lv_area_t * strip, const lv_area_t * clip,
                            const lv_area_t * buf_area, uint32_t color);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/* Axis-aligned line draw via PPA fill. The geometry the engine can express is
 * a rectangle, so only purely horizontal or vertical line segments fit; any
 * diagonal slope, dashed pattern or rounded ends would require coverage that
 * the fill client cannot produce. The line thickness is centred around the
 * geometric segment (`p1`/`p2`) using `width / 2` extents on each side, which
 * matches what `lv_draw_sw_line` does for thick perpendicular lines. The
 * `points` array path (multi-segment polylines) stays on SW: the iteration
 * helper would defeat the up-front evaluation contract used by the draw unit
 * scheduler. */
void LV_ATTRIBUTE_FAST_MEM lv_draw_ppa_line(lv_draw_task_t * t, const lv_draw_line_dsc_t * dsc)
{
    if(dsc->opa < (lv_opa_t)LV_OPA_MAX) return;
    if(dsc->width <= 0) return;

    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)t->draw_unit;
    lv_layer_t * layer = t->target_layer;
    lv_draw_buf_t * draw_buf = layer->draw_buf;

    int32_t p1x = (int32_t)dsc->p1.x;
    int32_t p1y = (int32_t)dsc->p1.y;
    int32_t p2x = (int32_t)dsc->p2.x;
    int32_t p2y = (int32_t)dsc->p2.y;

    int32_t half = dsc->width / 2;
    int32_t extra = dsc->width - half - 1;
    if(extra < 0) extra = 0;

    lv_area_t strip;
    if(p1y == p2y) {
        /* Horizontal line: thickness extends in y. */
        int32_t x1 = LV_MIN(p1x, p2x);
        int32_t x2 = LV_MAX(p1x, p2x);
        strip.x1 = x1;
        strip.x2 = x2;
        strip.y1 = p1y - half;
        strip.y2 = p1y + extra;
    }
    else if(p1x == p2x) {
        /* Vertical line: thickness extends in x. */
        int32_t y1 = LV_MIN(p1y, p2y);
        int32_t y2 = LV_MAX(p1y, p2y);
        strip.x1 = p1x - half;
        strip.x2 = p1x + extra;
        strip.y1 = y1;
        strip.y2 = y2;
    }
    else {
        return; /* should be filtered out by ppa_evaluate already */
    }

    if(strip.x2 < strip.x1 || strip.y2 < strip.y1) return;

    uint32_t color = lv_color_to_u32(dsc->color);
    enqueue_segment(u, draw_buf, &strip, &t->clip_area, &layer->buf_area, color);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void LV_ATTRIBUTE_FAST_MEM enqueue_segment(lv_draw_ppa_unit_t * u, lv_draw_buf_t * draw_buf,
                                                  const lv_area_t * strip, const lv_area_t * clip,
                                                  const lv_area_t * buf_area, uint32_t color)
{
    lv_area_t fill_area;
    if(!lv_area_intersect(&fill_area, strip, clip)) return;
    lv_area_move(&fill_area, -buf_area->x1, -buf_area->y1);

    ppa_fill_oper_config_t cfg = {0};
    cfg.fill_argb_color.val = color;
    cfg.out.block_offset_x  = fill_area.x1;
    cfg.out.block_offset_y  = fill_area.y1;
    cfg.out.fill_cm         = lv_color_format_to_ppa_fill(draw_buf->header.cf);
    cfg.fill_block_w        = lv_area_get_width(&fill_area);
    cfg.fill_block_h        = lv_area_get_height(&fill_area);
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
        LV_LOG_ERROR("PPA line fill failed: %d", ret);
    }
}

#endif /* LV_USE_PPA_LINE */
