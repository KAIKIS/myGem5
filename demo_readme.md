# gem5 自定义 CHI 协议缓存层次结构

## 整体目标

在 gem5 模拟器中实现自定义 CHI 协议缓存层次结构，替换 gem5 默认的 Home Node（目录），并逐步从直接依赖 gem5 CHI 消息结构演进到中间件解耦架构。

## 架构总览

```
CPU → L1 (gem5 CHI PrivateL1MOESICache) → Ruby Network → 自定义组件 → gem5 MemCtrl → 物理内存
```

最终架构分为三层：

1. **gem5 侧**：L1 缓存 + MemoryController，完全使用 gem5 现有实现
2. **中间件层**（CHIMiddleware）：gem5 CHI 消息与自定义消息格式之间的转换适配器
3. **自定义层**（CustomCache）：纯 C++ 实现的缓存逻辑，不依赖 gem5 任何头文件

## 架构演进

```
Phase 1/2:
  gem5 L1 → CHIRequestMsg → MyCHICache（直接处理gem5 CHI消息）→ gem5 MemCtrl

Phase 3 (最终):
  gem5 L1 → CHIRequestMsg → CHIMiddleware（gem5适配层）
                              ↓ MyCHIMsg（自定义格式）
                            CustomCache（纯C++，不依赖gem5）
                              ↓ ReadNoSnp/WriteNoSnp
                            gem5 MemCtrl（读写真实物理内存）
```

## 编译与运行

```bash
# 编译
scons build/ARM/gem5.opt -j$(nproc)

# 运行
./build/ARM/gem5.opt configs/example/arm/chi_my_cache_hierarchy.py
```

输出说明：

| 前缀 | 含义 |
|------|------|
| `[MW_REQ]` | 中间件从 gem5 L1 收到的 CHI 请求 |
| `[MW_RSP]` | 中间件从 gem5 MemCtrl 收到的响应 |
| `[MW_DAT]` | 中间件从 gem5 收到的数据消息 |
| `[CustomCache]` | 自定义缓存的处理日志（命中/未命中/转发） |
| `[MW→gem5]` | 中间件发回 gem5 的消息 |
| `[CustomMemory]` | 自定义内存的读写日志 |

---

## 文件清单与作用

### 构建配置

**[src/mem/my_l2/SConscript](../src/mem/my_l2/SConscript)**

注册所有 SimObject（CHIMiddleware、MyCHICache）并编译对应的 .cc 文件。定义 `MyCHICache` 调试标志。

---

### 第一组：经典 gem5 端口模型的 L2 缓存 demo

这是一组独立的 demo，展示如何用 gem5 经典的 RequestPort/ResponsePort 模型实现自定义 L2 缓存，不涉及 CHI 协议。

**[src/mem/my_l2/MyL2Cache.py](../src/mem/my_l2/MyL2Cache.py)**

SimObject 定义。继承 `ClockedObject`，声明 `cpu_side`（ResponsePort，接收 L1 请求）和 `mem_side`（RequestPort，向内存发请求）两个端口，以及 `latency` 参数。

**[src/mem/my_l2/my_l2_cache.hh](../src/mem/my_l2/my_l2_cache.hh)**

L2 缓存头文件。内嵌两个端口类：`CpuSidePort`（ResponsePort，将 `recvTimingReq`/`recvAtomic`/`recvFunctional` 委托给宿主对象）和 `MemSidePort`（RequestPort，将 `recvTimingResp`/`recvReqRetry` 委托给宿主对象）。维护 `pendingRetryQueue`（待重发的下游请求）和 `respQueue`（待发回 L1 的响应）。

**[src/mem/my_l2/my_l2_cache.cc](../src/mem/my_l2/my_l2_cache.cc)**

L2 缓存实现。核心逻辑非常简单：`handleTimingReq` 接收 L1 的 timing 请求，打印请求信息（地址、大小、类型、requestorId），然后直接转发到 `memPort`。如果下游忙则缓存到 `pendingRetryQueue`。`handleTimingResp` 将内存响应发回 L1。这是一个透传 demo，不做任何缓存逻辑。

**[configs/example/arm/arm_my_l2_demo.py](../configs/example/arm/arm_my_l2_demo.py)**

测试配置。架构：ARM TimingSimpleCPU → gem5 L1I/L1D (32KiB) → L2XBar → MyL2Cache → DDR3_1600。运行 hello binary。

**[tests/arm_sequential_access.cpp](../tests/arm_sequential_access.cpp)**

测试程序。1KB int 数组顺序写入 + 顺序读取 + 求和输出。用于验证缓存对顺序访问的处理。

---

### 第二组：CHI 协议自定义 Home Node

这组文件实现了直接处理 gem5 CHI 协议消息的 Home Node，替换 gem5 的 SimpleDirectory。这是中间件架构的前身。

**[src/mem/my_l2/MyCHICache.py](../src/mem/my_l2/MyCHICache.py)**

SimObject 定义。继承 `CHIGenericController`（gem5 CHI 协议的通用控制器基类），无额外参数。`CHIGenericController` 提供了 4 通道 MessageBuffer（req/snp/rsp/dat 的 In/Out）和 `sendRequestMsg`/`sendResponseMsg`/`sendDataMsg` 等发送方法。

**[src/mem/my_l2/my_chi_cache.hh](../src/mem/my_l2/my_chi_cache.hh)**

头文件。定义三个事务结构：`ReadTxn`（记录原始请求者和地址）、`WriteTxn`（同上）、`WritebackData`（额外记录数据块和 bitmask）。维护三个映射表：`pendingReads`（txnId → ReadTxn）、`pendingWrites`（txnId → WriteTxn）、`writebackBuf`（txnId → WritebackData），用于跟踪未完成的事务。

**[src/mem/my_l2/my_chi_cache.cc](../src/mem/my_l2/my_chi_cache.cc)**

核心实现（~426 行）。直接处理 gem5 的 `CHIRequestMsg`/`CHIResponseMsg`/`CHIDataMsg`：

- **recvRequestMsg**：按请求类型分发 — ReadShared/ReadUnique/ReadOnce → 记录 pendingRead 并转发 ReadNoSnp 到 MemCtrl；WriteBackFull/WriteCleanFull → 发送 CompDBIDResp 等待 L1 发数据；Evict → 直接回复 Comp；CleanUnique → 回复 Comp_UC
- **recvDataMsg**：CompData（来自 MemCtrl）→ 通过 `sendDataToL1` 转发给 L1（按 32 字节分块，使用原始 bitmask）；CBWrData（来自 L1 写回）→ 暂存到 writebackBuf 并发 WriteNoSnp 到 MemCtrl
- **recvResponseMsg**：CompDBIDResp（来自 MemCtrl）→ 检查是否有暂存的写回数据需要转发
- **sendReadToMemory**：构造 ReadNoSnp 消息，设置 `fwdRequestor`（DMT，MemCtrl 直接发数据给 L1）和 `dataToFwdRequestor=false`（DCT，数据先回 Home Node）
- **sendDataToL1**：构造 CompData 消息，使用 `msg->getdataBlk()`（注意：缓存命中时返回的是缓存中的数据而非内存数据）和原始 bitmask
- **sendWriteToMemory** / **sendWriteDataToMemory**：构造 WriteNoSnp + NCBWrData 消息发给 MemCtrl
- **sendCompDBIDResp** / **sendComp**：构造响应消息发给 L1

---

### 第三组：中间件架构（核心改动）

这是将自定义缓存逻辑与 gem5 完全解耦的关键层。

**[src/mem/my_l2/my_chi_msg.hh](../src/mem/my_l2/my_chi_msg.hh)**

**自定义消息格式**，完全不依赖 gem5。这是解耦的基础。定义：

- `MyCHIMsgType`：`MY_MSG_REQUEST` / `MY_MSG_RESPONSE` / `MY_MSG_DATA`
- `MyCHIReqType`：13 种请求类型（ReadShared、ReadUnique、WriteBackFull、ReadNoSnp、WriteNoSnp 等）
- `MyCHIRespType`：10 种响应类型（Comp、Comp_UC、Comp_SC、CompDBIDResp、ReadReceipt 等）
- `MyCHIDataType`：11 种数据类型（CompData_UC/SC/UD_PD/SD_PD、CBWrData_UC/SC/UD_PD/SD_PD/I、NCBWrData、DataSepResp_UC）
- `MyCHIDirection`：`MY_DIR_L1_TO_CACHE` / `MY_DIR_CACHE_TO_L1` / `MY_DIR_CACHE_TO_MEM` / `MY_DIR_MEM_TO_CACHE`
- `MyAddr = uint64_t`，`MY_CACHE_LINE_SIZE = 64`
- `MyCHIMsg` 结构体：包含 msgType、reqType/respType/dataType（联合语义）、addr、txnId、srcNodeId/dstNodeId、data[64]、dataOffset、dataSize、lastChunk

**[src/mem/my_l2/CHIMiddleware.py](../src/mem/my_l2/CHIMiddleware.py)**

SimObject 定义。继承 `CHIGenericController`，与 MyCHICache.py 结构相同但指向不同的 C++ 类。无额外参数。

**[src/mem/my_l2/chi_middleware.hh](../src/mem/my_l2/chi_middleware.hh)**

中间件头文件。声明：

- `sendResponseToGem5` / `sendDataToGem5` / `sendRequestToGem5` / `sendWriteDataToGem5`：自定义消息 → gem5 消息的发送接口
- `recvRequestMsg` / `recvSnoopMsg` / `recvResponseMsg` / `recvDataMsg`：gem5 消息 → 自定义消息的接收回调
- `nodeIdToMachine` / `machineToNodeId`：`MachineID ↔ int` 双向映射表（gem5 的 MachineID 是 {type, num} 结构体，自定义层用简单的 int 节点 ID）
- `convertReqType` / `convertRespType` / `convertDataType` 及其反向函数：gem5 CHI 枚举 ↔ MyCHI 枚举的一一映射
- `PendingDataChunk` 结构体 + `pendingDataChunks` 队列 + `sendDataEvent`：用于按 32 字节分块、4 cycle 间隔发送数据（gem5 的 `data_channel_size=32` 限制了单次数据传输大小）

**[src/mem/my_l2/chi_middleware.cc](../src/mem/my_l2/chi_middleware.cc)**

**中间件核心实现**（~570 行）。职责是 gem5 CHI 消息 ↔ MyCHIMsg 的双向转换。

**构造函数**：创建 `CustomCache`（64KB, 8路, 64字节行）和 `CustomMemory`，设置三个回调：

- `sendToGem5`：CustomCache 发数据/响应/请求给 L1 或 MemCtrl
- `sendToMemory`：CustomCache 发请求/数据给内存。**关键设计**：ReadNoSnp 走 `sendRequestToGem5`（转发到 gem5 MemCtrl 读取真实物理内存数据），WriteNoSnp 走 `sendRequestToGem5`，NCBWrData 走 `sendWriteDataToGem5`
- `sendToCache`：CustomMemory 发数据/响应回 CustomCache

**recvRequestMsg**：收到 gem5 CHIRequestMsg → 忽略 WriteEvictFull（来自 I 状态 cache，无法合法回复）→ `convertRequest` 转成 MyCHIMsg → `customCache->handleRequest(myMsg)`

**recvResponseMsg**：收到 gem5 CHIResponseMsg → `convertResponse` 转成 MyCHIMsg → `customCache->handleRequest(myMsg)`

**recvDataMsg**：收到 gem5 CHIDataMsg → `convertData` 转成 MyCHIMsg（从 DataBlock 按 bitmask 拷贝有效字节到 data[]，计算 dataOffset/dataSize，根据 offset+size 是否 >= 64 判断 lastChunk）→ `customCache->handleData(myMsg)`

**sendResponseToGem5**：MyCHIMsg → CHIResponseMsg，设置 responder、destination（通过 `getMachineID(dstNodeId)` 查回 MachineID）、txnId、dbid

**sendDataToGem5**：MyCHIMsg → 入队 PendingDataChunks → `processPendingDataChunks` 按 32 字节分块发送 CHIDataMsg，每个 chunk 设置 WriteMask（`setMask(offset, size)`），chunk 间间隔 4 cycle

**sendRequestToGem5**：MyCHIMsg → CHIRequestMsg，发到 MemoryController。ReadNoSnp 设置 `fwdRequestor`（DMT，MemCtrl 将数据直接发给 L1）

**sendWriteDataToGem5**：MyCHIMsg → CHIDataMsg (NCBWrData)，目的地设为 Memory，用于将写数据注入 gem5 MemCtrl

**convertData**：gem5 CHIDataMsg → MyCHIMsg。遍历 cacheLineSize（64 字节），用 `mask.test(i)` 检查哪些字节有效，将有效字节拷贝到 `myMsg.data[]`，记录起始 offset 和连续长度。判断方向：CBWrData 系列 → L1_TO_CACHE，NCBWrData → CACHE_TO_MEM，CompData 系列 → MEM_TO_CACHE。`lastChunk = (offset + copied >= cacheLineSize)`

**MachineID 映射**：`getNodeId(MachineID)` 将 gem5 的 MachineID 映射为从 0 开始递增的 int，首次见到新 MachineID 时分配新 ID。`getMachineID(int)` 反向查找。

---

### 第四组：纯 C++ 自定义缓存和内存

**[src/mem/my_l2/custom_cache.hh](../src/mem/my_l2/custom_cache.hh)**

头文件。定义：

- `CacheEntry`：valid、state（INVALID/SHARED/EXCLUSIVE/MODIFIED）、data[64]
- `PendingRead`：记录 originalReq（MyCHIMsg）
- `PendingWrite`：记录 originalReq 和 hasData/data[64]
- 回调：`sendToGem5` 和 `sendToMemory`（`std::function<void(const MyCHIMsg&)>`）
- 数据结构：`cache`（`unordered_map<MyAddr, CacheSet>`，CacheSet 包含 `map<int, CacheEntry>` 按 way 索引）、`pendingReads`（lineAddr → PendingRead）、`pendingWrites`（lineAddr → PendingWrite）、`txnToReadAddr`（txnId → lineAddr，用于匹配内存返回数据与原始请求）

**[src/mem/my_l2/custom_cache.cc](../src/mem/my_l2/custom_cache.cc)**

**纯 C++ 缓存实现**（~370 行）。不 include 任何 gem5 头文件。

- **handleRequest**：按 msgType 分发。REQUEST → 按 reqType 分发到 handleReadRequest/handleWriteRequest/handleEvict/CleanUnique 处理。RESPONSE → 处理来自内存的 CompDBIDResp（检查是否有暂存写回数据需要转发）和 Comp/Comp_I（清理 pendingWrites）
- **handleReadRequest**：检查缓存（`cache[lineAddr]` 中查找 valid 的 way）→ 命中则通过 `sendCompData` 直接返回数据（Shared 状态返回 CompData_UC，否则 CompData_SC）→ ReadUnique/ReadOnce 命中后使缓存行无效。未命中则记录 pendingRead + txnToReadAddr 映射，调用 `forwardReadToMemory` 发送 ReadNoSnp
- **handleWriteRequest**：WriteEvictFull 直接回复 CompDBIDResp（让 L1 发 CBWrData_I）。其他写请求记录 pendingWrite，回复 CompDBIDResp 等待数据
- **handleData**：CompData 系列（来自内存）→ 用 txnId 查 txnToReadAddr 找到 pendingRead → 转发数据给 L1（设置 dstNodeId = originalReq.srcNodeId）→ lastChunk 时清理。CBWrData 系列（来自 L1 写回）→ 拷贝数据到 pendingWrite → forwardWriteToMemory。NCBWrData → forwardWriteToMemory
- **handleEvict**：清除缓存行，回复 Comp
- **sendCompData** / **sendCompResponse**：构造 MyCHIMsg，通过 `sendToGem5` 回调发送
- **forwardReadToMemory**：构造 ReadNoSnp，通过 `sendToMemory` 回调发送
- **forwardWriteToMemory**：构造 WriteNoSnp + NCBWrData，通过 `sendToMemory` 回调发送

**[src/mem/my_l2/custom_memory.hh](../src/mem/my_l2/custom_memory.hh)**

头文件。声明 `setBackingStore(uint8_t *pmem, uint64_t rangeStart, uint64_t rangeSize)` 用于接收 gem5 物理内存的 mmap 指针。回调 `sendToCache`。

**[src/mem/my_l2/custom_memory.cc](../src/mem/my_l2/custom_memory.cc)**

自定义内存实现。`setBackingStore` 保存 gem5 物理内存指针和地址范围。`getBlock(addr)` 如果有 backing store 则直接返回 `pmemAddr + (lineAddr - rangeStart)`，否则返回全零（warning）。`handleRead` 从 backing store 读取 64 字节，通过 `sendDataResponse` 返回 CompData_UC。`handleWrite` 写入 backing store，返回 Comp_I。

---

### 第五组：测试配置

**[configs/example/arm/chi_my_cache_hierarchy.py](../configs/example/arm/chi_my_cache_hierarchy.py)**

完整的 CHI 缓存层次配置脚本（~287 行）。自定义 `MyCHICacheHierarchy(AbstractRubyCacheHierarchy)`：

- `incorporate_cache`：创建 RubySystem + SimplePt2Pt 网络（4 虚拟网络）→ 创建 L1 集群（每个 core 一对 PrivateL1MOESICache: icache + dcache，各自带 RubySequencer）→ 创建 CHIMiddleware 替代 Home Node → 创建 MemoryController → 设置 downstream\_destinations → 连接 Ruby 网络
- `_create_my_chi_cache`：创建 CHIMiddleware，手动配置 8 个 MessageBuffer（req/snp/rsp/dat × In/Out），连接到网络端口
- `_create_core_cluster`：创建 icache/dcache 集群，连接 sequencer、walker ports、interrupt
- 主程序：ARM Timing CPU (1 核)、64KiB L1 8 路、DDR3\_1600 32MiB、运行 hello binary

---

### 对已有文件的修改

**[src/mem/my_l2/my_chi_cache.cc](../src/mem/my_l2/my_chi_cache.cc)**

两处改动：

1. `recvRequestMsg` 的 WriteBackFull 分支：添加调试打印，输出 WRITEBACK 类型、地址、txnId、reqid
2. `sendDataToL1` 的调试打印：增加 `bitmask.count()` 和 `curTick()` 输出，用于排查数据分块发送的时序问题

---

## 文件关系

```
┌─────────────────────────────────────────────────────────────┐
│ configs/example/arm/chi_my_cache_hierarchy.py               │
│  ├─ 创建 L1: PrivateL1MOESICache (gem5自带)                 │
│  ├─ 创建 CHIMiddleware (通过 CHIMiddleware.py 注册)          │
│  └─ 创建 MemoryController (gem5自带)                        │
└─────────────────────────────────────────────────────────────┘
            │
            │ Python层创建SimObject，C++层对应：
            ▼
┌─────────────────────────────────────────────────────────────┐
│ chi_middleware.cc / .hh                                      │
│  (CHIMiddleware，继承 CHIGenericController，gem5侧)          │
│                                                              │
│  内部持有两个纯C++对象：                                      │
│  ├─ custom_cache.cc / .hh  ←── CustomCache (无gem5依赖)      │
│  └─ custom_memory.cc / .hh ←── CustomMemory (无gem5依赖)     │
│                                                              │
│  两个纯C++对象通过 std::function 回调反向调用CHIMiddleware：   │
│  ├─ CustomCache.sendToGem5   → CHIMiddleware的发送函数        │
│  ├─ CustomCache.sendToMemory → CHIMiddleware的路由函数        │
│  └─ CustomMemory.sendToCache → CustomCache.handleData/       │
│                                  CustomCache.handleRequest    │
└─────────────────────────────────────────────────────────────┘
            │
            │ 消息格式转换依赖：
            ▼
┌─────────────────────────────────────────────────────────────┐
│ my_chi_msg.hh                                               │
│  (MyCHIMsg，自定义消息格式，被CustomCache和CustomMemory使用)   │
└─────────────────────────────────────────────────────────────┘
```

**依赖方向（include）：**

```
chi_middleware.cc ──→ chi_middleware.hh ──→ my_chi_msg.hh
                     ↘ (include)           (被所有自定义层引用)
                       custom_cache.hh  ──→ my_chi_msg.hh
                       custom_memory.hh ──→ my_chi_msg.hh

chi_middleware.hh 还依赖 gem5 头文件：
  → CHIGenericController.hh (基类)
  → WriteMask.hh (数据块掩码)
  → params/CHIMiddleware.hh (SimObject参数)

custom_cache.hh 和 custom_memory.hh 不依赖任何 gem5 头文件
```

---

## 数据流向

### 流向1：读请求（L1 Cache Miss → 获取数据）

```
① CPU 发出 Load 指令
   │
   ▼
② L1 dcache (PrivateL1MOESICache, gem5 SLICC状态机)
   状态 I + Load 事件 → 发送 CHIRequestMsg (ReadShared)
   │
   ▼
③ Ruby Network (MessageBuffer → SimplePt2Pt网络)
   │
   ▼
④ CHIMiddleware::recvRequestMsg()
   gem5 CHIRequestMsg → MyCHIMsg 转换:
     convertRequest(msg, myMsg)
       msg->gettype()       → convertReqType()    → myMsg.reqType
       msg->getaddr()       → myMsg.addr
       msg->gettxnId()      → myMsg.txnId
       msg->getrequestor()  → getNodeId()         → myMsg.srcNodeId  (MachineID→int映射)
   │
   ▼
⑤ CustomCache::handleRequest(MyCHIMsg)
   检查缓存 cache[lineAddr]:
     ┌─ 命中 ─→ sendCompData() → sendToGem5(CompData)
     │                                         │
     │                                         ▼ 跳转到 ⑧
     │
     └─ 未命中 ─→ 记录 pendingReads[lineAddr] + txnToReadAddr[txnId]
                  forwardReadToMemory(ReadNoSnp)
                    sendToMemory(ReadNoSnp)
                      │
                      ▼
   ┌─────────────────────────────────────────────
   │ sendToMemory 回调中的路由逻辑:
   │   if ReadNoSnp → sendRequestToGem5()  ← 走gem5真实内存
   │   if WriteNoSnp → sendRequestToGem5()
   │   if NCBWrData → sendWriteDataToGem5()
   └─────────────────────────────────────────────
                      │
                      ▼
⑥ CHIMiddleware::sendRequestToGem5()
   MyCHIMsg → CHIRequestMsg (ReadNoSnp)
     设置 requestor=m_machineID (中间件自己)
     设置 fwdRequestor=getMachineID(srcNodeId) (原始L1)
     设置 dataToFwdRequestor=false (数据先回中间件)
     设置 destination=MachineType_Memory
   │
   ▼
⑦ gem5 MemoryController
   从物理内存 (pmemAddr mmap'd host memory) 读取真实数据
   发送 CHIDataMsg (CompData_UC) 回来，分2个chunk (32字节×2)
   │
   ▼
⑧ CHIMiddleware::recvDataMsg(CHIDataMsg)
   gem5 CHIDataMsg → MyCHIMsg 转换:
     convertData(msg, myMsg)
       遍历64字节，用 WriteMask.test(i) 提取有效字节 → myMsg.data[]
       myMsg.dataOffset = 第一个有效字节的偏移
       myMsg.dataSize = 有效字节总数
       myMsg.lastChunk = (offset + size >= 64)  ← 判断是否最后一个chunk
   │
   ▼
⑨ CustomCache::handleData(MyCHIMsg)
   用 msg.txnId 查 txnToReadAddr → 得到 lineAddr
   查 pendingReads[lineAddr] → 得到 originalReq (原始L1请求)
   │
   ├─ 构造转发消息:
   │    resp.dataType = CompData_UC/SC
   │    resp.addr = lineAddr (原始地址)
   │    resp.txnId = originalReq.txnId (原始事务ID)
   │    resp.dstNodeId = originalReq.srcNodeId (原始L1节点)
   │    resp.data[] = msg.data[] (从内存来的数据)
   │    resp.lastChunk = msg.lastChunk
   │
   └─ sendToGem5(resp)
        │
        ▼
⑩ CHIMiddleware::sendDataToGem5(MyCHIMsg)
   入队 PendingDataChunks
   processPendingDataChunks():
     第1个chunk: offset=0, size=32 → setMask(0,32) → sendDataMsg → tick=T
     第2个chunk: offset=32, size=32 → setMask(32,32) → sendDataMsg → tick=T+4cycle
   │
   │  MyCHIMsg.data[] → DataBlock.setByte()  (逐字节拷回gem5结构)
   │  dstNodeId → getMachineID() → NetDest  (int→MachineID→路由目标)
   │
   ▼
⑪ Ruby Network → L1 dcache
   收到 CompData_UC (2个chunk，间隔4cycle)
   SLICC状态机: BUSY_INTR → Receive_ReqDataResp → Callback_Miss → 数据写入缓存
   │
   ▼
⑫ L1 dcache 发送 CompAck → CHIMiddleware::recvResponseMsg()
   → CustomCache::handleRequest(CompAck) → 无特殊处理(已清理)
```

### 流向2：写回请求（L1 Eviction → 写入内存）

```
① L1 dcache 驱逐脏数据
   发送 CHIRequestMsg (WriteBackFull)
   │
   ▼
② CHIMiddleware::recvRequestMsg()
   convertRequest → MyCHIMsg(reqType=WriteBackFull)
   │
   ▼
③ CustomCache::handleRequest(WriteBackFull)
   记录 pendingWrites[lineAddr]
   sendCompResponse(CompDBIDResp) → sendToGem5
   │                          告诉L1: "可以发数据了"
   ▼
④ CHIMiddleware::sendResponseToGem5(CompDBIDResp)
   MyCHIMsg → CHIResponseMsg, 设置 dbid=txnId
   │
   ▼ Ruby Network → L1 dcache
⑤ L1 收到 CompDBIDResp
   SLICC状态机: 发送 CBWrData (写回数据)
   │
   ▼ Ruby Network
⑥ CHIMiddleware::recvDataMsg(CBWrData)
   convertData → MyCHIMsg (direction=L1_TO_CACHE, data[]=写回数据)
   │
   ▼
⑦ CustomCache::handleData(CBWrData)
   找到 pendingWrites[lineAddr]
   拷贝数据: pendingWrite.data = msg.data
   forwardWriteToMemory() → sendToMemory(WriteNoSnp + NCBWrData)
   │
   ├─ WriteNoSnp → sendToMemory → sendRequestToGem5()
   │    MyCHIMsg → CHIRequestMsg (WriteNoSnp) → gem5 MemCtrl
   │
   └─ NCBWrData → sendToMemory → sendWriteDataToGem5()
        MyCHIMsg.data[] → DataBlock → CHIDataMsg (NCBWrData)
        destination = MachineType_Memory → gem5 MemCtrl
   │
   ▼
⑧ gem5 MemCtrl
   收到 WriteNoSnp + NCBWrData
   写入物理内存 (pmemAddr)
   发回 Comp_I 响应
   │
   ▼
⑨ CHIMiddleware::recvResponseMsg(Comp_I)
   → CustomCache::handleRequest(Comp_I)
   → pendingWrites.erase(lineAddr)  清理
```

### 流向3：自定义缓存命中（不经过内存）

```
① L1 发 ReadShared → CHIMiddleware::recvRequestMsg() → CustomCache::handleRequest()
   │
   ▼
② CustomCache 缓存命中 (cache[lineAddr].ways[i].valid == true)
   直接返回缓存中的数据，不需要访问内存:
   │
   ├─ sendCompData(lineAddr, srcNodeId, entry.data, dataType, txnId)
   │    构造 MyCHIMsg (data[]=entry.data, dstNodeId=srcNodeId)
   │    sendToGem5(msg)
   │
   ▼
③ CHIMiddleware::sendDataToGem5(MyCHIMsg)
   MyCHIMsg.data[] → DataBlock (分块) → CHIDataMsg → L1
   (同流向1的步骤⑩)
```

---

## 消息格式转换

```
                        gem5 侧                    │  自定义侧
                      (CHI协议结构体)               │  (MyCHIMsg)
                                                   │
  CHIRequestMsg ─── recvRequestMsg() ──────────→ MyCHIMsg (msgType=REQUEST)
    .type        → convertReqType()    → .reqType
    .addr                           → .addr
    .txnId                          → .txnId
    .requestor  → getNodeId()       → .srcNodeId
                                                   │
  MyCHIMsg ────── sendRequestToGem5() ─────────→ CHIRequestMsg
    .reqType     → convertReqTypeBack() → .type
    .addr                           → .addr
    .txnId                          → .txnId
    .srcNodeId  → getMachineID()    → .fwdRequestor
                                                   │
  CHIResponseMsg ─ recvResponseMsg() ─────────→ MyCHIMsg (msgType=RESPONSE)
    .type        → convertRespType()   → .respType
    .addr, .txnId, .responder        → .addr, .txnId, .srcNodeId
                                                   │
  CHIDataMsg ──── recvDataMsg() ───────────────→ MyCHIMsg (msgType=DATA)
    .type        → convertDataType()   → .dataType
    .dataBlk     → 按bitMask遍历64字节  → .data[] (紧凑拷贝)
    .bitMask     → test(i)逐位检查     → .dataOffset, .dataSize
                                           .lastChunk = (offset+size>=64)
                                                   │
  MyCHIMsg ────── sendDataToGem5() ────────────→ CHIDataMsg
    .data[]      → DataBlock.setByte()  (逐字节写回)
    .dataOffset  → WriteMask.setMask(offset, size)
    .dstNodeId   → getMachineID() → NetDest.add()
    .dataType    → convertDataTypeBack() → .type
```

---

## MachineID ↔ int 映射

```
gem5 的 MachineID: { type=MachineType_L1Cache, num=0 }  ← dcache
                   { type=MachineType_L1Cache, num=1 }  ← icache
                   { type=MachineType_Memory,  num=0 }  ← MemCtrl

中间件的映射表 (首次遇到时分配):
  machineToNodeId[{L1Cache,0}] = 2   ← dcache (第一个请求来自icache=node0)
  machineToNodeId[{L1Cache,1}] = 0   ← icache (最先被映射)
  machineToNodeId[{Memory, 0}] = 1   ← MemCtrl

自定义层只看到 int: srcNodeId=0 (icache), srcNodeId=2 (dcache), dstNodeId=0/1/2
中间件通过 getMachineID(int) 查回 MachineID 用于构造gem5消息的路由目标
```

---

## 关键设计决策

### 1. 为什么读请求走 gem5 MemCtrl 而不是 CustomMemory

`CustomMemory` 的 `std::unordered_map` 初始化为空，返回全零数据。gem5 的二进制文件通过 `Process::initState()` 加载到 `AbstractMemory` 的 `pmemAddr`（mmap'd host memory）中。如果 CustomMemory 返回零数据，CPU 执行的是零指令（ARM 的 `ANDEQ r0, r0, r0`），导致 dcache 永远不会收到请求（CPU 不执行任何 Load/Store），最终死锁。

因此 `sendToMemory` 回调对 ReadNoSnp 走 `sendRequestToGem5`，让 gem5 MemCtrl 从真实物理内存读取数据。

### 2. 为什么忽略 WriteEvictFull

WriteEvictFull 来自 I 状态的 L1 cache（dcache 从未加载过该地址）。回复任何响应（CompDBIDResp、Comp、Comp\_I）都会导致 SLICC 状态机 "Invalid transition" panic。I 状态的 cache 没有数据需要写回，忽略是安全的。

### 3. 为什么数据要分块发送

gem5 的 `data_channel_size=32` 限制单次数据传输最大 32 字节，而 cache line 是 64 字节。`processPendingDataChunks` 将 64 字节分成 2 个 32 字节 chunk，间隔 4 cycle 发送，与 gem5 原生 MemoryController 的行为一致。如果在同一个 tick 发送两个 chunk，L1 的 SLICC 状态机处理异常，会触发 WriteEvictFull。

### 4. 为什么 CustomCache 用 txnId 匹配内存响应

gem5 CHI 中，data 消息的 `usesTxnId=false`，但 txnId 值仍然被保留（gem5 实现细节）。CustomCache 在 `forwardReadToMemory` 时用原始 txnId 发送 ReadNoSnp，MemCtrl 返回的 CompData 携带相同 txnId。CustomCache 通过 `txnToReadAddr[txnId]` 找到原始请求的地址和目标节点，将数据正确转发给对应的 L1。

### 5. 缓存命中时为什么用缓存数据而非重新访问内存

当 CustomCache 命中时（`entry.valid == true`），直接使用 `entry.data`（缓存中的数据）通过 `sendCompData` 返回给 L1，不需要访问内存。这与标准缓存行为一致——命中时数据来自缓存而不是下级存储。

---

## 测试方式

### 快速测试（Hello World）

最简单的验证方式，确认整个中间件架构端到端运行正常：

```bash
# 编译
scons build/ARM/gem5.opt -j$(nproc)

# 运行（默认无调试输出）
./build/ARM/gem5.opt configs/example/arm/chi_my_cache_hierarchy.py
```

预期输出末尾：

```
Hello world!
Exiting @ tick 18446744073709551615 because simulate() limit reached
```

### 调试模式运行

开启 gem5 调试输出，观察各层消息转换：

```bash
# 开启 MyCHICache 调试标志（CustomCache / CustomMemory 日志）
./build/ARM/gem5.opt --debug-flags=MyCHICache configs/example/arm/chi_my_cache_hierarchy.py

# 开启 Ruby 协议调试（gem5 CHI 消息详情）
./build/ARM/gem5.opt --debug-flags=ProtocolTrace configs/example/arm/chi_my_cache_hierarchy.py

# 开启 Ruby 队列调试（MessageBuffer 收发）
./build/ARM/gem5.opt --debug-flags=RubyQueue configs/example/arm/chi_my_cache_hierarchy.py

# 开启端口调试（Ruby Sequencer 收发请求）
./build/ARM/gem5.opt --debug-flags=RubyPort configs/example/arm/chi_my_cache_hierarchy.py

# 组合多个调试标志
./build/ARM/gem5.opt --debug-flags=MyCHICache,RubyPort configs/example/arm/chi_my_cache_hierarchy.py

# 输出到文件（推荐，调试输出量很大）
./build/ARM/gem5.opt --debug-flags=MyCHICache --debug-file=debug.log configs/example/arm/chi_my_cache_hierarchy.py
```

### 常用调试标志速查

| 标志 | 作用 | 观察内容 |
|------|------|----------|
| `MyCHICache` | CustomCache / CustomMemory 日志 | 缓存命中/未命中、内存读写 |
| `ProtocolTrace` | CHI 协议消息跟踪 | gem5 CHI 消息类型和地址 |
| `RubyQueue` | MessageBuffer 收发 | 消息入队/出队时序 |
| `RubyPort` | Ruby Sequencer | CPU 请求映射到 Ruby |
| `RubyNetwork` | Ruby 网络传输 | 消息路由和链路 |
| `CHI` | CHI 协议状态机 | SLICC 状态转换详情 |

### 性能统计

gem5 自动输出统计信息（无须额外配置）：

```bash
./build/ARM/gem5.opt configs/example/arm/chi_my_cache_hierarchy.py
# 运行后查看 m5out/stats.txt
cat m5out/stats.txt
```

关键指标：

| 指标 | 说明 |
|------|------|
| `sim_seconds` | 模拟耗时（秒） |
| `sim_ticks` | 模拟 tick 数 |
| `system.cpu.numCycles` | CPU 周期数 |
| `system.ruby.m_cntrl.L1Cache.*.demandHits` | L1 命中次数 |
| `system.ruby.m_cntrl.L1Cache.*.demandMisses` | L1 未命中次数 |
| `system.ruby.m_cntrl.L1Cache.*.demandAccesses` | L1 总访问次数 |

### 配置修改

修改 `configs/example/arm/chi_my_cache_hierarchy.py` 中的参数进行不同测试：

```python
# 修改核心数
processor = SimpleProcessor(
    cpu_type=CPUTypes.TIMING,
    isa=ISA.ARM,
    num_cores=2,          # 改为 2 核
)

# 修改 L1 缓存大小
cache_hierarchy = MyCHICacheHierarchy(
    l1_size="128KiB",     # 改为 128KB
    l1_assoc=4,           # 改为 4 路
)

# 修改内存大小
memory = SingleChannelDDR3_1600(size="128MiB")  # 改为 128MB
```

### 运行自定义测试程序

默认运行 `tests/test-progs/hello/bin/arm/linux/hello`。如需使用其他程序，修改配置文件中的 binary 路径：

```python
# 在 chi_my_cache_hierarchy.py 底部修改
board.set_se_binary_workload(
    binary=BinaryResource("/path/to/your/binary"),
)
```

### 常见问题排查

**1. 编译报错 `Unknown type CHIMiddleware`**

确认 `src/mem/my_l2/SConscript` 中 `SimObject('CHIMiddleware.py', sim_objects=['CHIMiddleware'])` 存在，然后重新编译。

**2. 运行时 panic `Invalid transition`**

通常是 SLICC 状态机收到意外的消息类型。开启 `--debug-flags=CHI` 查看状态转换详情，对照 CHI 协议规范定位问题。

**3. 输出全零或死锁**

确认 memory 请求是否走 gem5 MemCtrl（`sendToMemory` 回调中 ReadNoSnp/WriteNoSnp 走 `sendRequestToGem5`）。用 `--debug-flags=RubyPort` 检查 dcache sequencer 是否有 "Timing request" 输出。

**4. 数据 chunk 丢失**

检查 `lastChunk` 判断逻辑：`convertData` 中应为 `(offset + copied >= cacheLineSize)` 而非硬编码 `true`。开启 `--debug-flags=MyCHICache` 观察 chunk 发送日志。
