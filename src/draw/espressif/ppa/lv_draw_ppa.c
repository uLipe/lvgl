/**
 * @file lv_draw_ppa.c
 *
 */

/*********************
*      INCLUDES
*********************/

#include "lv_draw_ppa_private.h"
#include "lv_draw_ppa.h"

#if LV_USE_PPA

/*********************
*      DEFINES
*********************/

#define DRAW_UNIT_ID_PPA         80
#define DRAW_UNIT_PPA_PREF_SCORE 70

/**********************
*  STATIC PROTOTYPES
**********************/

static int32_t ppa_evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task);
static int32_t ppa_dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer);
static int32_t ppa_delete(lv_draw_unit_t * draw_unit);
static void  ppa_execute_drawing(lv_draw_ppa_unit_t * u);
static bool ppa_rotation_supported(int32_t rotation);
#if LV_USE_PPA_ASYNC
    static int32_t ppa_wait_for_finish(lv_draw_unit_t * draw_unit);
    static bool ppa_trans_done_cb(ppa_client_handle_t client, ppa_event_data_t * evt, void * user_data);
    static void ppa_finalize_task(lv_draw_ppa_unit_t * u);
#endif

/**********************
*   GLOBAL FUNCTIONS
**********************/

void LV_ATTRIBUTE_FAST_MEM lv_draw_ppa_init(void)
{
    esp_err_t res;
    ppa_client_config_t cfg = {0};

    /* Create draw unit */
    lv_draw_buf_ppa_init_handlers();
    lv_draw_ppa_unit_t * draw_ppa_unit = lv_draw_create_unit(sizeof(lv_draw_ppa_unit_t));
    draw_ppa_unit->base_unit.evaluate_cb = ppa_evaluate;
    draw_ppa_unit->base_unit.dispatch_cb  = ppa_dispatch;
    draw_ppa_unit->base_unit.delete_cb    = ppa_delete;
    draw_ppa_unit->base_unit.name         = "ESP_PPA";
#if LV_USE_PPA_ASYNC
    draw_ppa_unit->base_unit.wait_for_finish_cb = ppa_wait_for_finish;
    lv_thread_sync_init(&draw_ppa_unit->done_sync);
    atomic_init(&draw_ppa_unit->pending_ops, 0);
#endif

#if (LV_PPA_BURST_LENGTH == 128)
    const ppa_data_burst_length_t burst_len = PPA_DATA_BURST_LENGTH_128;
#elif (LV_PPA_BURST_LENGTH == 64)
    const ppa_data_burst_length_t burst_len = PPA_DATA_BURST_LENGTH_64;
#elif (LV_PPA_BURST_LENGTH == 32)
    const ppa_data_burst_length_t burst_len = PPA_DATA_BURST_LENGTH_32;
#elif (LV_PPA_BURST_LENGTH == 16)
    const ppa_data_burst_length_t burst_len = PPA_DATA_BURST_LENGTH_16;
#elif (LV_PPA_BURST_LENGTH == 8)
    const ppa_data_burst_length_t burst_len = PPA_DATA_BURST_LENGTH_8;
#else
#error "Invalid burst length selection for PPA"
#endif

    /* Register SRM client */
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.data_burst_length = burst_len;
#if LV_USE_PPA_ASYNC
    cfg.max_pending_trans_num = LV_PPA_SRM_PENDING_TRANS;
#else
    cfg.max_pending_trans_num = 1;
#endif
    res = ppa_register_client(&cfg, &draw_ppa_unit->srm_client);
    LV_ASSERT(res == ESP_OK);

    /* Register Fill client */
    cfg.oper_type = PPA_OPERATION_FILL;
#if LV_USE_PPA_ASYNC
    cfg.max_pending_trans_num = LV_PPA_FILL_PENDING_TRANS;
#endif
    res = ppa_register_client(&cfg, &draw_ppa_unit->fill_client);
    LV_ASSERT(res == ESP_OK);

    /* Register Blend client */
    cfg.oper_type = PPA_OPERATION_BLEND;
#if LV_USE_PPA_ASYNC
    cfg.max_pending_trans_num = LV_PPA_BLEND_PENDING_TRANS;
#endif
    res = ppa_register_client(&cfg, &draw_ppa_unit->blend_client);
    LV_ASSERT(res == ESP_OK);

#if LV_USE_PPA_ASYNC
    /* Use a single completion callback for all clients; the user_data carries the
     * draw unit so the ISR can decrement the shared sub-op counter and wake the
     * dispatcher exactly once per LVGL task. */
    const ppa_event_callbacks_t cbs = { .on_trans_done = ppa_trans_done_cb };
    res = ppa_client_register_event_callbacks(draw_ppa_unit->srm_client, &cbs);
    LV_ASSERT(res == ESP_OK);
    res = ppa_client_register_event_callbacks(draw_ppa_unit->fill_client, &cbs);
    LV_ASSERT(res == ESP_OK);
    res = ppa_client_register_event_callbacks(draw_ppa_unit->blend_client, &cbs);
    LV_ASSERT(res == ESP_OK);
#endif

#if LV_USE_PPA_TILE_COMPOSER
    if(!lv_draw_ppa_tile_pool_init(draw_ppa_unit)) {
        LV_LOG_WARN("PPA tile composer pool unavailable; multi-pass paths will fall back to SW");
    }
#endif
}

void LV_ATTRIBUTE_FAST_MEM lv_draw_ppa_deinit(void)
{
    /* No global deinit required */
}

/**********************
*   STATIC FUNCTIONS
**********************/
static int32_t LV_ATTRIBUTE_FAST_MEM ppa_evaluate(lv_draw_unit_t * u, lv_draw_task_t * t)
{
    LV_UNUSED(u);
    const lv_draw_dsc_base_t * base = (lv_draw_dsc_base_t *)t->draw_dsc;

    if(!ppa_dest_cf_supported(base->layer->color_format)) return 0;

    switch(t->type) {
        case LV_DRAW_TASK_TYPE_FILL: {
                const lv_draw_fill_dsc_t * dsc = (lv_draw_fill_dsc_t *)t->draw_dsc;
                if((dsc->radius != 0 || dsc->grad.dir != LV_GRAD_DIR_NONE)) return 0;
                if(dsc->opa <= (lv_opa_t)LV_OPA_MAX) return 0;

                if(t->preference_score > DRAW_UNIT_PPA_PREF_SCORE) {
                    t->preference_score = DRAW_UNIT_PPA_PREF_SCORE;
                    t->preferred_draw_unit_id = DRAW_UNIT_ID_PPA;
                }
                return 1;
            }

#if LV_USE_PPA_BORDER
        case LV_DRAW_TASK_TYPE_BORDER: {
                const lv_draw_border_dsc_t * dsc = (lv_draw_border_dsc_t *)t->draw_dsc;
                /* Only sharp-corner, opaque borders fit the strip-fill decomposition. */
                if(dsc->radius != 0) return 0;
                if(dsc->opa < (lv_opa_t)LV_OPA_MAX) return 0;
                if(dsc->width <= 0) return 0;
                if(dsc->side == LV_BORDER_SIDE_NONE) return 0;

                if(t->preference_score > DRAW_UNIT_PPA_PREF_SCORE) {
                    t->preference_score = DRAW_UNIT_PPA_PREF_SCORE;
                    t->preferred_draw_unit_id = DRAW_UNIT_ID_PPA;
                }
                return 1;
            }
#endif

#if LV_USE_PPA_MASK_RECT
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE: {
                const lv_draw_mask_rect_dsc_t * dsc = (lv_draw_mask_rect_dsc_t *)t->draw_dsc;
                /* Phase 1 only handles sharp-corner masks; rounded ones need the
                 * tile composer (Phase 2) because the engine has no per-pixel
                 * alpha-multiply primitive. */
                if(dsc->radius != 0) return 0;

                if(t->preference_score > DRAW_UNIT_PPA_PREF_SCORE) {
                    t->preference_score = DRAW_UNIT_PPA_PREF_SCORE;
                    t->preferred_draw_unit_id = DRAW_UNIT_ID_PPA;
                }
                return 1;
            }
#endif

        case LV_DRAW_TASK_TYPE_IMAGE: {
                lv_draw_image_dsc_t * dsc = t->draw_dsc;
                bool common_ok = dsc->header.cf < LV_COLOR_FORMAT_PROPRIETARY_START
                                 && dsc->clip_radius == 0
                                 && dsc->bitmap_mask_src == NULL
                                 && dsc->sup == NULL
                                 && dsc->tile == 0
                                 && dsc->blend_mode == LV_BLEND_MODE_NORMAL
                                 && dsc->recolor_opa <= LV_OPA_MIN
                                 && dsc->opa >= (lv_opa_t)LV_OPA_MAX
                                 && dsc->skew_y == 0
                                 && dsc->skew_x == 0
                                 && lv_image_src_get_type(dsc->src) == LV_IMAGE_SRC_VARIABLE;
                if(!common_ok) return 0;

                bool is_identity = (dsc->scale_x == LV_SCALE_NONE && dsc->scale_y == LV_SCALE_NONE && dsc->rotation == 0);
                bool clip_is_full = lv_area_is_equal(&t->area, &t->clip_area);

                bool ppa_ok = false;
#if LV_USE_PPA_IMG
                if(is_identity && ppa_src_cf_supported(dsc->header.cf) && ppa_dest_cf_supported(dsc->base.layer->color_format)) {
                    ppa_ok = true;
                }
#endif
#if LV_USE_PPA_TRANSFORM
                if(!ppa_ok && clip_is_full
                   && ppa_rotation_supported(dsc->rotation)
                   && ppa_srm_src_cf_supported(dsc->header.cf)
                   && ppa_srm_dest_cf_supported(dsc->base.layer->color_format)) {
                    ppa_ok = true;
                }
#endif
                if(!ppa_ok) return 0;

                if(t->preference_score > DRAW_UNIT_PPA_PREF_SCORE) {
                    t->preference_score = DRAW_UNIT_PPA_PREF_SCORE;
                    t->preferred_draw_unit_id = DRAW_UNIT_ID_PPA;
                }
                return 1;
            }
#if LV_USE_PPA_LAYER || LV_USE_PPA_TRANSFORM
        case LV_DRAW_TASK_TYPE_LAYER: {
                lv_draw_image_dsc_t * dsc = t->draw_dsc;
                lv_layer_t * src_layer = (lv_layer_t *)dsc->src;
                if(src_layer == NULL || src_layer->draw_buf == NULL) return 0;

                bool common_ok = dsc->clip_radius == 0
                                 && dsc->bitmap_mask_src == NULL
                                 && dsc->sup == NULL
                                 && dsc->tile == 0
                                 && dsc->blend_mode == LV_BLEND_MODE_NORMAL
                                 && dsc->skew_y == 0
                                 && dsc->skew_x == 0;
                if(!common_ok) return 0;

                bool is_identity = (dsc->scale_x == LV_SCALE_NONE
                                    && dsc->scale_y == LV_SCALE_NONE
                                    && dsc->rotation == 0);
                bool layer_ok = false;

#if LV_USE_PPA_TILE_COMPOSER
                /* Recolor combined with global opa needs the tile composer; check
                 * eligibility first because identity_layer below would otherwise
                 * reject these tasks via the recolor_opa<=MIN constraint. */
                if(is_identity && lv_draw_ppa_layer_recolor_opa_supported(dsc)) {
                    layer_ok = true;
                }
#endif

                /* The simpler paths below cannot recolor a layer; bail out before
                 * checking them when LVGL asks for recolor and we did not catch it
                 * with the tile composer above. */
                if(!layer_ok && dsc->recolor_opa > LV_OPA_MIN) return 0;
#if LV_USE_PPA_LAYER
                /* Composer path: identity layer is just a blit/blend, dispatched via
                 * lv_draw_ppa_img which already handles opa (ALPHA_SCALE) and the
                 * source alpha channel. */
                if(is_identity
                   && ppa_src_cf_supported(src_layer->draw_buf->header.cf)
                   && ppa_dest_cf_supported(dsc->base.layer->color_format)) {
                    layer_ok = true;
                }
#endif
#if LV_USE_PPA_TRANSFORM
                /* Transform path: SRM client requires opaque alpha and the full
                 * transformed area within the clip window. */
                if(!layer_ok
                   && dsc->opa >= (lv_opa_t)LV_OPA_MAX
                   && lv_area_is_equal(&t->area, &t->clip_area)
                   && ppa_rotation_supported(dsc->rotation)
                   && ppa_srm_src_cf_supported(src_layer->draw_buf->header.cf)
                   && ppa_srm_dest_cf_supported(dsc->base.layer->color_format)) {
                    layer_ok = true;
                }
#endif
                if(!layer_ok) return 0;

                if(t->preference_score > DRAW_UNIT_PPA_PREF_SCORE) {
                    t->preference_score = DRAW_UNIT_PPA_PREF_SCORE;
                    t->preferred_draw_unit_id = DRAW_UNIT_ID_PPA;
                }
                return 1;
            }
#endif
        default:
            return 0;
    }
}

static int32_t LV_ATTRIBUTE_FAST_MEM ppa_dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer)
{
    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)draw_unit;
    if(u->task_act) {
        return LV_DRAW_UNIT_IDLE;
    }

    lv_draw_task_t * t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_PPA);
    if(!t || t->preferred_draw_unit_id != DRAW_UNIT_ID_PPA) return LV_DRAW_UNIT_IDLE;
    if(lv_draw_layer_alloc_buf(layer) == NULL) return LV_DRAW_UNIT_IDLE;

    t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    u->task_act = t;
    u->task_act->draw_unit = draw_unit;

    ppa_execute_drawing(u);

#if LV_USE_PPA_ASYNC
    /* Async path: if the worker queued at least one PPA sub-op the ISR will
     * signal `done_sync` once the last one finishes. If it submitted nothing
     * (fully clipped area, geometry rejected by the hardware constraints, ...)
     * the ISR will never fire, and leaving `task_act` set would stall every
     * dependent task in the layer because LVGL only calls `wait_for_finish_cb`
     * when *all* units idle simultaneously. Finalize synchronously in that
     * case so the scheduler can move on. */
    if(atomic_load(&u->pending_ops) == 0) {
        ppa_finalize_task(u);
        return 1;
    }
    return LV_DRAW_UNIT_IDLE;
#else
    u->task_act->state = LV_DRAW_TASK_STATE_FINISHED;
    u->task_act = NULL;
    lv_draw_dispatch_request();

    return 1;
#endif
}

static int32_t LV_ATTRIBUTE_FAST_MEM ppa_delete(lv_draw_unit_t * draw_unit)
{
    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)draw_unit;
#if LV_USE_PPA_TILE_COMPOSER
    lv_draw_ppa_tile_pool_deinit(u);
#endif
    ppa_unregister_client(u->srm_client);
    ppa_unregister_client(u->fill_client);
    ppa_unregister_client(u->blend_client);
#if LV_USE_PPA_ASYNC
    lv_thread_sync_delete(&u->done_sync);
#endif
    return 0;
}

static void LV_ATTRIBUTE_FAST_MEM ppa_execute_drawing(lv_draw_ppa_unit_t * u)
{
    lv_draw_task_t * t         = u->task_act;
    lv_layer_t * layer         = t->target_layer;
    lv_draw_buf_t * buf        = layer->draw_buf;
    lv_area_t area;

    if(!lv_area_intersect(&area, &t->area, &t->clip_area)) return;

    /* In sync mode the cache is flushed both before and after the PPA op so the
     * engine sees the latest CPU writes and the next consumer sees the engine's
     * output. In async mode the post-op flush is deferred to `wait_for_finish_cb`
     * because the operation is still pending when this function returns. */
    lv_draw_buf_invalidate_cache(buf, &area);

    switch(t->type) {
        case LV_DRAW_TASK_TYPE_FILL:
            lv_draw_ppa_fill(t, (lv_draw_fill_dsc_t *)t->draw_dsc, &area);
            break;
#if LV_USE_PPA_BORDER
        case LV_DRAW_TASK_TYPE_BORDER:
            lv_draw_ppa_border(t, (lv_draw_border_dsc_t *)t->draw_dsc, &t->area);
            break;
#endif
#if LV_USE_PPA_MASK_RECT
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE:
            lv_draw_ppa_mask_rect(t, (lv_draw_mask_rect_dsc_t *)t->draw_dsc);
            break;
#endif
        case LV_DRAW_TASK_TYPE_IMAGE:
            lv_draw_ppa_img(t, (lv_draw_image_dsc_t *)t->draw_dsc, &area);
            break;
#if LV_USE_PPA_LAYER || LV_USE_PPA_TRANSFORM
        case LV_DRAW_TASK_TYPE_LAYER: {
                lv_draw_image_dsc_t * dsc = (lv_draw_image_dsc_t *)t->draw_dsc;
#if LV_USE_PPA_TILE_COMPOSER
                if(lv_draw_ppa_layer_recolor_opa_supported(dsc)) {
                    lv_draw_ppa_layer_composite(t, dsc, &area);
                    break;
                }
#endif
                lv_draw_ppa_layer(t, dsc, &area);
                break;
            }
#endif
        default:
            break;
    }

#if !LV_USE_PPA_ASYNC
    lv_draw_buf_invalidate_cache(buf, &area);
#endif
}

static bool LV_ATTRIBUTE_FAST_MEM ppa_rotation_supported(int32_t rotation)
{
    int32_t r = rotation % 3600;
    if(r < 0) r += 3600;
    return (r == 0 || r == 900 || r == 1800 || r == 2700);
}

#if LV_USE_PPA_ASYNC

static bool LV_ATTRIBUTE_FAST_MEM ppa_trans_done_cb(ppa_client_handle_t client, ppa_event_data_t * evt,
                                                    void * user_data)
{
    LV_UNUSED(client);
    LV_UNUSED(evt);
    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)user_data;
    /* fetch_sub returns the value before subtraction; the last completion (counter
     * was 1) is the one that releases the dispatcher waiting in wait_for_finish_cb. */
    if(atomic_fetch_sub(&u->pending_ops, 1) == 1) {
        lv_thread_sync_signal_isr(&u->done_sync);
    }
    return false;
}

static void LV_ATTRIBUTE_FAST_MEM ppa_finalize_task(lv_draw_ppa_unit_t * u)
{
    lv_draw_task_t * t = u->task_act;
    if(t == NULL) return;

    /* Hardware just finished writing the destination buffer. Invalidate the CPU
     * cache so subsequent readers (next draw unit, display flush) observe the
     * fresh pixels instead of stale lines. The handler installed in
     * lv_draw_ppa_buf.c performs the actual `esp_cache_msync`. */
    lv_layer_t * layer  = t->target_layer;
    lv_draw_buf_t * buf = layer ? layer->draw_buf : NULL;
    if(buf != NULL) {
        lv_area_t area;
        if(lv_area_intersect(&area, &t->area, &t->clip_area)) {
            lv_draw_buf_invalidate_cache(buf, &area);
        }
    }

#if LV_USE_PPA_TILE_COMPOSER
    /* Hardware is done reading the intermediate tile; safe to recycle it. */
    if(u->pending_tile) {
        lv_draw_ppa_tile_release(u, u->pending_tile);
        u->pending_tile = NULL;
    }
#endif

    t->state = LV_DRAW_TASK_STATE_FINISHED;
    u->task_act = NULL;
    lv_draw_dispatch_request();
}

static int32_t LV_ATTRIBUTE_FAST_MEM ppa_wait_for_finish(lv_draw_unit_t * draw_unit)
{
    lv_draw_ppa_unit_t * u = (lv_draw_ppa_unit_t *)draw_unit;
    if(u->task_act == NULL) return 0;

    /* Block until the ISR signals completion. The PPA driver itself is
     * FreeRTOS-aware and already blocks the producer when the per-client queue
     * is full, so this wait covers only the in-flight portion of the work. */
    if(atomic_load(&u->pending_ops) > 0) {
        lv_thread_sync_wait(&u->done_sync);
    }

    ppa_finalize_task(u);
    return 0;
}

#endif /*LV_USE_PPA_ASYNC*/

#endif /*LV_USE_PPA*/
