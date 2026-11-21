/*++

Module Name:

    aivcloopback.cpp

Abstract:

    AI 变声器虚拟声卡 —— 渲染端 -> 采集端 的内核环形缓冲实现。
    说明见 aivcloopback.h。

--*/

#include <sysvad.h>
#include "aivcloopback.h"

#define AIVC_POOLTAG        'cviA'
#define AIVC_ONE            (1ULL << 32)

//
// 全局环形缓冲（非分页内存）
//
static LONG        *g_AivcRing      = NULL;     // AIVC_RING_FRAMES * AIVC_RING_CHANNELS
static ULONGLONG    g_AivcWritePos  = 0;        // 已写入的总帧数（绝对位置）
static KSPIN_LOCK   g_AivcLock;

#pragma code_seg("PAGE")
NTSTATUS AivcLoopbackInitialize()
{
    PAGED_CODE();

    KeInitializeSpinLock(&g_AivcLock);
    g_AivcWritePos = 0;

    g_AivcRing = (LONG *)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(LONG) * AIVC_RING_FRAMES * AIVC_RING_CHANNELS,
        AIVC_POOLTAG);

    return (g_AivcRing != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

#pragma code_seg("PAGE")
VOID AivcLoopbackCleanup()
{
    PAGED_CODE();

    if (g_AivcRing != NULL)
    {
        ExFreePoolWithTag(g_AivcRing, AIVC_POOLTAG);
        g_AivcRing = NULL;
    }
}

#pragma code_seg()

//=============================================================================
// 工具函数
//=============================================================================
BOOLEAN AivcParseFormat(_In_ PWAVEFORMATEX pWfx, _Out_ AIVC_PCM_FORMAT *pFmt)
{
    RtlZeroMemory(pFmt, sizeof(*pFmt));

    if (pWfx == NULL)
    {
        return FALSE;
    }

    if (pWfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        if (pWfx->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        {
            return FALSE;
        }
        PWAVEFORMATEXTENSIBLE pExt = (PWAVEFORMATEXTENSIBLE)pWfx;
        if (!IsEqualGUIDAligned(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
        {
            return FALSE;
        }
    }
    else if (pWfx->wFormatTag != WAVE_FORMAT_PCM)
    {
        return FALSE;
    }

    ULONG bps = pWfx->wBitsPerSample / 8;
    if (bps < 2 || bps > 4 || pWfx->nChannels < 1 || pWfx->nChannels > AIVC_RING_CHANNELS)
    {
        return FALSE;
    }
    if (pWfx->nBlockAlign != pWfx->nChannels * bps || pWfx->nSamplesPerSec == 0)
    {
        return FALSE;
    }

    pFmt->SampleRate     = pWfx->nSamplesPerSec;
    pFmt->Channels       = pWfx->nChannels;
    pFmt->BytesPerSample = bps;
    pFmt->BlockAlign     = pWfx->nBlockAlign;
    return TRUE;
}

//
// 读取一个样本，返回左对齐 int32。
//
static __forceinline LONG AivcLoadSample(_In_ const BYTE *p, _In_ ULONG bps)
{
    ULONG u;
    switch (bps)
    {
    case 2:
        u = ((ULONG)p[0] << 16) | ((ULONG)p[1] << 24);
        break;
    case 3:
        u = ((ULONG)p[0] << 8) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 24);
        break;
    default:
        u = (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
        break;
    }
    return (LONG)u;
}

//
// 写出一个左对齐 int32 样本（截断低位）。
//
static __forceinline VOID AivcStoreSample(_Out_ BYTE *p, _In_ ULONG bps, _In_ LONG v)
{
    ULONG u = (ULONG)v;
    switch (bps)
    {
    case 2:
        p[0] = (BYTE)(u >> 16);
        p[1] = (BYTE)(u >> 24);
        break;
    case 3:
        p[0] = (BYTE)(u >> 8);
        p[1] = (BYTE)(u >> 16);
        p[2] = (BYTE)(u >> 24);
        break;
    default:
        p[0] = (BYTE)u;
        p[1] = (BYTE)(u >> 8);
        p[2] = (BYTE)(u >> 16);
        p[3] = (BYTE)(u >> 24);
        break;
    }
}

static __forceinline LONG AivcLerp(_In_ LONG a, _In_ LONG b, _In_ ULONGLONG phase)
{
    // phase 的低 32 位是 [0,1) 小数部分
    LONGLONG frac = (LONGLONG)(phase & 0xFFFFFFFFULL);
    return (LONG)((LONGLONG)a + ((((LONGLONG)b - (LONGLONG)a) * frac) >> 32));
}

//=============================================================================
// CAivcLoopbackWriter
//=============================================================================
VOID CAivcLoopbackWriter::Reset(_In_ const AIVC_PCM_FORMAT *pFmt)
{
    m_Fmt        = *pFmt;
    m_Valid      = (pFmt->BlockAlign != 0) && (g_AivcRing != NULL);
    m_PartialLen = 0;
    m_Resample   = (pFmt->SampleRate != AIVC_RING_RATE);
    m_Step       = m_Resample ? (((ULONGLONG)pFmt->SampleRate << 32) / AIVC_RING_RATE) : AIVC_ONE;
    m_Phase      = 0;
    m_HaveCur    = FALSE;
    for (ULONG c = 0; c < AIVC_RING_CHANNELS; c++)
    {
        m_Prev[c] = 0;
        m_Cur[c]  = 0;
    }
}

// 调用者持有 g_AivcLock
VOID CAivcLoopbackWriter::EmitRingFrame(_In_ LONG l, _In_ LONG r)
{
    ULONG idx = (ULONG)(g_AivcWritePos % AIVC_RING_FRAMES) * AIVC_RING_CHANNELS;
    g_AivcRing[idx]     = l;
    g_AivcRing[idx + 1] = r;
    g_AivcWritePos++;
}

// 调用者持有 g_AivcLock
VOID CAivcLoopbackWriter::ProcessInputFrame(_In_reads_bytes_(AIVC_MAX_BLOCK_ALIGN) const BYTE *pFrame)
{
    LONG l = AivcLoadSample(pFrame, m_Fmt.BytesPerSample);
    LONG r = (m_Fmt.Channels >= 2) ? AivcLoadSample(pFrame + m_Fmt.BytesPerSample, m_Fmt.BytesPerSample) : l;

    if (!m_Resample)
    {
        EmitRingFrame(l, r);
        return;
    }

    if (!m_HaveCur)
    {
        m_Cur[0] = l;
        m_Cur[1] = r;
        m_HaveCur = TRUE;
    }

    m_Prev[0] = m_Cur[0];
    m_Prev[1] = m_Cur[1];
    m_Cur[0]  = l;
    m_Cur[1]  = r;

    while (m_Phase < AIVC_ONE)
    {
        EmitRingFrame(AivcLerp(m_Prev[0], m_Cur[0], m_Phase),
                      AivcLerp(m_Prev[1], m_Cur[1], m_Phase));
        m_Phase += m_Step;
    }
    m_Phase -= AIVC_ONE;
}

VOID CAivcLoopbackWriter::Write(_In_reads_bytes_(cb) const BYTE *pData, _In_ ULONG cb)
{
    if (!m_Valid || pData == NULL || cb == 0)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_AivcLock, &oldIrql);

    ULONG ba = m_Fmt.BlockAlign;

    // 先补齐上次残留的半帧
    while (m_PartialLen > 0 && cb > 0)
    {
        m_Partial[m_PartialLen++] = *pData++;
        cb--;
        if (m_PartialLen == ba)
        {
            ProcessInputFrame(m_Partial);
            m_PartialLen = 0;
        }
    }

    // 整帧
    while (cb >= ba)
    {
        ProcessInputFrame(pData);
        pData += ba;
        cb    -= ba;
    }

    // 剩余半帧
    while (cb > 0)
    {
        m_Partial[m_PartialLen++] = *pData++;
        cb--;
    }

    KeReleaseSpinLock(&g_AivcLock, oldIrql);
}

//=============================================================================
// CAivcLoopbackReader
//=============================================================================
VOID CAivcLoopbackReader::Reset(_In_ const AIVC_PCM_FORMAT *pFmt)
{
    m_Fmt           = *pFmt;
    m_Valid         = (pFmt->BlockAlign != 0) && (g_AivcRing != NULL);
    m_PendingOffset = 0;
    m_PendingLen    = 0;
    m_Resample      = (pFmt->SampleRate != AIVC_RING_RATE);
    m_Step          = m_Resample ? (((ULONGLONG)AIVC_RING_RATE << 32) / pFmt->SampleRate) : AIVC_ONE;
    Start();
}

VOID CAivcLoopbackReader::Start()
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_AivcLock, &oldIrql);

    m_ReadPos       = g_AivcWritePos;
    m_Primed        = FALSE;
    m_Gain          = 0;
    m_Phase         = AIVC_ONE;     // 第一次输出时先取一帧
    m_PendingOffset = 0;
    m_PendingLen    = 0;
    for (ULONG c = 0; c < AIVC_RING_CHANNELS; c++)
    {
        m_Last[c] = 0;
        m_Prev[c] = 0;
        m_Cur[c]  = 0;
    }

    KeReleaseSpinLock(&g_AivcLock, oldIrql);
}

// 调用者持有 g_AivcLock
VOID CAivcLoopbackReader::FetchRingFrame(_Out_ LONG *pL, _Out_ LONG *pR)
{
    ULONGLONG writePos = g_AivcWritePos;

    if (m_ReadPos > writePos)
    {
        m_ReadPos = writePos;
    }

    ULONGLONG avail = writePos - m_ReadPos;

    // 积压过多（采集端暂停过、或刚启动时渲染端已写入大量数据）：追赶到预缓冲深度
    if (avail > AIVC_MAX_LATENCY_FRAMES)
    {
        m_ReadPos = writePos - AIVC_PREFILL_FRAMES;
        avail     = AIVC_PREFILL_FRAMES;
    }

    if (!m_Primed && avail >= AIVC_PREFILL_FRAMES)
    {
        m_Primed = TRUE;
    }

    if (m_Primed && avail > 0)
    {
        ULONG idx = (ULONG)(m_ReadPos % AIVC_RING_FRAMES) * AIVC_RING_CHANNELS;
        m_Last[0] = g_AivcRing[idx];
        m_Last[1] = g_AivcRing[idx + 1];
        m_ReadPos++;

        if (m_Gain < 256)
        {
            m_Gain = min(256, m_Gain + AIVC_FADE_STEP);
        }
    }
    else
    {
        // 欠载：重新预缓冲，并把最后一个样本平滑衰减到 0，避免爆音
        m_Primed = FALSE;
        if (m_Gain > 0)
        {
            m_Gain = max(0, m_Gain - AIVC_FADE_STEP);
        }
    }

    if (m_Gain >= 256)
    {
        *pL = m_Last[0];
        *pR = m_Last[1];
    }
    else
    {
        *pL = (LONG)(((LONGLONG)m_Last[0] * m_Gain) >> 8);
        *pR = (LONG)(((LONGLONG)m_Last[1] * m_Gain) >> 8);
    }

    if (m_Gain == 0)
    {
        m_Last[0] = 0;
        m_Last[1] = 0;
    }
}

// 调用者持有 g_AivcLock
VOID CAivcLoopbackReader::ProduceOutputFrame(_Out_writes_bytes_(AIVC_MAX_BLOCK_ALIGN) BYTE *pFrame)
{
    LONG l, r;

    if (!m_Resample)
    {
        FetchRingFrame(&l, &r);
    }
    else
    {
        while (m_Phase >= AIVC_ONE)
        {
            m_Prev[0] = m_Cur[0];
            m_Prev[1] = m_Cur[1];
            FetchRingFrame(&m_Cur[0], &m_Cur[1]);
            m_Phase -= AIVC_ONE;
        }
        l = AivcLerp(m_Prev[0], m_Cur[0], m_Phase);
        r = AivcLerp(m_Prev[1], m_Cur[1], m_Phase);
        m_Phase += m_Step;
    }

    ULONG bps = m_Fmt.BytesPerSample;
    if (m_Fmt.Channels == 1)
    {
        AivcStoreSample(pFrame, bps, (LONG)(((LONGLONG)l + (LONGLONG)r) / 2));
    }
    else
    {
        AivcStoreSample(pFrame, bps, l);
        AivcStoreSample(pFrame + bps, bps, r);
    }
}

VOID CAivcLoopbackReader::Read(_Out_writes_bytes_(cb) BYTE *pData, _In_ ULONG cb)
{
    if (pData == NULL || cb == 0)
    {
        return;
    }

    if (!m_Valid)
    {
        RtlZeroMemory(pData, cb);
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_AivcLock, &oldIrql);

    ULONG ba = m_Fmt.BlockAlign;

    // 上次剩余的半帧
    while (m_PendingLen > 0 && cb > 0)
    {
        *pData++ = m_Pending[m_PendingOffset++];
        m_PendingLen--;
        cb--;
    }

    // 整帧直接写入 DMA 缓冲
    while (cb >= ba)
    {
        ProduceOutputFrame(pData);
        pData += ba;
        cb    -= ba;
    }

    // 结尾半帧：生成一整帧，只拷贝需要的部分，其余留到下次
    if (cb > 0)
    {
        ProduceOutputFrame(m_Pending);
        m_PendingOffset = 0;
        m_PendingLen    = ba;
        while (cb > 0)
        {
            *pData++ = m_Pending[m_PendingOffset++];
            m_PendingLen--;
            cb--;
        }
    }

    KeReleaseSpinLock(&g_AivcLock, oldIrql);
}
