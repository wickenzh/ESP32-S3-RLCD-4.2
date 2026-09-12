// 验证独立模拟设置导航、保护规则、超时和假服务，不访问设备。
#include "web_demo_state.h"
#include <cassert>
#include <iostream>
int main() {
    WebDemoState s;
    s.press(0,false,1);assert(s.page==1);
    s.press(1,false,2);assert(s.scene==WebDemoState::Settings);
    s.press(1,false,3);assert(s.primary==1);
    s.press(0,false,4);assert(s.secondary);
    s.press(0,false,5);assert(s.volume==100);
    s.press(0,false,6);assert(s.volume==20);
    s.press(1,true,10);assert(!s.secondary);
    s.press(1,true,20);assert(s.scene==WebDemoState::Settings);
    s.press(1,true,900);assert(s.scene==WebDemoState::Work);
    s.scene=WebDemoState::Pages;s.secondary=true;s.enabled=1;s.selection=0;
    s.press(0,false,1000);assert(s.enabled==1);
    s.enabled=65;s.press(0,false,1001);assert(s.enabled==65);
    s.reset(2000);s.settings(2001);s.advance(32002);assert(s.scene==WebDemoState::Work);
    s.settings(40000);s.primary=3;s.secondary=true;s.selection=2;
    s.press(0,false,40001);assert(s.confirming);
    s.press(0,false,40002);assert(s.scene==WebDemoState::Work&&s.enabled==255);
    s.scene=WebDemoState::Diagnostics;s.start_operation("test",50000,3000);
    s.advance(53000);assert(!s.pending&&s.progress==100);
    for(int i=0;i<10000;++i){s.press(i%2,false,54000+i);assert(s.page>=0&&s.page<8);assert(s.enabled!=0);assert(s.ordered_page(0)!=6);}
    std::cout<<"Web demo navigation and state tests passed\n";
}
