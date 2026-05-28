/**
 * @file lv_draw_ppa_layer.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

#if LV_USE_PPA

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/* Layer composition entry point. The actual blit reuses the image worker
 * because, from the PPA point of view, drawing a finalized child layer onto
 * the parent layer is identical to blending an ARGB8888/RGB565/RGB888 image
 * source. Behavior is shared between the LV_USE_PPA_LAYER (identity
 * composer) and LV_USE_PPA_TRANSFORM (rotate/scale/mirror) gates: the routing
 * decision sits in lv_draw_ppa.c::ppa_evaluate, so this function handles both
 * paths transparently. */
void LV_ATTRIBUTE_FAST_MEM lv_draw_ppa_layer(lv_draw_task_t * t, const lv_draw_image_dsc_t * dsc,
                                             const lv_area_t * coords)
{
    lv_layer_t * layer_to_draw = (lv_layer_t *)dsc->src;
    if(layer_to_draw == NULL || layer_to_draw->draw_buf == NULL) return;

    lv_draw_image_dsc_t new_draw_dsc = *dsc;
    new_draw_dsc.src = layer_to_draw->draw_buf;
    new_draw_dsc.header = layer_to_draw->draw_buf->header;

    lv_draw_ppa_img(t, &new_draw_dsc, coords);
}

#endif /* LV_USE_PPA */
