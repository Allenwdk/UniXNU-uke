// SPDX-License-Identifier: MIT
// Actual GPU texture reads, clipping and layer blending -> genuine scanout.
// No replacement desktop, CPU compositor or automatic system Metal device.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <CoreVideo/CVPixelBuffer.h>
#import <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <unistd.h>
#include <vector>
#include "NativeGOPWire.h"
extern "C" id<MTLDevice> offline_metal_frontend_open(void);
extern "C" void offline_metal_frontend_finish(void);
static void require(bool value,int code) {if(!value)_exit(code);}
static NSString *hashBytes(const void *data,size_t bytes) {
 require(bytes<=UINT32_MAX,155);unsigned char out[32];CC_SHA256(data,(CC_LONG)bytes,out);
 NSMutableString *s=[NSMutableString string];for(unsigned i=0;i<32;i++)[s appendFormat:@"%02x",out[i]];return s;
}
int main() {
 @autoreleasepool { @try {
  require(getpid()==1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096,61);
  require(MTLCreateSystemDefaultDevice()==nil,120);
  id<MTLDevice> device=offline_metal_frontend_open();require(device!=nil,128);
  void *io=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW);require(io!=nullptr,152);
  auto matching=(CFMutableDictionaryRef (*)(const char *))dlsym(io,"IOServiceMatching");
  auto get=(unsigned (*)(unsigned,CFDictionaryRef))dlsym(io,"IOServiceGetMatchingService");
  auto open=(int (*)(unsigned,unsigned,unsigned,unsigned *))dlsym(io,"IOServiceOpen");
  auto release=(int (*)(unsigned))dlsym(io,"IOObjectRelease");
  auto close=(int (*)(unsigned))dlsym(io,"IOServiceClose");
  auto method=(int (*)(unsigned,unsigned,const void *,size_t,void *,size_t *))dlsym(io,"IOConnectCallStructMethod");
  auto map=(int (*)(unsigned,unsigned,unsigned,uint64_t *,uint64_t *,unsigned))dlsym(io,"IOConnectMapMemory64");
  auto unmap=(int (*)(unsigned,unsigned,unsigned,uint64_t))dlsym(io,"IOConnectUnmapMemory64");
  require(matching && get && open && release && close && method && map && unmap,152);
  unsigned service=get(0,matching("NativeGOPPresenter")),connection=0;require(service && !open(service,mach_task_self(),0,&connection) && connection,153);
  unsigned second=0;require(open(service,mach_task_self(),0,&second)!=0 && !second,154);require(!release(service),153);
  NGPMode mode={};size_t count=sizeof(mode);require(!method(connection,0,nullptr,0,&mode,&count) && count==sizeof(mode),155);
  require(mode.version==NGP_VERSION && mode.format==NGP_BGRX8888 && mode.width && mode.height && mode.width<=2048 && mode.height<=2048 &&
          mode.stride==mode.width*4 && mode.bytes==mode.stride*mode.height,155);
  uint64_t scanout=0,scanoutBytes=0;
  // Public IOKit kIOMapAnywhere=1. The kernel independently forces read-only.
  require(!map(connection,0,mach_task_self(),&scanout,&scanoutBytes,1) && scanout && scanoutBytes>=mode.bytes,156);
  NGPTransfer transfer={};count=sizeof(transfer);
  require(method(connection,2,nullptr,0,&transfer,&count)!=0,157);
  NGPPresent bad={1,mode.bytes,(uint32_t)mode.width+1,(uint32_t)mode.height,(uint32_t)mode.stride,mode.format};
  count=sizeof(transfer);require(method(connection,1,&bad,sizeof(bad),&transfer,&count)!=0,157);
  bad.width=(uint32_t)mode.width;bad.address=UINT64_MAX-mode.bytes+1;count=sizeof(transfer);
  require(method(connection,1,&bad,sizeof(bad),&transfer,&count)!=0,157);
  NSDictionary *properties=@{(__bridge NSString *)kIOSurfaceWidth:@(mode.width),(__bridge NSString *)kIOSurfaceHeight:@(mode.height),
      (__bridge NSString *)kIOSurfaceBytesPerElement:@4,(__bridge NSString *)kIOSurfaceBytesPerRow:@(mode.stride),
      (__bridge NSString *)kIOSurfaceAllocSize:@(mode.bytes),(__bridge NSString *)kIOSurfacePixelFormat:@(kCVPixelFormatType_32BGRA)};
  IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);require(surface!=nullptr,140);
  require(IOSurfaceGetWidth(surface)==mode.width && IOSurfaceGetHeight(surface)==mode.height &&
          IOSurfaceGetBytesPerRow(surface)==mode.stride && IOSurfaceGetAllocSize(surface)>=mode.bytes,141);
  mach_port_t port=IOSurfaceCreateMachPort(surface);require(MACH_PORT_VALID(port),143);
  IOSurfaceRef alias=IOSurfaceLookupFromMachPort(port);require(alias && IOSurfaceGetID(alias)==IOSurfaceGetID(surface),144);
  id<MTLCommandQueue> queue=[device newCommandQueue];require(queue!=nil,129);
  NSData *libraryBytes=[NSData dataWithContentsOfFile:@"/System/Library/Frameworks/QuartzCore.framework/default.metallib" options:NSDataReadingMappedIfSafe error:nil];require(libraryBytes!=nil,121);
  dispatch_data_t data=dispatch_data_create(libraryBytes.bytes,libraryBytes.length,dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{(void)[libraryBytes length];});
  NSError *error=nil;id<MTLLibrary> library=[device newLibraryWithData:data error:&error];require(library && !error && library.functionNames.count==167,122);
  id<MTLFunction> vertex=[library newFunctionWithName:@"read_surf_vert"],fragment=[library newFunctionWithName:@"read_surf_frag"];require(vertex && fragment,130);
  MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=vertex;pd.fragmentFunction=fragment;pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
  id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];require(pipeline && !error,132);
  MTLRenderPipelineDescriptor *blendDescription=[pd copy];MTLRenderPipelineColorAttachmentDescriptor *blend=blendDescription.colorAttachments[0];
  blend.blendingEnabled=YES;blend.sourceRGBBlendFactor=MTLBlendFactorOne;blend.destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
  blend.sourceAlphaBlendFactor=MTLBlendFactorOne;blend.destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
  id<MTLRenderPipelineState> premultiplied=[device newRenderPipelineStateWithDescriptor:blendDescription error:&error];require(premultiplied && !error,164);
  blend.sourceRGBBlendFactor=MTLBlendFactorBlendColor;blend.destinationRGBBlendFactor=MTLBlendFactorOneMinusBlendColor;
  blend.sourceAlphaBlendFactor=MTLBlendFactorZero;blend.destinationAlphaBlendFactor=MTLBlendFactorOne;
  id<MTLRenderPipelineState> constant=[device newRenderPipelineStateWithDescriptor:blendDescription error:&error];require(constant && !error,164);
  MTLRenderPipelineDescriptor *unsupported=[blendDescription copy];unsupported.rasterSampleCount=2;NSError *negative=nil;
  require(![device newRenderPipelineStateWithDescriptor:unsupported error:&negative] && negative!=nil,164);
  MTLTextureDescriptor *sourceDescription=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:mode.width height:mode.height mipmapped:NO];
  sourceDescription.storageMode=MTLStorageModeShared;sourceDescription.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
  id<MTLTexture> blue=[device newTextureWithDescriptor:sourceDescription],red=[device newTextureWithDescriptor:sourceDescription];require(blue && red,165);
  id<MTLCommandBuffer> sources=[queue commandBuffer];
  for(unsigned i=0;i<2;i++) {
   MTLRenderPassDescriptor *clear=[MTLRenderPassDescriptor renderPassDescriptor];clear.colorAttachments[0].texture=i?red:blue;
   clear.colorAttachments[0].loadAction=MTLLoadActionClear;clear.colorAttachments[0].storeAction=MTLStoreActionStore;
   clear.colorAttachments[0].clearColor=i?MTLClearColorMake(0.5,0,0,0.5):MTLClearColorMake(0,0,1,1);
   id<MTLRenderCommandEncoder> encoder=[sources renderCommandEncoderWithDescriptor:clear];require(encoder!=nil,165);[encoder endEncoding];
  }
  [sources commit];[sources waitUntilCompleted];require(sources.status==MTLCommandBufferStatusCompleted && !sources.error &&
      sources.GPUStartTime>0 && sources.GPUEndTime>sources.GPUStartTime,165);
  NSMutableArray *sourceHashes=[NSMutableArray array];
  for(unsigned i=0;i<2;i++) {
   std::vector<uint32_t> words(mode.width*mode.height);id<MTLTexture> texture=i?red:blue;
   [texture getBytes:words.data() bytesPerRow:mode.stride fromRegion:MTLRegionMake2D(0,0,mode.width,mode.height) mipmapLevel:0];
   uint32_t expected=i?0x80800000:0xff0000ff;for(uint32_t pixel:words)require(pixel==expected,165);
   [sourceHashes addObject:hashBytes(words.data(),mode.bytes)];
  }
  NSMutableArray *frames=[NSMutableArray array];
  for(unsigned frame=0;frame<3;frame++) { @autoreleasepool {
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:mode.width height:mode.height mipmapped:NO];
   td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
   id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:surface plane:0];require(texture && texture.iosurface==surface,123);
   MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
   pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
   pass.colorAttachments[0].clearColor=MTLClearColorMake(0.2,0.4,0.6,1);
   id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,125);
   if(!frame) {
    BOOL refused=NO;@try {[encoder setScissorRect:(MTLScissorRect){(NSUInteger)mode.width+1,0,1,1}];}
    @catch(NSException *exception){refused=[exception.name isEqualToString:@"OfflineMetalTransport"];}
    require(refused,166);
   }
   [encoder setViewport:(MTLViewport){0,0,(double)mode.width,(double)mode.height,0,1}];
   [encoder setScissorRect:(MTLScissorRect){0,0,(NSUInteger)mode.width,(NSUInteger)mode.height}];
   [encoder setRenderPipelineState:pipeline];[encoder setCullMode:MTLCullModeNone];[encoder setFragmentTexture:blue atIndex:0];
   [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
   if(frame) {
    [encoder setRenderPipelineState:frame==1?premultiplied:constant];
    if(frame==1)[encoder setScissorRect:(MTLScissorRect){(NSUInteger)mode.width/2,0,(NSUInteger)mode.width/2,(NSUInteger)mode.height}];
    else [encoder setBlendColorRed:0.25 green:0.25 blue:0.25 alpha:0.25];
    [encoder setFragmentTexture:red atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
   }
   [encoder endEncoding];[command commit];[command waitUntilCompleted];
   require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,126);
   require(!IOSurfaceLock(alias,kIOSurfaceLockReadOnly,nullptr),142);
   const void *base=IOSurfaceGetBaseAddress(alias);require(base!=nullptr,141);
   for(size_t i=0;i<mode.width*mode.height;i++) {
    uint32_t expected=frame==2?0xff2000bf:frame==1 && i%mode.width>=mode.width/2?0xff80007f:0xff0000ff;
    require(((const uint32_t *)base)[i]==expected,147);
   }
   NGPPresent request={(uint64_t)base,mode.bytes,(uint32_t)mode.width,(uint32_t)mode.height,(uint32_t)mode.stride,mode.format};
   count=sizeof(transfer);require(!method(connection,1,&request,sizeof(request),&transfer,&count) && count==sizeof(transfer) &&
           transfer.sequence==frame+1 && transfer.bytes==mode.bytes,158);
   __asm__ volatile("dmb ish" ::: "memory");
   for(size_t i=0;i<mode.width*mode.height;i++) {
    uint32_t expected=frame==2?0xff2000bf:frame==1 && i%mode.width>=mode.width/2?0xff80007f:0xff0000ff;
    require(((volatile const uint32_t *)scanout)[i]==expected,159);
   }
   NSString *surfaceHash=hashBytes(base,mode.bytes),*scanoutHash=hashBytes((const void *)scanout,mode.bytes);
   require([surfaceHash isEqualToString:scanoutHash],159);
   [frames addObject:@{@"sequence":@(transfer.sequence),@"bytes":@(transfer.bytes),@"surface_sha256":surfaceHash,@"scanout_sha256":scanoutHash,
       @"gpu_start":@(command.GPUStartTime),@"gpu_end":@(command.GPUEndTime),@"original_shader_draw":@(frame!=0)}];
   require(!IOSurfaceUnlock(alias,kIOSurfaceLockReadOnly,nullptr),142);
  } }
  NSDictionary *report=@{@"width":@(mode.width),@"height":@(mode.height),@"stride":@(mode.stride),@"frames":frames,
      @"source_gpu_texture_sha256":sourceHashes,@"actual_gpu_texture_composition":@YES,
      @"cpu_pixel_renderer":@NO,@"hardware_vblank_qualified":@NO,@"system_device_discovery":@NO,@"original_desktop":@NO,@"adreno_execution":@NO};
  NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
  require(json && !error && [json writeToFile:@"/private/var/tmp/offline-gop-composition.json" options:0 error:&error] && !error,133);
  require(!unmap(connection,0,mach_task_self(),scanout) && !close(connection),160);
  CFRelease(alias);CFRelease(surface);require(!mach_port_deallocate(mach_task_self(),port),143);
  offline_metal_frontend_finish();_exit(167);
 } @catch(NSException *exception) {
  [exception.reason writeToFile:@"/private/var/tmp/offline-gop-error.txt" atomically:YES encoding:NSUTF8StringEncoding error:nil];_exit(134);
 } }
}
