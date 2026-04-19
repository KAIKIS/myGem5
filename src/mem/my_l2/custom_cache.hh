#ifndef __MEM_MY_L2_CUSTOM_CACHE_HH__
#define __MEM_MY_L2_CUSTOM_CACHE_HH__

#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>

#include "mem/my_l2/my_chi_msg.hh"

// 纯C++缓存，不依赖gem5
class CustomCache
{
  public:
    // 回调函数：通过中间件发送消息回gem5或发给CustomMemory
    std::function<void(const MyCHIMsg&)> sendToGem5;    // Response/Data
    std::function<void(const MyCHIMsg&)> sendToMemory;   // Request/Data

    CustomCache(int cacheSizeBytes, int assoc, int lineSize);
    ~CustomCache() = default;

    // 接收来自中间件的消息
    void handleRequest(const MyCHIMsg &msg);
    void handleData(const MyCHIMsg &msg);

    void printStats() const;

  private:
    int cacheLineSize;
    int numSets;
    int numWays;

    enum CacheState {
        INVALID,
        SHARED,
        EXCLUSIVE_CLEAN,
        EXCLUSIVE_DIRTY
    };

    struct CacheEntry {
        uint8_t data[64];
        CacheState state;
        bool valid;
        CacheEntry() : state(INVALID), valid(false) {
            memset(data, 0, sizeof(data));
        }
    };

    // 地址 → (way → entry) — 简化为全相联
    struct CacheSet {
        std::unordered_map<int, CacheEntry> ways;  // way index → entry
    };
    std::unordered_map<MyAddr, CacheSet> cache;

    // 待处理的读事务：用txnId作为key（匹配从Memory返回的数据）
    struct PendingRead {
        MyCHIMsg originalReq;
    };
    std::unordered_map<MyAddr, PendingRead> pendingReads;
    // txnId → lineAddr 映射，用于从Memory数据匹配pendingRead
    std::unordered_map<MyAddr, MyAddr> txnToReadAddr;

    // 待处理的写事务
    struct PendingWrite {
        MyCHIMsg originalReq;
        uint8_t data[64];
        bool hasData;
        PendingWrite() : hasData(false) { memset(data, 0, sizeof(data)); }
    };
    std::unordered_map<MyAddr, PendingWrite> pendingWrites;

    int getSetIndex(MyAddr addr) const;
    int findWay(MyAddr addr, int setIndex) const;
    int findVictim(MyAddr addr, int setIndex);

    void handleReadRequest(const MyCHIMsg &msg);
    void handleWriteRequest(const MyCHIMsg &msg);
    void handleEvict(const MyCHIMsg &msg);

    void sendCompData(MyAddr addr, int nodeId, const uint8_t *data,
                      MyCHIDataType type, MyAddr txnId);
    void sendCompResponse(MyAddr addr, int nodeId, MyCHIRespType type,
                          MyAddr txnId);
    void forwardReadToMemory(const MyCHIMsg &msg);
    void forwardWriteToMemory(const MyCHIMsg &msg, const uint8_t *data);
};

#endif // __MEM_MY_L2_CUSTOM_CACHE_HH__
