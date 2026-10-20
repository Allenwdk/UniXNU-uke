// SPDX-License-Identifier: MIT
// Genuine constructor/ancestry/lifetime evidence, without normal publication.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeOfflineAccelerator.cpp");
extern "C" IOService *native_offline_accelerator_allocate(void);
extern "C" unsigned native_offline_accelerator_instance_size(void);
extern "C" unsigned native_offline_accelerator_instances(void);
extern "C" {
volatile uint32_t native_accelerator_stage,native_accelerator_allocated,native_accelerator_initialized;
volatile uint32_t native_accelerator_parent_match,native_accelerator_unrelated_match,native_accelerator_attached,native_accelerator_released,native_accelerator_size;
volatile uint32_t native_accelerator_instances_before,native_accelerator_instances_live,native_accelerator_instances_after;
volatile int native_accelerator_status=-1;
volatile uint64_t native_accelerator_object,native_accelerator_vptr,native_accelerator_parent,native_accelerator_superclass;
__attribute__((noinline)) void native_accelerator_checkpoint(void) {__asm__ volatile("" ::: "memory");}
void native_accelerator_probe(void) {
 unsigned enabled=0;PE_parse_boot_argn("offline-accelerator-abi",&enabled,sizeof(enabled));if(enabled!=1)return;
 native_accelerator_stage=1;
 const OSSymbol *name=OSSymbol::withCString("IOAcceleratorES");
 const OSMetaClass *parent=name?OSMetaClass::getMetaClassWithName(name):nullptr;
 OSSafeReleaseNULL(name);
 native_accelerator_parent=(uint64_t)parent;
 if(!parent || parent->getClassSize()!=136){native_accelerator_status=kIOReturnNotFound;native_accelerator_checkpoint();return;}
 void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
 if(!handle){native_accelerator_status=kIOReturnNoResources;native_accelerator_checkpoint();return;}
 native_accelerator_static_init();native_accelerator_status=OSMetaClass::postModLoad(handle);
 if(native_accelerator_status){native_accelerator_checkpoint();return;}
 native_accelerator_instances_before=native_offline_accelerator_instances();
 native_accelerator_stage=2;IOService *object=native_offline_accelerator_allocate();native_accelerator_allocated=object!=nullptr;
 native_accelerator_instances_live=native_offline_accelerator_instances();
 native_accelerator_object=(uint64_t)object;native_accelerator_size=native_offline_accelerator_instance_size();
 if(object) {
  memcpy((void *)&native_accelerator_vptr,object,sizeof(uint64_t));
  native_accelerator_initialized=object->init();
  native_accelerator_superclass=(uint64_t)object->getMetaClass()->getSuperClass();
  native_accelerator_parent_match=object->metaCast("IOAcceleratorES")!=nullptr;
  native_accelerator_unrelated_match=object->metaCast("IOGPU")!=nullptr || object->metaCast("NativeOfflineGPU")!=nullptr;
  native_accelerator_attached=object->getProvider()!=nullptr || object->inPlane(gIOServicePlane);
  native_accelerator_status=native_accelerator_initialized && native_accelerator_parent_match && !native_accelerator_unrelated_match && !native_accelerator_attached && native_accelerator_superclass==native_accelerator_parent && native_accelerator_size==136?kIOReturnSuccess:kIOReturnError;
  object->release();native_accelerator_released=1;
 } else native_accelerator_status=kIOReturnNoMemory;
 native_accelerator_instances_after=native_offline_accelerator_instances();
 native_accelerator_stage=3;native_accelerator_checkpoint();
}
}
