// SPDX-License-Identifier: MIT
// Unpublished constructor/lifetime or required-dispatch observation only.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_original_provider_static_init(void) __asm__("__GLOBAL__sub_I_NativeIOGPUProviderProbe.cpp");
extern "C" IOService *native_original_provider_allocate(void);
extern "C" unsigned native_original_provider_instances(void);
extern "C" {
volatile uint32_t native_original_provider_stage,native_original_provider_allocated,native_original_provider_initialized;
volatile uint32_t native_original_provider_parent_match,native_original_provider_attached,native_original_provider_registered,native_original_provider_released;
volatile uint32_t native_original_provider_instances_before,native_original_provider_instances_live,native_original_provider_instances_after;
volatile uint32_t native_original_provider_size,native_original_provider_callback,native_original_provider_start_returned,native_original_provider_start_result;
volatile uint32_t native_original_provider_factory_returned,native_original_provider_client_type,native_original_provider_client_instances_before,native_original_provider_client_instances_live;
volatile uint32_t native_original_provider_client_instances_at_callback,native_original_provider_descriptor_refused,native_original_provider_descriptor_provisioned;
volatile int native_original_provider_factory_result;
volatile int native_original_provider_status=-1;
volatile uint64_t native_original_provider_object,native_original_provider_vptr,native_original_provider_parent,native_original_provider_backend;
volatile uint64_t native_original_provider_factory_out_client;
volatile uint64_t native_original_provider_frame[16];
static const OSMetaClass *clientMeta;
static unsigned probeMode;
class IOMemoryDescriptor;
__attribute__((noinline)) void native_original_provider_checkpoint(void) {__asm__ volatile("" ::: "memory");}
static void capture_required(unsigned slot,const unsigned long long *frame) {
 native_original_provider_callback=slot;
 for(unsigned i=0;i<16;i++)native_original_provider_frame[i]=frame[i];
 if(clientMeta){unsigned count=clientMeta->getInstanceCount();native_original_provider_client_instances_at_callback=count;native_original_provider_client_instances_live=count;}
 auto object=reinterpret_cast<IOService *>((uintptr_t)frame[0]);
 native_original_provider_attached=object->getProvider()!=nullptr || object->inPlane(gIOServicePlane);
 native_original_provider_registered=(object->getState()&kIOServiceRegisteredState)!=0;
}
__attribute__((noreturn)) void native_original_provider_required(unsigned slot,const unsigned long long *frame) {
 capture_required(slot,frame);
 native_original_provider_status=kIOReturnUnsupported;native_original_provider_stage=4;native_original_provider_checkpoint();
 // There is deliberately no successful return from an unimplemented required
 // callback or from this own probe's attempted publication.
 panic("Unimplemented original-provider probe callback %u",slot);
}
IOMemoryDescriptor *native_original_provider_descriptor(const unsigned long long *frame) {
 if(probeMode>=5 && probeMode<=7) {
  // Explicit descriptor-availability checkpoint: supply one real owned
  // buffer descriptor, but no device, mapping success, GPU submission or
  // completion. The original caller retains/uses the descriptor and then
  // reaches its next required hook, where the probe stops.
  capture_required(170,frame);
  IOBufferMemoryDescriptor *descriptor=IOBufferMemoryDescriptor::withCapacity(4096,kIODirectionInOut);
  native_original_provider_descriptor_provisioned=descriptor!=nullptr;
  if(!descriptor){native_original_provider_status=kIOReturnNoMemory;native_original_provider_stage=4;native_original_provider_checkpoint();return nullptr;}
  return descriptor;
 }
 if(probeMode!=4)native_original_provider_required(170,frame);
 capture_required(170,frame);native_original_provider_descriptor_refused=1;
 // This opt-in control has NO backend descriptor. A genuine NULL refusal lets
 // original code execute its own error, detach and release paths. No successful
 // descriptor, mapping, device, GPU submission or completion is supplied.
 return nullptr;
}
void native_original_provider_probe(IOService *backend) {
 unsigned mode=0;PE_parse_boot_argn("offline-original-provider-abi",&mode,sizeof(mode));if(!mode)return;
 native_original_provider_stage=1;native_original_provider_backend=(uint64_t)backend;
 if(mode>7 || !backend){native_original_provider_status=kIOReturnBadArgument;native_original_provider_checkpoint();return;}
 probeMode=mode;
 const OSSymbol *name=OSSymbol::withCString("IOGPU");const OSMetaClass *parent=name?OSMetaClass::getMetaClassWithName(name):nullptr;OSSafeReleaseNULL(name);
 native_original_provider_parent=(uint64_t)parent;
 if(!parent || parent->getClassSize()!=736){native_original_provider_status=kIOReturnNotFound;native_original_provider_checkpoint();return;}
 void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
 if(!handle){native_original_provider_status=kIOReturnNoResources;native_original_provider_checkpoint();return;}
 native_original_provider_static_init();native_original_provider_status=OSMetaClass::postModLoad(handle);
 if(native_original_provider_status){native_original_provider_checkpoint();return;}
 native_original_provider_instances_before=native_original_provider_instances();native_original_provider_stage=2;
 IOService *object=native_original_provider_allocate();native_original_provider_allocated=object!=nullptr;native_original_provider_object=(uint64_t)object;
 native_original_provider_instances_live=native_original_provider_instances();
 if(!object){native_original_provider_status=kIOReturnNoMemory;native_original_provider_checkpoint();return;}
 memcpy((void *)&native_original_provider_vptr,object,sizeof(uint64_t));native_original_provider_size=object->getMetaClass()->getClassSize();
 native_original_provider_parent_match=object->getMetaClass()->getSuperClass()==parent && object->metaCast("IOGPU") && object->metaCast("IOAcceleratorES");
 native_original_provider_initialized=object->init();
 if(mode==2 && native_original_provider_initialized){native_original_provider_start_result=object->start(backend);native_original_provider_start_returned=1;}
 if((mode>=3 && mode<=7) && native_original_provider_initialized) {
  // Exercise the ORIGINAL provider factory/custom task initializer and real
  // original client start. The missing descriptor hook captures by default;
  // mode4 returns a genuine NULL refusal, never a placeholder or client success.
  native_original_provider_client_type=1;
  PE_parse_boot_argn("offline-original-client-type",(void *)&native_original_provider_client_type,sizeof(native_original_provider_client_type));
  const OSSymbol *clientName=OSSymbol::withCString("IOGPUDeviceUserClient");
  clientMeta=clientName?OSMetaClass::getMetaClassWithName(clientName):nullptr;OSSafeReleaseNULL(clientName);
  if(!clientMeta){native_original_provider_status=kIOReturnNotFound;object->release();native_original_provider_released=1;native_original_provider_checkpoint();return;}
  native_original_provider_client_instances_before=clientMeta->getInstanceCount();
  if(mode==6 || mode==7) {
   // Explicit attach boundary: mode 6 additionally registers, mode 7 keeps
   // the provider attached-only so the original factory can be evaluated
   // without the required slot-84 registration-save hook.
   if(!object->attach(backend)) {
    native_original_provider_status=kIOReturnNotAttached;
    object->release();native_original_provider_released=1;
    native_original_provider_stage=3;native_original_provider_checkpoint();return;
   }
   native_original_provider_attached=1;
   if(mode==6) {
    object->registerService();
    native_original_provider_registered=(object->getState()&kIOServiceRegisteredState)!=0;
   }
  }
  IOUserClient *client=nullptr;
  native_original_provider_factory_result=object->newUserClient(kernel_task,nullptr,native_original_provider_client_type,&client);
  native_original_provider_factory_out_client=(uint64_t)client;
  native_original_provider_factory_returned=1;native_original_provider_client_instances_live=clientMeta->getInstanceCount();
  // An unexpected returned object still gets genuine stop/detach/release.
  if(client){client->stop(object);client->detach(object);client->release();native_original_provider_factory_result=kIOReturnError;}
 }
 native_original_provider_attached=object->getProvider()!=nullptr || object->inPlane(gIOServicePlane);
 native_original_provider_registered=(object->getState()&kIOServiceRegisteredState)!=0;
 native_original_provider_status=native_original_provider_initialized && native_original_provider_parent_match && !native_original_provider_attached && !native_original_provider_registered && native_original_provider_size==736 && mode==1?kIOReturnSuccess:kIOReturnError;
 if((mode>=3 && mode<=7) && native_original_provider_factory_returned)native_original_provider_status=native_original_provider_factory_result;
 object->release();native_original_provider_released=1;native_original_provider_instances_after=native_original_provider_instances();
 if(native_original_provider_instances_live!=native_original_provider_instances_before+1 || native_original_provider_instances_after!=native_original_provider_instances_before)native_original_provider_status=kIOReturnError;
 native_original_provider_stage=3;native_original_provider_checkpoint();
}
}
