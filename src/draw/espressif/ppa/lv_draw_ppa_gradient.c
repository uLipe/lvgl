/**
 * @file lv_draw_ppa_gradient.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

#if LV_USE_PPA_GRADIENT

/**********************
 *  STATIC PROTOTYPES
 **********************/

static uint32_t mix_color_u32(lv_color_t a, lv_color_t b, uint32_t mix_q8);
static void enqueue_strip(lv_draw_ppa_unit_t * u, lv_draw_buf_t * draw_buf,
                          const lv_area_t * strip, const lv_area_t * buf_area, uint32_t color);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/* Approximate horizontal/vertical two-stop gradients with a configurable number
 * of solid-color strips (`LV_PPA_GRADIENT_STEPS`). Each strip is dispatched as
 * an independent PPA fill, so the whole task scales linearly with the step
 * count. We accept only the simple 2-stop case here; multi-stop, linear,
 * radial and conical paths require either a tile-composer driven blit chain
 * or full per-pixel rasterization and stay on the SW renderer for now.
 *
 * The mix between the two stops is done in a `uint32_t` accumulator so we can
 * preserve the alpha channel when the destination is ARGB8888. Coordinates
 * are translated into the layer-relative space before submission, exactly as
 * lv_draw_ppa_fill does for solid-color rectangles. */
void LV_ATTRIBUTE_FAST_MEM lv_draw_ppa_gradient(lv_draw_task_t * t, const lv_draw_fill_dsc_t * dsc,
                                                const lv_area_t * coords)
{
    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)t->draw_unit;
    lv_layer_t * layer = t->target_layer;
    lv_draw_buf_t * draw_buf = layer->draw_buf;

    lv_area_t rel_coords;
    lv_area_copy(&rel_coords, coords);

    lv_area_t rel_clip;
    lv_area_copy(&rel_clip, &t->clip_area);

    lv_area_t blend_area;
    if(!lv_area_intersect(&blend_area, &rel_coords, &rel_clip)) return;

    int32_t total_w = lv_area_get_width(&rel_coords);
    int32_t total_h = lv_area_get_height(&rel_coords);
    if(total_w <= 0 || total_h <= 0) return;

    lv_color_t color_a = dsc->grad.stops[0].color;
    lv_color_t color_b = dsc->grad.stops[1].color;
    bool vertical = (dsc->grad.dir == LV_GRAD_DIR_VER);

    int32_t span = vertical ? total_h : total_w;
    int32_t steps = LV_PPA_GRADIENT_STEPS;
    if(steps > span) steps = span;
    if(steps <= 0) return;

    for(int32_t i = 0; i < steps; i++) {
        /* Pick the strip color at the centre of each band so the boundary
         * colours match what the SW renderer produces with a stops_count==2. */
        uint32_t mix_q8 = (uint32_t)((i * 2 + 1) * 256 / (steps * 2));
        if(mix_q8 > 256) mix_q8 = 256;
        uint32_t color = mix_color_u32(color_a, color_b, mix_q8);

        lv_area_t strip;
        if(vertical) {
            int32_t y1 = rel_coords.y1 + (i * total_h) / steps;
            int32_t y2 = rel_coords.y1 + ((i + 1) * total_h) / steps - 1;
            if(y2 < y1) continue;
            lv_area_set(&strip, rel_coords.x1, y1, rel_coords.x2, y2);
        }
        else {
            int32_t x1 = rel_coords.x1 + (i * total_w) / steps;
            int32_t x2 = rel_coords.x1 + ((i + 1) * total_w) / steps - 1;
            if(x2 < x1) continue;
            lv_area_set(&strip, x1, rel_coords.y1, x2, rel_coords.y2);
        }

        lv_area_t clipped;
        if(!lv_area_intersect(&clipped, &strip, &blend_area)) continue;
        enqueue_strip(u, draw_buf, &clipped, &layer->buf_area, color);
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static uint32_t LV_ATTRIBUTE_FAST_MEM mix_color_u32(lv_color_t a, lv_color_t b, uint32_t mix_q8)
{
    uint32_t inv = 256u - mix_q8;
    uint32_t r = (a.red   * inv + b.red   * mix_q8) >> 8;
    uint32_t g = (a.green * inv + b.green * mix_q8) >> 8;
    uint32_t blue = (a.blue * inv + b.blue * mix_q8) >> 8;
    return (0xFFu << 24) | (r << 16) | (g << 8) | blue;
}

static void LV_ATTRIBUTE_FAST_MEM enqueue_strip(lv_draw_ppa_unit_t * u, lv_draw_buf_t * draw_buf,
                                                const lv_area_t * strip, const lv_area_t * buf_area, uint32_t color)
{
    lv_area_t rel;
    lv_area_copy(&rel, strip);
    lv_area_move(&rel, -buf_area->x1, -buf_area->y1);

    ppa_fill_oper_config_t cfg = {0};
    cfg.fill_argb_color.val = color;
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
        LV_LOG_ERROR("PPA gradient strip failed: %d", ret);
    }
}

#endif /* LV_USE_PPA_GRADIENT */
