/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    micintopotable.h

Abstract:

    Declaration of topology table for the mic (external: headphone)

--*/

#ifndef _SYSVAD_MICINTOPTABLE_H_
#define _SYSVAD_MICINTOPTABLE_H_

// Function declarations.
NTSTATUS
PropertyHandler_MicInTopoFilter( 
    _In_ PPCPROPERTY_REQUEST      PropertyRequest 
    );

//
// 采集端点名称 GUID，对应 INF 中 MediaCategories\{...}\Name -> "AI变声器 麦克风"
// {2618fdb3-5af7-4a6e-a769-49901650d46f}
DEFINE_GUID(MICIN_CUSTOM_NAME,
0x2618fdb3, 0x5af7, 0x4a6e, 0xa7, 0x69, 0x49, 0x90, 0x16, 0x50, 0xd4, 0x6f);

//=============================================================================
static
KSJACK_DESCRIPTION MicInJackDesc =
{
    KSAUDIO_SPEAKER_STEREO,
    JACKDESC_RGB(0, 0, 0),
    eConnTypeUnknown,
    eGeoLocNotApplicable,
    eGenLocOther,
    ePortConnIntegratedDevice,
    TRUE
};

//=============================================================================
static
KSDATARANGE MicInTopoPinDataRangesBridge[] =
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

//=============================================================================
static
PKSDATARANGE MicInTopoPinDataRangePointersBridge[] =
{
  &MicInTopoPinDataRangesBridge[0]
};

//=============================================================================
static
PCPIN_DESCRIPTOR MicInTopoMiniportPins[] =
{
  // KSPIN - topology filter in-pin
  {
    0,
    0,
    0,                                                  // InstanceCount
    NULL,                                               // AutomationTable
    {                                                   // KsPinDescriptor
      0,                                                // InterfacesCount
      NULL,                                             // Interfaces
      0,                                                // MediumsCount
      NULL,                                             // Mediums
      SIZEOF_ARRAY(MicInTopoPinDataRangePointersBridge),// DataRangesCount
      MicInTopoPinDataRangePointersBridge,              // DataRanges
      KSPIN_DATAFLOW_IN,                                // DataFlow
      KSPIN_COMMUNICATION_NONE,                         // Communication
      &KSNODETYPE_MICROPHONE,                           // Category
      &MICIN_CUSTOM_NAME,                               // Name
      0                                                 // Reserved
    }
  },

  // KSPIN - topology filter out-in
  {
    0,
    0,
    0,                                                  // InstanceCount
    NULL,                                               // AutomationTable
    {                                                   // KsPinDescriptor
      0,                                                // InterfacesCount
      NULL,                                             // Interfaces
      0,                                                // MediumsCount
      NULL,                                             // Mediums
      SIZEOF_ARRAY(MicInTopoPinDataRangePointersBridge),// DataRangesCount
      MicInTopoPinDataRangePointersBridge,              // DataRanges
      KSPIN_DATAFLOW_OUT,                               // DataFlow
      KSPIN_COMMUNICATION_NONE,                         // Communication
      &KSCATEGORY_AUDIO,                                // Category
      NULL,                                             // Name
      0                                                 // Reserved
    }
  }
};

//=============================================================================
// Only return a KSJACK_DESCRIPTION for the physical bridge pin.
static 
PKSJACK_DESCRIPTION MicInJackDescriptions[] =
{
    &MicInJackDesc,
    NULL
};

//=============================================================================
// 不暴露硬件音量/静音节点：由 Windows 音频引擎做软件音量，
// 这样系统里的麦克风音量滑块对环回数据真正生效。
static
PCCONNECTION_DESCRIPTOR MicInMiniportConnections[] =
{
    //  FromNode,                 FromPin,                    ToNode,                 ToPin
    {   PCFILTER_NODE,            KSPIN_TOPO_MIC_ELEMENTS,    PCFILTER_NODE,          KSPIN_TOPO_BRIDGE }
};

//=============================================================================
static
PCPROPERTY_ITEM MicInPropertiesTopoFilter[] =
{
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_MicInTopoFilter
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_MicInTopoFilter
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationMicInTopoFilter, MicInPropertiesTopoFilter);

//=============================================================================
static
PCFILTER_DESCRIPTOR MicInTopoMiniportFilterDescriptor =
{
  0,                                        // Version
  &AutomationMicInTopoFilter,               // AutomationTable
  sizeof(PCPIN_DESCRIPTOR),                 // PinSize
  SIZEOF_ARRAY(MicInTopoMiniportPins),      // PinCount
  MicInTopoMiniportPins,                    // Pins
  sizeof(PCNODE_DESCRIPTOR),                // NodeSize
  0,                                        // NodeCount
  NULL,                                     // Nodes
  SIZEOF_ARRAY(MicInMiniportConnections),   // ConnectionCount
  MicInMiniportConnections,                 // Connections
  0,                                        // CategoryCount
  NULL                                      // Categories
};

#endif // _SYSVAD_MICINTOPTABLE_H_

