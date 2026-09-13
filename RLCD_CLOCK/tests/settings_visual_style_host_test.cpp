// 验证设置皮肤切换只修改样式，不改变对象几何和控件状态。
#include "ui_settings_visual_style.h"
#include "lvgl_lock_health.h"
#include <cassert>

int main() {
    LvglLockHealth health;
    assert(!health.failed(100,60000));
    assert(!health.failed(60099,60000));
    assert(health.failed(60100,60000));
    assert(!health.failed(90000,60000));
    health.progress();
    assert(!health.failed(500000,60000));
    health.progress();
    assert(!health.failed(UINT32_MAX-100,200));
    assert(!health.failed(50,200));
    assert(health.failed(100,200));
    lv_init();
    static lv_color_t pixels[400*10];
    lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer,pixels,nullptr,400*10);
    lv_disp_drv_t driver; lv_disp_drv_init(&driver);
    driver.hor_res=400; driver.ver_res=300; driver.draw_buf=&buffer;
    driver.flush_cb=[](lv_disp_drv_t *d,const lv_area_t *,lv_color_t *) {lv_disp_flush_ready(d);};
    lv_disp_drv_register(&driver);
    lv_obj_t *label=lv_label_create(lv_scr_act());
    lv_obj_set_pos(label,150,66); lv_obj_set_size(label,111,30);
    lv_label_set_text(label,"settings");
    settings_attach_category_marker(label);
    const lv_font_t *font=lv_obj_get_style_text_font(label,0);
    for(int i=0;i<100;++i) {
        settings_visual_style(label,true,true);
        assert(lv_obj_get_style_border_width(label,0)==1);
        assert(lv_obj_get_style_line_width(label,0)==3);
        assert(lv_obj_get_style_pad_left(label,0)==14);
        assert(lv_obj_get_style_pad_right(label,0)==10);
        settings_visual_style(label,false,true);
        assert(lv_obj_get_style_pad_left(label,0)==10);
        assert(lv_obj_get_style_line_width(label,0)==0);
        assert(lv_obj_get_style_text_color(label,0).full==lv_color_black().full);
        settings_visual_style(label,true,false);
        assert(lv_obj_get_style_radius(label,0)==4);
        assert(lv_obj_get_style_text_color(label,0).full==lv_color_white().full);
        settings_visual_style(label,false,false);
        assert(lv_obj_get_style_border_width(label,0)==1);
        assert(lv_obj_get_style_radius(label,0)==4);
        assert(lv_obj_get_style_line_width(label,0)==0);
        assert(lv_obj_get_style_text_font(label,0)==font);
        lv_obj_update_layout(label);
        assert(lv_obj_get_x(label)==150 && lv_obj_get_y(label)==66);
        assert(lv_obj_get_width(label)==111 && lv_obj_get_height(label)==30);
    }
    assert(lv_mem_test()==LV_RES_OK);
}
