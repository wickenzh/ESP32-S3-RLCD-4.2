// 检测持续无进展的LVGL锁等待；正常空闲不计时，成功取锁即恢复。
#pragma once
#include <atomic>
#include <cstdint>

class LvglLockHealth {
public:
    void progress() { first_.store(0); }
    bool failed(uint32_t now,uint32_t timeout) {
        uint32_t first=first_.load();
        if(first==0) {
            const uint32_t encoded=now+1;
            if(encoded) first_.compare_exchange_strong(first,encoded);
            return false;
        }
        if(static_cast<uint32_t>(now-(first-1))<timeout)return false;
        return first_.compare_exchange_strong(first,0);
    }
private:
    std::atomic<uint32_t> first_{0};
};
