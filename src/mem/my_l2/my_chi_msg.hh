#ifndef __MEM_MY_L2_MY_CHI_MSG_HH__
#define __MEM_MY_L2_MY_CHI_MSG_HH__

/**
 * MyCHIMsg - 完全独立于gem5的CHI消息定义
 *
 * 这个文件不include任何gem5头文件，可以在纯C++环境中使用。
 * CHIMiddleware负责gem5 CHI消息与此格式之间的转换。
 */

#include <cstdint>
#include <cstring>

// 使用标准uint64_t代替gem5::Addr
using MyAddr = uint64_t;

// ========================================================================
// 枚举定义
// ========================================================================

enum MyCHIMsgType {
    MY_MSG_REQUEST,
    MY_MSG_RESPONSE,
    MY_MSG_DATA
};

// 对应CHI Request类型
enum MyCHIReqType {
    MY_REQ_ReadShared,
    MY_REQ_ReadUnique,
    MY_REQ_ReadOnce,
    MY_REQ_ReadNotSharedDirty,
    MY_REQ_WriteBackFull,
    MY_REQ_WriteCleanFull,
    MY_REQ_WriteEvictFull,
    MY_REQ_Evict,
    MY_REQ_CleanUnique,
    MY_REQ_WriteUniquePtl,
    MY_REQ_WriteUniqueFull,
    MY_REQ_ReadNoSnp,       // Cache→Memory
    MY_REQ_WriteNoSnp,      // Cache→Memory
    MY_REQ_NUM
};

// 对应CHI Response类型
enum MyCHIRespType {
    MY_RESP_Comp,
    MY_RESP_Comp_UC,
    MY_RESP_Comp_SC,
    MY_RESP_Comp_UD_PD,
    MY_RESP_CompDBIDResp,
    MY_RESP_DBIDResp,
    MY_RESP_ReadReceipt,
    MY_RESP_RespSepData,
    MY_RESP_CompAck,
    MY_RESP_Comp_I,
    MY_RESP_NUM
};

// 对应CHI Data类型
enum MyCHIDataType {
    MY_DATA_CompData_UC,
    MY_DATA_CompData_SC,
    MY_DATA_CompData_UD_PD,
    MY_DATA_CompData_SD_PD,
    MY_DATA_CBWrData_UC,
    MY_DATA_CBWrData_SC,
    MY_DATA_CBWrData_UD_PD,
    MY_DATA_CBWrData_SD_PD,
    MY_DATA_CBWrData_I,
    MY_DATA_NCBWrData,
    MY_DATA_DataSepResp_UC,
    MY_DATA_NUM
};

// 消息方向（谁发给谁）
enum MyCHIDirection {
    MY_DIR_L1_TO_CACHE,     // L1 → CustomCache
    MY_DIR_CACHE_TO_L1,     // CustomCache → L1
    MY_DIR_CACHE_TO_MEM,    // CustomCache → CustomMemory
    MY_DIR_MEM_TO_CACHE     // CustomMemory → CustomCache
};

// ========================================================================
// 统一消息结构
// ========================================================================

static constexpr int MY_CACHE_LINE_SIZE = 64;

struct MyCHIMsg {
    MyCHIMsgType msgType;
    MyCHIDirection direction;

    // 请求/响应/数据类型（根据msgType使用对应的字段）
    MyCHIReqType reqType;
    MyCHIRespType respType;
    MyCHIDataType dataType;

    MyAddr addr;                // 地址
    MyAddr txnId;               // 事务ID
    int srcNodeId;              // 源节点ID（整数，不依赖MachineID）
    int dstNodeId;              // 目标节点ID

    // 数据
    uint8_t data[MY_CACHE_LINE_SIZE];  // cache line数据
    int dataOffset;             // 数据在cache line中的偏移
    int dataSize;               // 有效数据大小（字节）
    bool lastChunk;             // 是否是最后一个数据chunk

    MyCHIMsg() = default;
};

// ========================================================================
// 辅助函数
// ========================================================================

inline const char* myCHIReqTypeStr(MyCHIReqType type) {
    switch (type) {
      case MY_REQ_ReadShared:         return "ReadShared";
      case MY_REQ_ReadUnique:         return "ReadUnique";
      case MY_REQ_ReadOnce:           return "ReadOnce";
      case MY_REQ_ReadNotSharedDirty: return "ReadNotSharedDirty";
      case MY_REQ_WriteBackFull:      return "WriteBackFull";
      case MY_REQ_WriteCleanFull:     return "WriteCleanFull";
      case MY_REQ_WriteEvictFull:     return "WriteEvictFull";
      case MY_REQ_Evict:              return "Evict";
      case MY_REQ_CleanUnique:        return "CleanUnique";
      case MY_REQ_WriteUniquePtl:     return "WriteUniquePtl";
      case MY_REQ_WriteUniqueFull:    return "WriteUniqueFull";
      case MY_REQ_ReadNoSnp:          return "ReadNoSnp";
      case MY_REQ_WriteNoSnp:         return "WriteNoSnp";
      default:                        return "Unknown";
    }
}

inline const char* myCHIRespTypeStr(MyCHIRespType type) {
    switch (type) {
      case MY_RESP_Comp:          return "Comp";
      case MY_RESP_Comp_UC:       return "Comp_UC";
      case MY_RESP_Comp_SC:       return "Comp_SC";
      case MY_RESP_Comp_UD_PD:    return "Comp_UD_PD";
      case MY_RESP_CompDBIDResp:  return "CompDBIDResp";
      case MY_RESP_DBIDResp:      return "DBIDResp";
      case MY_RESP_ReadReceipt:   return "ReadReceipt";
      case MY_RESP_RespSepData:   return "RespSepData";
      case MY_RESP_CompAck:       return "CompAck";
      case MY_RESP_Comp_I:        return "Comp_I";
      default:                    return "Unknown";
    }
}

inline const char* myCHIDataTypeStr(MyCHIDataType type) {
    switch (type) {
      case MY_DATA_CompData_UC:      return "CompData_UC";
      case MY_DATA_CompData_SC:      return "CompData_SC";
      case MY_DATA_CompData_UD_PD:   return "CompData_UD_PD";
      case MY_DATA_CompData_SD_PD:   return "CompData_SD_PD";
      case MY_DATA_CBWrData_UC:      return "CBWrData_UC";
      case MY_DATA_CBWrData_SC:      return "CBWrData_SC";
      case MY_DATA_CBWrData_UD_PD:   return "CBWrData_UD_PD";
      case MY_DATA_CBWrData_SD_PD:   return "CBWrData_SD_PD";
      case MY_DATA_CBWrData_I:       return "CBWrData_I";
      case MY_DATA_NCBWrData:        return "NCBWrData";
      case MY_DATA_DataSepResp_UC:   return "DataSepResp_UC";
      default:                       return "Unknown";
    }
}

#endif // __MEM_MY_L2_MY_CHI_MSG_HH__
