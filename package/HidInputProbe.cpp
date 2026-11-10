// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off original HID observation; a separate explicit gate starts an
// owned offline input source. No entitlement or original provider is replaced.
#include "UkeClassRegistration.h"
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSKext.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" IOService *native_hid_buttons_allocate(void);
extern "C" {
extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
volatile uint32_t native_hid_stage,native_hid_resource_found;
volatile int native_hid_load_status=-1;
volatile int native_hid_matching_load_status=-1;
volatile uint32_t native_hid_class_sizes[5];
volatile int native_hid_input_meta_status=-1;
volatile uint64_t native_hid_input_object;
volatile uint32_t native_hid_input_events[8];
volatile int native_hid_input_results[8]={-1,-1,-1,-1,-1,-1,-1,-1};
volatile uint32_t native_hid_input_command;
char native_hid_input_registry[2048];
char native_hid_input_props[1024];
extern volatile uint32_t native_legacy_transfer_calls;
// Optional unattended Home presses for hardware, which has no external harness writing the mailbox:
// offline-hid-auto-home=<seconds after the second presented frame>, offline-hid-auto-home-count (default 1)
// and offline-hid-auto-home-interval (default 20 s). Disabled unless the first argument is set.
static unsigned autoConfigured,autoFirst,autoCount,autoInterval,autoSent;
static uint64_t autoNext,autoFrequency;
static uint64_t counterNow(void){uint64_t v;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(v));return v;}
bool native_hid_input_requested(void) {
    if(native_legacy_transfer_calls<2)return false;
    if(!autoConfigured) {
        autoConfigured=1;autoFirst=0;autoCount=1;autoInterval=20;
        PE_parse_boot_argn("offline-hid-auto-home",&autoFirst,sizeof(autoFirst));
        PE_parse_boot_argn("offline-hid-auto-home-count",&autoCount,sizeof(autoCount));
        PE_parse_boot_argn("offline-hid-auto-home-interval",&autoInterval,sizeof(autoInterval));
        if(autoCount>8)autoCount=8;if(autoInterval<5)autoInterval=5;
        __asm__ volatile("mrs %0, cntfrq_el0":"=r"(autoFrequency));
        if(autoFirst && autoFrequency)autoNext=counterNow()+(uint64_t)autoFirst*autoFrequency;
    }
    if(autoFirst && autoFrequency && autoSent<autoCount && counterNow()>=autoNext) {
        ++autoSent;autoNext+=(uint64_t)autoInterval*autoFrequency;return true;
    }
    if(native_hid_input_command!=1)return false;
    native_hid_input_command=0;return true;
}
static void describeInput(IORegistryEntry *entry,unsigned depth,size_t &used) {
    if(depth>4 || used>=sizeof(native_hid_input_registry)-128)return;
    int count=snprintf(native_hid_input_registry+used,sizeof(native_hid_input_registry)-used,
        "%u %s [%s]\n",depth,entry->getName(),entry->getMetaClass()->getClassName());
    if(count<=0 || (size_t)count>=sizeof(native_hid_input_registry)-used)return;
    used+=count;OSIterator *it=entry->getChildIterator(gIOServicePlane);
    if(!it)return;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *next=OSDynamicCast(IORegistryEntry,child);
        if(next)describeInput(next,depth+1,used);
    }
    it->release();
}
// Compact one property of the first IOHIDEventDriver below entry: XML tags dropped, key end -> '=', value end -> ' '.
static IORegistryEntry *findEventDriver(IORegistryEntry *parent,unsigned depth) {
    if(!depth)return nullptr;
    OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
    IORegistryEntry *found=nullptr;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
        if(!entry)continue;
        if(entry->metaCast("IOHIDEventDriver")){found=entry;break;}
        if((found=findEventDriver(entry,depth-1)))break;
    }
    it->release();return found;
}
// The internal Consumer Power key is carried by the upstream keyboard service.
// Apple's iOS keyboard filter delays that key by 150 ms unless this per-service
// parameter is zero. Do not change global keyboard preferences or report data.
extern "C" {volatile int32_t native_button_policy_stats[6];}
static uint64_t buttonPolicyService;
static uint64_t buttonPolicyAttemptedService;
static unsigned buttonPolicyAttempts;
static bool publishButtonPolicy(IORegistryEntry *entry) {
    OSObject *old=entry->copyProperty("HIDEventServiceProperties");
    OSDictionary *existing=OSDynamicCast(OSDictionary,old);
    OSDictionary *properties=existing?OSDictionary::withDictionary(existing):OSDictionary::withCapacity(1);
    OSNumber *zero=OSNumber::withNumber((unsigned long long)0,32);
    const bool ok=properties && zero && properties->setObject("LockKeyDelay",zero) &&
        entry->setProperty("HIDEventServiceProperties",properties);
    OSSafeReleaseNULL(zero);OSSafeReleaseNULL(properties);OSSafeReleaseNULL(old);
    return ok;
}
static void applyButtonPolicy(IOService *device,IOService *service) {
    if(!device || (uintptr_t)device!=native_hid_input_object || !service)return;
    const uint64_t identity=service->getRegistryEntryID();
    if(!identity || identity==buttonPolicyService)return;
    if(identity!=buttonPolicyAttemptedService) {buttonPolicyAttemptedService=identity;buttonPolicyAttempts=0;}
    if(buttonPolicyAttempts>=30)return;
    ++buttonPolicyAttempts;
    native_button_policy_stats[0]=1;
    OSDictionary *parameters=OSDictionary::withCapacity(2);
    OSNumber *zero=OSNumber::withNumber((unsigned long long)0,32);
    IOReturn status=kIOReturnNoMemory;
    if(parameters && zero && parameters->setObject("HIDDeviceParameters",kOSBooleanTrue) &&
       parameters->setObject("LockKeyDelay",zero))status=service->setProperties(parameters);
    OSObject *value=service->copyProperty("HIDEventServiceProperties");
    OSDictionary *properties=OSDynamicCast(OSDictionary,value);
    OSNumber *delay=properties?OSDynamicCast(OSNumber,properties->getObject("LockKeyDelay")):nullptr;
    const bool verified=status==kIOReturnSuccess && delay && delay->unsigned32BitValue()==0;
    native_button_policy_stats[1]=(int32_t)status;
    native_button_policy_stats[2]=delay?(int32_t)delay->unsigned32BitValue():-1;
    native_button_policy_stats[3]=(int32_t)buttonPolicyAttempts;
    if(verified) {buttonPolicyService=identity;native_button_policy_stats[0]=2;}
    if(verified || buttonPolicyAttempts<=3)
        printf("UKE BTN policy: service=%llu status=%x LockKeyDelay=%d verified=%u attempt=%u\n",
               (unsigned long long)identity,(unsigned)status,native_button_policy_stats[2],(unsigned)verified,buttonPolicyAttempts);
    OSSafeReleaseNULL(value);OSSafeReleaseNULL(zero);OSSafeReleaseNULL(parameters);
}
static IOService *findButtonEventService(IORegistryEntry *parent,unsigned depth) {
    if(!depth)return nullptr;
    OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
    IOService *found=nullptr;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);if(!entry)continue;
        if(entry->metaCast("IOHIDEventService")) {
            // A composite HID device can have several interfaces. Match the
            // keyboard usage which activates Apple's Consumer Power delay;
            // never pick the adjacent digitizer service merely by tree order.
            OSObject *value=entry->copyProperty("DeviceUsagePairs");
            if(!value) {
                if(IORegistryEntry *interface=entry->getParentEntry(gIOServicePlane))
                    value=interface->copyProperty("DeviceUsagePairs");
            }
            OSArray *pairs=OSDynamicCast(OSArray,value);
            bool keyboard=false;
            for(unsigned i=0;pairs && i<pairs->getCount();++i) {
                OSDictionary *pair=OSDynamicCast(OSDictionary,pairs->getObject(i));
                OSNumber *page=pair?OSDynamicCast(OSNumber,pair->getObject("DeviceUsagePage")):nullptr;
                OSNumber *usage=pair?OSDynamicCast(OSNumber,pair->getObject("DeviceUsage")):nullptr;
                if(page && usage && page->unsigned32BitValue()==1 && usage->unsigned32BitValue()==6)keyboard=true;
            }
            OSSafeReleaseNULL(value);
            if(keyboard) {found=OSDynamicCast(IOService,entry);if(found)found->retain();}
        } else found=findButtonEventService(entry,depth-1);
        if(found)break;
    }
    it->release();return found;
}
static void appendProperty(IORegistryEntry *driver,const char *key,const char *label,size_t &used) {
    OSObject *value=driver->copyProperty(key);
    int n=snprintf(native_hid_input_props+used,sizeof(native_hid_input_props)-used,"%s:",label);
    if(n>0)used+=n;
    if(value) {
        OSSerialize *text=OSSerialize::withCapacity(4096);
        if(text && value->serialize(text)) {
            const char *in=text->text();bool tag=false;char name[16];unsigned nameLen=0;
            for(unsigned i=0;in && in[i] && used<sizeof(native_hid_input_props)-2;++i) {
                char c=in[i];
                if(c=='<'){tag=true;nameLen=0;continue;}
                if(tag) {
                    if(c=='>') {
                        tag=false;name[nameLen]=0;
                        if(!strcmp(name,"/key"))native_hid_input_props[used++]='=';
                        else if(name[0]=='/')native_hid_input_props[used++]=' ';
                        else if(!strcmp(name,"true/")||!strcmp(name,"false/")){
                            used+=snprintf(native_hid_input_props+used,sizeof(native_hid_input_props)-used,"%s ",name[0]=='t'?"T":"F");
                        }
                    } else if(nameLen<sizeof(name)-1)name[nameLen++]=c;
                    continue;
                }
                if(c=='\n'||c=='\t')continue;
                native_hid_input_props[used++]=c;
            }
        }
        OSSafeReleaseNULL(text);OSSafeReleaseNULL(value);
    } else if(used<sizeof(native_hid_input_props)-6) {memcpy(native_hid_input_props+used,"none ",5);used+=5;}
    native_hid_input_props[used]=0;
}
__attribute__((noinline)) void native_hid_live_checkpoint(IOService *device) {
    size_t used=0;native_hid_input_registry[0]=0;describeInput(device,0,used);
    if((uintptr_t)device==native_hid_input_object && !device->getProperty("UkePhysicalButtons")) {
        IOService *service=findButtonEventService(device,4);
        applyButtonPolicy(device,service);OSSafeReleaseNULL(service);
    }
    if(IORegistryEntry *driver=findEventDriver(device,4)) {
        size_t props=0;native_hid_input_props[0]=0;
        // backboardd's digitizer service classification reads Built-In/DisplayIntegrated from the event service it
        // opened, not from the HID device below it; publish them on the service itself (idempotent, once per second).
        // Set once only: every setProperty on a service with attached user clients is a registry property-change
        // notification, and repeating it each second keeps backboardd/hidd busy re-evaluating the service.
        static IORegistryEntry *publishedFor;
        if(device->getProperty("Built-In") && !device->getProperty("UkePhysicalButtons") && publishedFor!=driver) {
            publishedFor=driver;
            driver->setProperty("Built-In",kOSBooleanTrue);driver->setProperty("DisplayIntegrated",kOSBooleanTrue);
            // backboardd (-[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:], decoded from the target) drops a
            // digitizer event unless the sending service has PrimaryUsagePage 0x0D and PrimaryUsage 1, 4 or 0x20
            // ("ignoring digitizer event ... from unsupported service"). The combined device lists its keyboard collection
            // first, so the services would publish (1,6); publish Digitizer/Touch Screen on the services after they have
            // matched (setting it on the device would change driver matching, which is observed to depend on the layout).
            driver->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);driver->setProperty("PrimaryUsage",(unsigned long long)4,32);
            // backboardd's digitizer filter is {DeviceUsagePage, DeviceUsage, Built-In}; make the flag visible on the
            // interface (the event service's provider) as well, not only on the device two levels down.
            if(IORegistryEntry *interface=driver->getParentEntry(gIOServicePlane)) {
                interface->setProperty("Built-In",kOSBooleanTrue);interface->setProperty("DisplayIntegrated",kOSBooleanTrue);
                interface->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);interface->setProperty("PrimaryUsage",(unsigned long long)4,32);
            }
        }
        // The Digitizer/DebugState dumps (appendProperty) are not read on hardware runs without a photograph and walk
        // driver-internal state from a foreign thread; only the cheap Built-In readback stays.
        appendProperty(driver,"Built-In","BI",props);
        appendProperty(driver,"PrimaryUsagePage","PP",props);appendProperty(driver,"PrimaryUsage","PU",props);
    }
    __asm__ volatile("" ::: "memory");
}
// Upstream diagnostic ABI: allocated by BootDiagExport only while sampling.
volatile uint64_t *native_touch_report_ring;
volatile uint32_t native_touch_report_count;
extern uint64_t mach_absolute_time(void);
void native_hid_input_observed(unsigned event,int result) {
    if(event<8){native_hid_input_events[event]=native_hid_input_events[event]+1;native_hid_input_results[event]=result;}
    if(event==6 && native_touch_report_ring) {
        const uint32_t n=native_touch_report_count;
        native_touch_report_ring[n&65535]=mach_absolute_time();
        __atomic_store_n(&native_touch_report_count,n+1,__ATOMIC_RELEASE);
    }
}
__attribute__((noinline)) void native_hid_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
void native_hid_input_probe(void) {
    unsigned enabled=0;
    if(!PE_parse_boot_argn("offline-hid-input",&enabled,sizeof(enabled)) || enabled!=1)return;
    if(native_hid_stage)return;
    native_hid_stage=1;
    native_hid_load_status=OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if(!native_hid_load_status)
        native_hid_matching_load_status=OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
            true,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr);
    native_hid_stage=2;
    const char *names[]={"IOHIDUserDevice","IOHIDEventDriver","IOHIDEventService","IOHIDResourceDeviceUserClient","IOHIDDevice"};
    for(unsigned i=0;i<5;++i) {
        const OSSymbol *name=OSSymbol::withCString(names[i]);
        const OSMetaClass *meta=name?OSMetaClass::getMetaClassWithName(name):nullptr;
        if(meta)native_hid_class_sizes[i]=meta->getClassSize();
        OSSafeReleaseNULL(name);
    }
    if(native_hid_load_status==0) {
        OSDictionary *match=IOService::serviceMatching("IOHIDResource");
        IOService *resource=match?IOService::waitForMatchingService(match,5000000000ULL):nullptr;
        native_hid_resource_found=resource!=nullptr;
        unsigned sendHome=0;PE_parse_boot_argn("offline-hid-home",&sendHome,sizeof(sendHome));
        if(resource && sendHome==1 && native_hid_class_sizes[4]==208) {
            void *handle=ukeClassBegin(g_kernel_kmod_info.name);
            if(handle) {
                native_hid_static_init();native_hid_input_meta_status=ukeClassEnd(handle);
                if(!native_hid_input_meta_status) {
                    unsigned split=0,buttons=0;
                    PE_parse_boot_argn("sheng-touch-split",&split,sizeof(split));
                    PE_parse_boot_argn("sheng-buttons",&buttons,sizeof(buttons));
                    const bool physical=split==1 && buttons==1;
                    IOService *device=physical?native_hid_buttons_allocate():native_hid_input_allocate();
                    native_hid_input_object=(uintptr_t)device;
                    bool valid=device && device->metaCast("IOHIDDevice") && device->init();
                    native_hid_input_observed(0,valid?0:kIOReturnError);
                    bool attached=valid && device->attach(resource);
                    native_hid_input_observed(1,attached?0:kIOReturnError);
                    // Seed the property before matching; the live checkpoint
                    // also verifies/merges it on the actual event service.
                    native_button_policy_stats[4]=attached && !physical && publishButtonPolicy(device);
                    bool started=attached && device->start(resource);
                    if(started && physical)printf("UKE BTN: Consumer Control device started report=2 usage=c/1 keyboard=0\n");
                    if(!started) {
                        native_hid_input_observed(7,kIOReturnError);
                        if(attached)device->detach(resource);
                        OSSafeReleaseNULL(device);native_hid_input_object=0;
                    }
                    unsigned touchHid=0;
                    if(started && split==1 && PE_parse_boot_argn("sheng-touch-hid",&touchHid,sizeof(touchHid)) && touchHid==1) {
                        // Separate digitizer service (primary usage Digitizer/Touch Screen, Built-In).
                        IOService *touch=native_hid_touch_allocate();
                        bool touchStarted=touch && touch->metaCast("IOHIDDevice") && touch->init() && touch->attach(resource) && touch->start(resource);
                        if(!touchStarted && touch){touch->detach(resource);OSSafeReleaseNULL(touch);}
                    }
                    // The offline devices live for this bounded boot. Its
                    // service attachment and timer remain genuine live objects.
                }
            }
        }
        OSSafeReleaseNULL(resource);OSSafeReleaseNULL(match);
    }
    native_hid_stage=3;native_hid_checkpoint();
}
}
