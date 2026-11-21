/*++

Copyright (c) Microsoft Corporation All Rights Reserved
Modified for AI 变声器虚拟声卡.

Module Name:

    minipairs.h

Abstract:

    端点定义：只有一对环回端点。
      渲染端  WaveSpeaker / TopologySpeaker  -> "AI变声器 内部输出"
      采集端  WaveMicIn   / TopologyMicIn    -> "AI变声器 麦克风"

--*/

#ifndef _SYSVAD_MINIPAIRS_H_
#define _SYSVAD_MINIPAIRS_H_

#include "speakertopo.h"
#include "speakertoptable.h"
#include "speakerwavtable.h"

#include "micintopo.h"
#include "micintoptable.h"
#include "micinwavtable.h"

NTSTATUS
CreateMiniportWaveRTSYSVAD
(
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

NTSTATUS
CreateMiniportTopologySYSVAD
(
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

//=============================================================================
//
// 渲染端（变声器播放到这里）
//
/*********************************************************************
*              +------+                +------+                      *
*              | Wave |                | Topo |                      *
* System   --->|0    1|--------------->|0    1|---> Line Out         *
*              +------+                +------+                      *
*********************************************************************/
static
PHYSICALCONNECTIONTABLE SpeakerTopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_WAVEOUT_SOURCE,  // TopologyIn
        KSPIN_WAVE_RENDER3_SOURCE,  // WaveOut
        CONNECTIONTYPE_WAVE_OUTPUT
    }
};

static
ENDPOINT_MINIPAIR SpeakerMiniports =
{
    eSpeakerDevice,
    L"TopologySpeaker",                                     // 与 INF 中 KSNAME_TopologySpeaker 一致
    NULL,                                                   // optional template name
    CreateMiniportTopologySYSVAD,
    &SpeakerTopoMiniportFilterDescriptor,
    0, NULL,                                                // Interface properties
    L"WaveSpeaker",                                         // 与 INF 中 KSNAME_WaveSpeaker 一致
    NULL,                                                   // optional template name
    CreateMiniportWaveRTSYSVAD,
    &SpeakerWaveMiniportFilterDescriptor,
    0, NULL,                                                // Interface properties
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,                                      // 无硬件 offload / loopback
    NULL, 0, NULL,                                          // audio module settings.
};

//=============================================================================
//
// 采集端（直播软件选它）
//
/*********************************************************************
*              +------+    +------+                                  *
*              | Topo |    | Wave |                                  *
*  Mic in  --->|0    1|===>|0    1|---> Capture Host Pin             *
*              +------+    +------+                                  *
*********************************************************************/
static
PHYSICALCONNECTIONTABLE MicInTopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_BRIDGE,          // TopologyOut
        KSPIN_WAVE_BRIDGE,          // WaveIn
        CONNECTIONTYPE_TOPOLOGY_OUTPUT
    }
};

static
ENDPOINT_MINIPAIR MicInMiniports =
{
    eMicInDevice,
    L"TopologyMicIn",                       // 与 INF 中 KSNAME_TopologyMicIn 一致
    NULL,                                   // optional template name
    CreateMiniportTopologySYSVAD,
    &MicInTopoMiniportFilterDescriptor,
    0, NULL,                                // Interface properties
    L"WaveMicIn",                           // 与 INF 中 KSNAME_WaveMicIn 一致
    NULL,                                   // optional template name
    CreateMiniportWaveRTSYSVAD,
    &MicInWaveMiniportFilterDescriptor,
    0, NULL,                                // Interface properties
    MICIN_DEVICE_MAX_CHANNELS,
    MicInPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicInPinDeviceFormatsAndModes),
    MicInTopologyPhysicalConnections,
    SIZEOF_ARRAY(MicInTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
    NULL, 0, NULL,                          // audio module settings.
};

//=============================================================================
static
PENDPOINT_MINIPAIR  g_RenderEndpoints[] =
{
    &SpeakerMiniports,
};

#define g_cRenderEndpoints  (SIZEOF_ARRAY(g_RenderEndpoints))

static
PENDPOINT_MINIPAIR  g_CaptureEndpoints[] =
{
    &MicInMiniports,
};

#define g_cCaptureEndpoints (SIZEOF_ARRAY(g_CaptureEndpoints))

//
// Total miniports = # endpoints * 2 (topology + wave).
//
#define g_MaxMiniports  ((g_cRenderEndpoints + g_cCaptureEndpoints) * 2)

#endif // _SYSVAD_MINIPAIRS_H_
