/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_audio_ps5.h"
#include <assert.h>
#include <string.h>

static unsigned inits,opens,volumes,outputs,closes,states;
static int expect_user=0xff,expect_type=0;
static int16_t last[PW_AUDIO_PS5_GRAIN*2];

static int op_init(void){inits++;return 0;}
static int op_open(int u,int t,int i,uint32_t g,uint32_t r,uint32_t f)
{assert(u==expect_user&&t==expect_type&&i==0&&g==256&&r==48000&&f==1);opens++;return 7;}
static int op_volume(int h,int flags,const int32_t *v)
{assert(h==7&&flags==3&&v[0]==0x8000&&v[7]==0x8000);volumes++;return 0;}
static int op_output(int h,const void *p)
{
    assert(h==7);memcpy(last,p,sizeof(last));outputs++;return 0;
}
static int op_close(int h){assert(h==7);closes++;return 0;}
static int op_state(int h,PwAudioPs5PortState *s)
{
    assert(h==7);states++;
    s->output=PW_AUDIO_ROUTE_OUTPUT_HEADPHONE;s->reroute_counter=3;s->volume=-1;s->slack[31]=9;
    return 0;
}
static int fail_state(int h,PwAudioPs5PortState *s){(void)h;s->output=0x40;return -5;}

static int fail_init(void){inits++;return -1;}
static int fail_open(int u,int t,int i,uint32_t g,uint32_t r,uint32_t f)
{(void)u;(void)t;(void)i;(void)g;(void)r;(void)f;opens++;return -2;}
static int fail_volume(int h,int flags,const int32_t *v)
{(void)flags;(void)v;assert(h==7);volumes++;return -3;}

/* The bare port Wine's audio sink plays grains on. */
static void test_open_port(void)
{
    PwAudioPs5Ops ops={op_init,op_open,op_volume,op_output,op_close,NULL};
    int handle=-1;
    inits=opens=volumes=outputs=closes=0;
    assert(pw_audio_ps5_open_port(NULL,&handle)==PW_ERR_PRECONDITION);
    assert(pw_audio_ps5_open_port(&ops,NULL)==PW_ERR_PRECONDITION);
    PwAudioPs5Ops partial=ops;partial.volume=NULL;
    assert(pw_audio_ps5_open_port(&partial,&handle)==PW_ERR_PRECONDITION);
    assert(inits==0 && handle==-1);
    assert(pw_audio_ps5_open_port(&ops,&handle)==PW_OK);
    assert(handle==7 && inits==1 && opens==1 && volumes==1 && closes==0);
    int16_t grain[PW_AUDIO_PS5_GRAIN*2];
    for(int i=0;i<PW_AUDIO_PS5_GRAIN*2;i++)grain[i]=(int16_t)i;
    assert(ops.output(handle,grain)==0 && outputs==1 && last[511]==511);

    handle=-1;
    PwAudioPs5Ops broken=ops;broken.init=fail_init;
    assert(pw_audio_ps5_open_port(&broken,&handle)==PW_ERR_STATE && opens==1 && handle==-1);
    broken=ops;broken.open=fail_open;
    assert(pw_audio_ps5_open_port(&broken,&handle)==PW_ERR_STATE && volumes==1 && handle==-1);
    /* a port whose volume cannot be set is closed again */
    broken=ops;broken.volume=fail_volume;
    assert(pw_audio_ps5_open_port(&broken,&handle)==PW_ERR_STATE && closes==1 && handle==-1);
    inits=opens=volumes=outputs=closes=0;
}

/* Another user's port of another type, and what the console answered. */
static void test_open_port_as(void)
{
    PwAudioPs5Ops ops={op_init,op_open,op_volume,op_output,op_close,NULL};
    int handle=-1,result=0;
    expect_user=0x1234;expect_type=PW_AUDIO_PS5_PORT_PERSONAL;
    assert(pw_audio_ps5_open_port_as(&ops,0x1234,PW_AUDIO_PS5_PORT_PERSONAL,&handle,&result)==PW_OK);
    assert(handle==7 && result==7);
    handle=-1;
    PwAudioPs5Ops broken=ops;broken.open=fail_open;
    assert(pw_audio_ps5_open_port_as(&broken,0x1234,PW_AUDIO_PS5_PORT_PERSONAL,&handle,&result)==PW_ERR_STATE);
    assert(result==-2 && handle==-1);
    broken=ops;broken.init=fail_init;
    assert(pw_audio_ps5_open_port_as(&broken,0x1234,3,&handle,&result)==PW_ERR_STATE && result==-1);
    assert(pw_audio_ps5_open_port_as(NULL,0,0,&handle,&result)==PW_ERR_PRECONDITION && result==PW_ERR_PRECONDITION);
    expect_user=0xff;expect_type=0;
    inits=opens=volumes=outputs=closes=0;
}

/* The router's view of a port's state, and the raw bytes for the log. */
static void test_port_state(void)
{
    PwAudioPs5Ops ops={op_init,op_open,op_volume,op_output,op_close,op_state};
    PwAudioPs5PortState raw;
    int status=1;
    PwAudioRouteState state=pw_audio_ps5_port_state(&ops,7,&status,&raw);
    assert(status==0 && state.known && state.output==0x40 && state.reroute_counter==3);
    assert(raw.volume==-1 && raw.slack[31]==9 && states==1);
    state=pw_audio_ps5_port_state(&ops,-1,&status,NULL);
    assert(!state.known && status==PW_ERR_UNSUPPORTED && states==1);
    ops.port_state=NULL;
    state=pw_audio_ps5_port_state(&ops,7,&status,NULL);
    assert(!state.known && status==PW_ERR_UNSUPPORTED);
    ops.port_state=fail_state;
    state=pw_audio_ps5_port_state(&ops,7,NULL,&raw);
    assert(!state.known && state.output==0);
    state=pw_audio_ps5_port_state(NULL,7,&status,NULL);
    assert(!state.known && status==PW_ERR_UNSUPPORTED);
    _Static_assert(sizeof(PwAudioPs5PortState)>=32,"the PS4 layout fits");
}

int main(void)
{
    test_open_port();
    test_open_port_as();
    test_port_state();
    PwAudioPs5Ops ops;
    assert(pw_audio_ps5_platform_ops(NULL)==PW_ERR_PRECONDITION);
    /* The host build has no console library behind the ops. */
    assert(pw_audio_ps5_platform_ops(&ops)==PW_ERR_UNSUPPORTED && !ops.init && !ops.output);
    return 0;
}
