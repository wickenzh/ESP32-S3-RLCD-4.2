// 手工描绘云层轮廓：固定贝塞尔控制点，编译期生成逐列边界。
#pragma once
#include <array>
#include <cstdint>

namespace aggregate_cloud_paths {
struct Curve { int left,right,start,control1,control2,end; };
struct Shape {
    std::array<uint8_t,222> top{};
    std::array<uint8_t,222> bottom{};
};

// Each span is individually drawn against the concept composition, not repeated.
inline constexpr Curve cloudy_top[]={
    {2,12,52,52,44,39},{12,26,39,44,33,34},
    {26,44,34,39,39,33},{44,62,33,36,31,28},
    {175,188,28,32,39,34},{188,203,34,39,43,39},{203,219,39,43,46,52}};
inline constexpr Curve cloudy_bottom[]={
    {2,17,81,78,90,87},{17,31,87,90,95,101},{31,45,101,102,117,120},
    {183,197,119,109,93,95},{197,207,95,91,88,91},{207,219,91,81,71,75}};
inline constexpr Curve overcast_top[]={
    {2,16,49,51,37,40},{16,31,40,44,34,36},{31,45,36,42,31,33},
    {45,63,33,42,44,37},{63,78,37,42,31,33},{78,97,33,38,34,28},
    {147,167,28,39,33,35},{167,186,35,42,31,37},
    {186,201,37,45,36,41},{201,219,41,40,52,57}};
inline constexpr Curve overcast_bottom[]={
    {2,19,77,75,87,88},{19,36,88,83,98,103},
    {51,66,99,87,90,97},{66,88,97,92,95,99},
    {112,132,99,92,92,96},{132,148,96,91,91,98},
    {148,168,98,100,90,92},{168,185,92,87,91,93},
    {185,202,93,83,89,85},{202,219,85,88,70,73}};

constexpr int sample(const Curve &c,int x) {
    const int w=c.right-c.left,t=x-c.left,s=w-t;
    return (s*s*s*c.start+3*s*s*t*c.control1+3*s*t*t*c.control2+t*t*t*c.end+w*w*w/2)/(w*w*w);
}
template<std::size_t N,std::size_t M>
constexpr Shape make_shape(const Curve (&top)[N],const Curve (&bottom)[M]) {
    Shape result{};
    for(auto &y:result.bottom)y=120;
    for(const auto &c:top)for(int x=c.left;x<=c.right;++x) {
        const auto y=static_cast<uint8_t>(sample(c,x));
        if(y>result.top[x])result.top[x]=y;
    }
    for(const auto &c:bottom)for(int x=c.left;x<=c.right;++x) {
        const auto y=static_cast<uint8_t>(sample(c,x));
        if(y<result.bottom[x])result.bottom[x]=y;
    }
    return result;
}
inline constexpr Shape cloudy=make_shape(cloudy_top,cloudy_bottom);
inline constexpr Shape overcast=make_shape(overcast_top,overcast_bottom);
static_assert(sizeof(Shape)==444,"cloud contour storage must remain bounded");
} // namespace aggregate_cloud_paths
