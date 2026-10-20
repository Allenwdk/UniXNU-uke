// SPDX-License-Identifier: MIT
// Real GPU IOSurface through unchanged original framebuffer transaction APIs.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <unistd.h>
extern "C" id<MTLDevice> offline_metal_frontend_open(void);
extern "C" void offline_metal_frontend_finish(void);
static uint64_t result[16];
static void require(bool ok, int code) {if(!ok)_exit(code);}
int main() {
 @autoreleasepool { @try {
  result[0]=0x47505357;result[1]=1;result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);
  result[8]=result[10]=result[11]=UINT64_MAX;
  require(getpid()==1 && result[2]==4096 && result[3]==4096,61);
  require(MTLCreateSystemDefaultDevice()==nil,120);
  void *library=dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer",RTLD_NOW|RTLD_LOCAL);
  require(library!=nullptr,152);
  auto get=(int (*)(void **))dlsym(library,"IOMobileFramebufferGetMainDisplay");
  auto geometry=(int (*)(void *,CGSize *))dlsym(library,"IOMobileFramebufferGetDisplaySize");
  auto begin=(int (*)(void *,uint32_t *))dlsym(library,"IOMobileFramebufferSwapBegin");
  auto set=(int (*)(void *,uint32_t,IOSurfaceRef,CGRect,CGRect,uint32_t))dlsym(library,"IOMobileFramebufferSwapSetLayer");
  auto end=(int (*)(void *))dlsym(library,"IOMobileFramebufferSwapEnd");
  require(get && geometry && begin && set && end,152);
  void *display=nullptr;require(!get(&display) && display,153);
  CGSize size={};require(!geometry(display,&size) && size.width==800 && size.height==600,155);
  result[4]=size.width;result[5]=size.height;
  NSDictionary *properties=@{(__bridge NSString *)kIOSurfaceWidth:@800,(__bridge NSString *)kIOSurfaceHeight:@600,
   (__bridge NSString *)kIOSurfaceBytesPerElement:@4,(__bridge NSString *)kIOSurfaceBytesPerRow:@3200,
   (__bridge NSString *)kIOSurfaceAllocSize:@1920000,(__bridge NSString *)kIOSurfacePixelFormat:@(0x42475241)};
  IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);require(surface!=nullptr,140);
  require(IOSurfaceGetWidth(surface)==800 && IOSurfaceGetHeight(surface)==600 && IOSurfaceGetBytesPerRow(surface)==3200,141);
  result[6]=IOSurfaceGetID(surface);require(result[6]!=0,141);
  id<MTLDevice> device=offline_metal_frontend_open();require(device!=nil,128);
  id<MTLCommandQueue> queue=[device newCommandQueue];require(queue!=nil,129);
  MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:800 height:600 mipmapped:NO];
  td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
  id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:surface plane:0];require(texture && texture.iosurface==surface,123);
  MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
  pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
  pass.colorAttachments[0].clearColor=MTLClearColorMake(0.2,0.4,0.6,1);
  id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,125);
  [encoder endEncoding];[command commit];[command waitUntilCompleted];
  require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,126);
  require(!IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr),142);
  const uint32_t *pixels=(const uint32_t *)IOSurfaceGetBaseAddress(surface);require(pixels!=nullptr,141);
  for(unsigned i=0;i<800*600;i++)require(pixels[i]==0xff336699,147);
  require(!IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr),142);
  result[7]=800*600;
  // Close the real GPU session before entering the unqualified original swap.
  // The original IOSurface stays alive with its actual completed GPU pixels.
  offline_metal_frontend_finish();result[14]=1;
  uint32_t token=0;result[8]=(uint32_t)begin(display,&token);result[9]=token;result[15]=1;
  if(!result[8]) {
   CGRect rect={{0,0},{size.width,size.height}};
   result[10]=(uint32_t)set(display,0,surface,rect,rect,0);result[15]=2;
   if(!result[10]) {result[11]=(uint32_t)end(display);result[15]=3;}
  }
#if OFFLINE_LEGACY_SWAP_WAIT
  // Original ABI uses a floating-point timeout in d0, not a fourth GPR.
  // Type 1 waits for removal from pending/current. Type 0 also waits until
  // the last completed generation is superseded; it is not this checkpoint.
  // The original high bit requests a nonblocking query. A pending control
  // must return real NotReady; only a completed query proceeds to blocking
  // wait. This avoids unimplemented hardware stall diagnostics on timeout.
  auto wait=(int (*)(void *,uint32_t,uint32_t,double))dlsym(library,"IOMobileFramebufferSwapWaitWithTimeout");
  require(wait && !result[11],154);
  result[14]=(uint32_t)wait(display,token,0x80000001u,0.1);
  if(!result[14])result[14]=(uint32_t)wait(display,token,1,0.1);
#endif
  // Give genuine queued display work a bounded opportunity to run. The
  // earlier immediate breakpoint did not qualify asynchronous presentation.
  uint64_t before=0,after=0;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(before));
  require(!usleep(1000000),168);
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(after));
  result[12]=before;result[13]=after-before;result[15]=4;
  register const uint64_t *pointer __asm__("x19")=result;
  __asm__ volatile("brk #0" : : "r"(pointer) : "memory");
  __builtin_unreachable();
 } @catch(NSException *exception) {
  [exception.reason writeToFile:@"/private/var/tmp/offline-legacy-swap-error.txt" atomically:YES encoding:NSUTF8StringEncoding error:nil];_exit(134);
 } }
}
