/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../ShengBattmgr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {++checks; assert(x);} while (0)
static void put(uint8_t *p, uint32_t v) {for (unsigned i=0;i<4;++i) p[i]=v>>(8*i);}
static unsigned get(const uint8_t *p) {return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
static const uint32_t required = (1u<<BmStatus)|(1u<<BmPresent)|(1u<<BmCapacity)|
    (1u<<BmVoltage)|(1u<<BmCurrent)|(1u<<BmTemperature)|(1u<<BmUsbOnline);
static void reply(uint8_t *p, const uint8_t *q, unsigned percent, unsigned plugged)
{
    memcpy(p,q,12);unsigned property=get(q+16),op=get(q+8),v=0;
    if (op==SHENG_BATTMGR_OP_USB_GET) {if (!property) v=plugged;}
    else if (property==0) v=plugged?1:2;
    else if (property==2) v=1;
    else if (property==4) v=percent*100;
    else if (property==7) v=4000000;
    else if (property==9) v=plugged?(unsigned)-1000000:1000000;
    else if (property==13) v=2500;
    else if (property==18 || property==17) v=8000000;
    put(p+12,property);put(p+16,v);put(p+20,0);
}
static void notify(ShengBattmgr *b)
{
    uint8_t p[16]={0};put(p,SHENG_BATTMGR_OWNER);put(p+4,2);put(p+8,7);
    CHECK(sheng_battmgr_receive(b,p,sizeof p)==BmNotified);
}
/* Drop each supported field in turn; then unplug and change capacity. The next
 * complete cycle must publish the new values without reinitializing transport. */
int main(void)
{
    unsigned scenarios=0;
    for (unsigned lost=0;lost<BmFields;++lost) {
        ShengBattmgr b;sheng_battmgr_init(&b);b.enabled=1;
        sheng_battmgr_refresh(&b,1);
        uint8_t q[24],p[24],late[24];int dropped=0,done=0;
        while (sheng_battmgr_next(&b,q)) {
            unsigned field=b.cursor;reply(p,q,70,1);
            int result;
            if (field==lost) {
                memcpy(late,p,sizeof late);dropped=1;
                result=sheng_battmgr_timeout(&b);
                CHECK(!b.poisoned && !(b.valid&(1u<<field)));
            } else {
                if (dropped) {
                    /* A late answer for a different key cannot satisfy q. */
                    CHECK(sheng_battmgr_receive(&b,late,sizeof late)==BmIgnored);
                    CHECK(b.in_flight && b.cursor==field);
                }
                result=sheng_battmgr_receive(&b,p,sizeof p);
            }
            if (result==BmCycleDone) {done=1;break;}
        }
        if (!dropped) continue; // Unverified board properties are intentionally skipped.
        ++scenarios;CHECK(done && b.cycles_done==1 && b.timeouts==1);
        CHECK(((b.valid&required)==required)==!(required&(1u<<lost)));
        CHECK(sheng_battmgr_receive(&b,late,sizeof late)==BmIgnored);
        notify(&b);done=0;
        /* Slow fields invalidated by a timeout are explicitly retried too. */
        sheng_battmgr_refresh(&b,1);
        while (sheng_battmgr_next(&b,q)) {
            reply(p,q,71,0);
            int result=sheng_battmgr_receive(&b,p,sizeof p);
            if (result==BmCycleDone) {done=1;break;}
        }
        CHECK(done && (b.valid&required)==required && b.timeouts==1);
        ShengBatterySnapshot s;sheng_battmgr_snapshot(&b,&s);
        CHECK(s.percent==71 && !s.external && !s.charging && s.current_ma==-1000);
        /* Forty-five minutes of periodic cycles, including repeated plug changes. */
        for (unsigned minute=0;minute<45;++minute) {
            unsigned plugged=minute%2,percent=72+minute/10;
            sheng_battmgr_refresh(&b,minute%10==0);
            done=0;
            while (sheng_battmgr_next(&b,q)) {
                reply(p,q,percent,plugged);
                if (sheng_battmgr_receive(&b,p,sizeof p)==BmCycleDone) {done=1;break;}
            }
            CHECK(done && (b.valid&required)==required && !b.poisoned);
            sheng_battmgr_snapshot(&b,&s);
            CHECK(s.percent==(int)percent && s.external==(int)plugged && s.charging==(int)plugged);
        }
    }
    CHECK(scenarios==22);
    ShengBattmgr b;sheng_battmgr_init(&b);b.notification_pending=1;
    CHECK(sheng_battmgr_timeout(&b)==BmAnswer && !b.poisoned && !b.notification_pending);
    b.notification_pending=1;uint8_t ack[12]={0};put(ack,32778);put(ack+4,2);put(ack+8,4);
    CHECK(sheng_battmgr_receive(&b,ack,sizeof ack)==BmEnabled && b.enabled);
    b.poisoned=1;CHECK(sheng_battmgr_timeout(&b)==BmIgnored);
    printf("PASS battery loss/retry/late-key rejection/45-minute simulation: %u checks, %u fields\n",checks,scenarios);
}
