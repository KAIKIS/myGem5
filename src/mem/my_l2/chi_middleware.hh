#ifndef __MEM_MY_L2_CHI_MIDDLEWARE_HH__
#define __MEM_MY_L2_CHI_MIDDLEWARE_HH__

#include <functional>
#include <memory>
#include <unordered_map>

#include "mem/ruby/protocol/chi/generic/CHIGenericController.hh"
#include "mem/ruby/common/WriteMask.hh"
#include "mem/my_l2/my_chi_msg.hh"
#include "params/CHIMiddleware.hh"

// Forward declarations (no gem5 deps in custom headers)
class CustomCache;
class CustomMemory;

namespace gem5
{

namespace ruby
{

class CHIMiddleware : public CHIGenericController
{
  public:
    PARAMS(CHIMiddleware);
    CHIMiddleware(const Params &p);
    ~CHIMiddleware();

    void wakeup() override;
    void regStats() override;

    // 回调接口：CustomCache/CustomMemory通过这些函数发回gem5消息
    void sendResponseToGem5(const MyCHIMsg &msg);
    void sendDataToGem5(const MyCHIMsg &msg);
    void sendRequestToGem5(const MyCHIMsg &msg);
    void sendWriteDataToGem5(const MyCHIMsg &msg);

  protected:
    bool recvRequestMsg(const CHI::CHIRequestMsg *msg) override;
    bool recvSnoopMsg(const CHI::CHIRequestMsg *msg) override;
    bool recvResponseMsg(const CHI::CHIResponseMsg *msg) override;
    bool recvDataMsg(const CHI::CHIDataMsg *msg) override;

  private:
    std::unique_ptr<CustomCache> customCache;
    std::unique_ptr<CustomMemory> customMemory;

    // MachineID → int 节点ID 映射
    std::unordered_map<int, MachineID> nodeIdToMachine;
    std::unordered_map<MachineID, int> machineToNodeId;
    int nextNodeId = 0;

    int getNodeId(const MachineID &mid);
    const MachineID& getMachineID(int nodeId);

    // gem5 CHI类型 ↔ MyCHI类型 转换
    MyCHIReqType convertReqType(CHI::CHIRequestType type);
    MyCHIRespType convertRespType(CHI::CHIResponseType type);
    MyCHIDataType convertDataType(CHI::CHIDataType type);

    CHI::CHIRequestType convertReqTypeBack(MyCHIReqType type);
    CHI::CHIResponseType convertRespTypeBack(MyCHIRespType type);
    CHI::CHIDataType convertDataTypeBack(MyCHIDataType type);

    // gem5 msg → MyCHIMsg 转换
    void convertRequest(const CHI::CHIRequestMsg *gem5Msg, MyCHIMsg &myMsg);
    void convertResponse(const CHI::CHIResponseMsg *gem5Msg, MyCHIMsg &myMsg);
    void convertData(const CHI::CHIDataMsg *gem5Msg, MyCHIMsg &myMsg);

    // 待发送给gem5的数据分块队列
    struct PendingDataChunk {
        MyCHIMsg msg;       // 完整的MyCHIMsg（含64字节数据）
        int nextOffset;     // 下一个要发送的chunk偏移
    };
    std::vector<PendingDataChunk> pendingDataChunks;
    EventFunctionWrapper sendDataEvent;
    void processPendingDataChunks();
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_MY_L2_CHI_MIDDLEWARE_HH__
