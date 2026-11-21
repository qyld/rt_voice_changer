/*++

Copyright (c) Microsoft Corporation All Rights Reserved
Modified for AI 变声器虚拟声卡.

Module Name:

    micinwavtable.h

Abstract:

    采集端 "AI变声器 麦克风" 的 wave miniport 表。
    数据来自渲染端写入的内核环形缓冲（见 aivcloopback.h）。

--*/

#ifndef _SYSVAD_MICINWAVTABLE_H_
#define _SYSVAD_MICINWAVTABLE_H_

#include "aivcformats.h"

#define MICIN_DEVICE_MAX_CHANNELS           AIVC_MAX_CHANNELS

//
// 允许多个程序同时打开麦克风（每个采集流有独立读指针）。
//
#define MICIN_MAX_INPUT_STREAMS             8

//=============================================================================
AIVC_DECLARE_FORMAT_TABLE(MicInPinSupportedDeviceFormats);

//
// Supported modes (only on streaming pins).
//
static
MODE_AND_DEFAULT_FORMAT MicInPinSupportedDeviceModes[] =
{
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_RAW,
        &MicInPinSupportedDeviceFormats[AIVC_DEFAULT_FORMAT_INDEX].DataFormat,
    },
    {
        STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,
        &MicInPinSupportedDeviceFormats[AIVC_DEFAULT_FORMAT_INDEX].DataFormat,
    },
};

//
// The entries here must follow the same order as the filter's pin
// descriptor array.
static
PIN_DEVICE_FORMATS_AND_MODES MicInPinDeviceFormatsAndModes[] =
{
    {
        BridgePin,
        NULL,
        0,
        NULL,
        0
    },
    {
        SystemCapturePin,
        MicInPinSupportedDeviceFormats,
        SIZEOF_ARRAY(MicInPinSupportedDeviceFormats),
        MicInPinSupportedDeviceModes,
        SIZEOF_ARRAY(MicInPinSupportedDeviceModes)
    }
};

//=============================================================================
static
KSDATARANGE MicInPinDataRangesBridge[] =
{
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static
PKSDATARANGE MicInPinDataRangePointersBridge[] =
{
    &MicInPinDataRangesBridge[0]
};

//=============================================================================
static
KSDATARANGE_AUDIO MicInPinDataRangesStream[] =
{
    {
        {
            sizeof(KSDATARANGE_AUDIO),
            KSDATARANGE_ATTRIBUTES,         // An attributes list follows this data range
            0,
            0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        AIVC_MAX_CHANNELS,
        AIVC_MIN_BITS_PER_SAMPLE,
        AIVC_MAX_BITS_PER_SAMPLE,
        AIVC_MIN_SAMPLE_RATE,
        AIVC_MAX_SAMPLE_RATE
    },
};

static
PKSDATARANGE MicInPinDataRangePointersStream[] =
{
    PKSDATARANGE(&MicInPinDataRangesStream[0]),
    PKSDATARANGE(&PinDataRangeAttributeList),
};

//=============================================================================
static
PCPIN_DESCRIPTOR MicInWaveMiniportPins[] =
{
    // Wave In Bridge Pin (Capture - From Topology) KSPIN_WAVE_BRIDGE
    {
        0,
        0,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(MicInPinDataRangePointersBridge),
            MicInPinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            NULL,
            0
        }
    },

    // Wave In Streaming Pin (Capture) KSPIN_WAVEIN_HOST
    {
        MICIN_MAX_INPUT_STREAMS,
        MICIN_MAX_INPUT_STREAMS,
        0,
        NULL,
        {
            0,
            NULL,
            0,
            NULL,
            SIZEOF_ARRAY(MicInPinDataRangePointersStream),
            MicInPinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            &KSAUDFNAME_RECORDING_CONTROL,
            0
        }
    }
};

//=============================================================================
static
PCNODE_DESCRIPTOR MicInWaveMiniportNodes[] =
{
    // KSNODE_WAVE_ADC
    {
        0,                      // Flags
        NULL,                   // AutomationTable
        &KSNODETYPE_ADC,        // Type
        NULL                    // Name
    }
};

//=============================================================================
static
PCCONNECTION_DESCRIPTOR MicInWaveMiniportConnections[] =
{
    { PCFILTER_NODE,        KSPIN_WAVE_BRIDGE,      KSNODE_WAVE_ADC,     1 },
    { KSNODE_WAVE_ADC,      0,                      PCFILTER_NODE,       KSPIN_WAVEIN_HOST },
};

//=============================================================================
static
PCPROPERTY_ITEM PropertiesMicInWaveFilter[] =
{
    {
        &KSPROPSETID_General,
        KSPROPERTY_GENERAL_COMPONENTID,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_WaveFilter
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMicInWaveFilter, PropertiesMicInWaveFilter);

//=============================================================================
static
PCFILTER_DESCRIPTOR MicInWaveMiniportFilterDescriptor =
{
    0,                                          // Version
    &AutomationMicInWaveFilter,                 // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),                   // PinSize
    SIZEOF_ARRAY(MicInWaveMiniportPins),        // PinCount
    MicInWaveMiniportPins,                      // Pins
    sizeof(PCNODE_DESCRIPTOR),                  // NodeSize
    SIZEOF_ARRAY(MicInWaveMiniportNodes),       // NodeCount
    MicInWaveMiniportNodes,                     // Nodes
    SIZEOF_ARRAY(MicInWaveMiniportConnections), // ConnectionCount
    MicInWaveMiniportConnections,               // Connections
    0,                                          // CategoryCount
    NULL                                        // Categories  - use defaults (audio, render, capture)
};

#endif // _SYSVAD_MICINWAVTABLE_H_
