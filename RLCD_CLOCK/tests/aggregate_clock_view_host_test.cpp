// 用真实LVGL验证聚合时钟秒变化只失效秒牌，其他模块缓冲保持不变。
#include "ui_aggregate_clock_view.h"
#include "ui_aggregate_weather_policy.h"
#include "ui_aggregate_weather_texture.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>

static std::vector<lv_area_t> areas;
static void flush(lv_disp_drv_t *driver,const lv_area_t *area,lv_color_t *) {
    areas.push_back(*area); lv_disp_flush_ready(driver);
}
int main() {
    int sun_pixels=0,rain_pixels=0;
    for(int y=0;y<120;++y) for(int x=0;x<222;++x) {
        sun_pixels+=aggregate_weather_texture_pixel(1,x,y);
        rain_pixels+=aggregate_weather_texture_pixel(2,x,y);
        assert(!aggregate_weather_texture_pixel(0,x,y));
        if(y<28 || y>98) assert(!aggregate_weather_texture_pixel(2,x,y));
    }
    assert(sun_pixels>50 && rain_pixels>300);
    for(int x=2;x<192;++x) for(int y=36;y<44;++y)
        assert(!aggregate_weather_texture_pixel(2,x,y,35));
    assert(aggregate_weather_kind("100")==1);
    assert(aggregate_weather_kind("150")==1);
    assert(aggregate_weather_kind(nullptr)==0);
    assert(aggregate_weather_kind("30x")==0);
    assert(aggregate_weather_kind("305")==2);
    assert(aggregate_weather_kind("400")==0);
    assert(aggregate_weather_kind("999")==0);
    lv_init();
    static lv_color_t display_pixels[400*300];
    static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw,display_pixels,nullptr,400*300);
    static lv_disp_drv_t driver; lv_disp_drv_init(&driver);
    driver.hor_res=400;driver.ver_res=300;driver.draw_buf=&draw;driver.flush_cb=flush;
    lv_disp_drv_register(&driver);
    static lv_color_t pixels[3][104*80];
    lv_color_t *buffers[3]={pixels[0],pixels[1],pixels[2]};
    AggregateClockView view;
    aggregate_clock_view_build(lv_scr_act(),view,buffers);
    lv_obj_update_layout(lv_scr_act());
    assert(lv_obj_get_y(view.icon)>174+40);
    assert(lv_obj_get_height(view.icon)>=lv_obj_get_style_text_font(view.icon,0)->line_height);
    for(int day=1;day<=31;++day) {
        char text[3]; std::snprintf(text,sizeof(text),"%d",day);
        aggregate_clock_set_text(view.day,text);
        lv_point_t size;
        lv_txt_get_size(&size,text,lv_obj_get_style_text_font(view.day,0),0,0,400,LV_TEXT_FLAG_NONE);
        assert(size.x<=lv_obj_get_width(view.day));
        assert(size.y<=lv_obj_get_height(view.day));
    }
    assert(aggregate_clock_view_time(view,14,36,0));
    assert(aggregate_clock_weather_theme(view,1));
    lv_refr_now(nullptr);
    std::vector<lv_color_t> hour(pixels[0],pixels[0]+104*80);
    std::vector<lv_color_t> minute(pixels[1],pixels[1]+104*80);
    areas.clear();
    assert(!aggregate_clock_view_time(view,14,36,0));
    lv_refr_now(nullptr); assert(areas.empty());
    assert(aggregate_clock_view_time(view,14,36,1));
    lv_refr_now(nullptr); assert(!areas.empty());
    for(const auto &area:areas) {
        std::fprintf(stderr,"second flush: %d,%d-%d,%d\n",area.x1,area.y1,area.x2,area.y2);
        // LVGL canvas reserves five pixels of transform draw margin.
        assert(area.x1>=267 && area.x2<=380);
        assert(area.y1>=71 && area.y2<=160);
    }
    assert(std::memcmp(hour.data(),pixels[0],sizeof(pixels[0]))==0);
    assert(std::memcmp(minute.data(),pixels[1],sizeof(pixels[1]))==0);
    assert(aggregate_clock_view_time(view,23,59,59));
    assert(aggregate_clock_view_time(view,0,0,0));
    assert(!aggregate_clock_view_time(view,0,0,0));
    areas.clear();
    lv_refr_now(nullptr); areas.clear();
    assert(aggregate_clock_weather_theme(view,2));
    lv_refr_now(nullptr);
    for(const auto &area:areas) {std::fprintf(stderr,"theme flush: %d,%d-%d,%d\n",area.x1,area.y1,area.x2,area.y2);assert(area.y1>=169 && area.x1>=13 && area.x2<=253);}
    areas.clear();
    assert(!aggregate_clock_weather_theme(view,2));
    lv_refr_now(nullptr); assert(areas.empty());
    // Canvas allocation failure must leave recoverable objects, not missing slots.
    lv_obj_clean(lv_scr_act());
    lv_color_t *missing[3]={nullptr,nullptr,nullptr};
    aggregate_clock_view_build(lv_scr_act(),view,missing);
    assert(!aggregate_clock_view_time(view,12,34,56));
    for(int i=0;i<3;++i) {
        assert(view.digits[i]);
        lv_canvas_set_buffer(view.digits[i],buffers[i],104,80,LV_IMG_CF_TRUE_COLOR);
    }
    assert(aggregate_clock_view_time(view,12,34,56));
    // Retain a conservative eight-page object load then add settings controls.
    lv_obj_clean(lv_scr_act());
    for(int page=0;page<8;++page) {
        lv_obj_t *root=lv_obj_create(lv_scr_act());
        for(int item=0;item<60;++item) {
            lv_obj_t *text=lv_label_create(root);
            lv_label_set_text(text,"clock status 12:34:56");
        }
    }
    lv_mem_monitor_t memory; lv_mem_monitor(&memory);
    std::fprintf(stderr,"8-page object pressure: free=%u largest=%u\n",
                 (unsigned)memory.free_size,(unsigned)memory.free_biggest_size);
    assert(memory.free_biggest_size>=32768);
    lv_obj_t *settings=lv_obj_create(lv_scr_act());
    for(int i=0;i<32;++i) assert(lv_obj_create(settings));
    assert(lv_mem_test()==LV_RES_OK);
}
