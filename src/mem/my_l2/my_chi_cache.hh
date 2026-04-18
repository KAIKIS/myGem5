#ifndef __MEM_MY_L2_MY_CHI_CACHE_HH__
#define __MEM_MY_L2_MY_CHI_CACHE_HH__

#include <unordered_map>

#include "mem/ruby/protocol/chi/generic/CHIGenericController.hh"
#include "mem/ruby/common/WriteMask.hh"
#include "params/MyCHICache.hh"

namespace gem5
{

namespace ruby
{

class MyCHICache : public CHIGenericController
{
  public:
    PARAMS(MyCHICache);
    MyCHICache(const Params &p);

    void wakeup() override;
    void regStats() override;

  protected:
    bool recvRequestMsg(const CHI::CHIRequestMsg *msg) override;
    bool recvSnoopMsg(const CHI::CHIRequestMsg *msg) override;
    bool recvResponseMsg(const CHI::CHIResponseMsg *msg) override;
    bool recvDataMsg(const CHI::CHIDataMsg *msg) override;

  private:
    // 跟踪待处理的读事务：txnId → original requestor
    struct ReadTxn {
        MachineID originalRequestor;
        Addr addr;
    };
    std::unordered_map<Addr, ReadTxn> pendingReads;

    // 跟踪待处理的写事务：txnId → original requestor
    struct WriteTxn {
        MachineID originalRequestor;
        Addr addr;
        bool dataReceived = false;
    };
    std::unordered_map<Addr, WriteTxn> pendingWrites;

    // 暂存从L1收到的写回数据（等待MemCtrl的CompDBIDResp后转发）
    struct WritebackData {
        MachineID originalRequestor;
        Addr addr;
        CHI::CHIDataType dataType;
        DataBlock dataBlk;
        WriteMask bitMask;
        bool hasData = false;
    };
    std::unordered_map<Addr, WritebackData> writebackBuf;

    // 发送ReadNoSnp到MemoryController
    void sendReadToMemory(Addr addr, const MachineID &origRequestor,
                          Addr txnId);
    // 发送CompData到L1
    void sendDataToL1(Addr addr, const MachineID &dest,
                      const DataBlock &data, Addr txnId,
                      CHI::CHIDataType dataType,
                      const WriteMask &bitMask);
    // 发送WriteNoSnp到MemoryController
    void sendWriteToMemory(Addr addr, Addr txnId);
    // 发送NCBWrData到MemoryController
    void sendWriteDataToMemory(Addr addr, const DataBlock &data,
                               const WriteMask &bitMask, Addr txnId);
    // 发送CompDBIDResp到L1
    void sendCompDBIDResp(Addr addr, const MachineID &dest, Addr txnId);
    // 发送Comp到L1（写完成确认）
    void sendComp(Addr addr, const MachineID &dest, Addr txnId);
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_MY_L2_MY_CHI_CACHE_HH__
