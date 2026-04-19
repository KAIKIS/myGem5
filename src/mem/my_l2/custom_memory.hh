#ifndef __MEM_MY_L2_CUSTOM_MEMORY_HH__
#define __MEM_MY_L2_CUSTOM_MEMORY_HH__

#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>

#include "mem/my_l2/my_chi_msg.hh"

// 纯C++内存，不依赖gem5
class CustomMemory
{
  public:
    // 回调函数：通过中间件发送消息回CustomCache
    std::function<void(const MyCHIMsg&)> sendToCache;

    CustomMemory(int latencyCycles = 100);
    ~CustomMemory() = default;

    // 设置gem5物理内存的backing store
    // pmem: 指向gem5 mmap'd host memory的指针
    // rangeStart: 物理地址范围的起始
    // rangeSize: 地址范围大小
    void setBackingStore(uint8_t *pmem, uint64_t rangeStart, uint64_t rangeSize);

    // 接收来自CustomCache的请求
    void handleRequest(const MyCHIMsg &msg);
    void handleData(const MyCHIMsg &msg);

    void printStats() const;

  private:
    int accessLatency;
    uint8_t *pmemAddr = nullptr;
    uint64_t rangeStart = 0;
    uint64_t rangeSize = 0;

    // 获取指定地址的数据指针（从backing store或本地hash map）
    uint8_t* getBlock(MyAddr addr);

    void handleRead(const MyCHIMsg &msg);
    void handleWrite(const MyCHIMsg &msg);

    void sendDataResponse(MyAddr addr, int nodeId, const uint8_t *data,
                          MyCHIDataType type, MyAddr txnId);
    void sendCompResponse(MyAddr addr, int nodeId, MyCHIRespType type,
                          MyAddr txnId);

    // 本地hash map：用于写入新数据（backing store直接写入）
    // 如果有backing store，写入直接写到backing store
};

#endif // __MEM_MY_L2_CUSTOM_MEMORY_HH__
