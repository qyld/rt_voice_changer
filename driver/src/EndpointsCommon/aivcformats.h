/*++

Module Name:

    aivcformats.h

Abstract:

    AI 变声器虚拟声卡：渲染端与采集端共用的设备格式表。
    采样率 44100 / 48000 Hz，单/双声道，16 / 24 / 24-in-32 / 32 bit 整数 PCM。

--*/

#ifndef _AIVC_FORMATS_H_
#define _AIVC_FORMATS_H_

#define AIVC_MAX_CHANNELS           2
#define AIVC_MIN_BITS_PER_SAMPLE    16
#define AIVC_MAX_BITS_PER_SAMPLE    32
#define AIVC_MIN_SAMPLE_RATE        44100
#define AIVC_MAX_SAMPLE_RATE        48000

#define AIVC_PCM_FORMAT(_ch, _mask, _rate, _container, _valid)                          \
    {                                                                                   \
        {                                                                               \
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),                                  \
            0,                                                                          \
            0,                                                                          \
            0,                                                                          \
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                                      \
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),                                     \
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)                           \
        },                                                                              \
        {                                                                               \
            {                                                                           \
                WAVE_FORMAT_EXTENSIBLE,                                                 \
                (_ch),                                                                  \
                (_rate),                                                                \
                (_rate) * (_ch) * ((_container) / 8),                                   \
                (_ch) * ((_container) / 8),                                             \
                (_container),                                                           \
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)                     \
            },                                                                          \
            (_valid),                                                                   \
            (_mask),                                                                    \
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)                                      \
        }                                                                               \
    }

#define AIVC_FORMATS_FOR_RATE(_rate)                                                    \
    AIVC_PCM_FORMAT(2, KSAUDIO_SPEAKER_STEREO, _rate, 16, 16),                          \
    AIVC_PCM_FORMAT(2, KSAUDIO_SPEAKER_STEREO, _rate, 24, 24),                          \
    AIVC_PCM_FORMAT(2, KSAUDIO_SPEAKER_STEREO, _rate, 32, 24),                          \
    AIVC_PCM_FORMAT(2, KSAUDIO_SPEAKER_STEREO, _rate, 32, 32),                          \
    AIVC_PCM_FORMAT(1, KSAUDIO_SPEAKER_MONO,   _rate, 16, 16),                          \
    AIVC_PCM_FORMAT(1, KSAUDIO_SPEAKER_MONO,   _rate, 24, 24),                          \
    AIVC_PCM_FORMAT(1, KSAUDIO_SPEAKER_MONO,   _rate, 32, 24),                          \
    AIVC_PCM_FORMAT(1, KSAUDIO_SPEAKER_MONO,   _rate, 32, 32)

//
// 索引 0 = 48000Hz / 立体声 / 16bit，作为各模式的默认格式。
//
#define AIVC_DEFAULT_FORMAT_INDEX   0

#define AIVC_DECLARE_FORMAT_TABLE(_name)                                                \
    static KSDATAFORMAT_WAVEFORMATEXTENSIBLE _name[] =                                  \
    {                                                                                   \
        AIVC_FORMATS_FOR_RATE(48000),                                                   \
        AIVC_FORMATS_FOR_RATE(44100)                                                    \
    }

#endif // _AIVC_FORMATS_H_
