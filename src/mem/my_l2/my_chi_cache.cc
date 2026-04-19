#include "mem/my_l2/my_chi_cache.hh"

#include "base/trace.hh"
#include "debug/MyCHICache.hh"
#include "mem/ruby/protocol/CHI/CHIRequestType.hh"
#include "mem/ruby/protocol/CHI/CHIResponseType.hh"
#include "mem/ruby/protocol/CHI/CHIDataType.hh"
#include "mem/ruby/common/WriteMask.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

using namespace CHI;

MyCHICache::MyCHICache(const MyCHICacheParams &p)
    : CHIGenericController(p)
{
}

void
MyCHICache::wakeup()
{
    CHIGenericController::wakeup();
}

// ========================================================================
// recvRequestMsg: 处理来自L1的请求
// ========================================================================

bool
MyCHICache::recvRequestMsg(const CHI::CHIRequestMsg *msg)
{
    CHI::CHIRequestType type = msg->gettype();
    Addr addr = msg->getaddr();
    Addr txnId = msg->gettxnId();
    MachineID requestor = msg->getrequestor();

    std::string type_str = CHIRequestType_to_string(type);
    cprintf("%s: [CHI_REQ] type=%s addr=%#x reqid=%d txnId=%#x\n",
            name(), type_str, addr, requestor.getNum(), txnId);

    switch (type) {
      case CHIRequestType_ReadShared:
      case CHIRequestType_ReadUnique:
      case CHIRequestType_ReadOnce:
      case CHIRequestType_ReadNotSharedDirty: {
        // 读请求：转发到MemoryController获取数据
        ReadTxn txn;
        txn.originalRequestor = requestor;
        txn.addr = addr;
        pendingReads[txnId] = txn;

        sendReadToMemory(addr, requestor, txnId);
        break;
      }

      case CHIRequestType_WriteBackFull:
      case CHIRequestType_WriteCleanFull:
      case CHIRequestType_WriteEvictFull: {
        // 写回请求：先回复CompDBIDResp，等待L1发数据
        WriteTxn txn;
        txn.originalRequestor = requestor;
        txn.addr = addr;
        pendingWrites[txnId] = txn;

        cprintf("%s: *** WRITEBACK type=%s addr=%#x txnId=%#x reqid=%d ***\n",
                name(), type_str, addr, txnId, requestor.getNum());
        sendCompDBIDResp(addr, requestor, txnId);
        break;
      }

      case CHIRequestType_Evict: {
        // Evict：L1丢弃干净缓存行，无需数据传输
        sendComp(addr, requestor, txnId);
        break;
      }

      case CHIRequestType_WriteUniquePtl:
      case CHIRequestType_WriteUniqueFull: {
        WriteTxn txn;
        txn.originalRequestor = requestor;
        txn.addr = addr;
        pendingWrites[txnId] = txn;

        sendCompDBIDResp(addr, requestor, txnId);
        break;
      }

      case CHIRequestType_CleanUnique: {
        // L1需要独占权限，直接回复Comp_UC
        auto rsp = std::make_shared<CHIResponseMsg>(
            curTick(), cacheLineSize, m_ruby_system);
        rsp->setaddr(addr);
        rsp->settype(CHIResponseType_Comp_UC);
        rsp->setresponder(m_machineID);
        NetDest dest(m_ruby_system);
        dest.add(requestor);
        rsp->setDestination(dest);
        rsp->setusesTxnId(true);
        rsp->settxnId(txnId);
        sendResponseMsg(rsp);
        break;
      }

      default:
        cprintf("%s: [CHI_REQ] UNHANDLED type=%s addr=%#x\n",
                name(), type_str, addr);
        break;
    }

    return true;
}

// ========================================================================
// recvSnoopMsg: 处理snoop请求（HomeNode一般不接收snoop）
// ========================================================================

bool
MyCHICache::recvSnoopMsg(const CHI::CHIRequestMsg *msg)
{
    CHI::CHIRequestType type = msg->gettype();
    Addr addr = msg->getaddr();
    std::string type_str = CHIRequestType_to_string(type);
    cprintf("%s: [CHI_SNP] type=%s addr=%#x\n", name(), type_str, addr);
    return true;
}

// ========================================================================
// recvResponseMsg: 处理来自MemoryController的响应
// ========================================================================

bool
MyCHICache::recvResponseMsg(const CHI::CHIResponseMsg *msg)
{
    CHI::CHIResponseType type = msg->gettype();
    Addr addr = msg->getaddr();
    Addr txnId = msg->gettxnId();

    std::string type_str = CHIResponseType_to_string(type);
    cprintf("%s: [CHI_RSP] type=%s addr=%#x txnId=%#x\n",
            name(), type_str, addr, txnId);

    switch (type) {
      case CHIResponseType_CompDBIDResp:
      case CHIResponseType_DBIDResp: {
        // MemCtrl准备好接收写数据
        // 查找是否有暂存的写回数据需要转发
        auto it = writebackBuf.find(txnId);
        if (it != writebackBuf.end() && it->second.hasData) {
            sendWriteToMemory(addr, txnId);
            sendWriteDataToMemory(addr, it->second.dataBlk,
                                  it->second.bitMask, txnId);
            writebackBuf.erase(it);
        }
        break;
      }

      case CHIResponseType_RespSepData:
      case CHIResponseType_ReadReceipt:
        // MemCtrl确认已收到读请求，数据随后到达
        break;

      case CHIResponseType_Comp_I:
      case CHIResponseType_Comp: {
        // 写操作完成
        pendingWrites.erase(txnId);
        break;
      }

      default:
        break;
    }

    return true;
}

// ========================================================================
// recvDataMsg: 处理数据消息（来自MemoryController或L1）
// ========================================================================

bool
MyCHICache::recvDataMsg(const CHI::CHIDataMsg *msg)
{
    CHI::CHIDataType type = msg->gettype();
    Addr addr = msg->getaddr();
    Addr txnId = msg->gettxnId();

    std::string type_str = CHIDataType_to_string(type);
    cprintf("%s: [CHI_DAT] type=%s addr=%#x txnId=%#x\n",
            name(), type_str, addr, txnId);

    switch (type) {
      case CHIDataType_CompData_UC:
      case CHIDataType_CompData_SC:
      case CHIDataType_CompData_UD_PD:
      case CHIDataType_CompData_SD_PD:
      case CHIDataType_DataSepResp_UC: {
        // 从MemCtrl收到读数据，转发给L1
        // 注意：大数据分多个chunk发送（data_channel_size=32 < cacheLineSize=64）
        // 每个chunk都转发给L1，不在这里清理pendingReads
        auto it = pendingReads.find(txnId);
        if (it != pendingReads.end()) {
            CHIDataType respType =
                (type == CHIDataType_CompData_SC ||
                 type == CHIDataType_CompData_SD_PD)
                ? CHIDataType_CompData_SC
                : CHIDataType_CompData_UC;

            sendDataToL1(addr, it->second.originalRequestor,
                         msg->getdataBlk(), txnId, respType,
                         msg->getbitMask());
        }
        break;
      }

      case CHIDataType_CBWrData_UC:
      case CHIDataType_CBWrData_SC:
      case CHIDataType_CBWrData_UD_PD:
      case CHIDataType_CBWrData_SD_PD:
      case CHIDataType_CBWrData_I: {
        // 从L1收到写回数据
        auto wit = pendingWrites.find(txnId);
        if (wit == pendingWrites.end()) {
            break;
        }

        // 暂存写回数据
        WritebackData wbd;
        wbd.originalRequestor = wit->second.originalRequestor;
        wbd.addr = addr;
        wbd.dataType = type;
        wbd.dataBlk = msg->getdataBlk();
        wbd.bitMask = msg->getbitMask();
        wbd.hasData = true;
        writebackBuf[txnId] = wbd;

        // 发WriteNoSnp给MemCtrl（如果还没发过）
        sendWriteToMemory(addr, txnId);
        break;
      }

      case CHIDataType_NCBWrData: {
        // 非缓存写数据
        auto wit = pendingWrites.find(txnId);
        if (wit != pendingWrites.end()) {
            sendWriteToMemory(addr, txnId);
            sendWriteDataToMemory(addr, msg->getdataBlk(),
                                  msg->getbitMask(), txnId);
            pendingWrites.erase(wit);
        }
        break;
      }

      default:
        break;
    }

    return true;
}

// ========================================================================
// 辅助函数
// ========================================================================

void
MyCHICache::sendReadToMemory(Addr addr, const MachineID &origRequestor,
                             Addr txnId)
{
    auto req = std::make_shared<CHIRequestMsg>(
        curTick(), cacheLineSize, m_ruby_system);
    req->setaddr(addr);
    req->settype(CHIRequestType_ReadNoSnp);
    req->setrequestor(m_machineID);
    req->setfwdRequestor(origRequestor);
    req->setdataToFwdRequestor(false);
    req->setallowRetry(true);
    req->setusesTxnId(true);
    req->settxnId(txnId);
    req->setaccAddr(addr);
    req->setaccSize(cacheLineSize);

    NetDest dest(m_ruby_system);
    MachineID memId;
    memId.type = MachineType_Memory;
    memId.num = 0;
    dest.add(memId);
    req->setDestination(dest);

    cprintf("%s: ->MemCtrl ReadNoSnp addr=%#x txnId=%#x\n",
            name(), addr, txnId);
    sendRequestMsg(req);
}

void
MyCHICache::sendDataToL1(Addr addr, const MachineID &dest,
                         const DataBlock &data, Addr txnId,
                         CHIDataType dataType, const WriteMask &bitMask)
{
    auto dat = std::make_shared<CHIDataMsg>(
        curTick(), cacheLineSize, m_ruby_system);
    dat->setaddr(addr);
    dat->settype(dataType);
    dat->setresponder(m_machineID);
    dat->setdataBlk(data);

    NetDest netDest(m_ruby_system);
    netDest.add(dest);
    dat->setDestination(netDest);

    dat->setusesTxnId(false);
    dat->setbitMask(bitMask);

    cprintf("%s: ->L1 %s addr=%#x dest=%d bitmask.count=%d tick=%llu\n",
            name(), CHIDataType_to_string(dataType), addr,
            dest.getNum(), bitMask.count(), (unsigned long long)curTick());
    sendDataMsg(dat);
}

void
MyCHICache::sendWriteToMemory(Addr addr, Addr txnId)
{
    auto req = std::make_shared<CHIRequestMsg>(
        curTick(), cacheLineSize, m_ruby_system);
    req->setaddr(addr);
    req->settype(CHIRequestType_WriteNoSnp);
    req->setrequestor(m_machineID);
    req->setallowRetry(true);
    req->setusesTxnId(true);
    req->settxnId(txnId);
    req->setaccAddr(addr);
    req->setaccSize(cacheLineSize);

    NetDest dest(m_ruby_system);
    MachineID memId;
    memId.type = MachineType_Memory;
    memId.num = 0;
    dest.add(memId);
    req->setDestination(dest);

    cprintf("%s: ->MemCtrl WriteNoSnp addr=%#x txnId=%#x\n",
            name(), addr, txnId);
    sendRequestMsg(req);
}

void
MyCHICache::sendWriteDataToMemory(Addr addr, const DataBlock &data,
                                  const WriteMask &bitMask, Addr txnId)
{
    auto dat = std::make_shared<CHIDataMsg>(
        curTick(), cacheLineSize, m_ruby_system);
    dat->setaddr(addr);
    dat->settype(CHIDataType_NCBWrData);
    dat->setresponder(m_machineID);
    dat->setdataBlk(data);
    dat->setbitMask(bitMask);

    NetDest dest(m_ruby_system);
    MachineID memId;
    memId.type = MachineType_Memory;
    memId.num = 0;
    dest.add(memId);
    dat->setDestination(dest);

    dat->setusesTxnId(true);
    dat->settxnId(txnId);

    cprintf("%s: ->MemCtrl NCBWrData addr=%#x txnId=%#x\n",
            name(), addr, txnId);
    sendDataMsg(dat);
}

void
MyCHICache::sendCompDBIDResp(Addr addr, const MachineID &dest, Addr txnId)
{
    auto rsp = std::make_shared<CHIResponseMsg>(
        curTick(), cacheLineSize, m_ruby_system);
    rsp->setaddr(addr);
    rsp->settype(CHIResponseType_CompDBIDResp);
    rsp->setresponder(m_machineID);

    NetDest netDest(m_ruby_system);
    netDest.add(dest);
    rsp->setDestination(netDest);

    rsp->setusesTxnId(true);
    rsp->settxnId(txnId);
    rsp->setdbid(txnId);

    cprintf("%s: ->L1 CompDBIDResp addr=%#x dest=%d txnId=%#x\n",
            name(), addr, dest.getNum(), txnId);
    sendResponseMsg(rsp);
}

void
MyCHICache::sendComp(Addr addr, const MachineID &dest, Addr txnId)
{
    auto rsp = std::make_shared<CHIResponseMsg>(
        curTick(), cacheLineSize, m_ruby_system);
    rsp->setaddr(addr);
    rsp->settype(CHIResponseType_Comp);
    rsp->setresponder(m_machineID);

    NetDest netDest(m_ruby_system);
    netDest.add(dest);
    rsp->setDestination(netDest);

    rsp->setusesTxnId(true);
    rsp->settxnId(txnId);

    cprintf("%s: ->L1 Comp addr=%#x dest=%d txnId=%#x\n",
            name(), addr, dest.getNum(), txnId);
    sendResponseMsg(rsp);
}

void
MyCHICache::regStats()
{
    CHIGenericController::regStats();
}

} // namespace ruby
} // namespace gem5
