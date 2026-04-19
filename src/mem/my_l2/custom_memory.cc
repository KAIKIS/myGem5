#include "mem/my_l2/custom_memory.hh"

#include <cstdio>
#include <cstring>

CustomMemory::CustomMemory(int latencyCycles)
    : accessLatency(latencyCycles)
{
}

void
CustomMemory::setBackingStore(uint8_t *pmem, uint64_t start, uint64_t size)
{
    pmemAddr = pmem;
    rangeStart = start;
    rangeSize = size;
    printf("[CustomMemory] 设置backing store: pmem=%p range=[%#lx, %#lx)\n",
           pmem, (unsigned long)start, (unsigned long)(start + size));
}

uint8_t*
CustomMemory::getBlock(MyAddr addr)
{
    MyAddr lineAddr = addr & ~(MyAddr)(64 - 1);

    // 如果有backing store，直接从那里读取
    if (pmemAddr && lineAddr >= rangeStart &&
        lineAddr < rangeStart + rangeSize) {
        return pmemAddr + (lineAddr - rangeStart);
    }

    // 没有backing store，返回零（不应发生）
    printf("[CustomMemory] WARNING: 地址%#lx 不在backing store范围内!\n",
           (unsigned long)lineAddr);
    static uint8_t zeroBlock[64] = {0};
    return zeroBlock;
}

void
CustomMemory::handleRequest(const MyCHIMsg &msg)
{
    if (msg.msgType == MY_MSG_REQUEST) {
        switch (msg.reqType) {
          case MY_REQ_ReadNoSnp:
            handleRead(msg);
            break;
          case MY_REQ_WriteNoSnp:
            // WriteNoSnp请求本身只是通知，实际数据通过NCBWrData传
            printf("[CustomMemory] 收到WriteNoSnp请求: addr=%#lx txnId=%#lx\n",
                   (unsigned long)msg.addr, (unsigned long)msg.txnId);
            break;
          default:
            printf("[CustomMemory] 未处理请求: %s addr=%#lx\n",
                   myCHIReqTypeStr(msg.reqType),
                   (unsigned long)msg.addr);
            break;
        }
    }
}

void
CustomMemory::handleData(const MyCHIMsg &msg)
{
    if (msg.msgType != MY_MSG_DATA) return;

    if (msg.dataType == MY_DATA_NCBWrData) {
        handleWrite(msg);
    }
}

void
CustomMemory::handleRead(const MyCHIMsg &msg)
{
    MyAddr lineAddr = msg.addr & ~(MyAddr)(64 - 1);
    uint8_t *blk = getBlock(lineAddr);

    printf("[CustomMemory] 读请求: addr=%#lx txnId=%#lx\n",
           (unsigned long)msg.addr, (unsigned long)msg.txnId);

    // 返回数据给CustomCache
    sendDataResponse(lineAddr, msg.srcNodeId, blk,
                     MY_DATA_CompData_UC, msg.txnId);
}

void
CustomMemory::handleWrite(const MyCHIMsg &msg)
{
    MyAddr lineAddr = msg.addr & ~(MyAddr)(64 - 1);
    uint8_t *blk = getBlock(lineAddr);

    // 写入数据
    int offset = msg.dataOffset;
    int size = msg.dataSize;
    for (int i = 0; i < size && (offset + i) < 64; i++) {
        blk[offset + i] = msg.data[i];
    }

    printf("[CustomMemory] 写请求: addr=%#lx txnId=%#lx size=%d offset=%d\n",
           (unsigned long)msg.addr, (unsigned long)msg.txnId,
           size, offset);

    // 回复Comp/Comp_I表示写完成
    sendCompResponse(lineAddr, msg.srcNodeId, MY_RESP_Comp_I, msg.txnId);
}

void
CustomMemory::sendDataResponse(MyAddr addr, int nodeId, const uint8_t *data,
                                MyCHIDataType type, MyAddr txnId)
{
    MyCHIMsg msg{};
    msg.msgType = MY_MSG_DATA;
    msg.direction = MY_DIR_MEM_TO_CACHE;
    msg.dataType = type;
    msg.addr = addr;
    msg.txnId = txnId;
    msg.dstNodeId = 0;  // 发给CustomCache
    memcpy(msg.data, data, 64);
    msg.dataOffset = 0;
    msg.dataSize = 64;
    msg.lastChunk = true;

    printf("[CustomMemory] 返回数据: %s addr=%#lx txnId=%#lx\n",
           myCHIDataTypeStr(type),
           (unsigned long)addr, (unsigned long)txnId);
    sendToCache(msg);
}

void
CustomMemory::sendCompResponse(MyAddr addr, int nodeId,
                                MyCHIRespType type, MyAddr txnId)
{
    MyCHIMsg msg{};
    msg.msgType = MY_MSG_RESPONSE;
    msg.direction = MY_DIR_MEM_TO_CACHE;
    msg.respType = type;
    msg.addr = addr;
    msg.txnId = txnId;
    msg.dstNodeId = 0;

    printf("[CustomMemory] 发送响应: %s addr=%#lx txnId=%#lx\n",
           myCHIRespTypeStr(type),
           (unsigned long)addr, (unsigned long)txnId);
    sendToCache(msg);
}

void
CustomMemory::printStats() const
{
    printf("[CustomMemory] backing store: %s\n",
           pmemAddr ? "已设置" : "未设置");
}
