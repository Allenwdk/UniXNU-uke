// SPDX-License-Identifier: MIT
// Own PID 1 calls standard Metal APIs through an explicit offline frontend.
// This is not automatic system device discovery or a desktop claim.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CommonCrypto/CommonDigest.h>
#import <IOSurface/IOSurfaceRef.h>
#import <CoreVideo/CVPixelBuffer.h>
#include <cstdio>
#include <mach/mach.h>
#include <unistd.h>
#include <vector>
extern "C" id<MTLDevice> offline_metal_frontend_open(void);
extern "C" void offline_metal_frontend_finish(void);
#if defined(OFFLINE_METAL_BGRA_DIAGNOSTIC) && OFFLINE_METAL_BGRA_DIAGNOSTIC
static constexpr bool bgra=true;
#else
static constexpr bool bgra=false;
#endif
static constexpr MTLPixelFormat format=bgra?MTLPixelFormatBGRA8Unorm:MTLPixelFormatR32Float;
static constexpr unsigned attachment=bgra?0:2;
[[maybe_unused]] static constexpr uint32_t surfaceFormat=bgra?kCVPixelFormatType_32BGRA:kCVPixelFormatType_OneComponent32Float;
[[maybe_unused]] static constexpr uint32_t initialPixel=bgra?0x7b3d1f0f:0x40000000;
static constexpr uint32_t clearPixel=bgra?0xff336699:0x3f800000;
static void require(bool value,int code) {if(!value)_exit(code);}
static id<MTLLibrary> library(id<MTLDevice> device,NSString *path,NSUInteger count) {
 NSError *error=nil;NSData *bytes=[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:&error];require(bytes!=nil,121);
 dispatch_data_t data=dispatch_data_create(bytes.bytes,bytes.length,dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{(void)[bytes length];});
 id<MTLLibrary> result=[device newLibraryWithData:data error:&error];require(result && !error && result.functionNames.count==count,122);return result;
}
static NSDictionary *render(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLRenderPipelineState> pipeline,bool draw,IOSurfaceRef surface=nullptr,IOSurfaceRef alias=nullptr) {
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
 td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
 id<MTLTexture> texture=surface?[device newTextureWithDescriptor:td iosurface:surface plane:0]:[device newTextureWithDescriptor:td];
 require(texture && texture.width==64 && texture.height==64 && texture.pixelFormat==format,123);
 if(surface)require(texture.iosurface==surface && texture.iosurfacePlane==0,146);
 MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[attachment].texture=texture;
 pass.colorAttachments[attachment].loadAction=MTLLoadActionClear;pass.colorAttachments[attachment].storeAction=MTLStoreActionStore;pass.colorAttachments[attachment].clearColor=bgra?MTLClearColorMake(0.2,0.4,0.6,1):MTLClearColorMake(1,0,0,0);
 id<MTLCommandBuffer> command=[queue commandBuffer];require(command!=nil,124);
 id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,125);
 if(draw){[encoder setRenderPipelineState:pipeline];[encoder setCullMode:MTLCullModeNone];[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];}
 [encoder endEncoding];[command commit];[command waitUntilCompleted];
 require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,126);
 std::vector<uint32_t> words(4096,0x6b6b6b6b);[texture getBytes:words.data() bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
 uint32_t expected=draw?0:clearPixel;for(uint32_t word:words)require(word==expected,127);
 if(alias) {
  require(!IOSurfaceLock(alias,kIOSurfaceLockReadOnly,nullptr),142);
  const unsigned char *base=(const unsigned char *)IOSurfaceGetBaseAddress(alias);require(base!=nullptr,141);
  size_t stride=IOSurfaceGetBytesPerRow(alias);
  for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)require(((const uint32_t *)(base+y*stride))[x]==expected,147);
  require(!IOSurfaceUnlock(alias,kIOSurfaceLockReadOnly,nullptr),142);
 }
 unsigned char hash[32];CC_SHA256(words.data(),(CC_LONG)(words.size()*4),hash);NSMutableString *digest=[NSMutableString string];for(unsigned i=0;i<32;i++)[digest appendFormat:@"%02x",hash[i]];
 return @{@"draw":@(draw),@"pixels":@(words.size()),@"sha256":digest,@"status":@(command.status),@"gpu_start":@(command.GPUStartTime),@"gpu_end":@(command.GPUEndTime)};
}
int main() {
 @autoreleasepool { @try {
  require(getpid()==1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096,61);
  // Automatic original discovery remains honestly empty. Only the explicit
  // own instrument constructs this partial frontend; system clients cannot
  // mistake it for a finished Metal driver.
  require(MTLCreateSystemDefaultDevice()==nil,120);
  id<MTLDevice> device=offline_metal_frontend_open();require(device && device.name.length && [device isKindOfClass:NSClassFromString(@"_MTLDevice")],128);
  id<MTLCommandQueue> queue=[device newCommandQueue];require(queue && queue.device==device,129);
  id<MTLLibrary> qc=library(device,@"/System/Library/Frameworks/QuartzCore.framework/default.metallib",167);
  (void)library(device,@"/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib",73);
  id<MTLFunction> vertex=[qc newFunctionWithName:@"read_surf_vert"],fragment=[qc newFunctionWithName:bgra?@"attachment_clear_frag_lpf":@"path_clear_frag_lpf"];
  require(vertex && fragment && vertex.functionType==MTLFunctionTypeVertex && fragment.functionType==MTLFunctionTypeFragment,130);
  // An actual unknown library identity and function name must refuse.
  static const char unknown[]="not-an-original-metal-library";dispatch_data_t bad=dispatch_data_create(unknown,sizeof(unknown),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError *negative=nil;require(![device newLibraryWithData:bad error:&negative] && negative && ![qc newFunctionWithName:@"__absent_offline_function__"],131);
  MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=vertex;pd.fragmentFunction=fragment;pd.colorAttachments[attachment].pixelFormat=format;
  NSError *error=nil;id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];require(pipeline && !error,132);
  IOSurfaceRef surface=nullptr,alias=nullptr;mach_port_t port=MACH_PORT_NULL;
#if defined(OFFLINE_METAL_IOSURFACE_DIAGNOSTIC) && OFFLINE_METAL_IOSURFACE_DIAGNOSTIC
  NSDictionary *properties=@{(__bridge NSString *)kIOSurfaceWidth:@64,(__bridge NSString *)kIOSurfaceHeight:@64,
      (__bridge NSString *)kIOSurfaceBytesPerElement:@4,(__bridge NSString *)kIOSurfaceBytesPerRow:@256,
      (__bridge NSString *)kIOSurfacePixelFormat:@(surfaceFormat),(__bridge NSString *)kIOSurfaceAllocSize:@16384};
  surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);require(surface!=nullptr,140);
  require(IOSurfaceGetWidth(surface)==64 && IOSurfaceGetHeight(surface)==64 && IOSurfaceGetBytesPerRow(surface)>=256
      && IOSurfaceGetAllocSize(surface)>=16384 && IOSurfaceGetPixelFormat(surface)==surfaceFormat,141);
  require(!IOSurfaceLock(surface,0,nullptr),142);
  unsigned char *base=(unsigned char *)IOSurfaceGetBaseAddress(surface);require(base!=nullptr,141);
  for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)((uint32_t *)(base+y*IOSurfaceGetBytesPerRow(surface)))[x]=initialPixel;
  require(!IOSurfaceUnlock(surface,0,nullptr),142);
  port=IOSurfaceCreateMachPort(surface);require(MACH_PORT_VALID(port),143);
  alias=IOSurfaceLookupFromMachPort(port);require(alias && IOSurfaceGetID(alias)==IOSurfaceGetID(surface),144);
  require(!IOSurfaceLock(alias,kIOSurfaceLockReadOnly,nullptr),142);
  require(*(const uint32_t *)IOSurfaceGetBaseAddress(alias)==initialPixel,145);
  require(!IOSurfaceUnlock(alias,kIOSurfaceLockReadOnly,nullptr),142);
  MTLTextureDescriptor *wrongPlane=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
  require(![device newTextureWithDescriptor:wrongPlane iosurface:surface plane:1],148);
  if(bgra) {
   MTLTextureDescriptor *mismatch=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:64 height:64 mipmapped:NO];
   require(![device newTextureWithDescriptor:mismatch iosurface:surface plane:0],148);
  }
#endif
  NSDictionary *clear=render(device,queue,pipeline,false,surface,alias),*draw=render(device,queue,pipeline,true,surface,alias);
  NSDictionary *report=@{@"name":device.name,@"clear":clear,@"draw":draw,@"explicit_partial_frontend":@YES,@"system_device_discovery":@NO,
      @"actual_iosurface":@(surface!=nullptr),@"surface_id":@(surface?IOSurfaceGetID(surface):0),@"surface_bytes":@(surface?IOSurfaceGetAllocSize(surface):0),
      @"adreno_execution":@NO,@"original_desktop":@NO};
  NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];require(json && !error && [json writeToFile:@"/private/var/tmp/offline-metal-api-report.json" options:0 error:&error] && !error,133);
  if(alias)CFRelease(alias);if(port)require(!mach_port_deallocate(mach_task_self(),port),143);if(surface)CFRelease(surface);
  offline_metal_frontend_finish();
#if defined(OFFLINE_METAL_IOSURFACE_DIAGNOSTIC) && OFFLINE_METAL_IOSURFACE_DIAGNOSTIC
  _exit(bgra?151:149);
#else
  _exit(119);
#endif
 } @catch(NSException *exception) {
  [exception.reason writeToFile:@"/private/var/tmp/offline-metal-api-error.txt" atomically:YES encoding:NSUTF8StringEncoding error:nil];_exit(134);
 } }
}
