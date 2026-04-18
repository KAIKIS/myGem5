#include "mem/my_l2/my_l2_cache.hh"

#include "base/trace.hh"
#include "debug/MyL2Cache.hh"
#include "sim/system.hh"

namespace gem5
{

MyL2Cache::MyL2Cache(const MyL2CacheParams &p)
    : ClockedObject(p),
      cpuPort(name() + ".cpu_side", this),
      memPort(name() + ".mem_side", this)
{
}

Port &
MyL2Cache::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cpu_side")
        return cpuPort;
    else if (if_name == "mem_side")
        return memPort;
    else
        return ClockedObject::getPort(if_name, idx);
}

void
MyL2Cache::init()
{
    if (!cpuPort.isConnected() || !memPort.isConnected())
        fatal("MyL2Cache %s: ports are not connected\n", name());

    cpuPort.sendRangeChange();
}

AddrRangeList
MyL2Cache::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(params().addr_range);
    return ranges;
}

void
MyL2Cache::printRequestInfo(PacketPtr pkt, const char *stage) const
{
    // 获取请求类型字符串
    const char *cmd_str;
    if (pkt->isRead()) {
        if (pkt->isLLSC())
            cmd_str = "ReadLinked";
        else
            cmd_str = "Read";
    } else if (pkt->isWrite()) {
        if (pkt->isLLSC())
            cmd_str = "WriteCond";
        else
            cmd_str = "Write";
    } else if (pkt->isInvalidate()) {
        cmd_str = "Invalidate";
    } else if (pkt->isFlush()) {
        cmd_str = "Flush";
    } else {
        cmd_str = pkt->cmdString().c_str();
    }

    cprintf("%s: [%s] addr=%#x size=%d cmd=%s reqid=%d\n",
            name(), stage, pkt->getAddr(), pkt->getSize(),
            cmd_str, pkt->requestorId());
}

bool
MyL2Cache::handleTimingReq(PacketPtr pkt)
{
    printRequestInfo(pkt, "RECV_FROM_L1");

    // 这是demo：不做任何处理，直接转发到下游memory
    // 如果downstream忙则缓存到队列
    if (!memPort.sendTimingReq(pkt)) {
        DPRINTF(MyL2Cache, "Downstream busy, queuing request for addr=%#x\n",
                pkt->getAddr());
        pendingRetryQueue.push(pkt);
        needRetry = true;
    }

    return true;  // 总是接受来自L1的请求
}

bool
MyL2Cache::handleTimingResp(PacketPtr pkt)
{
    DPRINTF(MyL2Cache, "Response from memory: addr=%#x\n", pkt->getAddr());

    printRequestInfo(pkt, "RESP_FROM_MEM");

    // 将响应发回给L1
    if (!cpuPort.sendTimingResp(pkt)) {
        DPRINTF(MyL2Cache, "CPU side busy, queuing response\n");
        respQueue.push(pkt);
    }

    return true;
}

Tick
MyL2Cache::handleAtomic(PacketPtr pkt)
{
    printRequestInfo(pkt, "ATOMIC");
    return memPort.sendAtomic(pkt);
}

void
MyL2Cache::handleFunctional(PacketPtr pkt)
{
    printRequestInfo(pkt, "FUNCTIONAL");
    memPort.sendFunctional(pkt);
}

void
MyL2Cache::handleCpuSideRetry()
{
    DPRINTF(MyL2Cache, "CPU side retry triggered\n");

    // 尝试发送之前缓存的响应
    while (!respQueue.empty()) {
        PacketPtr pkt = respQueue.front();
        if (cpuPort.sendTimingResp(pkt)) {
            respQueue.pop();
            DPRINTF(MyL2Cache, "Sent queued response for addr=%#x\n",
                    pkt->getAddr());
        } else {
            break;
        }
    }
}

void
MyL2Cache::handleMemSideRetry()
{
    DPRINTF(MyL2Cache, "Memory side retry triggered\n");

    // 尝试发送之前被拒绝的请求
    while (!pendingRetryQueue.empty()) {
        PacketPtr pkt = pendingRetryQueue.front();
        if (memPort.sendTimingReq(pkt)) {
            pendingRetryQueue.pop();
            DPRINTF(MyL2Cache, "Sent queued request for addr=%#x\n",
                    pkt->getAddr());
        } else {
            break;
        }
    }

    if (pendingRetryQueue.empty()) {
        needRetry = false;
    }
}

} // namespace gem5
