// SPDX-License-Identifier: MIT
// Reuse the real Mach/IOSurface/GPU test through an actual own NSBundle factory.
#include <mach/mach.h>
#include <unistd.h>
#include <fcntl.h>
static int plugin_pipe(int *);
static mach_msg_return_t plugin_mach_msg(mach_msg_header_t *,mach_msg_option_t,mach_msg_size_t,mach_msg_size_t,mach_port_name_t,mach_msg_timeout_t,mach_port_name_t);
#define pipe plugin_pipe
#define mach_msg plugin_mach_msg
#define offline_metal_frontend_open_service plugin_open_service
#define offline_metal_frontend_finish plugin_finish
#define main shared_surface_test_main
#include "OfflineMetalSharedSurfaces.mm"
#undef main
#undef offline_metal_frontend_open_service
#undef offline_metal_frontend_finish
#undef pipe
#undef mach_msg
#include <dlfcn.h>
#include <objc/runtime.h>
#ifndef OFFLINE_PLUGIN_SYSTEM_PATH
#define OFFLINE_PLUGIN_SYSTEM_PATH 0
#endif
#include "OfflineMetalAddon.h"
static const char *pluginExecutable() {return OFFLINE_PLUGIN_SYSTEM_PATH?"/System/Library/Extensions/NativeOfflineMetal.bundle/NativeOfflineMetal":"/private/var/tmp/NativeOfflineMetal.bundle/NativeOfflineMetal";}
@interface NSObject (OfflineAcceleratorFactory)
- (id)initWithAcceleratorPort:(unsigned)port;
@end
static NSBundle *pluginBundle;
static int64_t executableExtension=-1;
static int executableExtensionError;
static int parentReplies[2]={-1,-1};
static unsigned parentPipeCount;
static int plugin_pipe(int *fds) {
 int status=pipe(fds);
 if(!status && getpid()==1){if(parentPipeCount%2)parentReplies[parentPipeCount/2]=fds[0];parentPipeCount++;}
 return status;
}
static mach_msg_return_t plugin_mach_msg(mach_msg_header_t *message,mach_msg_option_t options,mach_msg_size_t sendSize,
 mach_msg_size_t receiveSize,mach_port_name_t receiveName,mach_msg_timeout_t timeout,mach_port_name_t notify) {
 auto status=mach_msg(message,options,sendSize,receiveSize,receiveName,timeout,notify);
 if(status!=MACH_MSG_SUCCESS && getpid()==1 && (options&MACH_RCV_MSG)) {
  for(unsigned i=0;i<2;i++)if(parentReplies[i]>=0) {
   int flags=fcntl(parentReplies[i],F_GETFL);if(flags<0 || fcntl(parentReplies[i],F_SETFL,flags|O_NONBLOCK))continue;
   uint64_t packet[8]={};ssize_t size=read(parentReplies[i],packet,sizeof(packet));
   if(size==sizeof(packet)){memcpy(result+4+8*i,packet,sizeof(packet));
    char detail[64]={};if(read(parentReplies[i],detail,sizeof(detail))>0)memcpy(result+24,detail,sizeof(detail));
    if(packet[0]==0x53484641){int childStatus=-1;if(waitpid((pid_t)packet[1],&childStatus,WNOHANG)==(pid_t)packet[1])result[20+i]=(uint32_t)childStatus;}}
  }
 }
 return status;
}
static unsigned findService(const char *name) {
 void *iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);require(iokit!=nullptr,253);
 auto matching=(void *(*)(const char *))dlsym(iokit,"IOServiceMatching");auto services=(int (*)(unsigned,void *,unsigned *))dlsym(iokit,"IOServiceGetMatchingServices");
 auto next=(unsigned (*)(unsigned))dlsym(iokit,"IOIteratorNext");auto release=(int (*)(unsigned))dlsym(iokit,"IOObjectRelease");
 require(matching && services && next && release,254);unsigned iterator=0;
 require(!services(0,matching(name),&iterator) && iterator,255);unsigned service=next(iterator),extra=next(iterator);
 if(extra)release(extra);release(iterator);require(service && !extra,256);return service;
}
extern "C" id<MTLDevice> plugin_open_service(const char *name) {
 require(name && !strcmp(name,"NativeOfflineGPU"),257);
 if(!OFFLINE_PLUGIN_SYSTEM_PATH && access("/private/var/tmp/plugin-without-executable-extension",F_OK)) {
  void *sandbox=dlopen("/usr/lib/system/libsystem_sandbox.dylib",RTLD_NOW|RTLD_LOCAL);
  auto issue=sandbox?(char *(*)(const char *,const char *,uint32_t))dlsym(sandbox,"sandbox_extension_issue_file_to_self"):nullptr;
  auto consume=sandbox?(int64_t (*)(const char *))dlsym(sandbox,"sandbox_extension_consume"):nullptr;
  require(issue && consume,268);
  char *token=issue("com.apple.sandbox.executable","/private/var/tmp/NativeOfflineMetal.bundle/NativeOfflineMetal",0);
  if(token){executableExtension=consume(token);executableExtensionError=errno;free(token);}else executableExtensionError=errno;
 }
 pluginBundle=[NSBundle bundleWithPath:OFFLINE_PLUGIN_SYSTEM_PATH?@"/System/Library/Extensions/NativeOfflineMetal.bundle":@"/private/var/tmp/NativeOfflineMetal.bundle"];NSError *error=nil;
 require(pluginBundle!=nil,258);
 if(![pluginBundle loadAndReturnError:&error] || error) {
  // Preserve the real NSBundle failure. The secondary loader call only
  // collects its real reason; it never becomes a successful factory fallback.
  int privateAccess=access("/private/var/tmp/NativeOfflineMetal.bundle/NativeOfflineMetal",R_OK)?errno:0;
  int aliasAccess=access("/var/tmp/NativeOfflineMetal.bundle/NativeOfflineMetal",R_OK)?errno:0;
  dlerror();void *image=dlopen(pluginExecutable(),RTLD_NOW|RTLD_LOCAL);
  const char *why=dlerror();errno=(int)error.code;
  static char detail[512];snprintf(detail,sizeof(detail),"%s",why?:image?"loaded but NSBundle failed":"no loader detail");
  if(image)dlclose(image);
  result[0]=0x53484641;result[1]=getpid();result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);
  result[4]=error.code;result[5]=privateAccess;result[6]=aliasAccess;result[7]=image?1:0;
  result[8]=(uint64_t)executableExtension;result[9]=executableExtensionError;
  register const uint64_t *packet __asm__("x19")=result;register const char *text __asm__("x20")=detail;
  __asm__ volatile("brk #0" : : "r"(packet),"r"(text) : "memory");__builtin_unreachable();
 }
 Class principal=pluginBundle.principalClass,base=NSClassFromString(@"_MTLDevice");
 require(principal && base && [NSStringFromClass(principal) isEqualToString:@"NativeOfflineMetalPlugin"] &&
   [principal isSubclassOfClass:base] && class_getInstanceSize(base)==536 && class_getInstanceSize(principal)==536 &&
   [[NSBundle bundleForClass:principal].bundleIdentifier isEqualToString:@"org.blu-sharky.NativeOfflineMetal"],259);
 require([[(id)principal alloc] initWithAcceleratorPort:MACH_PORT_NULL]==nil,260);
 unsigned unrelated=findService("NativeVirtioConsole");
 require([[(id)principal alloc] initWithAcceleratorPort:unrelated]==nil,261);
 auto release=(int (*)(unsigned))dlsym(RTLD_DEFAULT,"IOObjectRelease");require(release && !release(unrelated),262);
 unsigned service=findService(name);uint64_t actualID=0;
 auto registry=(int (*)(unsigned,uint64_t *))dlsym(RTLD_DEFAULT,"IORegistryEntryGetRegistryEntryID");require(registry && !registry(service,&actualID) && actualID,263);
 id<MTLDevice> device=[[(id)principal alloc] initWithAcceleratorPort:service];require(!release(service),263);
 require(device && [(id)device isKindOfClass:base] && device.registryID==actualID,264);return device;
}
extern "C" void plugin_finish(void) {
 void *image=dlopen(pluginBundle.executablePath.fileSystemRepresentation,RTLD_NOW|RTLD_LOCAL);require(image!=nullptr,265);
 auto finish=(void (*)(void))dlsym(image,"offline_metal_frontend_finish");require(finish!=nullptr,266);finish();require(!dlclose(image),267);
 if(executableExtension>=0) {
  auto release=(int (*)(int64_t))dlsym(RTLD_DEFAULT,"sandbox_extension_release");require(release && !release(executableExtension),269);executableExtension=-1;
 }
}
int main(int argc,char **argv) {
 if(OFFLINE_PLUGIN_SYSTEM_PATH && argc==1){@autoreleasepool {mountOwnMetalAddon();}}
 return shared_surface_test_main(argc,argv);
}
