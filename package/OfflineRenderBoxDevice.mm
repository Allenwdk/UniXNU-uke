// SPDX-License-Identifier: MIT
// Execute the unchanged original RenderBox device initializer with a real
// explicit own Metal device, then the existing two-process GPU/surface test.
#define main shared_surface_test_main
#include "OfflineMetalSharedSurfaces.mm"
#undef main
#include <dlfcn.h>
#include <objc/runtime.h>
#include "OfflineMetalAddon.h"
@interface NSObject (OriginalRenderBoxDevice)
- (id)initWithDevice:(id<MTLDevice>)device;
- (id<MTLDevice>)device;
- (id)queue;
@end
@interface NSObject (OriginalAcceleratorFactory)
- (id)initWithAcceleratorPort:(unsigned)port;
@end
static NSBundle *pluginBundle;
static id renderBoxDevice;
static unsigned phase;
#if OFFLINE_RENDERBOX_DRAW
static void qualifyOriginalRenderBoxDrawing(id<MTLDevice>,id);
#endif
__attribute__((noreturn)) static void observeFailure(NSString *reason) {
 static char detail[4096];snprintf(detail,sizeof(detail),"%s",reason.UTF8String?:"Missing actual exception reason");
 result[0]=0x52424641;result[1]=getpid();result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);result[4]=phase;
 register const uint64_t *packet __asm__("x19")=result;register const char *text __asm__("x20")=detail;
 __asm__ volatile("brk #0" : : "r"(packet),"r"(text) : "memory");__builtin_unreachable();
}
static void rbRequire(bool ok,int code) {
 if(!ok)observeFailure([NSString stringWithFormat:@"Actual diagnostic condition %d failed, errno %d",code,errno]);
}
#define require rbRequire
extern "C" id<MTLDevice> offline_metal_frontend_open_service(const char *name) {
 @try {
  phase=10;require(name && !strcmp(name,"NativeOfflineGPU"),290);
  void *iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);require(iokit!=nullptr,291);
  auto matching=(void *(*)(const char *))dlsym(iokit,"IOServiceMatching");auto services=(int (*)(unsigned,void *,unsigned *))dlsym(iokit,"IOServiceGetMatchingServices");
  auto next=(unsigned (*)(unsigned))dlsym(iokit,"IOIteratorNext");auto release=(int (*)(unsigned))dlsym(iokit,"IOObjectRelease");
  auto registry=(int (*)(unsigned,uint64_t *))dlsym(iokit,"IORegistryEntryGetRegistryEntryID");require(matching && services && next && release && registry,292);
  phase=20;pluginBundle=[NSBundle bundleWithPath:@"/System/Library/Extensions/NativeOfflineMetal.bundle"];NSError *error=nil;
  if(!pluginBundle || ![pluginBundle loadAndReturnError:&error] || error)observeFailure(error.description?:@"Own bundle did not load");
  Class principal=pluginBundle.principalClass,base=NSClassFromString(@"_MTLDevice");require(principal && base && [principal isSubclassOfClass:base] && class_getInstanceSize(base)==536,293);
  phase=30;unsigned iterator=0;require(!services(0,matching(name),&iterator) && iterator,294);unsigned service=next(iterator),extra=next(iterator);
  require(service && !extra,295);uint64_t actualID=0;require(!registry(service,&actualID) && actualID,296);
  id<MTLDevice> device=[[(id)principal alloc] initWithAcceleratorPort:service];release(service);release(iterator);
  require(device && [(id)device isKindOfClass:base] && device.registryID==actualID,297);
  for(NSInteger family=1001;family<=1008;family++)require(![device supportsFamily:(MTLGPUFamily)family],306);
  require(![device supportsFamily:(MTLGPUFamily)3003] && ![device supportsFamily:(MTLGPUFamily)-1]
      && ![device supportsFeatureSet:(MTLFeatureSet)0] && ![device supportsFeatureSet:(MTLFeatureSet)NSUIntegerMax],307);
#if defined(OFFLINE_RENDERBOX_ENABLE_ASYNC) && OFFLINE_RENDERBOX_ENABLE_ASYNC
  void *asyncFactory=dlopen(pluginBundle.executablePath.fileSystemRepresentation,RTLD_NOW|RTLD_LOCAL);require(asyncFactory!=nullptr,308);
  auto enableAsync=(BOOL (*)(void))dlsym(asyncFactory,"offline_metal_frontend_enable_async");require(enableAsync && enableAsync(),309);require(!dlclose(asyncFactory),310);
#endif
  phase=40;require(dlopen("/System/Library/PrivateFrameworks/RenderBox.framework/RenderBox",RTLD_NOW|RTLD_LOCAL)!=nullptr,298);
  Class rb=NSClassFromString(@"RBDevice");SEL initialize=NSSelectorFromString(@"initWithDevice:");
  Method method=rb?class_getInstanceMethod(rb,initialize):nullptr;result[5]=rb?class_getInstanceSize(rb):0;
  if(!rb || !method || strcmp(method_getTypeEncoding(method),"@24@0:8@16"))
   observeFailure([NSString stringWithFormat:@"Actual RenderBox class %@ size %llu initializer %s",NSStringFromClass(rb),result[5],method?method_getTypeEncoding(method):"missing"]);
  phase=41;renderBoxDevice=[[(id)rb alloc] initWithDevice:device];require(renderBoxDevice!=nil,300);
  phase=42;require([renderBoxDevice device]==device,301);
  phase=43;require([renderBoxDevice queue]!=nil,302);
  phase=44;
#if OFFLINE_RENDERBOX_DRAW
  qualifyOriginalRenderBoxDrawing(device,renderBoxDevice);
#endif
  return device;
 } @catch(NSException *exception){observeFailure(exception.reason);}
}
extern "C" void offline_metal_frontend_finish(void) {
 @try {
  phase=50;renderBoxDevice=nil;
  void *image=dlopen(pluginBundle.executablePath.fileSystemRepresentation,RTLD_NOW|RTLD_LOCAL);require(image!=nullptr,303);
  auto finish=(void (*)(void))dlsym(image,"offline_metal_frontend_finish");require(finish!=nullptr,304);finish();require(!dlclose(image),305);phase=51;
 } @catch(NSException *exception){observeFailure(exception.reason);}
}
#ifndef OFFLINE_RENDERBOX_EXTERNAL_MAIN
int main(int argc,char **argv) {
 if(argc==1){@autoreleasepool {mountOwnMetalAddon();}}
 return shared_surface_test_main(argc,argv);
}
#endif
