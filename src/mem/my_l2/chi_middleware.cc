#include "mem/my_l2/chi_middleware.hh"

#include <algorithm>

#include "base/trace.hh"
#include "debug/MyCHICache.hh"
#include "mem/ruby/protocol/CHI/CHIRequestType.hh"
#include "mem/ruby/protocol/CHI/CHIResponseType.hh"
#include "mem/ruby/protocol/CHI/CHIDataType.hh"
#include "mem/ruby/common/WriteMask.hh"
#include "mem/ruby/system/RubySystem.hh"

#include "mem/my_l2/custom_cache.hh"
#include "mem/my_l2/custom_memory.hh"

namespace gem5
{

namespace ruby
{

using namespace CHI;

CHIMiddleware::CHIMiddleware(const CHIMiddlewareParams &p)
    : CHIGenericController(p),
      sendDataEvent([this]{ processPendingDataChunks(); }, name())
{
    // 创建CustomCache：64KB, 8路, 64字节cache line
    customCache = std::make_unique<CustomCache>(65536, 8, cacheLineSize);

    // 创建CustomMemory：100周期延迟
    customMemory = std::make_unique<CustomMemory>(100);

    // 设置回调：CustomCache → gem5
    customCache->sendToGem5 = [this](const MyCHIMsg &msg) {
        if (msg.msgType == MY_MSG_DATA) {
            sendDataToGem5(msg);
        } else if (msg.msgType == MY_MSG_RESPONSE) {
            sendResponseToGem5(msg);
        } else if (msg.msgType == MY_MSG_REQUEST) {
            sendRequestToGem5(msg);
        }
    };

    // 设置回调：CustomCache → Memory
    // 读请求转发到gem5 MemCtrl（获取真实物理内存数据）
    // 写请求转发到CustomMemory（或也可以转发到gem5）
    customCache->sendToMemory = [this](const MyCHIMsg &msg) {
        if (msg.msgType == MY_MSG_REQUEST) {
            // 所有内存请求都转发到gem5 MemCtrl（获取真实物理内存数据）
            sendRequestToGem5(msg);
        } else if (msg.msgType == MY_MSG_DATA) {
            if (msg.dataType == MY_DATA_NCBWrData) {
                // 写数据：转发到gem5 MemCtrl
                sendWriteDataToGem5(msg);
            } else {
                customMemory->handleData(msg);
            }
        }
    };

    // 设置回调：CustomMemory → CustomCache
    customMemory->sendToCache = [this](const MyCHIMsg &msg) {
        if (msg.msgType == MY_MSG_DATA) {
            customCache->handleData(msg);
        } else if (msg.msgType == MY_MSG_RESPONSE) {
            // Memory发的Comp等响应
            MyCHIMsg resp = msg;
            resp.msgType = MY_MSG_RESPONSE;
            customCache->handleRequest(resp);
        }
    };
}

CHIMiddleware::~CHIMiddleware() {}

void
CHIMiddleware::wakeup()
{
    CHIGenericController::wakeup();
}

// ========================================================================
// recvRequestMsg: gem5 L1请求 → MyCHIMsg → CustomCache
// ========================================================================

bool
CHIMiddleware::recvRequestMsg(const CHI::CHIRequestMsg *msg)
{
    // WriteEvictFull来自I状态的cache，无法回复任何响应
    // 直接忽略，不从队列中删除
    if (msg->gettype() == CHI::CHIRequestType_WriteEvictFull) {
        cprintf("%s: [MW_REQ] 忽略WriteEvictFull addr=%#x src=%d raw_reqid=%d (I状态)\n",
                name(), msg->getaddr(), getNodeId(msg->getrequestor()),
                msg->getrequestor().getNum());
        return true;
    }

    MyCHIMsg myMsg;
    convertRequest(msg, myMsg);

    cprintf("%s: [MW_REQ] gem5=%s → my=%s addr=%#x txnId=%#x src=%d "
            "(raw_reqid=%d)\n",
            name(),
            CHIRequestType_to_string(msg->gettype()).c_str(),
            myCHIReqTypeStr(myMsg.reqType),
            myMsg.addr, myMsg.txnId, myMsg.srcNodeId,
            msg->getrequestor().getNum());

    customCache->handleRequest(myMsg);
    return true;
}

// ========================================================================
// recvSnoopMsg: gem5 snoop → 转发（HomeNode一般不收snoop）
// ========================================================================

bool
CHIMiddleware::recvSnoopMsg(const CHI::CHIRequestMsg *msg)
{
    cprintf("%s: [MW_SNP] type=%s addr=%#x (忽略)\n",
            name(),
            CHIRequestType_to_string(msg->gettype()).c_str(),
            msg->getaddr());
    return true;
}

// ========================================================================
// recvResponseMsg: gem5响应（来自MemCtrl）→ MyCHIMsg → CustomCache
// ========================================================================

bool
CHIMiddleware::recvResponseMsg(const CHI::CHIResponseMsg *msg)
{
    MyCHIMsg myMsg;
    convertResponse(msg, myMsg);

    cprintf("%s: [MW_RSP] gem5=%s → my=%s addr=%#x txnId=%#x\n",
            name(),
            CHIResponseType_to_string(msg->gettype()).c_str(),
            myCHIRespTypeStr(myMsg.respType),
            myMsg.addr, myMsg.txnId);

    // 响应主要是CompDBIDResp等，CustomCache需要处理
    // 这里转成一个特殊方向的消息给CustomCache
    customCache->handleRequest(myMsg);
    return true;
}

// ========================================================================
// recvDataMsg: gem5数据消息 → MyCHIMsg → CustomCache
// ========================================================================

bool
CHIMiddleware::recvDataMsg(const CHI::CHIDataMsg *msg)
{
    MyCHIMsg myMsg;
    convertData(msg, myMsg);

    cprintf("%s: [MW_DAT] gem5=%s → my=%s addr=%#x txnId=%#x\n",
            name(),
            CHIDataType_to_string(msg->gettype()).c_str(),
            myCHIDataTypeStr(myMsg.dataType),
            myMsg.addr, myMsg.txnId);

    customCache->handleData(myMsg);
    return true;
}

// ========================================================================
// sendResponseToGem5: CustomCache → gem5 L1 响应
// ========================================================================

void
CHIMiddleware::sendResponseToGem5(const MyCHIMsg &msg)
{
    auto rsp = std::make_shared<CHIResponseMsg>(
        curTick(), cacheLineSize, m_ruby_system);
    rsp->setaddr(msg.addr);
    rsp->settype(convertRespTypeBack(msg.respType));
    rsp->setresponder(m_machineID);

    NetDest dest(m_ruby_system);
    dest.add(getMachineID(msg.dstNodeId));
    rsp->setDestination(dest);

    rsp->setusesTxnId(true);
    rsp->settxnId(msg.txnId);
    // CompDBIDResp需要设置dbid
    if (msg.respType == MY_RESP_CompDBIDResp ||
        msg.respType == MY_RESP_DBIDResp) {
        rsp->setdbid(msg.txnId);
    }

    cprintf("%s: [MW→gem5] Response %s addr=%#x → node%d txnId=%#x\n",
            name(), myCHIRespTypeStr(msg.respType),
            msg.addr, msg.dstNodeId, msg.txnId);
    sendResponseMsg(rsp);
}

// ========================================================================
// sendDataToGem5: CustomCache → gem5 L1 数据
// ========================================================================

void
CHIMiddleware::sendDataToGem5(const MyCHIMsg &msg)
{
    // 将数据加入队列，按chunk逐个发送（每个cycle发一个chunk）
    PendingDataChunk pdc;
    pdc.msg = msg;
    pdc.nextOffset = 0;
    pendingDataChunks.push_back(pdc);

    // 立即处理第一个chunk
    if (!sendDataEvent.scheduled()) {
        schedule(sendDataEvent, curTick());
    }
}

void
CHIMiddleware::processPendingDataChunks()
{
    int chunkSize = dataChannelSize;  // 32
    bool hasMore = false;

    for (auto it = pendingDataChunks.begin();
         it != pendingDataChunks.end(); ) {
        const MyCHIMsg &msg = it->msg;
        int offset = it->nextOffset;
        int thisChunkSize = std::min(chunkSize, msg.dataSize - offset);

        auto dat = std::make_shared<CHIDataMsg>(
            curTick(), cacheLineSize, m_ruby_system);
        dat->setaddr(msg.addr);
        dat->settype(convertDataTypeBack(msg.dataType));
        dat->setresponder(m_machineID);

        // 拷贝chunk数据到DataBlock
        DataBlock &db = dat->getdataBlk();
        for (int i = 0; i < thisChunkSize; i++) {
            db.setByte(msg.dataOffset + offset + i, msg.data[offset + i]);
        }

        // bitmask只覆盖这个chunk
        WriteMask mask(cacheLineSize);
        mask.setMask(msg.dataOffset + offset, thisChunkSize);
        dat->setbitMask(mask);

        NetDest dest(m_ruby_system);
        dest.add(getMachineID(msg.dstNodeId));
        dat->setDestination(dest);

        dat->setusesTxnId(false);

        cprintf("%s: [MW→gem5] Data %s addr=%#x → node%d "
                "chunk off=%d size=%d bitmask.count=%d tick=%llu\n",
                name(), myCHIDataTypeStr(msg.dataType),
                msg.addr, msg.dstNodeId, offset, thisChunkSize,
                mask.count(), (unsigned long long)curTick());
        sendDataMsg(dat);

        // 推进到下一个chunk
        it->nextOffset += chunkSize;
        if (it->nextOffset >= msg.dataSize) {
            it = pendingDataChunks.erase(it);
        } else {
            ++it;
            hasMore = true;
        }
    }

    // 如果还有chunk待发，调度4个cycle后（与MemCtrl行为匹配）
    if (hasMore) {
        schedule(sendDataEvent, curTick() + cyclesToTicks(Cycles(4)));
    }
}

// ========================================================================
// sendRequestToGem5: CustomCache → gem5 MemCtrl 请求
// ========================================================================

void
CHIMiddleware::sendRequestToGem5(const MyCHIMsg &msg)
{
    auto req = std::make_shared<CHIRequestMsg>(
        curTick(), cacheLineSize, m_ruby_system);
    req->setaddr(msg.addr);
    req->settype(convertReqTypeBack(msg.reqType));
    req->setrequestor(m_machineID);
    req->setallowRetry(true);
    req->setusesTxnId(true);
    req->settxnId(msg.txnId);
    req->setaccAddr(msg.addr);
    req->setaccSize(cacheLineSize);

    // ReadNoSnp需要fwdRequestor
    if (msg.reqType == MY_REQ_ReadNoSnp) {
        req->setfwdRequestor(getMachineID(msg.srcNodeId));
        req->setdataToFwdRequestor(false);
    }

    NetDest dest(m_ruby_system);
    MachineID memId;
    memId.type = MachineType_Memory;
    memId.num = 0;
    dest.add(memId);
    req->setDestination(dest);

    cprintf("%s: [MW→gem5] Request %s addr=%#x txnId=%#x → MemCtrl\n",
            name(), myCHIReqTypeStr(msg.reqType), msg.addr, msg.txnId);
    sendRequestMsg(req);
}

// ========================================================================
// sendWriteDataToGem5: CustomCache → gem5 MemCtrl 写数据 (NCBWrData)
// ========================================================================

void
CHIMiddleware::sendWriteDataToGem5(const MyCHIMsg &msg)
{
    auto dat = std::make_shared<CHIDataMsg>(
        curTick(), cacheLineSize, m_ruby_system);
    dat->setaddr(msg.addr);
    dat->settype(CHIDataType_NCBWrData);
    dat->setresponder(m_machineID);

    DataBlock &db = dat->getdataBlk();
    for (int i = 0; i < msg.dataSize; i++) {
        db.setByte(msg.dataOffset + i, msg.data[i]);
    }

    WriteMask mask(cacheLineSize);
    mask.setMask(msg.dataOffset, msg.dataSize);
    dat->setbitMask(mask);

    NetDest dest(m_ruby_system);
    MachineID memId;
    memId.type = MachineType_Memory;
    memId.num = 0;
    dest.add(memId);
    dat->setDestination(dest);

    dat->setusesTxnId(true);
    dat->settxnId(msg.txnId);

    cprintf("%s: [MW→gem5] NCBWrData addr=%#x txnId=%#x → MemCtrl\n",
            name(), msg.addr, msg.txnId);
    sendDataMsg(dat);
}

// ========================================================================
// MachineID ↔ int 节点ID 映射
// ========================================================================

int
CHIMiddleware::getNodeId(const MachineID &mid)
{
    auto it = machineToNodeId.find(mid);
    if (it != machineToNodeId.end()) {
        return it->second;
    }
    int id = nextNodeId++;
    machineToNodeId[mid] = id;
    nodeIdToMachine[id] = mid;
    return id;
}

const MachineID&
CHIMiddleware::getMachineID(int nodeId)
{
    auto it = nodeIdToMachine.find(nodeId);
    assert(it != nodeIdToMachine.end());
    return it->second;
}

// ========================================================================
// gem5 CHI类型 → MyCHI类型 转换
// ========================================================================

MyCHIReqType
CHIMiddleware::convertReqType(CHI::CHIRequestType type)
{
    switch (type) {
      case CHIRequestType_ReadShared:         return MY_REQ_ReadShared;
      case CHIRequestType_ReadUnique:         return MY_REQ_ReadUnique;
      case CHIRequestType_ReadOnce:           return MY_REQ_ReadOnce;
      case CHIRequestType_ReadNotSharedDirty: return MY_REQ_ReadNotSharedDirty;
      case CHIRequestType_WriteBackFull:      return MY_REQ_WriteBackFull;
      case CHIRequestType_WriteCleanFull:     return MY_REQ_WriteCleanFull;
      case CHIRequestType_WriteEvictFull:     return MY_REQ_WriteEvictFull;
      case CHIRequestType_Evict:              return MY_REQ_Evict;
      case CHIRequestType_CleanUnique:        return MY_REQ_CleanUnique;
      case CHIRequestType_WriteUniquePtl:     return MY_REQ_WriteUniquePtl;
      case CHIRequestType_WriteUniqueFull:    return MY_REQ_WriteUniqueFull;
      case CHIRequestType_ReadNoSnp:          return MY_REQ_ReadNoSnp;
      case CHIRequestType_WriteNoSnp:         return MY_REQ_WriteNoSnp;
      default: panic("Unknown CHI request type");
    }
}

MyCHIRespType
CHIMiddleware::convertRespType(CHI::CHIResponseType type)
{
    switch (type) {
      case CHIResponseType_Comp:          return MY_RESP_Comp;
      case CHIResponseType_Comp_UC:       return MY_RESP_Comp_UC;
      case CHIResponseType_Comp_SC:       return MY_RESP_Comp_SC;
      case CHIResponseType_Comp_UD_PD:    return MY_RESP_Comp_UD_PD;
      case CHIResponseType_CompDBIDResp:  return MY_RESP_CompDBIDResp;
      case CHIResponseType_DBIDResp:      return MY_RESP_DBIDResp;
      case CHIResponseType_ReadReceipt:   return MY_RESP_ReadReceipt;
      case CHIResponseType_RespSepData:   return MY_RESP_RespSepData;
      case CHIResponseType_CompAck:       return MY_RESP_CompAck;
      case CHIResponseType_Comp_I:        return MY_RESP_Comp_I;
      default: panic("Unknown CHI response type");
    }
}

MyCHIDataType
CHIMiddleware::convertDataType(CHI::CHIDataType type)
{
    switch (type) {
      case CHIDataType_CompData_UC:      return MY_DATA_CompData_UC;
      case CHIDataType_CompData_SC:      return MY_DATA_CompData_SC;
      case CHIDataType_CompData_UD_PD:   return MY_DATA_CompData_UD_PD;
      case CHIDataType_CompData_SD_PD:   return MY_DATA_CompData_SD_PD;
      case CHIDataType_CBWrData_UC:      return MY_DATA_CBWrData_UC;
      case CHIDataType_CBWrData_SC:      return MY_DATA_CBWrData_SC;
      case CHIDataType_CBWrData_UD_PD:   return MY_DATA_CBWrData_UD_PD;
      case CHIDataType_CBWrData_SD_PD:   return MY_DATA_CBWrData_SD_PD;
      case CHIDataType_CBWrData_I:       return MY_DATA_CBWrData_I;
      case CHIDataType_NCBWrData:        return MY_DATA_NCBWrData;
      case CHIDataType_DataSepResp_UC:   return MY_DATA_DataSepResp_UC;
      default: panic("Unknown CHI data type");
    }
}

CHI::CHIRequestType
CHIMiddleware::convertReqTypeBack(MyCHIReqType type)
{
    switch (type) {
      case MY_REQ_ReadShared:         return CHIRequestType_ReadShared;
      case MY_REQ_ReadUnique:         return CHIRequestType_ReadUnique;
      case MY_REQ_ReadOnce:           return CHIRequestType_ReadOnce;
      case MY_REQ_ReadNotSharedDirty: return CHIRequestType_ReadNotSharedDirty;
      case MY_REQ_WriteBackFull:      return CHIRequestType_WriteBackFull;
      case MY_REQ_WriteCleanFull:     return CHIRequestType_WriteCleanFull;
      case MY_REQ_WriteEvictFull:     return CHIRequestType_WriteEvictFull;
      case MY_REQ_Evict:              return CHIRequestType_Evict;
      case MY_REQ_CleanUnique:        return CHIRequestType_CleanUnique;
      case MY_REQ_WriteUniquePtl:     return CHIRequestType_WriteUniquePtl;
      case MY_REQ_WriteUniqueFull:    return CHIRequestType_WriteUniqueFull;
      case MY_REQ_ReadNoSnp:          return CHIRequestType_ReadNoSnp;
      case MY_REQ_WriteNoSnp:         return CHIRequestType_WriteNoSnp;
      default: panic("Unknown MyCHI request type");
    }
}

CHI::CHIResponseType
CHIMiddleware::convertRespTypeBack(MyCHIRespType type)
{
    switch (type) {
      case MY_RESP_Comp:          return CHIResponseType_Comp;
      case MY_RESP_Comp_UC:       return CHIResponseType_Comp_UC;
      case MY_RESP_Comp_SC:       return CHIResponseType_Comp_SC;
      case MY_RESP_Comp_UD_PD:    return CHIResponseType_Comp_UD_PD;
      case MY_RESP_CompDBIDResp:  return CHIResponseType_CompDBIDResp;
      case MY_RESP_DBIDResp:      return CHIResponseType_DBIDResp;
      case MY_RESP_ReadReceipt:   return CHIResponseType_ReadReceipt;
      case MY_RESP_RespSepData:   return CHIResponseType_RespSepData;
      case MY_RESP_CompAck:       return CHIResponseType_CompAck;
      case MY_RESP_Comp_I:        return CHIResponseType_Comp_I;
      default: panic("Unknown MyCHI response type");
    }
}

CHI::CHIDataType
CHIMiddleware::convertDataTypeBack(MyCHIDataType type)
{
    switch (type) {
      case MY_DATA_CompData_UC:      return CHIDataType_CompData_UC;
      case MY_DATA_CompData_SC:      return CHIDataType_CompData_SC;
      case MY_DATA_CompData_UD_PD:   return CHIDataType_CompData_UD_PD;
      case MY_DATA_CompData_SD_PD:   return CHIDataType_CompData_SD_PD;
      case MY_DATA_CBWrData_UC:      return CHIDataType_CBWrData_UC;
      case MY_DATA_CBWrData_SC:      return CHIDataType_CBWrData_SC;
      case MY_DATA_CBWrData_UD_PD:   return CHIDataType_CBWrData_UD_PD;
      case MY_DATA_CBWrData_SD_PD:   return CHIDataType_CBWrData_SD_PD;
      case MY_DATA_CBWrData_I:       return CHIDataType_CBWrData_I;
      case MY_DATA_NCBWrData:        return CHIDataType_NCBWrData;
      case MY_DATA_DataSepResp_UC:   return CHIDataType_DataSepResp_UC;
      default: panic("Unknown MyCHI data type");
    }
}

// ========================================================================
// gem5 msg → MyCHIMsg 转换
// ========================================================================

void
CHIMiddleware::convertRequest(const CHI::CHIRequestMsg *gem5Msg,
                              MyCHIMsg &myMsg)
{
    myMsg = MyCHIMsg{};
    myMsg.msgType = MY_MSG_REQUEST;
    myMsg.direction = MY_DIR_L1_TO_CACHE;
    myMsg.reqType = convertReqType(gem5Msg->gettype());
    myMsg.addr = gem5Msg->getaddr();
    myMsg.txnId = gem5Msg->gettxnId();
    myMsg.srcNodeId = getNodeId(gem5Msg->getrequestor());
    myMsg.dstNodeId = 0;  // middleware自己是目标
}

void
CHIMiddleware::convertResponse(const CHI::CHIResponseMsg *gem5Msg,
                               MyCHIMsg &myMsg)
{
    myMsg = MyCHIMsg{};
    myMsg.msgType = MY_MSG_RESPONSE;
    myMsg.direction = MY_DIR_MEM_TO_CACHE;
    myMsg.respType = convertRespType(gem5Msg->gettype());
    myMsg.addr = gem5Msg->getaddr();
    myMsg.txnId = gem5Msg->gettxnId();
    myMsg.srcNodeId = getNodeId(gem5Msg->getresponder());
    myMsg.dstNodeId = 0;
}

void
CHIMiddleware::convertData(const CHI::CHIDataMsg *gem5Msg,
                           MyCHIMsg &myMsg)
{
    myMsg = MyCHIMsg{};
    myMsg.msgType = MY_MSG_DATA;

    MyCHIDataType dataType = convertDataType(gem5Msg->gettype());

    // 判断数据方向
    if (dataType >= MY_DATA_CBWrData_UC &&
        dataType <= MY_DATA_CBWrData_I) {
        myMsg.direction = MY_DIR_L1_TO_CACHE;
    } else if (dataType == MY_DATA_NCBWrData) {
        // NCBWrData可以来自Cache→Mem
        myMsg.direction = MY_DIR_CACHE_TO_MEM;
    } else {
        // CompData等来自MemCtrl方向
        myMsg.direction = MY_DIR_MEM_TO_CACHE;
    }

    myMsg.dataType = dataType;
    myMsg.addr = gem5Msg->getaddr();
    myMsg.txnId = gem5Msg->gettxnId();
    myMsg.srcNodeId = getNodeId(gem5Msg->getresponder());

    // 拷贝数据：DataBlock → uint8_t[64]
    const DataBlock &db = gem5Msg->getdataBlk();
    const WriteMask &mask = gem5Msg->getbitMask();

    int copied = 0;
    int offset = -1;
    for (int i = 0; i < cacheLineSize; i++) {
        if (mask.test(i)) {
            if (offset < 0) offset = i;
            myMsg.data[copied] = db.getByte(i);
            copied++;
        }
    }
    myMsg.dataOffset = (offset >= 0) ? offset : 0;
    myMsg.dataSize = copied;
    // 判断是否是最后一个chunk：offset+size >= cacheLineSize
    myMsg.lastChunk = (offset >= 0) && (offset + copied >= cacheLineSize);
}

void
CHIMiddleware::regStats()
{
    CHIGenericController::regStats();
}

} // namespace ruby
} // namespace gem5
