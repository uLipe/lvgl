/**
 * @file lv_draw_ppa_border.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

#if LV_USE_PPA_BORDER

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void enqueue_strip(lv_draw_ppa_unit_t * u, lv_draw_buf_t * draw_buf,
                          const lv_area_t * strip, const lv_area_t * clip,
                          uint32_t fill_color);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/* Decompose a sharp-corner border into up to four axis-aligned strips and
 * submit each one as an independent PPA fill operation. Vertical strips skip
 * the rows already painted by the top/bottom horizontal strips so the corner
 * pixels are written exactly once. Borders with non-opaque alpha or rounded
 * corners are rejected upstream in lv_draw_ppa.c::ppa_evaluate; this worker
 * assumes the caller validated the simple case. */
void LV_ATTRIBUTE_FAST_MEM lv_draw_ppa_border(lv_draw_task_t * t, const lv_draw_border_dsc_t * dsc,
                                              const lv_area_t * coords)
{
    if(dsc->opa <= LV_OPA_MIN || dsc->width <= 0 || dsc->side == LV_BORDER_SIDE_NONE) return;

    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)t->draw_unit;
    lv_layer_t * layer = t->target_layer;
    lv_draw_buf_t * draw_buf = layer->draw_buf;

    /* Translate to layer-relative coordinates so the PPA receives pixel offsets
     * inside the destination buffer rather than absolute screen positions. */
    lv_area_t rel_coords;
    lv_area_copy(&rel_coords, coords);
    lv_area_move(&rel_coords, -layer->buf_area.x1, -layer->buf_area.y1);

    lv_area_t rel_clip;
    lv_area_copy(&rel_clip, &t->clip_area);
    lv_area_move(&rel_clip, -layer->buf_area.x1, -layer->buf_area.y1);

    int32_t width = dsc->width;
    uint32_t fill_color = lv_color_to_u32(dsc->color);

    /* Top strip spans the full width; corners belong to top/bottom by convention. */
    if(dsc->side & LV_BORDER_SIDE_TOP) {
        lv_area_t s = {
            .x1 = rel_coords.x1,
            .y1 = rel_coords.y1,
            .x2 = rel_coords.x2,
            .y2 = LV_MIN(rel_coords.y2, rel_coords.y1 + width - 1),
        };
        enqueue_strip(u, draw_buf, &s, &rel_clip, fill_color);
    }

    if(dsc->side & LV_BORDER_SIDE_BOTTOM) {
        lv_area_t s = {
            .x1 = rel_coords.x1,
            .y1 = LV_MAX(rel_coords.y1, rel_coords.y2 - width + 1),
            .x2 = rel_coords.x2,
            .y2 = rel_coords.y2,
        };
        enqueue_strip(u, draw_buf, &s, &rel_clip, fill_color);
    }

    /* Left/right strips trim the rows already covered by top/bottom to avoid
     * double-painting the corner cells. */
    int32_t side_y1 = (dsc->side & LV_BORDER_SIDE_TOP)    ? rel_coords.y1 + width : rel_coords.y1;
    int32_t side_y2 = (dsc->side & LV_BORDER_SIDE_BOTTOM) ? rel_coords.y2 - width : rel_coords.y2;
    if(side_y2 >= side_y1) {
        if(dsc->side & LV_BORDER_SIDE_LEFT) {
            lv_area_t s = {
                .x1 = rel_coords.x1,
                .y1 = side_y1,
                .x2 = LV_MIN(rel_coords.x2, rel_coords.x1 + width - 1),
                .y2 = side_y2,
            };
            enqueue_strip(u, draw_buf, &s, &rel_clip, fill_color);
        }
        if(dsc->side & LV_BORDER_SIDE_RIGHT) {
            lv_area_t s = {
                .x1 = LV_MAX(rel_coords.x1, rel_coords.x2 - width + 1),
                .y1 = side_y1,
                .x2 = rel_coords.x2,
                .y2 = side_y2,
            };
            enqueue_strip(u, draw_buf, &s, &rel_clip, fill_color);
        }
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void LV_ATTRIBUTE_FAST_MEM enqueue_strip(lv_draw_ppa_unit_t * u, lv_draw_buf_t * draw_buf,
                                                const lv_area_t * strip, const lv_area_t * clip,
                                                uint32_t fill_color)
{
    lv_area_t fill_area;
    if(!lv_area_intersect(&fill_area, strip, clip)) return;

    ppa_fill_oper_config_t cfg = {0};
    cfg.fill_argb_color.val = fill_color;
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
        LV_LOG_ERROR("PPA border strip failed: %d", ret);
    }
}

#endif /* LV_USE_PPA_BORDER */
