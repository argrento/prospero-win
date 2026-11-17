/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_audio_capture.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static PwAudioMixFormat pcm(uint32_t rate,uint16_t channels,uint16_t bits)
{
    return (PwAudioMixFormat){rate,channels,bits,(uint16_t)(channels*bits/8u),PW_AUDIO_MIX_PCM,0};
}

static int16_t s16(const uint8_t *ring,uint32_t frame,uint32_t channel,uint32_t channels)
{
    const uint8_t *at=ring+2*(frame*channels+channel);
    return (int16_t)(uint16_t)(at[0]|at[1]<<8);
}

static void test_init_errors(void)
{
    PwAudioCapture c;uint8_t ring[16];
    PwAudioMixFormat f=pcm(48000,1,16),bad=pcm(48000,1,12);
    assert(pw_audio_capture_init(NULL,&f,ring,4)==PW_ERR_PRECONDITION);
    assert(pw_audio_capture_init(&c,&f,NULL,4)==PW_ERR_PRECONDITION);
    assert(pw_audio_capture_init(&c,&f,ring,0)==PW_ERR_PRECONDITION);
    assert(pw_audio_capture_init(&c,NULL,ring,4)==PW_ERR_PRECONDITION);
    assert(pw_audio_capture_init(&c,&bad,ring,4)==PW_ERR_UNSUPPORTED);
    assert(pw_audio_capture_init(&c,&f,ring,4)==PW_OK);
    assert(c.held==0 && c.offset==0 && c.written==0 && c.source_rate==0);
    int16_t one=1000;
    assert(pw_audio_capture_write(NULL,&one,1,48000)==0);
    assert(pw_audio_capture_write(&c,&one,1,999)==0);
    assert(pw_audio_capture_write(&c,&one,1,384001)==0);
    assert(c.held==0 && c.source_rate==0);
    pw_audio_capture_reset(NULL);
    pw_audio_capture_consume(NULL,1);
}

/* Same rate, mono 16-bit: the samples pass through, one frame late. */
static void test_passthrough(void)
{
    PwAudioCapture c;uint8_t ring[2*8];
    PwAudioMixFormat f=pcm(48000,1,16);
    const int16_t in[4]={16384,-16384,32767,-32768};
    assert(pw_audio_capture_init(&c,&f,ring,8)==PW_OK);
    assert(pw_audio_capture_write(&c,in,4,48000)==4);
    assert(c.held==4 && c.written==4 && c.source_rate==48000);
    assert(s16(ring,0,0,1)==0);
    assert(s16(ring,1,0,1)==16384 && s16(ring,2,0,1)==-16384);
    assert(s16(ring,3,0,1)==32767);
    assert(pw_audio_capture_write(&c,in,1,48000)==1 && s16(ring,4,0,1)==-32768);
    pw_audio_capture_consume(&c,3);
    assert(c.offset==3 && c.held==2);
    pw_audio_capture_consume(&c,9);
    assert(c.offset==5 && c.held==0);
}

/* A mono microphone fills every channel. */
static void test_channels(void)
{
    PwAudioCapture c;uint8_t ring[2*2*4];
    PwAudioMixFormat f=pcm(48000,2,16);
    const int16_t in[2]={8192,-8192};
    assert(pw_audio_capture_init(&c,&f,ring,4)==PW_OK);
    assert(pw_audio_capture_write(&c,in,2,48000)==2);
    assert(s16(ring,1,0,2)==8192 && s16(ring,1,1,2)==8192);
}

static void test_encodings(void)
{
    PwAudioCapture c;uint8_t ring[4*4*2];
    PwAudioMixFormat f;
    const int16_t in[3]={0,16384,-32768};

    f=pcm(48000,1,8);
    assert(pw_audio_capture_init(&c,&f,ring,4)==PW_OK);
    assert(pw_audio_capture_write(&c,in,3,48000)==3);
    assert(ring[0]==128 && ring[1]==128 && ring[2]==192);   /* silence is 128 */
    assert(pw_audio_capture_write(&c,in,1,48000)==1 && ring[3]==0);  /* -1 */

    f=pcm(48000,1,24);
    assert(pw_audio_capture_init(&c,&f,ring,4)==PW_OK);
    assert(pw_audio_capture_write(&c,in,3,48000)==3);
    int32_t v=(int32_t)((uint32_t)ring[6]<<8|(uint32_t)ring[7]<<16|(uint32_t)ring[8]<<24)>>8;
    assert(v==4194304);

    f=pcm(48000,1,32);
    assert(pw_audio_capture_init(&c,&f,ring,4)==PW_OK);
    assert(pw_audio_capture_write(&c,in,3,48000)==3);
    int32_t w;memcpy(&w,ring+8,4);
    assert(w==1073741824);
    memcpy(&w,ring+4,4);
    assert(w==0);

    f=pcm(48000,2,32);f.encoding=PW_AUDIO_MIX_FLOAT;
    assert(pw_audio_capture_init(&c,&f,ring,4)==PW_OK);
    assert(pw_audio_capture_write(&c,in,3,48000)==3);
    float x[2];memcpy(x,ring+16,8);
    assert(x[0]==0.5f && x[1]==0.5f);
    assert(pw_audio_capture_write(&c,in,1,48000)==1);
    memcpy(x,ring+24,8);
    assert(x[0]==-1 && x[1]==-1);
}

/* 16 kHz into 48 kHz: three client frames per microphone frame. */
static void test_upsample(void)
{
    PwAudioCapture c;uint8_t ring[2*16];
    PwAudioMixFormat f=pcm(48000,1,16);
    const int16_t in[2]={-3000,3000};
    assert(pw_audio_capture_init(&c,&f,ring,16)==PW_OK);
    assert(pw_audio_capture_write(&c,in,2,16000)==6);
    const int16_t expected[6]={0,-1000,-2000,-3000,-1000,1000};
    for(int i=0;i<6;i++)assert(abs(s16(ring,i,0,1)-expected[i])<=1);
    /* the microphone reopens at another rate: the step follows */
    assert(pw_audio_capture_write(&c,in,2,48000)==2 && c.source_rate==48000);
}

/* 48 kHz into 24 kHz: every other microphone frame. */
static void test_downsample(void)
{
    PwAudioCapture c;uint8_t ring[2*16];
    PwAudioMixFormat f=pcm(24000,1,16);
    const int16_t in[6]={1000,2000,3000,4000,5000,6000};
    assert(pw_audio_capture_init(&c,&f,ring,16)==PW_OK);
    assert(pw_audio_capture_write(&c,in,6,48000)==3);
    assert(s16(ring,0,0,1)==0 && s16(ring,1,0,1)==2000 && s16(ring,2,0,1)==4000);
}

/* No microphone data (a failed or muted read): silence, and time still runs. */
static void test_silence(void)
{
    PwAudioCapture c;uint8_t ring[8];
    PwAudioMixFormat f=pcm(48000,1,8);
    assert(pw_audio_capture_init(&c,&f,ring,8)==PW_OK);
    memset(ring,0x55,sizeof(ring));
    assert(pw_audio_capture_write(&c,NULL,8,48000)==8);
    for(int i=0;i<8;i++)assert(ring[i]==128);
    assert(c.held==8);
}

/* A full ring keeps the newest frames; the ones the client reads stay. */
static void test_overrun(void)
{
    PwAudioCapture c;uint8_t ring[2*4];
    PwAudioMixFormat f=pcm(48000,1,16);
    const int16_t in[7]={100,200,300,400,500,600,700};
    assert(pw_audio_capture_init(&c,&f,ring,4)==PW_OK);
    assert(pw_audio_capture_write(&c,in,7,48000)==7);
    /* made: 0 100 200 300 400 500 600; kept: the last four */
    assert(c.held==4 && c.dropped==3 && c.written==7 && c.offset==3);
    assert(s16(ring,3,0,1)==300 && s16(ring,0,0,1)==400 && s16(ring,2,0,1)==600);
    /* while the client reads, a full ring loses the new frames, not its data */
    c.locked=2;
    uint8_t before[sizeof(ring)];memcpy(before,ring,sizeof(ring));
    assert(pw_audio_capture_write(&c,in,3,48000)==3);
    assert(c.held==4 && c.dropped==6 && c.offset==3 && !memcmp(before,ring,sizeof(ring)));
    pw_audio_capture_consume(&c,2);
    assert(c.locked==0 && c.held==2 && c.offset==1);
    /* then the oldest go again: made 300 400 500, the ring keeps 600 300 400 500 */
    assert(pw_audio_capture_write(&c,in+3,3,48000)==3);
    assert(c.held==4 && c.dropped==7 && c.offset==2);
    assert(s16(ring,2,0,1)==600 && s16(ring,3,0,1)==300 && s16(ring,0,0,1)==400 && s16(ring,1,0,1)==500);
    /* every frame locked: new ones are dropped, the ring is untouched */
    pw_audio_capture_reset(&c);
    assert(c.held==0 && c.offset==0 && c.locked==0);
    assert(pw_audio_capture_write(&c,in,4,48000)==4);
    c.locked=4;
    memcpy(before,ring,sizeof(ring));
    assert(pw_audio_capture_write(&c,in,2,48000)==2);
    assert(!memcmp(before,ring,sizeof(ring)) && c.held==4);
}

/* The ring wraps; reset clears the resampler's history. */
static void test_wrap_and_reset(void)
{
    PwAudioCapture c;uint8_t ring[2*3];
    PwAudioMixFormat f=pcm(48000,1,16);
    const int16_t in[4]={10,20,30,40};
    assert(pw_audio_capture_init(&c,&f,ring,3)==PW_OK);
    assert(pw_audio_capture_write(&c,in,2,48000)==2);
    pw_audio_capture_consume(&c,2);
    assert(pw_audio_capture_write(&c,in+2,2,48000)==2);
    assert(c.offset==2 && c.held==2);
    assert(s16(ring,2,0,1)==20 && s16(ring,0,0,1)==30);
    pw_audio_capture_reset(&c);
    assert(pw_audio_capture_write(&c,in,1,48000)==1 && s16(ring,0,0,1)==0);
}

int main(void)
{
    test_init_errors();
    test_passthrough();
    test_channels();
    test_encodings();
    test_upsample();
    test_downsample();
    test_silence();
    test_overrun();
    test_wrap_and_reset();
    puts("audio capture passed");
    return 0;
}
