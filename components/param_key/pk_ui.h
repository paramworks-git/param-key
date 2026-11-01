// Renders bridge packets (S, L, B, M; docs/protocol.md) onto the LVGL widgets
// declared in remote_core.yaml. Board-independent.
#pragma once

#include "esphome/core/defines.h"
#ifdef USE_PARAM_KEY_REMOTE  // param_key: role: remote

#include <string>
#include <vector>

#include "lvgl.h"
#include "pk_protocol.h"

namespace pk {

constexpr size_t LIST_ROWS = 6;

struct Ui {
  lv_obj_t *mode{nullptr};
  lv_obj_t *hotkeys[4]{};
  lv_obj_t *msg_box{nullptr};
  lv_obj_t *msg{nullptr};
  lv_obj_t *status{nullptr};
  lv_obj_t *title{nullptr};
  lv_obj_t *subtitle{nullptr};
  lv_obj_t *bar{nullptr};
  lv_obj_t *value{nullptr};
  lv_obj_t *list{nullptr};
  lv_obj_t *list_title{nullptr};
  lv_obj_t *rows[LIST_ROWS]{};
  uint32_t msg_until{0};

  static const std::string &field(const std::vector<std::string> &f, size_t i) {
    static const std::string EMPTY;
    return i < f.size() ? f[i] : EMPTY;
  }

  static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (hidden)
      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }

  // Returns true if the packet was a screen update.
  bool handle(char type, const std::vector<std::string> &f, uint32_t now) {
    switch (type) {
      case 'S': {
        lv_label_set_text(this->mode, field(f, 0).c_str());
        lv_label_set_text(this->title, field(f, 1).c_str());
        lv_label_set_text(this->subtitle, field(f, 2).c_str());
        int value = atoi(field(f, 3).c_str());
        set_hidden(this->bar, value < 0);
        if (value >= 0)
          lv_bar_set_value(this->bar, value > 100 ? 100 : value, LV_ANIM_OFF);
        lv_label_set_text(this->value, field(f, 4).c_str());
        set_hidden(this->list, true);
        set_hidden(this->status, false);
        return true;
      }
      case 'L': {
        lv_label_set_text(this->mode, field(f, 0).c_str());
        lv_label_set_text(this->list_title, field(f, 1).c_str());
        int cursor = atoi(field(f, 2).c_str());
        for (size_t i = 0; i < LIST_ROWS; i++) {
          size_t idx = 3 + i;
          set_hidden(this->rows[i], idx >= f.size());
          lv_label_set_text(this->rows[i], field(f, idx).c_str());
          if (static_cast<int>(i) == cursor)
            lv_obj_add_state(this->rows[i], LV_STATE_CHECKED);
          else
            lv_obj_clear_state(this->rows[i], LV_STATE_CHECKED);
        }
        set_hidden(this->status, true);
        set_hidden(this->list, false);
        return true;
      }
      case 'B':
        for (size_t i = 0; i < 4; i++)
          lv_label_set_text(this->hotkeys[i], field(f, i).c_str());
        return true;
      case 'M':
        lv_label_set_text(this->msg, field(f, 0).c_str());
        set_hidden(this->msg_box, false);
        this->msg_until = now + 1000u * static_cast<uint32_t>(atoi(field(f, 1).c_str()));
        return true;
      default:
        return false;
    }
  }

  void tick(uint32_t now) {
    if (this->msg_until != 0 && static_cast<int32_t>(now - this->msg_until) >= 0) {
      set_hidden(this->msg_box, true);
      this->msg_until = 0;
    }
  }
};

inline Ui ui;

}  // namespace pk

#endif  // USE_PARAM_KEY_REMOTE
