#include "app.h"

#include "pref_obj.h"

#include "ui/settings/settings.controller.h"

#include "util/i18n.h"

#include <lvgl.h>

#include <stdlib.h>
#include <string.h>

typedef struct telemetry_pane_t {
    lv_fragment_t base;
    settings_controller_t *parent;
} telemetry_pane_t;

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container);

static void pane_ctor(lv_fragment_t *self, void *args);

const lv_fragment_class_t settings_pane_telemetry_cls = {
    .constructor_cb = pane_ctor,
    .create_obj_cb = create_obj,
    .instance_size = sizeof(telemetry_pane_t),
};

static void pane_ctor(lv_fragment_t *self, void *args) {
    telemetry_pane_t *pane = (telemetry_pane_t *) self;
    pane->parent = args;
}

static void set_config_string(char **field, const char *value) {
    free(*field);
    *field = strdup(value != NULL ? value : "");
}

static void url_changed_cb(lv_event_t *e) {
    set_config_string(&app_configuration->telemetry_seq_url, lv_textarea_get_text(lv_event_get_target(e)));
}

static void key_changed_cb(lv_event_t *e) {
    set_config_string(&app_configuration->telemetry_seq_key, lv_textarea_get_text(lv_event_get_target(e)));
}

static lv_obj_t *create_text_field(lv_obj_t *parent, const char *placeholder, const char *value,
                                   lv_event_cb_t on_change) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, value != NULL ? value : "");
    lv_obj_add_event_cb(ta, on_change, LV_EVENT_VALUE_CHANGED, NULL);
    return ta;
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *container) {
    (void) self;
    lv_obj_t *view = pref_pane_container(container);
    lv_obj_set_layout(view, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    pref_header(view, locstr("Performance telemetry"));
    pref_desc_label(view, locstr("Sends this session's performance stats to your Seq instance after the stream "
                                 "ends. Nothing is sent while streaming, so it does not affect latency."), false);

    lv_obj_t *enabled = pref_checkbox(view, locstr("Enable telemetry"), &app_configuration->telemetry_enabled, false);
    pref_checkbox_prepare_for_dpad(enabled);

    pref_title_label(view, locstr("Seq server URL"));
    create_text_field(view, "https://seq.example.com", app_configuration->telemetry_seq_url, url_changed_cb);

    pref_title_label(view, locstr("Seq API key"));
    create_text_field(view, locstr("Paste your Seq API key"), app_configuration->telemetry_seq_key, key_changed_cb);

    return view;
}
