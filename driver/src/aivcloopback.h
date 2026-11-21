/*++

Module Name:

    aivcloopback.h

Abstract:

    AI 变声器虚拟声卡 —— 渲染端 -> 采集端 的内核环形缓冲。

    设计要点：
      * 环形缓冲内部统一为 48000Hz / 立体声 / int32（左对齐）样本。
      * 渲染流（唯一写入者）把自身格式转换（含 44100->48000 线性重采样）后写入。
      * 每个采集流是独立读者，拥有自己的读指针，可多个程序同时打开麦克风。
      * 读者在缓冲不足时输出静音，并做约 1.3ms 的淡入/淡出，避免爆音；
        首次启动或欠载后先预缓冲 AIVC_PREFILL_FRAMES 再开始输出；
        积压超过 AIVC_MAX_LATENCY_FRAMES 时丢弃旧数据，控制延迟。
      * 所有函数可在 DISPATCH_LEVEL 调用（WaveRT 位置更新在自旋锁/DPC 中执行）。

--*/

#ifndef _AIVC_LOOPBACK_H_
#define _AIVC_LOOPBACK_H_

#define AIVC_RING_RATE              48000
#define AIVC_RING_CHANNELS          2
#define AIVC_RING_FRAMES            (AIVC_RING_RATE)            // 1 秒容量
#define AIVC_PREFILL_FRAMES         (AIVC_RING_RATE * 30 / 1000)   // 30 ms 预缓冲
#define AIVC_MAX_LATENCY_FRAMES     (AIVC_RING_RATE * 150 / 1000)  // 超过 150 ms 积压则追赶
#define AIVC_FADE_STEP              4                           // Q8 增益每帧步进 (256/4 = 64 帧)
#define AIVC_MAX_BLOCK_ALIGN        8                           // 2ch * 4 bytes

NTSTATUS AivcLoopbackInitialize();
VOID     AivcLoopbackCleanup();

//
// 一个 PCM 流格式的简要描述（只支持整数 PCM，容器 2/3/4 字节）。
//
typedef struct _AIVC_PCM_FORMAT
{
    ULONG   SampleRate;
    ULONG   Channels;
    ULONG   BytesPerSample;     // 容器大小：2 / 3 / 4
    ULONG   BlockAlign;
} AIVC_PCM_FORMAT;

BOOLEAN AivcParseFormat(_In_ PWAVEFORMATEX pWfx, _Out_ AIVC_PCM_FORMAT *pFmt);

//=============================================================================
// 写入者：渲染流使用
//=============================================================================
class CAivcLoopbackWriter
{
public:
    VOID Reset(_In_ const AIVC_PCM_FORMAT *pFmt);
    VOID Write(_In_reads_bytes_(cb) const BYTE *pData, _In_ ULONG cb);

private:
    VOID ProcessInputFrame(_In_reads_bytes_(AIVC_MAX_BLOCK_ALIGN) const BYTE *pFrame);
    VOID EmitRingFrame(_In_ LONG l, _In_ LONG r);

    AIVC_PCM_FORMAT m_Fmt;
    BOOLEAN         m_Valid;
    BYTE            m_Partial[AIVC_MAX_BLOCK_ALIGN];
    ULONG           m_PartialLen;

    // 线性插值重采样（输入率 -> 48000）
    BOOLEAN         m_Resample;
    ULONGLONG       m_Step;         // 32.32 定点：输入帧 / 输出帧
    ULONGLONG       m_Phase;        // 32.32 定点
    LONG            m_Prev[AIVC_RING_CHANNELS];
    LONG            m_Cur[AIVC_RING_CHANNELS];
    BOOLEAN         m_HaveCur;
};

//=============================================================================
// 读者：每个采集流一个
//=============================================================================
class CAivcLoopbackReader
{
public:
    VOID Reset(_In_ const AIVC_PCM_FORMAT *pFmt);
    VOID Start();       // 进入 RUN 时调用：从当前写位置开始、重新预缓冲
    VOID Read(_Out_writes_bytes_(cb) BYTE *pData, _In_ ULONG cb);

private:
    VOID FetchRingFrame(_Out_ LONG *pL, _Out_ LONG *pR);    // 调用者持锁
    VOID ProduceOutputFrame(_Out_writes_bytes_(AIVC_MAX_BLOCK_ALIGN) BYTE *pFrame); // 调用者持锁

    AIVC_PCM_FORMAT m_Fmt;
    BOOLEAN         m_Valid;
    BYTE            m_Pending[AIVC_MAX_BLOCK_ALIGN];
    ULONG           m_PendingOffset;
    ULONG           m_PendingLen;

    ULONGLONG       m_ReadPos;      // 环形缓冲绝对帧位置
    BOOLEAN         m_Primed;
    LONG            m_Gain;         // Q8: 0..256
    LONG            m_Last[AIVC_RING_CHANNELS];

    // 线性插值重采样（48000 -> 输出率）
    BOOLEAN         m_Resample;
    ULONGLONG       m_Step;
    ULONGLONG       m_Phase;
    LONG            m_Prev[AIVC_RING_CHANNELS];
    LONG            m_Cur[AIVC_RING_CHANNELS];
};

#endif // _AIVC_LOOPBACK_H_
