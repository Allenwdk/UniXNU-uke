// SPDX-License-Identifier: MIT
// Actual resource-driven original shaders -> IOSurface -> genuine scanout.
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
  NSData *rbBytes=[NSData dataWithContentsOfFile:@"/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib" options:NSDataReadingMappedIfSafe error:nil];require(rbBytes!=nil,121);
  dispatch_data_t rbData=dispatch_data_create(rbBytes.bytes,rbBytes.length,dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{(void)[rbBytes length];});
  id<MTLLibrary> renderbox=[device newLibraryWithData:rbData error:&error];require(renderbox && !error && renderbox.functionNames.count==73,122);
  id<MTLFunction> vertex=[renderbox newFunctionWithName:@"accumulator_gradient_vertex"],fragment=[library newFunctionWithName:@"attachment_clear_frag_lpf"];require(vertex && fragment,130);
  MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=vertex;pd.fragmentFunction=fragment;pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
  id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];require(pipeline && !error,132);
  // Exact original RenderBox AIR declares an 84-byte buffer at index zero.
  // Plane matrix maps its four vertex IDs to the right half of clip space.
  float globals[21]={1,0,0,2,0,-1,0,1,0,0,1,0,0,0,0,0,0,0,0,0,0};
  const uint16_t indices[6]={0,1,2,0,2,3};
  require(![device newBufferWithLength:0 options:MTLResourceStorageModeShared] &&
          ![device newBufferWithLength:device.maxBufferLength+1 options:MTLResourceStorageModeShared] &&
          ![device newBufferWithLength:84 options:MTLResourceStorageModePrivate],162);
  id<MTLBuffer> source=[device newBufferWithBytes:globals length:sizeof(globals) options:MTLResourceStorageModeShared];
  id<MTLBuffer> uniforms=[device newBufferWithLength:sizeof(globals) options:MTLResourceStorageModeShared];
  id<MTLBuffer> index=[device newBufferWithBytes:indices length:sizeof(indices) options:MTLResourceStorageModeShared];
  require(source && uniforms && index && source.length==84 && uniforms.length==84 && index.length==12,162);
  memset(uniforms.contents,0x6b,84);
  id<MTLCommandBuffer> copy=[queue commandBuffer];id<MTLBlitCommandEncoder> blit=[copy blitCommandEncoder];require(blit!=nil,162);
  [blit copyFromBuffer:source sourceOffset:0 toBuffer:uniforms destinationOffset:0 size:84];[blit endEncoding];
  [copy commit];[copy waitUntilCompleted];require(copy.status==MTLCommandBufferStatusCompleted && !copy.error &&
      copy.GPUStartTime>0 && copy.GPUEndTime>copy.GPUStartTime && !memcmp(uniforms.contents,globals,84),162);
  NSString *copiedUniformHash=hashBytes(uniforms.contents,84);
  NSMutableArray *frames=[NSMutableArray array];
  for(unsigned frame=0;frame<3;frame++) { @autoreleasepool {
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:mode.width height:mode.height mipmapped:NO];
   td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
   id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:surface plane:0];require(texture && texture.iosurface==surface,123);
   MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
   pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
   pass.colorAttachments[0].clearColor=MTLClearColorMake(0.2,0.4,0.6,1);
   id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,125);
   if(frame) {
    [encoder setRenderPipelineState:pipeline];[encoder setCullMode:MTLCullModeNone];
    [encoder setVertexBuffer:uniforms offset:0 atIndex:0];[encoder setFragmentBuffer:uniforms offset:0 atIndex:0];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16 indexBuffer:index indexBufferOffset:0];
    if(frame==2) {
     // Shared writes after binding/encoding must be visible at commit.
     ((float *)uniforms.contents)[0]=2;((float *)uniforms.contents)[4]=-1;
    }
   }
   [encoder endEncoding];[command commit];[command waitUntilCompleted];
   require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,126);
   require(!IOSurfaceLock(alias,kIOSurfaceLockReadOnly,nullptr),142);
   const void *base=IOSurfaceGetBaseAddress(alias);require(base!=nullptr,141);
   for(size_t i=0;i<mode.width*mode.height;i++) {
    uint32_t expected=frame==0 || (frame==1 && i%mode.width<mode.width/2)?0xff336699:0;
    require(((const uint32_t *)base)[i]==expected,147);
   }
   NGPPresent request={(uint64_t)base,mode.bytes,(uint32_t)mode.width,(uint32_t)mode.height,(uint32_t)mode.stride,mode.format};
   count=sizeof(transfer);require(!method(connection,1,&request,sizeof(request),&transfer,&count) && count==sizeof(transfer) &&
           transfer.sequence==frame+1 && transfer.bytes==mode.bytes,158);
   __asm__ volatile("dmb ish" ::: "memory");
   for(size_t i=0;i<mode.width*mode.height;i++) {
    uint32_t expected=frame==0 || (frame==1 && i%mode.width<mode.width/2)?0xff336699:0;
    require(((volatile const uint32_t *)scanout)[i]==expected,159);
   }
   NSString *surfaceHash=hashBytes(base,mode.bytes),*scanoutHash=hashBytes((const void *)scanout,mode.bytes);
   require([surfaceHash isEqualToString:scanoutHash],159);
   [frames addObject:@{@"sequence":@(transfer.sequence),@"bytes":@(transfer.bytes),@"surface_sha256":surfaceHash,@"scanout_sha256":scanoutHash,
       @"gpu_start":@(command.GPUStartTime),@"gpu_end":@(command.GPUEndTime),@"original_shader_draw":@(frame!=0)}];
   require(!IOSurfaceUnlock(alias,kIOSurfaceLockReadOnly,nullptr),142);
  } }
  NSDictionary *report=@{@"width":@(mode.width),@"height":@(mode.height),@"stride":@(mode.stride),@"frames":frames,
      @"copied_uniform_sha256":copiedUniformHash,@"gpu_buffer_copy":@YES,@"original_indexed_draws":@2,
      @"cpu_pixel_renderer":@NO,@"hardware_vblank_qualified":@NO,@"system_device_discovery":@NO,@"original_desktop":@NO,@"adreno_execution":@NO};
  NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
  require(json && !error && [json writeToFile:@"/private/var/tmp/offline-gop-resources.json" options:0 error:&error] && !error,133);
  require(!unmap(connection,0,mach_task_self(),scanout) && !close(connection),160);
  CFRelease(alias);CFRelease(surface);require(!mach_port_deallocate(mach_task_self(),port),143);
  offline_metal_frontend_finish();_exit(163);
 } @catch(NSException *exception) {
  [exception.reason writeToFile:@"/private/var/tmp/offline-gop-error.txt" atomically:YES encoding:NSUTF8StringEncoding error:nil];_exit(134);
 } }
}
