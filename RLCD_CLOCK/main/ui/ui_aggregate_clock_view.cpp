// 绘制聚合时钟四块差异化信息区，数字仅按变化的两位时间局部失效。
#include "ui_aggregate_clock_view.h"
#include "aggregate_sensor_icons.h"
#include "ui_aggregate_weather_texture.h"
#include "dseg_digits.h"
#include <cstring>
#include <initializer_list>

LV_FONT_DECLARE(zh_font_16);
LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_flip_lunar_22);
LV_FONT_DECLARE(aggregate_numeric_20);

namespace {
lv_obj_t *panel(lv_obj_t *root, int x, int y, int w, int h, bool black) {
    lv_obj_t *p = lv_obj_create(root);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p,x,y); lv_obj_set_size(p,w,h);
    lv_obj_set_style_radius(p,6,0);
    lv_obj_set_style_bg_color(p,black ? lv_color_black() : lv_color_white(),0);
    lv_obj_set_style_bg_opa(p,LV_OPA_COVER,0);
    lv_obj_clear_flag(p,LV_OBJ_FLAG_SCROLLABLE);
    return p;
}
lv_obj_t *label(lv_obj_t *root,int x,int y,int w,int h,const char *text,
                const lv_font_t *font=&zh_font_16,bool white=false) {
    lv_obj_t *p=lv_label_create(root);
    lv_obj_set_pos(p,x,y); lv_obj_set_size(p,w,h);
    lv_obj_set_style_text_font(p,font,0);
    lv_obj_set_style_text_color(p,white ? lv_color_white() : lv_color_black(),0);
    lv_label_set_long_mode(p,LV_LABEL_LONG_DOT);
    lv_label_set_text(p,text);
    return p;
}
void embolden(lv_obj_t *obj) {
    // Reuse the lunar subset; a one-pixel overstrike avoids another Chinese font.
    lv_obj_add_event_cb(obj,[](lv_event_t *e) {
        lv_obj_t *label=lv_event_get_target(e);
        lv_area_t area; lv_obj_get_coords(label,&area);
        ++area.x1; ++area.x2;
        lv_draw_label_dsc_t style; lv_draw_label_dsc_init(&style);
        lv_obj_init_draw_label_dsc(label,LV_PART_MAIN,&style);
        lv_draw_label(lv_event_get_draw_ctx(e),&style,&area,lv_label_get_text(label),nullptr);
    },LV_EVENT_DRAW_MAIN,nullptr);
}
enum class Fade { Right, Left, Down, Up };
void stipple(lv_obj_t *root,int x,int y,int w,int h,Fade fade,bool white=false) {
    lv_obj_t *p=panel(root,x,y,w,h,false);
    lv_obj_set_style_bg_opa(p,LV_OPA_TRANSP,0);
    lv_obj_set_style_text_color(p,white?lv_color_white():lv_color_black(),0);
    lv_obj_add_event_cb(p,[](lv_event_t *e) {
        lv_obj_t *obj=lv_event_get_target(e);
        lv_draw_ctx_t *ctx=lv_event_get_draw_ctx(e);
        lv_area_t area; lv_obj_get_coords(obj,&area);
        const auto fade=static_cast<Fade>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
        // Ordered one-bit coverage fades spatially, never by alternating frames.
        static constexpr uint8_t bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
        lv_draw_rect_dsc_t style; lv_draw_rect_dsc_init(&style);
        style.bg_color=lv_obj_get_style_text_color(obj,0);
        const int width=lv_area_get_width(&area),height=lv_area_get_height(&area);
        const bool horizontal=fade==Fade::Right || fade==Fade::Left;
        const int extent=horizontal?width:height;
        const int max_coverage=style.bg_color.full==lv_color_white().full?8:3;
        for(int y=area.y1;y<=area.y2;++y)
            for(int x=area.x1;x<=area.x2;++x) {
                int distance=horizontal?x-area.x1:y-area.y1;
                if(fade==Fade::Left || fade==Fade::Up) distance=extent-1-distance;
                const int coverage=extent>1?max_coverage*(extent-1-distance)/(extent-1):0;
                if(bayer[(y-area.y1)%4][(x-area.x1)%4]>=coverage) continue;
                lv_area_t dot={static_cast<lv_coord_t>(x),static_cast<lv_coord_t>(y),static_cast<lv_coord_t>(x),static_cast<lv_coord_t>(y)};
                lv_draw_rect(ctx,&style,&dot);
            }
    },LV_EVENT_DRAW_MAIN,reinterpret_cast<void *>(static_cast<uintptr_t>(fade)));
}
void sensor_icon(lv_obj_t *root,int x,int y,const uint8_t *bits) {
    lv_obj_t *p=panel(root,x,y,24,24,true);
    lv_obj_set_style_bg_opa(p,LV_OPA_TRANSP,0);
    lv_obj_add_event_cb(p,[](lv_event_t *e) {
        lv_area_t area; lv_obj_get_coords(lv_event_get_target(e),&area);
        const auto *bitmap=static_cast<const uint8_t *>(lv_event_get_user_data(e));
        lv_draw_rect_dsc_t style; lv_draw_rect_dsc_init(&style); style.bg_color=lv_color_white();
        for(int y=0;y<24;++y) for(int x=0;x<24;++x) if(bitmap[y*3+x/8] & (128U>>(x%8))) {
            lv_area_t dot={static_cast<lv_coord_t>(area.x1+x),static_cast<lv_coord_t>(area.y1+y),static_cast<lv_coord_t>(area.x1+x),static_cast<lv_coord_t>(area.y1+y)};
            lv_draw_rect(lv_event_get_draw_ctx(e),&style,&dot);
        }
    },LV_EVENT_DRAW_MAIN,const_cast<uint8_t *>(bits));
}
void draw_pair(lv_obj_t *canvas,int value) {
    lv_img_dsc_t *image=lv_canvas_get_img(canvas);
    for(int y=0;y<kAggregateDigitHeight;++y)
        for(int x=0;x<kAggregateDigitWidth;++x)
            lv_img_buf_set_px_color(image,x,y,lv_color_black());
    if(value>=0 && value<=99) {
        const int digits[2]={value/10,value%10};
        for(int d=0;d<2;++d) {
            const DsegGlyph &g=kDSEG84Glyphs[digits[d]];
            for(int y=0;y<63;++y) for(int x=0;x<52;++x) {
                int sx=x*4/3-g.x_offset;
                int sy=y*4/3-84-g.y_offset;
                if(sx<0 || sy<0 || sx>=g.width || sy>=g.height) continue;
                unsigned bit=sy*g.width+sx;
                if(kDSEG84Bitmaps[g.bitmap_offset+bit/8] & (128U>>(bit%8)))
                    lv_img_buf_set_px_color(image,d*52+x,8+y,lv_color_white());
            }
        }
    }
    lv_obj_invalidate(canvas);
}
}

void aggregate_clock_view_build(lv_obj_t *root,AggregateClockView &v,lv_color_t *const buffers[3]) {
    v={};
    panel(root,18,66,364,100,true);
    for(int i=0;i<3;++i) {
        v.digits[i]=lv_canvas_create(root);
        lv_obj_remove_style_all(v.digits[i]);
        lv_obj_set_pos(v.digits[i],24+i*124,76);
        if(buffers[i]) {
            lv_canvas_set_buffer(v.digits[i],buffers[i],104,80,LV_IMG_CF_TRUE_COLOR);
            draw_pair(v.digits[i],-1);
        }
    }
    // Circle centers follow the visible digit bounds, not the font baseline.
    for(int x : {135,259}) for(int y : {100,124}) {
        lv_obj_t *dot=panel(root,x,y,6,6,false);
        lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);
    }
    // Static texture stays outside the changing glyphs and never needs a timer.
    stipple(root,20,72,10,88,Fade::Right,true);
    stipple(root,375,72,6,88,Fade::Left,true);
    lv_obj_t *weather_panel=panel(root,18,174,222,120,false);
    v.weather_panel=weather_panel;
    lv_obj_add_event_cb(weather_panel,[](lv_event_t *e) {
        const auto &v=*static_cast<AggregateClockView *>(lv_event_get_user_data(e));
        if(v.weather_kind<=0)return;
        lv_area_t a; lv_obj_get_coords(lv_event_get_target(e),&a);
        lv_draw_rect_dsc_t ink; lv_draw_rect_dsc_init(&ink);
        ink.bg_color=lv_color_black();
        // Leave a clean reading zone around the temperature, including long/negative values.
        for(int y=28;y<=98;++y) for(int x=2;x<=219;++x) {
            if(y>=44 && y<=88 && x>=88 && x<=92+v.texture_read_width)continue;
            // Taper the lobes into the reading zone instead of flattening the whole band.
            const int left=88,right=92+v.texture_read_width;
            const int distance=x<left?left-x:x>right?x-right:0;
            const int cloud_bottom=distance>=10?40:35+distance/2;
            if(!aggregate_weather_texture_pixel(v.weather_kind,x,y,cloud_bottom,right))continue;
            const int px=a.x1+x,py=a.y1+y;
            lv_area_t p={(lv_coord_t)px,(lv_coord_t)py,(lv_coord_t)px,(lv_coord_t)py};
            lv_draw_rect(lv_event_get_draw_ctx(e),&ink,&p);
        }
    },LV_EVENT_DRAW_MAIN,&v);
    lv_obj_set_style_border_width(weather_panel,1,0);
    lv_obj_set_style_border_color(weather_panel,lv_color_black(),0);
    panel(root,18,174,222,28,true);
    stipple(root,20,262,218,30,Fade::Up);
    lv_obj_t *date_panel=panel(root,248,174,134,58,false);
    lv_obj_set_style_border_width(date_panel,1,0);
    lv_obj_set_style_border_color(date_panel,lv_color_black(),0);
    stipple(root,252,176,126,8,Fade::Down);
    panel(root,323,185,1,40,true);
    panel(root,248,238,134,56,true);
    stipple(root,372,242,8,48,Fade::Left,true);
    v.city=label(root,26,180,126,20,"等待数据",&zh_font_16,true);
    label(root,160,180,72,20,"今日天气",&zh_font_16,true);
    v.icon=label(root,31,215,48,38,"",&qweather_icons_36);
    v.weather=label(root,28,254,88,17,"--");
    v.temperature=label(root,108,210,128,54,"-- C",&lv_font_montserrat_48);
    v.range=label(root,26,274,208,17,"最高 -- C  最低 -- C",&zh_font_16);
    v.day=label(root,251,176,74,56,"--",&lv_font_montserrat_48);
    lv_obj_set_style_text_align(v.day,LV_TEXT_ALIGN_CENTER,0);
    v.month=label(root,325,178,54,26,"--月",&zh_flip_lunar_22);
    v.lunar=label(root,325,204,54,27,"--",&zh_flip_lunar_22);
    lv_obj_set_style_text_align(v.month,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_align(v.lunar,LV_TEXT_ALIGN_CENTER,0);
    embolden(v.month);
    embolden(v.lunar);
    sensor_icon(root,255,240,aggregate_temperature_bits);
    sensor_icon(root,255,265,aggregate_humidity_bits);
    v.local_temp=label(root,285,241,93,24,"--.- C",&aggregate_numeric_20,true);
    v.humidity=label(root,285,266,93,24,"--%",&aggregate_numeric_20,true);
}

bool aggregate_clock_view_time(AggregateClockView &v,int hour,int minute,int second) {
    bool changed=false;
    const int next[3]={hour,minute,second};
    for(int i=0;i<3;++i) if(v.digits[i] && lv_canvas_get_img(v.digits[i])->data && next[i]!=v.values[i]) {
        draw_pair(v.digits[i],next[i]); v.values[i]=next[i]; changed=true;
    }
    return changed;
}
bool aggregate_clock_set_text(lv_obj_t *label,const char *text) {
    if(!label || !text || std::strcmp(lv_label_get_text(label),text)==0) return false;
    lv_label_set_text(label,text); return true;
}

bool aggregate_clock_weather_theme(AggregateClockView &v,int kind) {
    if(!v.weather_panel || !v.temperature)return false;
    if(kind<0 || kind>3)kind=0;
    lv_point_t size={};
    lv_txt_get_size(&size,lv_label_get_text(v.temperature),
                    lv_obj_get_style_text_font(v.temperature,0),0,0,400,LV_TEXT_FLAG_NONE);
    if(v.weather_kind==kind && (kind==0 || v.texture_read_width==size.x)) {
        v.texture_read_width=size.x;
        return false;
    }
    v.weather_kind=kind;
    v.texture_read_width=size.x;
    lv_obj_invalidate(v.weather_panel);
    return true;
}
