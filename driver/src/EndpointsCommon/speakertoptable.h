/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    speakertoptable.h

Abstract:

    Declaration of topology tables.

--*/

#ifndef _SYSVAD_SPEAKERTOPTABLE_H_
#define _SYSVAD_SPEAKERTOPTABLE_H_

//
// 渲染端点名称 GUID，对应 INF 中 MediaCategories\{...}\Name
// {b4be05cf-0dde-4d56-910e-85cb75128fb3}
DEFINE_GUID(AIVC_RENDER_CUSTOM_NAME,
0xb4be05cf, 0x0dde, 0x4d56, 0x91, 0x0e, 0x85, 0xcb, 0x75, 0x12, 0x8f, 0xb3);

//=============================================================================
static
KSDATARANGE SpeakerTopoPinDataRangesBridge[] =
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
PKSDATARANGE SpeakerTopoPinDataRangePointersBridge[] =
{
  &SpeakerTopoPinDataRangesBridge[0]
};

//=============================================================================
static
PCPIN_DESCRIPTOR SpeakerTopoMiniportPins[] =
{
  // KSPIN_TOPO_WAVEOUT_SOURCE
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
      SIZEOF_ARRAY(SpeakerTopoPinDataRangePointersBridge),// DataRangesCount
      SpeakerTopoPinDataRangePointersBridge,            // DataRanges
      KSPIN_DATAFLOW_IN,                                // DataFlow
      KSPIN_COMMUNICATION_NONE,                         // Communication
      &KSCATEGORY_AUDIO,                                // Category
      NULL,                                             // Name
      0                                                 // Reserved
    }
  },
  // KSPIN_TOPO_LINEOUT_DEST
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
      SIZEOF_ARRAY(SpeakerTopoPinDataRangePointersBridge),// DataRangesCount
      SpeakerTopoPinDataRangePointersBridge,            // DataRanges
      KSPIN_DATAFLOW_OUT,                               // DataFlow
      KSPIN_COMMUNICATION_NONE,                         // Communication
      &KSNODETYPE_SPEAKER,                              // Category
      &AIVC_RENDER_CUSTOM_NAME,                         // Name -> INF MediaCategories "AI变声器 内部输出"
      0                                                 // Reserved
    }
  }
};

//=============================================================================
static
KSJACK_DESCRIPTION SpeakerJackDescBridge =
{
    KSAUDIO_SPEAKER_STEREO,
    0xB3C98C,               // Color spec for green
    eConnTypeUnknown,
    eGeoLocFront,
    eGenLocPrimaryBox,
    ePortConnIntegratedDevice,
    TRUE
};

// Only return a KSJACK_DESCRIPTION for the physical bridge pin.
static 
PKSJACK_DESCRIPTION SpeakerJackDescriptions[] =
{
    NULL,
    &SpeakerJackDescBridge
};

static SYSVAD_AUDIOPOSTURE_INFO SpeakerAudioPostureInfo = { TRUE };

// Only support property for the physical bridge pin.
static
PSYSVAD_AUDIOPOSTURE_INFO SpeakerAudioPostureInfoPointers[]
{
    NULL,
    &SpeakerAudioPostureInfo
};

//=============================================================================
static
PCCONNECTION_DESCRIPTOR SpeakerTopoMiniportConnections[] =
{
  //  FromNode,                     FromPin,                        ToNode,                      ToPin
  {   PCFILTER_NODE,                KSPIN_TOPO_WAVEOUT_SOURCE,      PCFILTER_NODE,               KSPIN_TOPO_LINEOUT_DEST}
};


//=============================================================================
static
PCPROPERTY_ITEM PropertiesSpeakerTopoFilter[] =
{
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        KSPROPERTY_TYPE_GET |
        KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_SpeakerTopoFilter
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        KSPROPERTY_TYPE_GET |
        KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_SpeakerTopoFilter
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION3,
        KSPROPERTY_TYPE_GET |
        KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_SpeakerTopoFilter
    },
    {
        &KSPROPSETID_AudioResourceManagement,
        KSPROPERTY_AUDIORESOURCEMANAGEMENT_RESOURCEGROUP,
        KSPROPERTY_TYPE_SET,
        PropertyHandler_SpeakerTopoFilter
    }
    ,{
        &KSPROPSETID_AudioPosture,
        KSPROPERTY_AUDIOPOSTURE_ORIENTATION,
        KSPROPERTY_TYPE_SET |
        KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_SpeakerTopoFilter
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationSpeakerTopoFilter, PropertiesSpeakerTopoFilter);

//=============================================================================
static
PCFILTER_DESCRIPTOR SpeakerTopoMiniportFilterDescriptor =
{
  0,                                            // Version
  &AutomationSpeakerTopoFilter,                 // AutomationTable
  sizeof(PCPIN_DESCRIPTOR),                     // PinSize
  SIZEOF_ARRAY(SpeakerTopoMiniportPins),        // PinCount
  SpeakerTopoMiniportPins,                      // Pins
  sizeof(PCNODE_DESCRIPTOR),                    // NodeSize
  0,                                            // NodeCount
  NULL,                                         // Nodes
  SIZEOF_ARRAY(SpeakerTopoMiniportConnections), // ConnectionCount
  SpeakerTopoMiniportConnections,               // Connections
  0,                                            // CategoryCount
  NULL                                          // Categories
};

#endif // _SYSVAD_SPEAKERTOPTABLE_H_

