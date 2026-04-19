#include "mem/my_l2/custom_cache.hh"

#include <cassert>
#include <cstdio>
#include <cstring>

CustomCache::CustomCache(int cacheSizeBytes, int assoc, int lineSize)
    : cacheLineSize(lineSize), numWays(assoc)
{
    numSets = cacheSizeBytes / (assoc * lineSize);
}

int
CustomCache::getSetIndex(MyAddr addr) const
{
    return 0;
}

int
CustomCache::findWay(MyAddr addr, int setIndex) const
{
    auto it = cache.find(addr & ~(MyAddr)(cacheLineSize - 1));
    if (it == cache.end()) return -1;

    for (auto &w : it->second.ways) {
        if (w.second.valid) return w.first;
    }
    return -1;
}

int
CustomCache::findVictim(MyAddr addr, int setIndex)
{
    MyAddr lineAddr = addr & ~(MyAddr)(cacheLineSize - 1);
    auto &set = cache[lineAddr];

    for (int i = 0; i < numWays; i++) {
        auto wit = set.ways.find(i);
        if (wit == set.ways.end() || !wit->second.valid) {
            return i;
        }
    }
    return 0;
}

void
CustomCache::handleRequest(const MyCHIMsg &msg)
{
    if (msg.msgType == MY_MSG_REQUEST) {
        switch (msg.reqType) {
          case MY_REQ_ReadShared:
          case MY_REQ_ReadUnique:
          case MY_REQ_ReadOnce:
          case MY_REQ_ReadNotSharedDirty:
            handleReadRequest(msg);
            break;
          case MY_REQ_WriteBackFull:
          case MY_REQ_WriteCleanFull:
          case MY_REQ_WriteEvictFull:
          case MY_REQ_WriteUniquePtl:
          case MY_REQ_WriteUniqueFull:
            handleWriteRequest(msg);
            break;
          case MY_REQ_Evict:
            handleEvict(msg);
            break;
          case MY_REQ_CleanUnique:
            sendCompResponse(msg.addr, msg.srcNodeId,
                             MY_RESP_Comp_UC, msg.txnId);
            break;
          default:
            printf("[CustomCache] 未处理请求类型: %s addr=%#lx\n",
                   myCHIReqTypeStr(msg.reqType),
                   (unsigned long)msg.addr);
            break;
        }
    } else if (msg.msgType == MY_MSG_RESPONSE) {
        // 来自Memory的响应
        if (msg.respType == MY_RESP_CompDBIDResp ||
            msg.respType == MY_RESP_DBIDResp) {
            auto it = pendingWrites.find(msg.addr);
            if (it != pendingWrites.end() && it->second.hasData) {
                forwardWriteToMemory(msg, it->second.data);
                pendingWrites.erase(it);
            }
        } else if (msg.respType == MY_RESP_Comp ||
                   msg.respType == MY_RESP_Comp_I) {
            pendingWrites.erase(msg.addr);
        }
    }
}

void
CustomCache::handleData(const MyCHIMsg &msg)
{
    if (msg.msgType != MY_MSG_DATA) return;

    MyCHIDataType type = msg.dataType;

    // 来自Memory的读数据 → 转发给L1
    if (type == MY_DATA_CompData_UC || type == MY_DATA_CompData_SC ||
        type == MY_DATA_CompData_UD_PD || type == MY_DATA_CompData_SD_PD ||
        type == MY_DATA_DataSepResp_UC) {

        // 用txnId（msg.addr在此上下文是txnId）查找pending read
        MyAddr txnId = msg.txnId;
        auto tit = txnToReadAddr.find(txnId);
        if (tit == txnToReadAddr.end()) {
            printf("[CustomCache] 收到未知txnId=%#lx的数据\n",
                   (unsigned long)txnId);
            return;
        }

        MyAddr lineAddr = tit->second;
        auto it = pendingReads.find(lineAddr);
        if (it == pendingReads.end()) {
            printf("[CustomCache] txnId=%#lx → lineAddr=%#lx 但无pendingRead!\n",
                   (unsigned long)txnId, (unsigned long)lineAddr);
            return;
        }

        printf("[CustomCache] 匹配pendingRead: txnId=%#lx → addr=%#lx "
               "origSrcNode=%d origTxnId=%#lx\n",
               (unsigned long)txnId, (unsigned long)lineAddr,
               it->second.originalReq.srcNodeId,
               (unsigned long)it->second.originalReq.txnId);

        // 转发给L1
        MyCHIMsg resp{};
        resp.msgType = MY_MSG_DATA;
        resp.direction = MY_DIR_CACHE_TO_L1;
        resp.dataType = (type == MY_DATA_CompData_SC)
                        ? MY_DATA_CompData_SC
                        : MY_DATA_CompData_UC;
        resp.addr = lineAddr;
        resp.txnId = it->second.originalReq.txnId;
        resp.dstNodeId = it->second.originalReq.srcNodeId;
        memcpy(resp.data, msg.data, msg.dataSize);
        resp.dataOffset = msg.dataOffset;
        resp.dataSize = msg.dataSize;
        resp.lastChunk = msg.lastChunk;

        printf("[CustomCache] 转发读数据给L1: addr=%#lx node%d "
               "size=%d lastChunk=%d\n",
               (unsigned long)resp.addr, resp.dstNodeId,
               resp.dataSize, msg.lastChunk);
        sendToGem5(resp);

        // Memory返回的是完整64字节，lastChunk=true时清理pending
        // （中间件会把64字节拆成2个32字节chunk发给L1）
        if (msg.lastChunk) {
            pendingReads.erase(it);
            txnToReadAddr.erase(tit);
        }
        return;
    }

    // 来自L1的写回数据
    if (type == MY_DATA_CBWrData_UC || type == MY_DATA_CBWrData_SC ||
        type == MY_DATA_CBWrData_UD_PD || type == MY_DATA_CBWrData_SD_PD ||
        type == MY_DATA_CBWrData_I) {

        auto wit = pendingWrites.find(msg.addr);
        if (wit != pendingWrites.end()) {
            memcpy(wit->second.data, msg.data, msg.dataSize);
            wit->second.hasData = true;

            forwardWriteToMemory(msg, msg.data);
            pendingWrites.erase(wit);
        }
        return;
    }

    // NCBWrData
    if (type == MY_DATA_NCBWrData) {
        auto wit = pendingWrites.find(msg.addr);
        if (wit != pendingWrites.end()) {
            forwardWriteToMemory(msg, msg.data);
            pendingWrites.erase(wit);
        }
    }
}

void
CustomCache::handleReadRequest(const MyCHIMsg &msg)
{
    MyAddr lineAddr = msg.addr & ~(MyAddr)(cacheLineSize - 1);

    // 检查缓存是否命中
    auto it = cache.find(lineAddr);
    if (it != cache.end()) {
        int way = findWay(lineAddr, 0);
        if (way >= 0) {
            auto &entry = it->second.ways[way];
            if (entry.valid) {
                printf("[CustomCache] 缓存命中! addr=%#lx state=%d srcNode=%d txnId=%#lx\n",
                       (unsigned long)lineAddr, entry.state,
                       msg.srcNodeId, (unsigned long)msg.txnId);

                MyCHIDataType dataType;
                if (entry.state == SHARED) {
                    dataType = MY_DATA_CompData_SC;
                } else {
                    dataType = MY_DATA_CompData_UC;
                }

                sendCompData(lineAddr, msg.srcNodeId,
                             entry.data, dataType, msg.txnId);

                if (msg.reqType == MY_REQ_ReadUnique ||
                    msg.reqType == MY_REQ_CleanUnique) {
                    entry.state = INVALID;
                    entry.valid = false;
                }
                return;
            }
        }
    }

    // 缓存未命中：转发到Memory
    printf("[CustomCache] 缓存未命中，转发到Memory: addr=%#lx srcNode=%d txnId=%#lx\n",
           (unsigned long)msg.addr, msg.srcNodeId, (unsigned long)msg.txnId);

    // 检查是否已有相同txnId的pending read（可能来自另一个L1）
    auto existingTxn = txnToReadAddr.find(msg.txnId);
    if (existingTxn != txnToReadAddr.end()) {
        printf("[CustomCache] WARNING: txnId=%#lx 已映射到 addr=%#lx, "
               "新请求 addr=%#lx 覆盖!\n",
               (unsigned long)msg.txnId,
               (unsigned long)existingTxn->second,
               (unsigned long)lineAddr);
    }

    // 使用新的txnId发给Memory，建立txnId→pendingRead映射
    MyAddr memTxnId = msg.txnId;

    // 保存pending read
    PendingRead pr;
    pr.originalReq = msg;
    pendingReads[lineAddr] = pr;
    txnToReadAddr[memTxnId] = lineAddr;

    forwardReadToMemory(msg);
}

void
CustomCache::handleWriteRequest(const MyCHIMsg &msg)
{
    MyAddr lineAddr = msg.addr & ~(MyAddr)(cacheLineSize - 1);

    printf("[CustomCache] 收到写请求: type=%s addr=%#lx\n",
           myCHIReqTypeStr(msg.reqType),
           (unsigned long)msg.addr);

    if (msg.reqType == MY_REQ_WriteEvictFull) {
        // WriteEvictFull: cache进入BUSY_BLKD，等待CompDBIDResp+CBWrData_I
        // 回复CompDBIDResp让它发CBWrData_I
        printf("[CustomCache] WriteEvictFull回复CompDBIDResp\n");
        sendCompResponse(msg.addr, msg.srcNodeId,
                         MY_RESP_CompDBIDResp, msg.txnId);
        return;
    }

    // 记录pending write
    PendingWrite pw;
    pw.originalReq = msg;
    pw.hasData = false;
    pendingWrites[lineAddr] = pw;

    // 发送CompDBIDResp给L1，等待数据
    sendCompResponse(msg.addr, msg.srcNodeId,
                     MY_RESP_CompDBIDResp, msg.txnId);
}

void
CustomCache::handleEvict(const MyCHIMsg &msg)
{
    MyAddr lineAddr = msg.addr & ~(MyAddr)(cacheLineSize - 1);

    printf("[CustomCache] 收到Evict: addr=%#lx\n",
           (unsigned long)msg.addr);

    auto it = cache.find(lineAddr);
    if (it != cache.end()) {
        for (auto &w : it->second.ways) {
            w.second.valid = false;
            w.second.state = INVALID;
        }
    }

    sendCompResponse(msg.addr, msg.srcNodeId, MY_RESP_Comp, msg.txnId);
}

void
CustomCache::sendCompData(MyAddr addr, int nodeId, const uint8_t *data,
                           MyCHIDataType type, MyAddr txnId)
{
    MyCHIMsg msg{};
    msg.msgType = MY_MSG_DATA;
    msg.direction = MY_DIR_CACHE_TO_L1;
    msg.dataType = type;
    msg.addr = addr;
    msg.txnId = txnId;
    msg.dstNodeId = nodeId;
    memcpy(msg.data, data, cacheLineSize);
    msg.dataOffset = 0;
    msg.dataSize = cacheLineSize;
    msg.lastChunk = true;

    printf("[CustomCache] 发送CompData: %s addr=%#lx → node%d\n",
           myCHIDataTypeStr(type),
           (unsigned long)addr, nodeId);
    sendToGem5(msg);
}

void
CustomCache::sendCompResponse(MyAddr addr, int nodeId,
                               MyCHIRespType type, MyAddr txnId)
{
    MyCHIMsg msg{};
    msg.msgType = MY_MSG_RESPONSE;
    msg.direction = MY_DIR_CACHE_TO_L1;
    msg.respType = type;
    msg.addr = addr;
    msg.txnId = txnId;
    msg.dstNodeId = nodeId;

    printf("[CustomCache] 发送响应: %s addr=%#lx → node%d\n",
           myCHIRespTypeStr(type),
           (unsigned long)addr, nodeId);
    sendToGem5(msg);
}

void
CustomCache::forwardReadToMemory(const MyCHIMsg &msg)
{
    MyCHIMsg memReq{};
    memReq.msgType = MY_MSG_REQUEST;
    memReq.direction = MY_DIR_CACHE_TO_MEM;
    memReq.reqType = MY_REQ_ReadNoSnp;
    memReq.addr = msg.addr;
    memReq.txnId = msg.txnId;
    memReq.srcNodeId = msg.srcNodeId;

    printf("[CustomCache] →Memory ReadNoSnp addr=%#lx txnId=%#lx\n",
           (unsigned long)msg.addr, (unsigned long)msg.txnId);
    sendToMemory(memReq);
}

void
CustomCache::forwardWriteToMemory(const MyCHIMsg &msg, const uint8_t *data)
{
    MyCHIMsg memReq{};
    memReq.msgType = MY_MSG_REQUEST;
    memReq.direction = MY_DIR_CACHE_TO_MEM;
    memReq.reqType = MY_REQ_WriteNoSnp;
    memReq.addr = msg.addr;
    memReq.txnId = msg.txnId;
    memReq.srcNodeId = msg.srcNodeId;

    printf("[CustomCache] →Memory WriteNoSnp addr=%#lx txnId=%#lx\n",
           (unsigned long)msg.addr, (unsigned long)msg.txnId);
    sendToMemory(memReq);

    MyCHIMsg dataMsg{};
    dataMsg.msgType = MY_MSG_DATA;
    dataMsg.direction = MY_DIR_CACHE_TO_MEM;
    dataMsg.dataType = MY_DATA_NCBWrData;
    dataMsg.addr = msg.addr;
    dataMsg.txnId = msg.txnId;
    memcpy(dataMsg.data, data, cacheLineSize);
    dataMsg.dataOffset = 0;
    dataMsg.dataSize = cacheLineSize;
    dataMsg.lastChunk = true;

    printf("[CustomCache] →Memory NCBWrData addr=%#lx\n",
           (unsigned long)msg.addr);
    sendToMemory(dataMsg);
}

void
CustomCache::printStats() const
{
    printf("[CustomCache] 缓存条目数: %zu\n", cache.size());
    printf("[CustomCache] PendingReads: %zu\n", pendingReads.size());
    printf("[CustomCache] PendingWrites: %zu\n", pendingWrites.size());
}
