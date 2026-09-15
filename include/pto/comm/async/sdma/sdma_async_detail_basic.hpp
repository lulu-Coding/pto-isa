/**
Copyright (c) 2025 Huawei Technologies Co., Ltd.
This program is free software, you can redistribute it and/or modify it under the terms and conditions of
CANN Open Software License Agreement Version 2.0 (the "License").
Please refer to the License for details. You may not use this file except in compliance with the License.
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
See LICENSE in the root of the software repository for the full text of the License.
*/

#ifndef PTO_COMM_ASYNC_SDMA_SDMA_ASYNC_DETAIL_BASIC_HPP
#define PTO_COMM_ASYNC_SDMA_SDMA_ASYNC_DETAIL_BASIC_HPP

#include "pto/comm/async/sdma/sdma_types.hpp"
#include "pto/comm/comm_types.hpp"
#include "pto/comm/async_common/async_types.hpp"
#include "pto/pto-inst.hpp"
#include <cstddef>
#include <cstdint>

namespace pto {
namespace comm {
namespace sdma {
namespace detail {

static_assert(kSdmaEventSlotCount > 0, "SDMA_EVENT_SLOT_COUNT must be >= 1");

using UbTmpBuf = TmpBuffer;
constexpr uint32_t kPostDoneStrideBytes = 64U;
constexpr uint64_t kSdmaInt32AtomicAddOpcode = 0x21U;
constexpr uint32_t kPostPollLimit = 100000U;
constexpr uint32_t kSdmaHandleQueueBits = 6U;
constexpr uint32_t kSdmaHandlePostIdBits = 64U - kSdmaHandleQueueBits;
constexpr uint64_t kSdmaHandlePostIdMask = (1ULL << kSdmaHandlePostIdBits) - 1ULL;

PTO_INTERNAL uint64_t QueueCountToMask(uint32_t queueCount)
{
    return queueCount == 0U ? 0ULL : (1ULL << queueCount) - 1ULL;
}

PTO_INTERNAL bool EncodeSdmaEventHandle(uint64_t postId, uint32_t queueCount, uint64_t& handle)
{
    if (postId == 0ULL || postId > kSdmaHandlePostIdMask || queueCount == 0U || queueCount > kSdmaMaxChannelGroups) {
        handle = 0ULL;
        return false;
    }
    handle = (static_cast<uint64_t>(queueCount) << kSdmaHandlePostIdBits) | postId;
    return true;
}

PTO_INTERNAL bool DecodeSdmaEventHandle(uint64_t handle, uint64_t& postId, uint32_t& queueCount)
{
    postId = handle & kSdmaHandlePostIdMask;
    queueCount = static_cast<uint32_t>(handle >> kSdmaHandlePostIdBits);
    if (postId == 0ULL || queueCount == 0U || queueCount > kSdmaMaxChannelGroups) {
        postId = 0ULL;
        queueCount = 0U;
        return false;
    }
    return true;
}

PTO_INTERNAL bool MakeSdmaTmpLocal(__ubuf__ uint8_t* addr, uint32_t size, UbTmpBuf& tmpBuf)
{
    if (addr == nullptr || size < sizeof(uint64_t)) {
        return false;
    }
    tmpBuf.addr = addr;
    tmpBuf.size = size;
    return true;
}

PTO_INTERNAL bool IsValidTmpBuffer(const UbTmpBuf& tmpBuf)
{
    return tmpBuf.addr != nullptr && tmpBuf.size >= sizeof(uint64_t);
}

template <typename ScratchTile>
PTO_INTERNAL bool MakeTmpBufferFromTile(ScratchTile& scratchTile, UbTmpBuf& tmpBuf)
{
    static_assert(is_tile_data_v<ScratchTile>, "scratchTile must be a pto::Tile type");
    static_assert(ScratchTile::Loc == TileType::Vec, "scratchTile must be in Vec(UB) memory");
    tmpBuf.addr = reinterpret_cast<__ubuf__ uint8_t*>(scratchTile.data());
    tmpBuf.size = static_cast<uint32_t>(ScratchTile::Numel * sizeof(typename ScratchTile::DType));
    return IsValidTmpBuffer(tmpBuf);
}

template <typename T>
PTO_INTERNAL void SetValue(__gm__ uint8_t* addr, UbTmpBuf& tmpBuf, uint32_t syncId, T x)
{
    __ubuf__ T* ubPtr = reinterpret_cast<__ubuf__ T*>(tmpBuf.addr);
    *ubPtr = x;
    pipe_barrier(PIPE_ALL);

#ifdef PTO_NPU_ARCH_A5
    copy_ubuf_to_gm_align_v2(
        reinterpret_cast<__gm__ uint32_t*>(addr), reinterpret_cast<__ubuf__ uint32_t*>(ubPtr), 0, 1,
        static_cast<uint32_t>(sizeof(T)), 0, 0, 0);
#else
    copy_ubuf_to_gm_align_b32(
        (__gm__ void*)addr, (__ubuf__ void*)ubPtr, 0, 1, static_cast<uint32_t>(sizeof(T)), 0, 0, 0, 0);
#endif
    set_flag(PIPE_MTE3, PIPE_MTE2, syncId);
    wait_flag(PIPE_MTE3, PIPE_MTE2, syncId);
}

template <typename T>
PTO_INTERNAL T GetValue(__gm__ uint8_t* addr, UbTmpBuf& tmpBuf)
{
    __ubuf__ T* ubPtr = reinterpret_cast<__ubuf__ T*>(tmpBuf.addr);

#ifdef PTO_NPU_ARCH_A5
    copy_gm_to_ubuf_align_v2(
        reinterpret_cast<__ubuf__ uint32_t*>(ubPtr), reinterpret_cast<__gm__ uint32_t*>(addr), 0, 1,
        static_cast<uint32_t>(sizeof(T)), 0, 0, 0, 0, 0, 0);
#else
    copy_gm_to_ubuf_align_b32(
        (__ubuf__ void*)ubPtr, (__gm__ void*)addr, 0, 1, static_cast<uint32_t>(sizeof(T)), 0, 0, 0, 0);
#endif
    pipe_barrier(PIPE_ALL);

    return *ubPtr;
}

PTO_INTERNAL void AddOneMemcpySqe(
    __gm__ BatchWriteChannelInfo* channelInfo, __gm__ uint8_t* src, __gm__ uint8_t* dst, uint64_t opcode,
    uint32_t length, uint32_t sqTail, uint32_t taskId)
{
    __gm__ BatchWriteItem* sqe = (__gm__ BatchWriteItem*)(channelInfo->sq_base);
    sqe += (sqTail % channelInfo->sq_depth);

#ifdef PTO_NPU_ARCH_A5
    // Fully initialize the STARS v2 SQE (same rationale as AddOneCmoSqe):
    // SQ ring memory is unspecified, so write every hardware-parsed field.
    // Mirrors the SHMEM v2 data/flag SQE semantics (SHMEM PR #459).
    sqe->type = RT_STARS_SQE_TYPE_SDMA;
    sqe->lock = 0U;
    sqe->unlock = 0U;
    sqe->ie = 0U;
    sqe->preP = 0U;
    sqe->postP = 0U;
    sqe->wrCqe = 1U;
    sqe->ptrMode = 0U;
    sqe->rttMode = 0U;
    sqe->headUpdate = 0U;
    sqe->reserved0 = 0U;
    sqe->numBlocks = 0U;
    sqe->rtStreamId = channelInfo->stream_id;
    // task_id is derived from SQ tail/head distance and is bounded by SQ depth.
    sqe->taskId = static_cast<uint16_t>(taskId);
    sqe->res1 = 0U;
    sqe->res2 = 0U;
    sqe->kernelCredit = K_CREDIT_TIME_DEFAULT;
    sqe->res3 = 0U;
    sqe->opcode = static_cast<uint32_t>(opcode);
    sqe->sssv = 1U;
    sqe->dssv = 1U;
    sqe->sns = 1U;
    sqe->dns = 1U;
    sqe->sro = 0U;
    sqe->dro = 0U;
    sqe->stride = 0U;
    sqe->ie2 = 0U;
    sqe->compEn = 0U;
    sqe->res4 = 0U;
    sqe->sqeId = 0U;
    sqe->mapamPartId = 0U;
    sqe->mpamns = 0U;
    sqe->pmg = 0U;
    sqe->qos = 6U; // HCCL QoS (matches SHMEM STARS v2)
    sqe->d2dOffsetFlag = 0U;
    sqe->srcStreamId = 0U;
    sqe->srcSubStreamId = 0U;
    sqe->dstStreamId = 0U;
    sqe->dstSubStreamId = 0U;

    uint64_t src_addr = reinterpret_cast<uint64_t>(src);
    uint64_t dst_addr = reinterpret_cast<uint64_t>(dst);

    sqe->srcAddrLow = static_cast<uint32_t>(src_addr & 0xFFFFFFFFULL);
    sqe->srcAddrHigh = static_cast<uint32_t>((src_addr >> 32U) & 0xFFFFFFFFULL);
    sqe->dstAddrLow = static_cast<uint32_t>(dst_addr & 0xFFFFFFFFULL);
    sqe->dstAddrHigh = static_cast<uint32_t>((dst_addr >> 32U) & 0xFFFFFFFFULL);
    sqe->lengthMove = length;
    sqe->srcOffsetLow = 0U;
    sqe->dstOffsetLow = 0U;
    sqe->srcOffsetHigh = 0U;
    sqe->dstOffsetHigh = 0U;
#else
    sqe->type = RT_STARS_SQE_TYPE_SDMA;
    sqe->blockDim = 0;
    sqe->rtStreamId = channelInfo->stream_id;
    sqe->taskId = taskId;
    sqe->kernel_credit = K_CREDIT_TIME_DEFAULT;
    sqe->ptr_mode = 0;
    sqe->opcode = static_cast<uint32_t>(opcode);
    sqe->ie2 = 0;
    sqe->sssv = 1U;
    sqe->dssv = 1U;
    sqe->sns = 1U;
    sqe->dns = 1U;
    sqe->qos = 6;
    sqe->partid = 0U;
    sqe->mpam = 0;
    sqe->length = length;

    uint64_t src_addr = reinterpret_cast<uint64_t>(src);
    uint64_t dst_addr = reinterpret_cast<uint64_t>(dst);

    sqe->srcAddrLow = static_cast<uint32_t>(src_addr & 0xFFFFFFFFULL);
    sqe->srcAddrHigh = static_cast<uint32_t>((src_addr >> 32U) & 0xFFFFFFFFULL);
    sqe->dstAddrLow = static_cast<uint32_t>(dst_addr & 0xFFFFFFFFULL);
    sqe->dstAddrHigh = static_cast<uint32_t>((dst_addr >> 32U) & 0xFFFFFFFFULL);
    sqe->linkType = static_cast<uint8_t>(255U);
#endif
}

PTO_INTERNAL bool BuildTransferConfig(const SdmaBaseConfig& baseConfig, uint64_t messageLen, SdmaConfig& config)
{
    if (baseConfig.queue_num == 0 || baseConfig.block_bytes == 0) {
        return false;
    }
    config.queue_num = baseConfig.queue_num;
    config.block_bytes = baseConfig.block_bytes;
    config.comm_block_offset = baseConfig.comm_block_offset;
    config.per_core_bytes = messageLen;
    config.iter_num = (config.per_core_bytes + config.block_bytes - 1) / config.block_bytes;
    return true;
}

PTO_INTERNAL void PrepareWorkspace(
    __gm__ uint8_t* workspace, const SdmaConfig& config, WorkspaceLayout& layout, uint32_t channelGroupIdx)
{
    const uint64_t perCoreRecvSize = static_cast<uint64_t>(config.queue_num) * kSdmaFlagLength;
    const uint64_t perGroupSendSize = static_cast<uint64_t>(config.queue_num) * kSdmaMinTransferBytes;

    // Event workspace layout:
    // [send staging: kSdmaSendWorkspaceBytes — one 64B slot per queue per channel group]
    // [recv records: channelGroupIdx * (queue_num * kSdmaFlagLength)]
    __gm__ uint8_t* recvBase = workspace + kSdmaSendWorkspaceBytes;
    __gm__ uint8_t* myRecv = recvBase + static_cast<uint64_t>(channelGroupIdx) * perCoreRecvSize;

    layout.send_workspace = workspace + static_cast<uint64_t>(channelGroupIdx) * perGroupSendSize;
    layout.recv_workspace = myRecv;
}

PTO_INTERNAL __gm__ uint8_t* GetPostDoneRecordAddr(__gm__ uint8_t* postDoneBase, uint32_t queue)
{
    return postDoneBase + static_cast<uint64_t>(queue) * kPostDoneStrideBytes;
}

PTO_INTERNAL __gm__ uint8_t* GetFlagPayloadAddr(__gm__ uint8_t* flagPayloadBase, uint64_t postId)
{
    return flagPayloadBase + (postId % kSdmaFlagPayloadDepth) * kSdmaPostIdFlagBytes;
}

PTO_INTERNAL __gm__ uint8_t* GetSignalValueSlotAddr(__gm__ uint8_t* signalValueSlotsBase, uint64_t postId)
{
    return signalValueSlotsBase + (postId % kSdmaFlagPayloadDepth) * kSdmaSignalValueBytes;
}

PTO_INTERNAL __gm__ uint8_t* ResolveFlagPayloadBase(const SdmaExecContext& execCtx)
{
    return execCtx.contextGm + kSdmaContextWorkspaceBytes +
           static_cast<uint64_t>(execCtx.channelGroupIdx) * kSdmaFlagPayloadBytesPerGroup;
}

PTO_INTERNAL __gm__ uint8_t* ResolveSignalValueSlotsBase(const SdmaExecContext& execCtx)
{
    return execCtx.contextGm + kSdmaContextWorkspaceBytes +
           static_cast<uint64_t>(kSdmaMaxChannelGroups) * kSdmaFlagPayloadBytesPerGroup +
           static_cast<uint64_t>(execCtx.channelGroupIdx) * kSdmaSignalValueSlotsBytesPerGroup;
}

PTO_INTERNAL __gm__ uint8_t* ResolvePostDoneBase(const SdmaExecContext& execCtx)
{
    __gm__ uint8_t* workspace =
        execCtx.contextGm + sizeof(BatchWriteFlagInfo) + kSdmaMaxChannel * sizeof(BatchWriteChannelInfo);
    SdmaConfig config{};
    config.queue_num = execCtx.baseConfig.queue_num;
    WorkspaceLayout layout{};
    PrepareWorkspace(workspace, config, layout, execCtx.channelGroupIdx);
    return layout.recv_workspace;
}

} // namespace detail
} // namespace sdma
} // namespace comm
} // namespace pto

#endif // PTO_COMM_ASYNC_SDMA_SDMA_ASYNC_DETAIL_BASIC_HPP
