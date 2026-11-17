/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_audio_capture.h"
#include <string.h>

#define ONE ((uint64_t)1 << 32)

int pw_audio_capture_init(PwAudioCapture *capture,const PwAudioMixFormat *format,
                          uint8_t *frames,uint32_t capacity)
{
    if(!capture || !frames || !capacity)return PW_ERR_PRECONDITION;
    int status=pw_audio_mix_format_check(format);
    if(status!=PW_OK)return status;
    memset(capture,0,sizeof(*capture));
    capture->format=*format;capture->frames=frames;capture->capacity=capacity;
    return PW_OK;
}

void pw_audio_capture_reset(PwAudioCapture *capture)
{
    if(!capture)return;
    capture->offset=capture->held=capture->locked=0;
    capture->phase=0;
    capture->previous=capture->current=0;
}

/* value times full scale (2^(bits-1)), rounded and clamped: a 16-bit sample
 * comes back as itself. */
static int32_t scaled(float value,double full)
{
    double x=(double)value*full;
    x=x>=0?x+0.5:x-0.5;
    return x>=full-1?(int32_t)(full-1):x<=-full?(int32_t)-full:(int32_t)x;
}

/* One frame of value (-1..1) on every channel, in the client's encoding. */
static void encode(const PwAudioMixFormat *format,uint8_t *at,float value)
{
    uint8_t sample[4];
    unsigned bytes=format->bits/8u;
    int32_t x;

    value=value>1?1:value<-1?-1:value;
    switch(format->bits) {
    case 8:
        x=scaled(value,128)+128;
        sample[0]=(uint8_t)x;
        break;
    case 16:
        x=scaled(value,32768);
        sample[0]=(uint8_t)x;sample[1]=(uint8_t)((uint32_t)x>>8);
        break;
    case 24:
        x=scaled(value,8388608);
        sample[0]=(uint8_t)x;sample[1]=(uint8_t)((uint32_t)x>>8);sample[2]=(uint8_t)((uint32_t)x>>16);
        break;
    default:
        if(format->encoding==PW_AUDIO_MIX_FLOAT) {
            memcpy(sample,&value,sizeof(value));
            break;
        }
        x=scaled(value,2147483648.0);
        for(unsigned i=0;i<4;i++)sample[i]=(uint8_t)((uint32_t)x>>(8*i));
        break;
    }
    for(unsigned channel=0;channel<format->channels;channel++)
        memcpy(at+channel*bytes,sample,bytes);
}

static void put(PwAudioCapture *capture,float value)
{
    if(capture->held==capture->capacity) {
        /* full: the oldest frame goes, unless the client is reading it (the
         * locked frames are the oldest): then the new one does */
        if(capture->locked){capture->dropped++;return;}
        if(++capture->offset==capture->capacity)capture->offset=0;
        capture->held--;capture->dropped++;
    }
    uint32_t at=(capture->offset+capture->held)%capture->capacity;
    encode(&capture->format,capture->frames+(size_t)at*capture->format.block_align,value);
    capture->held++;capture->written++;
}

uint32_t pw_audio_capture_write(PwAudioCapture *capture,const int16_t *samples,uint32_t count,
                                uint32_t rate)
{
    uint32_t made=0;
    if(!capture || rate<PW_AUDIO_MIX_MIN_RATE || rate>PW_AUDIO_MIX_MAX_RATE)return 0;
    if(rate!=capture->source_rate) {
        capture->source_rate=rate;
        /* rounded up, so 16 kHz into 48 kHz makes exactly three frames each */
        capture->step=(((uint64_t)rate<<32)+capture->format.rate-1)/capture->format.rate;
    }
    for(uint32_t i=0;i<count;i++) {
        capture->previous=capture->current;
        capture->current=samples?samples[i]*(1.0f/32768):0;
        /* every client frame that falls between the last two source frames */
        while(capture->phase<ONE) {
            float t=(float)(uint32_t)capture->phase*(1.0f/4294967296.0f);
            put(capture,capture->previous+(capture->current-capture->previous)*t);
            made++;
            capture->phase+=capture->step;
        }
        capture->phase-=ONE;
    }
    return made;
}

void pw_audio_capture_consume(PwAudioCapture *capture,uint32_t frames)
{
    if(!capture)return;
    if(frames>capture->held)frames=capture->held;
    capture->offset=(capture->offset+frames)%capture->capacity;
    capture->held-=frames;
    capture->locked=0;
}
