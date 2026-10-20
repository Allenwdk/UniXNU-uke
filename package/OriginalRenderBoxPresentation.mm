// SPDX-License-Identifier: MIT
// Actual original RenderBox -> GPU format composition -> original display swaps.
// Explicit offline qualification only; no substitute system desktop.
#define OFFLINE_RENDERBOX_EXTERNAL_MAIN 1
#define OFFLINE_RENDERBOX_PRESENTATION 1
#define OFFLINE_RENDERBOX_WIDTH 800
#define OFFLINE_RENDERBOX_HEIGHT 600
#include "OfflineRenderBoxDraw.mm"
#import <CoreVideo/CVPixelBuffer.h>
#ifndef OFFLINE_RENDERBOX_PRESENT_FRAMES
#define OFFLINE_RENDERBOX_PRESENT_FRAMES 2
#endif
int main() {
 @autoreleasepool { @try {
  mountOwnMetalAddon();require(getpid()==1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096 && MTLCreateSystemDefaultDevice()==nil,400);
  id<MTLDevice> device=offline_metal_frontend_open_service("NativeOfflineGPU");
  require(device && actualOriginalRenderTarget && actualOriginalRenderTarget.iosurface && actualOriginalRenderTarget.width==rbWidth && actualOriginalRenderTarget.height==rbHeight,401);
  uint32_t originalSurface=IOSurfaceGetID(actualOriginalRenderTarget.iosurface);
  NSError *error=nil;id<MTLLibrary> library=[device newLibraryWithFile:@"/System/Library/Frameworks/QuartzCore.framework/default.metallib" error:&error];require(library && !error,402);
  MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"read_surf_vert"];pd.fragmentFunction=[library newFunctionWithName:@"read_surf_frag"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
  id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];require(pipeline && !error,403);
  id<MTLCommandQueue> queue=[device newCommandQueue];require(queue!=nil,404);
  constexpr unsigned frames=OFFLINE_RENDERBOX_PRESENT_FRAMES;static_assert(frames==1 || frames==2,"Bounded presentation sequence");
  IOSurfaceRef surfaces[frames]={};
  for(unsigned frame=0;frame<frames;frame++) { @autoreleasepool {
   NSDictionary *properties=@{(__bridge NSString *)kIOSurfaceWidth:@(rbWidth),(__bridge NSString *)kIOSurfaceHeight:@(rbHeight),
    (__bridge NSString *)kIOSurfaceBytesPerElement:@4,(__bridge NSString *)kIOSurfaceBytesPerRow:@(rbWidth*4),
    (__bridge NSString *)kIOSurfaceAllocSize:@(rbWidth*rbHeight*4),(__bridge NSString *)kIOSurfacePixelFormat:@(kCVPixelFormatType_32BGRA)};
   surfaces[frame]=IOSurfaceCreate((__bridge CFDictionaryRef)properties);require(surfaces[frame]!=nullptr,405);
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:rbWidth height:rbHeight mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
   id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:surfaces[frame] plane:0];require(texture && texture.iosurface==surfaces[frame],406);
   MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(1,1,1,1);
   id<MTLCommandBuffer> command=[queue commandBuffer];[command enqueue];id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,407);
   [encoder setRenderPipelineState:pipeline];[encoder setCullMode:MTLCullModeNone];[encoder setFragmentTexture:actualOriginalRenderTarget atIndex:0];
   if(frame)[encoder setScissorRect:(MTLScissorRect){rbWidth/2,0,rbWidth/2,rbHeight}];
   [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];[encoder endEncoding];[command commit];[command waitUntilCompleted];
   require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,408);
   require(!IOSurfaceLock(surfaces[frame],kIOSurfaceLockReadOnly,nullptr),409);
   const uint32_t *pixels=(const uint32_t *)IOSurfaceGetBaseAddress(surfaces[frame]);require(pixels && IOSurfaceGetBytesPerRow(surfaces[frame])==rbWidth*4,410);
   for(unsigned y=0;y<rbHeight;y++)for(unsigned x=0;x<rbWidth;x++)require(pixels[y*rbWidth+x]==(x>=rbWidth/2?0xff0000ff:frame?0xffffffff:0xffff0000),411);
   require(!IOSurfaceUnlock(surfaces[frame],kIOSurfaceLockReadOnly,nullptr),409);
  } }
  // All real GPU rendering/readbacks finish before either original swap. No
  // future producer completion, fabricated fence or CPU composition is used.
  offline_metal_frontend_finish();
  memset(result,0,sizeof(result));result[0]=0x52425052;result[1]=getpid();result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);result[4]=rbWidth;result[5]=rbHeight;result[6]=frames;result[24]=originalSurface;result[25]=frames;
  phase=90;void *iomfb=dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer",RTLD_NOW|RTLD_LOCAL);require(iomfb!=nullptr,412);
  auto get=(int (*)(void **))dlsym(iomfb,"IOMobileFramebufferGetMainDisplay");auto geometry=(int (*)(void *,CGSize *))dlsym(iomfb,"IOMobileFramebufferGetDisplaySize");
  auto begin=(int (*)(void *,uint32_t *))dlsym(iomfb,"IOMobileFramebufferSwapBegin");auto set=(int (*)(void *,uint32_t,IOSurfaceRef,CGRect,CGRect,uint32_t))dlsym(iomfb,"IOMobileFramebufferSwapSetLayer");auto end=(int (*)(void *))dlsym(iomfb,"IOMobileFramebufferSwapEnd");
  auto wait=(int (*)(void *,uint32_t,uint32_t,double))dlsym(iomfb,"IOMobileFramebufferSwapWaitWithTimeout");require(get && geometry && begin && set && end && wait,413);
  void *display=nullptr;CGSize size={};require(!get(&display) && display && !geometry(display,&size) && size.width==rbWidth && size.height==rbHeight,414);
  for(unsigned frame=0;frame<frames;frame++) {
   phase=91+frame;unsigned at=8+frame*8;result[at]=IOSurfaceGetID(surfaces[frame]);uint32_t token=0;
   result[at+2]=(uint32_t)begin(display,&token);result[at+1]=token;require(!result[at+2] && token,415);
   CGRect rect=CGRectMake(0,0,rbWidth,rbHeight);result[at+3]=(uint32_t)set(display,0,surfaces[frame],rect,rect,0);require(!result[at+3],416);
   result[at+4]=(uint32_t)end(display);require(!result[at+4],417);
   // Observe the actual first query. The original timeout/discard path can
   // later return success without any presentation; never poll that into a
   // claimed frame completion. Kernel copy/retirement and screen evidence
   // remain independent mandatory checks.
   uint32_t status=(uint32_t)wait(display,token,0x80000001u,0.1);
   result[at+5]=status;
   if(status){result[at+6]=UINT64_MAX;break;}
   result[at+6]=(uint32_t)wait(display,token,1,0.1);require(!result[at+6],419);result[7]++;
  }
  require(!usleep(100000),420);for(IOSurfaceRef surface:surfaces)CFRelease(surface);
  register const uint64_t *packet __asm__("x19")=result;
  __asm__ volatile("brk #0" : : "r"(packet) : "memory");__builtin_unreachable();
 } @catch(NSException *exception){observeFailure(exception.reason);} }
}
