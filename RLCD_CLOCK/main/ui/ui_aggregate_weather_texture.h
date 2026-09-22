// 单色天气装饰：晴天光晕、雨天云层雨线、雪天复用用户雪花位图。
#pragma once
#include "aggregate_snowflake_bits.h"
#include "ui_aggregate_cloud_paths.h"

inline int aggregate_cloud_depth(bool overcast,int x,int y) {
    if(x<2 || x>219 || y<28 || y>118)return 0;
    const auto &shape=overcast?aggregate_cloud_paths::overcast:aggregate_cloud_paths::cloudy;
    if(y<=shape.top[x])return 1+shape.top[x]-y;
    if(y>=shape.bottom[x])return 1+y-shape.bottom[x];
    return 0;
}

inline bool aggregate_cloud_pixel(int kind,int x,int y,int read_right,int weather_width=88,int range_width=208) {
    if(x<2 || x>219 || y<28 || y>=99)return false;
    // Clear margins protect the icon, weather text, temperature and high/low row.
    const int reading[][4]={{10,40,65,79},{8,79,12+weather_width,98},
                            {86,44,read_right+2,88},{7,99,10+range_width,117}};
    int clearance=4;
    for(const auto &r:reading) {
        const int dx=x<r[0]?r[0]-x:x>r[2]?x-r[2]:0;
        const int dy=y<r[1]?r[1]-y:y>r[3]?y-r[3]:0;
        const int distance=dx>dy?dx:dy;
        if(distance<clearance)clearance=distance;
    }
    if(clearance==0)return false;
    const bool overcast=kind==5;
    const int depth=aggregate_cloud_depth(overcast,x,y);
    if(!depth)return false;
    const bool outline=!aggregate_cloud_depth(overcast,x-1,y) || !aggregate_cloud_depth(overcast,x+1,y) ||
                       !aggregate_cloud_depth(overcast,x,y-1) || !aggregate_cloud_depth(overcast,x,y+1);
    // Coordinate-anchored grain avoids a mechanical grid without temporal noise.
    unsigned grain=static_cast<unsigned>(x)*374761393U+static_cast<unsigned>(y)*668265263U;
    grain=(grain^(grain>>13))*1274126177U;
    grain^=grain>>16;
    if(outline)return !overcast || (x+2*y)%5!=0;
    const unsigned density=static_cast<unsigned>((overcast?20:10)+(depth<24?depth:24)*(overcast?6:4)/24);
    return grain%100U < density;
}

inline bool aggregate_weather_texture_pixel(int kind,int x,int y,int cloud_bottom=40,int read_right=0) {
    if(kind==4 || kind==5)return aggregate_cloud_pixel(kind,x,y,read_right);
    if(x<2 || x>219 || y<28 || y>98) return false;
    if(kind==2 || kind==3) {
        static constexpr int widths[]={35,53,29,47,41,17};
        static constexpr int depths[]={5,10,4,8,6,4};
        int start=0,segment=0;
        while(segment<5 && x>=start+widths[segment])start+=widths[segment++];
        const int u=x-start,w=widths[segment];
        // Unequal lobes create a cloud silhouette rather than a repeating sine wave.
        const int allowed=cloud_bottom>30?cloud_bottom-30:0;
        const int depth=depths[segment]<allowed?depths[segment]:allowed;
        const int edge=30+4*depth*u*(w-u)/(w*w);
        if(y==edge || (y<edge && x%3==0 && y%3==0)) return true;
        if(kind==3) {
            static constexpr int origins[][2]={{204,43},{184,57},{202,73},{185,85}};
            for(const auto &origin:origins) {
                // Skip a whole motif if an unusually wide reading occupies its slot.
                if(origin[0]<=read_right && origin[0]+kAggregateSnowflakeSize-1>=88 && origin[1]<=88 && origin[1]+kAggregateSnowflakeSize-1>=44)continue;
                const int px=x-origin[0],py=y-origin[1];
                if(px>=0 && px<kAggregateSnowflakeSize && py>=0 && py<kAggregateSnowflakeSize &&
                   (kAggregateSnowflakeBits[py*2+px/8] & (128U>>(px%8))))return true;
            }
            return false;
        }
        if(x<192 || y<44)return false;
        const int row=(y-44)%16;
        const int col=(x-192+((y-44)/16%2)*6)%12;
        return row<5 && col==4-row;
    }
    if(kind==1) {
        const int dx=222-x,dy=y-28,r2=dx*dx+dy*dy;
        if(r2>=24*24 && r2<=25*25)return true;
        if(r2<28*28 || r2>90*90)return false;
        static constexpr int rays[][2]={{1,0},{4,1},{2,1},{1,1},{1,2},{1,4},{0,1}};
        for(const auto &ray:rays) {
            const int cross=dx*ray[1]-dy*ray[0];
            if(cross>=-1 && cross<=1 && (dx+dy)%5==0)return true;
        }
    }
    return false;
}
