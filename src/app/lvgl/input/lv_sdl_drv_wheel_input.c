#include "lvgl/lv_sdl_drv_input.h"

#include <SDL.h>

#include "app.h"
#include "stream/session.h"
#include "stream/session_events.h"
#include "ui/root.h"
#include "ui/ui_input.h"
#include "lv_gridview.h"

static void sdl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

static bool wheel_handle_gridview(app_ui_input_t *input, const SDL_MouseWheelEvent *wheel) {
    if (wheel->y == 0) {
        return false;
    }
    lv_group_t *group = app_input_get_group(input);
    if (group == NULL) {
        return false;
    }
    lv_obj_t *focused = lv_group_get_focused(group);
    if (focused == NULL || !lv_obj_check_type(focused, &lv_gridview_class)) {
        return false;
    }
    /* Wheel down (y > 0) advances one page; wheel up goes back one page. */
    lv_gridview_page(focused, wheel->y > 0);
    return true;
}

/**
 * Scroll the nearest vertical scrollable ancestor of the focused object.
 * Prevents Magic Remote / mouse wheel from driving LVGL encoder focus navigation
 * on settings panes and host lists.
 */
static bool wheel_handle_scrollable(app_ui_input_t *input, const SDL_MouseWheelEvent *wheel) {
    if (wheel->y == 0) {
        return false;
    }
    lv_group_t *group = app_input_get_group(input);
    if (group == NULL) {
        return false;
    }
    lv_obj_t *focused = lv_group_get_focused(group);
    if (focused == NULL) {
        return false;
    }

    lv_obj_t *scrollable = focused;
    while (scrollable != NULL) {
        if (lv_obj_has_flag(scrollable, LV_OBJ_FLAG_SCROLLABLE)) {
            const lv_dir_t dir = lv_obj_get_scroll_dir(scrollable);
            if (dir & LV_DIR_VER) {
                const lv_coord_t overflow =
                        lv_obj_get_scroll_top(scrollable) + lv_obj_get_scroll_bottom(scrollable);
                if (overflow > 0) {
                    /* SDL: y > 0 = away (scroll up). LVGL: positive dy scrolls content down. */
                    const lv_coord_t step = LV_MAX(lv_dpx(48), lv_obj_get_height(scrollable) / 6);
                    const lv_coord_t dy = (wheel->y > 0) ? -step : step;
                    lv_obj_scroll_by_bounded(scrollable, 0, dy, LV_ANIM_ON);
                    return true;
                }
            }
        }
        scrollable = lv_obj_get_parent(scrollable);
    }
    return false;
}

int lv_sdl_init_wheel(lv_indev_drv_t *drv, app_ui_input_t *input) {
    lv_indev_drv_init(drv);
    drv->user_data = input;
    drv->type = LV_INDEV_TYPE_ENCODER;
    drv->read_cb = sdl_input_read;

    return 0;
}

static void sdl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    app_ui_input_t *input = drv->user_data;
    SDL_Event e;
    data->state = LV_INDEV_STATE_RELEASED;
    data->enc_diff = 0;
    if (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL)) {
        app_t *app = input->ui->app;
        if (app->session != NULL && !ui_should_block_input()) {
            session_handle_input_event(app->session, &e);
        } else if (wheel_handle_gridview(input, &e.wheel)) {
            /* Page the game grid; do not move group focus or scroll parents. */
        } else if (wheel_handle_scrollable(input, &e.wheel)) {
            /* Scroll settings / host lists instead of changing focus. */
        } else if (e.wheel.y != 0) {
            /* Encoder scroll only: never set PRESSED — that triggers LVGL "encoder button" clicks. */
            data->enc_diff = (e.wheel.y > 0) ? -1 : 1;
        }
    }
    data->continue_reading = false;
}
