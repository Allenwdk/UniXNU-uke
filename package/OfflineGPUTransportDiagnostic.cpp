// SPDX-License-Identifier: MIT
// Own signed PID 1 instrument. Real IOKit/DMA/host GPU round-trip only.
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <CommonCrypto/CommonDigest.h>
#include "OfflineGPUWire.h"
using Call=int (*)(unsigned,unsigned,const uint64_t *,unsigned,const void *,size_t,uint64_t *,unsigned *,void *,size_t *);
static Call method;
static unsigned connection;
static void receive(void *data,size_t bytes) {
 auto *p=static_cast<unsigned char *>(data);
 while(bytes) {
  size_t count=bytes<4096?bytes:4096;
  if(method(connection,1,nullptr,0,nullptr,0,nullptr,nullptr,p,&count) || !count || count>bytes)_exit(102);
  bytes-=count;p+=count;
 }
}
int main() {
 if(getpid()!=1 || getpagesize()!=4096 || sysconf(_SC_PAGESIZE)!=4096)_exit(61);
 void *iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
 if(!iokit)_exit(95);
 auto matching=reinterpret_cast<void *(*)(const char *)>(dlsym(iokit,"IOServiceMatching"));
 auto services=reinterpret_cast<int (*)(unsigned,void *,unsigned *)>(dlsym(iokit,"IOServiceGetMatchingServices"));
 auto next=reinterpret_cast<unsigned (*)(unsigned)>(dlsym(iokit,"IOIteratorNext"));
 auto release=reinterpret_cast<int (*)(unsigned)>(dlsym(iokit,"IOObjectRelease"));
 auto open=reinterpret_cast<int (*)(unsigned,unsigned,unsigned,unsigned *)>(dlsym(iokit,"IOServiceOpen"));
 auto close=reinterpret_cast<int (*)(unsigned)>(dlsym(iokit,"IOServiceClose"));
 method=reinterpret_cast<Call>(dlsym(iokit,"IOConnectCallMethod"));
 if(!matching || !services || !next || !release || !open || !close || !method)_exit(96);
 unsigned iterator=0;if(services(0,matching("NativeVirtioConsole"),&iterator) || !iterator)_exit(97);
 unsigned service=next(iterator);if(!service || next(iterator))_exit(98);
 if(open(service,mach_task_self(),0,&connection) || !connection)_exit(99);
 if(release(service) || release(iterator))_exit(100);
 // Exercise real public user-client refusals before advertising any request
 // to the device. The observation must still show exactly two TX descriptors.
 unsigned char oversized[4097]={};size_t count=sizeof(oversized);
 if(!method(connection,2,nullptr,0,nullptr,0,nullptr,nullptr,nullptr,nullptr)
    || !method(connection,0,nullptr,0,nullptr,0,nullptr,nullptr,nullptr,nullptr)
    || !method(connection,0,nullptr,0,oversized,sizeof(oversized),nullptr,nullptr,nullptr,nullptr)
    || !method(connection,1,nullptr,0,nullptr,0,nullptr,nullptr,oversized,&count))_exit(108);
 FILE *report=fopen("/private/var/tmp/offline-gpu-report.txt","wb");if(!report)_exit(106);
 for(uint32_t request=1;request<=2;request++) {
  uint16_t op=request==1?GPU_WIRE_CLEAR:GPU_WIRE_ORIGINAL_DRAW;
  GPUWireHeader header={GPU_WIRE_MAGIC,GPU_WIRE_VERSION,op,0,request};
  if(method(connection,0,nullptr,0,&header,sizeof(header),nullptr,nullptr,nullptr,nullptr))_exit(101);
  GPUWireHeader response={};receive(&response,sizeof(response));
  if(response.magic!=GPU_WIRE_MAGIC || response.version!=GPU_WIRE_VERSION || response.operation!=(op|GPU_WIRE_RESPONSE)
     || response.request!=request || response.bytes!=sizeof(GPUWireResult)+GPU_WIRE_PIXEL_BYTES)_exit(103);
  GPUWireResult result={};receive(&result,sizeof(result));
  if(result.width!=64 || result.height!=64 || result.format!=GPU_WIRE_R32_FLOAT || result.status!=4
     || !result.start_ns || result.end_ns<=result.start_ns)_exit(104);
  uint32_t pixels[4096];receive(pixels,sizeof(pixels));
  uint32_t expected=request==1?0x3f800000:0;
  for(uint32_t pixel:pixels)if(pixel!=expected)_exit(105);
  unsigned char hash[32];CC_SHA256(pixels,sizeof(pixels),hash);
  // Return actual GPU bytes, timing and full digest in the captured user log.
  char line[256];int n=snprintf(line,sizeof(line),"OFFLINE_GPU request=%u bytes=%zu gpu_ns=%llu..%llu sha256=",request,sizeof(pixels),result.start_ns,result.end_ns);
  for(unsigned j=0;j<32;j++)n+=snprintf(line+n,sizeof(line)-(size_t)n,"%02x",hash[j]);
  line[n++]='\n';if(fwrite(line,1,(size_t)n,report)!=(size_t)n)_exit(106);
 }
 if(fclose(report))_exit(106);
 if(close(connection))_exit(107);
 _exit(94);
}
