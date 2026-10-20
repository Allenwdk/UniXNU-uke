// SPDX-License-Identifier: MIT
// Genuine registered original class lifecycle only. No matching, attachment,
// publication, operational client, policy bypass or GPU submission is provided.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
static_assert(sizeof(IOUserClient)==216,"Exact audited native reference parent extent");
extern "C" {
volatile uint32_t native_original_client_stage,native_original_client_allocated,native_original_client_initialized;
volatile uint32_t native_original_client_parent_match,native_original_client_unrelated_match,native_original_client_attached;
volatile uint32_t native_original_client_released,native_original_client_size,native_original_client_parent_size;
volatile uint32_t native_original_client_instances_before,native_original_client_instances_live,native_original_client_instances_after;
volatile uint32_t native_original_client_wrong_start,native_original_client_provider_initialized;
volatile int native_original_client_status=-1;
volatile uint64_t native_original_client_object,native_original_client_vptr,native_original_client_meta,native_original_client_parent;
__attribute__((noinline)) void native_original_client_checkpoint(void) {__asm__ volatile("" ::: "memory");}
void native_original_client_probe(void) {
 unsigned enabled=0;PE_parse_boot_argn("offline-original-client-abi",&enabled,sizeof(enabled));if(enabled!=1)return;
 native_original_client_stage=1;
 const OSSymbol *name=OSSymbol::withCString("IOGPUDeviceUserClient");const OSMetaClass *meta=name?OSMetaClass::getMetaClassWithName(name):nullptr;OSSafeReleaseNULL(name);
 native_original_client_meta=(uint64_t)meta;
 if(!meta){native_original_client_status=kIOReturnNotFound;native_original_client_checkpoint();return;}
 native_original_client_size=meta->getClassSize();native_original_client_parent=(uint64_t)meta->getSuperClass();
 if(meta->getSuperClass())native_original_client_parent_size=meta->getSuperClass()->getClassSize();
 if(meta->getClassSize()!=304 || meta->getSuperClass()!=&IOUserClient::gMetaClass || native_original_client_parent_size!=216){native_original_client_status=kIOReturnUnsupported;native_original_client_checkpoint();return;}
 native_original_client_instances_before=meta->getInstanceCount();native_original_client_stage=2;
 OSObject *object=meta->alloc();native_original_client_allocated=object!=nullptr;native_original_client_object=(uint64_t)object;
 native_original_client_instances_live=meta->getInstanceCount();
 if(!object){native_original_client_status=kIOReturnNoMemory;native_original_client_checkpoint();return;}
 memcpy((void *)&native_original_client_vptr,object,sizeof(uint64_t));
 IOUserClient *client=OSDynamicCast(IOUserClient,object);
 native_original_client_parent_match=client && object->getMetaClass()==meta && object->metaCast("IOGPUDeviceUserClient");
 native_original_client_unrelated_match=object->metaCast("IOGPU") || object->metaCast("NativeOfflineGPUClient");
 OSDictionary *properties=OSDictionary::withCapacity(1);
 if(client && properties)native_original_client_initialized=client->initWithTask(kernel_task,nullptr,0,properties);
 OSSafeReleaseNULL(properties);
 if(client && native_original_client_initialized) {
  IOService *provider=new IOService;
  if(provider)native_original_client_provider_initialized=provider->init();
  if(native_original_client_provider_initialized)native_original_client_wrong_start=client->start(provider);
  native_original_client_attached=client->getProvider()!=nullptr || client->inPlane(gIOServicePlane);
  // A rejected start is released directly, as with normal IOKit failed start.
  // We never attach this object or skip a successful original operational start.
  if(native_original_client_wrong_start)client->stop(provider);
  OSSafeReleaseNULL(provider);
 }
 native_original_client_status=native_original_client_initialized && native_original_client_parent_match && !native_original_client_unrelated_match && native_original_client_provider_initialized && !native_original_client_wrong_start && !native_original_client_attached?kIOReturnSuccess:kIOReturnError;
 object->release();native_original_client_released=1;native_original_client_instances_after=meta->getInstanceCount();
 if(native_original_client_instances_live!=native_original_client_instances_before+1 || native_original_client_instances_after!=native_original_client_instances_before)native_original_client_status=kIOReturnError;
 native_original_client_stage=3;native_original_client_checkpoint();
}
}
