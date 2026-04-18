#ifndef __MEM_MY_L2_MY_L2_CACHE_HH__
#define __MEM_MY_L2_MY_L2_CACHE_HH__

#include <queue>

#include "mem/port.hh"
#include "params/MyL2Cache.hh"
#include "sim/clocked_object.hh"

namespace gem5
{

class MyL2Cache : public ClockedObject
{
  private:
    // ---- 端口定义 ----

    // CPU侧端口：接收来自L1的请求 (ResponsePort/Slave)
    class CpuSidePort : public ResponsePort
    {
      public:
        CpuSidePort(const std::string &name, MyL2Cache *owner)
            : ResponsePort(name, owner), _owner(owner) {}

      protected:
        bool recvTimingReq(PacketPtr pkt) override {
            return _owner->handleTimingReq(pkt);
        }

        Tick recvAtomic(PacketPtr pkt) override {
            return _owner->handleAtomic(pkt);
        }

        void recvFunctional(PacketPtr pkt) override {
            _owner->handleFunctional(pkt);
        }

        void recvRespRetry() override {
            _owner->handleCpuSideRetry();
        }

        AddrRangeList getAddrRanges() const override {
            return _owner->getAddrRanges();
        }

      private:
        MyL2Cache *_owner;
    };

    // 内存侧端口：向下游发请求 (RequestPort/Master)
    class MemSidePort : public RequestPort
    {
      public:
        MemSidePort(const std::string &name, MyL2Cache *owner)
            : RequestPort(name, owner), _owner(owner) {}

      protected:
        bool recvTimingResp(PacketPtr pkt) override {
            return _owner->handleTimingResp(pkt);
        }

        void recvReqRetry() override {
            _owner->handleMemSideRetry();
        }

      private:
        MyL2Cache *_owner;
    };

    CpuSidePort cpuPort;
    MemSidePort memPort;

    // 待发往下游的请求队列（当sendTimingReq返回false时缓存）
    std::queue<PacketPtr> pendingRetryQueue;
    bool needRetry = false;

    // 待发回L1的响应队列
    std::queue<PacketPtr> respQueue;

    // ---- 请求处理 ----
    bool handleTimingReq(PacketPtr pkt);
    bool handleTimingResp(PacketPtr pkt);
    Tick handleAtomic(PacketPtr pkt);
    void handleFunctional(PacketPtr pkt);

    void handleCpuSideRetry();
    void handleMemSideRetry();

    // 辅助
    void sendResponse(PacketPtr pkt);
    bool sendToMem(PacketPtr pkt);

    AddrRangeList getAddrRanges() const;

    // 统计
    void printRequestInfo(PacketPtr pkt, const char *stage) const;

  public:
    PARAMS(MyL2Cache);
    MyL2Cache(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void init() override;
};

} // namespace gem5

#endif // __MEM_MY_L2_MY_L2_CACHE_HH__
