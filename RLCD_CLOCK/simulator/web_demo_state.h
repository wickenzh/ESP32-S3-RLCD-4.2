// 管理浏览器演示的独立状态，不调用硬件、网络或持久化服务。
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>
#include "work_page_ids.h"
#include "ui_settings_contract.h"

struct WebDemoState {
    enum Scene { Work, Settings, Pages, Order, Info, Diagnostics, Ota, Setup, Alert, Low, Boot };
    Scene scene = Work;
    int page = 0, primary = 0, selection = 0;
    bool secondary = false, offline = false, manual_city = true;
    bool hourly = true, all_day = false, auto_return = true, alarm = true;
    int volume = 80, sound = 0, rotation = 0, weather = 0, conversation = 0;
    double conversation_until = 0;
    bool pomodoro_completed = false;
    std::array<int, kWorkPageCount> order{{0,1,2,3,4,5,6,7}};
    uint8_t enabled = 255;
    bool dirty = true, confirming = false, pending = false;
    double last_input = 0, feedback_until = 0, complete_at = 0, operation_started = 0, return_block = 0;
    double pomodoro_until = 0;
    int progress = 0;
    char feedback[160]{};
    int count() const {
        if (scene == Pages) return kWorkPageCount;
        if (scene == Order) { int n=0; for(int i:order) if(enabled & (1<<i)) ++n; return n; }
        const int counts[] = {kNetworkSettingsSecondaryCount,kSoundSettingsSecondaryCount,kDisplaySettingsSecondaryCount,kSystemSettingsSecondaryCount};
        return counts[primary];
    }
    void message(const char *text, double now) {
        std::snprintf(feedback,sizeof(feedback),"%s",text); feedback_until=now+2500; dirty=true;
    }
    void reset(double now) { *this=WebDemoState{}; last_input=now; }
    void work(double now) { scene=Work; secondary=false; confirming=false; pending=false; last_input=now; dirty=true; }
    void settings(double now) { scene=Settings; primary=selection=0; secondary=false; confirming=false; pending=false; last_input=now; dirty=true; }
    int ordered_page(int index) const {
        for(int i:order) if(enabled & (1<<i)) { if(index--==0) return i; }
        return order[0];
    }
    void next_page() {
        int position=0; for(int i=0;i<kWorkPageCount;++i) if(order[i]==page) position=i;
        for(int i=1;i<=kWorkPageCount;++i) { int next=order[(position+i)%kWorkPageCount]; if(enabled & (1<<next)) {page=next;break;} }
        dirty=true;
    }
    void start_operation(const char *text, double now, double duration=1500) {
        pending=true; operation_started=now; complete_at=now+duration; progress=0; message(text,now);
    }
    void press(int key, bool held, double now) {
        last_input=now;
        if(pomodoro_completed){pomodoro_completed=false;dirty=true;return;}
        if (scene==Alert || scene==Low || scene==Boot) {work(now);return;}
        if (held) {
            if(key!=1) return;
            pending=false; confirming=false;
            if(scene==Work) return;
            if(scene==Pages || scene==Order) {selection=scene==Order?1:0;scene=Settings; secondary=true; message("设置已保存",now);}
            else if(scene!=Settings) {scene=Settings;secondary=true;dirty=true;}
            else if(secondary) {secondary=false;return_block=now+800;dirty=true;}
            else if(now>=return_block) work(now);
            return;
        }
        if(scene==Work) { if(key==0) next_page(); else settings(now); return; }
        if(scene==Info || scene==Setup) return;
        if(scene==Diagnostics) { if(key==0&&!pending) start_operation("正在网络检测...",now,3000); return; }
        if(scene==Ota) {
            if(offline){message("当前处于离线模式",now);return;}
            if(key==0&&!pending) {
                if(!confirming) {confirming=true;message("发现演示更新，BOOT确认",now);}
                else start_operation("正在更新...",now,3000);
            } return;
        }
        if(pending) return;
        if(key==1) {
            confirming=false;
            if(scene==Settings&&!secondary) {primary=(primary+1)%kSettingsPrimaryCount;selection=0;}
            else selection=(selection+1)%count();
            dirty=true;return;
        }
        if(scene==Settings&&!secondary) {secondary=true;selection=0;dirty=true;return;}
        if(scene==Pages) {
            const uint8_t mask=static_cast<uint8_t>(enabled^(1<<selection));
            if(mask==0 || mask==(1<<kWorkPageXiaozhiAI)) {message("至少保留一个非小智页面",now);return;}
            if(offline && (selection==0||selection==2||selection==6||selection==7) && !(enabled&(1<<selection))) {message("当前处于离线模式",now);return;}
            enabled=mask; if(!(enabled&(1<<page)))page=ordered_page(0);
            if(ordered_page(0)==kWorkPageXiaozhiAI) {
                for(int i=1;i<count();++i) {int p=ordered_page(i);if(p!=kWorkPageXiaozhiAI){for(int j=0;j<8;++j)if(order[j]==p){for(int k=0;k<8;++k)if(order[k]==kWorkPageXiaozhiAI){std::swap(order[j],order[k]);break;}break;}break;}}
            }
            message("页面设置已保存",now);return;
        }
        if(scene==Order) {
            int a=ordered_page(selection), b=ordered_page((selection+1)%count());
            if((selection==0&&b==6)||((selection+1)%count()==0&&a==6)){message("小智AI不能设为主页",now);return;}
            int ia=0,ib=0;for(int i=0;i<8;++i){if(order[i]==a)ia=i;if(order[i]==b)ib=i;}
            std::swap(order[ia],order[ib]);message("页面顺序已保存",now);return;
        }
        if(primary==kSettingsPrimaryNetwork) {
            if(selection==kNetworkSettingsWeatherCityItem) {
                if(!manual_city) message("已恢复自动定位",now);
                else if(!confirming){confirming=true;message("再次确认清除",now);}
                else {manual_city=false;confirming=false;message("已恢复自动定位",now);}
            } else if(offline) message("当前处于离线模式",now);
            else start_operation("正在同步...",now);
        } else if(primary==kSettingsPrimarySound) {
            if(selection==0)volume=volume%100+20;
            if(selection==1)sound=(sound+1)%4;
            if(selection==2)hourly=!hourly;
            if(selection==3)all_day=!all_day;
            message("声音设置已保存",now);
        } else if(primary==kSettingsPrimaryDisplay) {
            if(selection==0){scene=Pages;selection=0;}
            else if(selection==1){scene=Order;selection=0;}
            else if(selection==2){auto_return=!auto_return;message("设置已保存",now);}
            else if(selection==3){alarm=false;message("闹钟已关闭",now);}
            else {message("默认图片固定24h",now);}
        } else {
            if(selection==0){
                if(offline&&!confirming){confirming=true;message("再次确认关闭离线模式",now);}
                else {offline=!offline;confirming=false;message(offline?"离线模式已开启":"离线模式已关闭",now);}
            } else if(selection==1){scene=Diagnostics;start_operation("正在网络检测...",now,3000);}
            else if(selection==2){if(!confirming){confirming=true;message("再次按 BOOT 确认",now);}else reset(now);}
            else if(selection==3)scene=Info;
            else {scene=Ota; confirming=false; message(offline?"当前处于离线模式":"BOOT检查更新",now);}
        }
        dirty=true;
    }
    void advance(double now) {
        if(conversation && now>=conversation_until) {
            conversation=conversation==3?0:conversation+1;
            conversation_until=now+(conversation==3?4000:1000);dirty=true;
        }
        if(pending) {
            int next=static_cast<int>(100*(now-operation_started)/(complete_at-operation_started));if(next<0)next=0;if(next>100)next=100;
            if(next!=progress){progress=next;dirty=true;}
            if(now>=complete_at){pending=false;progress=100;confirming=false;message(scene==Ota?"更新完成（模拟）":offline?"网络不可用（模拟）":"操作完成（模拟）",now);}
        }
        if(feedback[0]&&now>=feedback_until&&!pending){feedback[0]=0;confirming=false;dirty=true;}
        if(scene!=Work && !pending && now-last_input>=30000) work(now);
        if(scene==Work && page==6 && auto_return && pomodoro_until==0 && now-last_input>=300000){page=ordered_page(0);dirty=true;}
        if(pomodoro_until>0 && now>=pomodoro_until){pomodoro_until=0;pomodoro_completed=true;page=6;scene=Work;message("专注完成（模拟）",now);}
    }
};
