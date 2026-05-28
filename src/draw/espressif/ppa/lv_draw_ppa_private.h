/**
 * @file lv_draw_ppa_private.h
 *
 */

#ifndef LV_DRAW_PPA_PRIVATE_H
#define LV_DRAW_PPA_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
*      INCLUDES
*********************/
#include "../../../lvgl_public.h"

#if LV_USE_PPA

#include "../../lv_draw_private.h"
#include "../../../display/lv_display_private.h"
#include "../../../misc/lv_area_private.h"

/* The ppa driver depends heavily on the esp-idf headers*/
#include <sdkconfig.h>
#include <stdatomic.h>

#if (CONFIG_LV_DRAW_BUF_ALIGN != CONFIG_CACHE_L2_CACHE_LINE_SIZE)
#error "CONFIG_LV_DRAW_BUF_ALIGN must be equal to CONFIG_CACHE_L2_CACHE_LINE_SIZE!"
#endif


#ifndef CONFIG_SOC_PPA_SUPPORTED
#error "This SoC does not support PPA"
#endif

#include <driver/ppa.h>
#include <esp_heap_caps.h>
#include <esp_err.h>
#include <hal/color_hal.h>
#include <esp_cache.h>
#include <esp_log.h>
/*********************
*      DEFINES
*********************/

/**********************
*      TYPEDEFS
**********************/
typedef struct lv_draw_ppa_unit {
    lv_draw_unit_t base_unit;
    lv_draw_task_t * task_act;
    ppa_client_handle_t srm_client;
    ppa_client_handle_t fill_client;
    ppa_client_handle_t blend_client;
    uint8_t * buf;
#if LV_USE_PPA_ASYNC
    /* Synchronization primitive signaled from the PPA ISR when the last sub-operation
     * of the active LVGL task completes. The draw scheduler waits on this from
     * `wait_for_finish_cb` before consuming the rendered buffer. */
    lv_thread_sync_t done_sync;
    /* Number of PPA sub-operations enqueued for the current LVGL task. A single LVGL
     * draw task may decompose into several PPA ops (e.g. border = 4 fills). Each
     * ISR completion decrements the counter; reaching zero signals `done_sync`. */
    atomic_int pending_ops;
#endif
} lv_draw_ppa_unit_t;

/**********************
*  STATIC PROTOTYPES
**********************/

/**********************
* GLOBAL PROTOTYPES
**********************/

/**********************
*      MACROS
**********************/

/**********************
*   STATIC FUNCTIONS
**********************/

static inline bool ppa_src_cf_supported(lv_color_format_t cf)
{
    bool is_cf_supported = false;

    switch(cf) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_ARGB8888:
        case LV_COLOR_FORMAT_XRGB8888:
            is_cf_supported = true;
            break;
        default:
            break;
    }

    return is_cf_supported;
}

static inline bool ppa_dest_cf_supported(lv_color_format_t cf)
{
    bool is_cf_supported = false;

    switch(cf) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_ARGB8888:
            is_cf_supported = true;
            break;
        default:
            break;
    }

    return is_cf_supported;
}

static inline bool ppa_srm_src_cf_supported(lv_color_format_t cf)
{
    switch(cf) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_ARGB8888:
        case LV_COLOR_FORMAT_XRGB8888:
            return true;
        default:
            return false;
    }
}

static inline bool ppa_srm_dest_cf_supported(lv_color_format_t cf)
{
    switch(cf) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_ARGB8888:
            return true;
        default:
            return false;
    }
}

static inline ppa_fill_color_mode_t lv_color_format_to_ppa_fill(lv_color_format_t lv_fmt)
{
    switch(lv_fmt) {
        case LV_COLOR_FORMAT_RGB565:
            return PPA_FILL_COLOR_MODE_RGB565;
        case LV_COLOR_FORMAT_RGB888:
            return PPA_FILL_COLOR_MODE_RGB888;
        case LV_COLOR_FORMAT_ARGB8888:
            return PPA_FILL_COLOR_MODE_ARGB8888;
        default:
            return PPA_FILL_COLOR_MODE_RGB565;
    }
}

static inline ppa_blend_color_mode_t lv_color_format_to_ppa_blend(lv_color_format_t lv_fmt)
{
    switch(lv_fmt) {
        case LV_COLOR_FORMAT_RGB565:
            return PPA_BLEND_COLOR_MODE_RGB565;
        case LV_COLOR_FORMAT_RGB888:
            return PPA_BLEND_COLOR_MODE_RGB888;
        case LV_COLOR_FORMAT_ARGB8888:
        case LV_COLOR_FORMAT_XRGB8888:
            return PPA_BLEND_COLOR_MODE_ARGB8888;
        default:
            return PPA_BLEND_COLOR_MODE_RGB565;
    }
}

static inline ppa_srm_color_mode_t lv_color_format_to_ppa_srm(lv_color_format_t lv_fmt)
{
    switch(lv_fmt) {
        case LV_COLOR_FORMAT_RGB565:
            return PPA_SRM_COLOR_MODE_RGB565;
        case LV_COLOR_FORMAT_RGB888:
            return PPA_SRM_COLOR_MODE_RGB888;
        case LV_COLOR_FORMAT_ARGB8888:
        case LV_COLOR_FORMAT_XRGB8888:
            return PPA_SRM_COLOR_MODE_ARGB8888;
        default:
            return PPA_SRM_COLOR_MODE_RGB565;
    }
}

/* Common transfer mode used by every `ppa_do_*` call. With async enabled the PPA driver
 * returns immediately and signals `done_sync` from its ISR; otherwise the call blocks
 * until the engine finishes the operation (legacy behavior). */
#if LV_USE_PPA_ASYNC
#define LV_PPA_TRANS_MODE PPA_TRANS_MODE_NON_BLOCKING
#else
#define LV_PPA_TRANS_MODE PPA_TRANS_MODE_BLOCKING
#endif

/* Sub-operation accounting helpers used by every `lv_draw_ppa_*` worker.
 * `begin_op` must be called before each `ppa_do_*` enqueue, `cancel_op`
 * compensates the counter when the enqueue itself fails so the wait callback
 * does not deadlock. */
static inline void lv_draw_ppa_begin_op(lv_draw_ppa_unit_t * u)
{
#if LV_USE_PPA_ASYNC
    atomic_fetch_add(&u->pending_ops, 1);
#else
    LV_UNUSED(u);
#endif
}

static inline void lv_draw_ppa_cancel_op(lv_draw_ppa_unit_t * u)
{
#if LV_USE_PPA_ASYNC
    atomic_fetch_sub(&u->pending_ops, 1);
#else
    LV_UNUSED(u);
#endif
}

#define PPA_ALIGN_UP(x, align)  ((((x) + (align) - 1) / (align)) * (align))
#define PPA_PTR_ALIGN_UP(p, align) \
    ((void*)(((uintptr_t)(p) + (uintptr_t)((align) - 1)) & ~(uintptr_t)((align) - 1)))

#define PPA_ALIGN_DOWN(x, align)  ((((x) - (align) - 1) / (align)) * (align))
#define PPA_PTR_ALIGN_DOWN(p, align) \
    ((void*)(((uintptr_t)(p) - (uintptr_t)((align) - 1)) & ~(uintptr_t)((align) - 1)))

#endif /* LV_USE_PPA */

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* LV_DRAW_PPA_PRIVATE_H */
