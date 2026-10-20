// SPDX-License-Identifier: MIT
// Actual original CAMetalLayer drawables on the explicit private Metal device.
// No normal discovery, original-method replacement or claimed desktop.
#define OFFLINE_RENDERBOX_EXTERNAL_MAIN 1
#define OFFLINE_RENDERBOX_PRESENTATION 1
#include "OfflineRenderBoxDraw.mm"
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#ifndef OFFLINE_METAL_ASYNC_DIAGNOSTIC
#define OFFLINE_METAL_ASYNC_DIAGNOSTIC 0
#endif
#ifndef OFFLINE_METAL_FENCE_DIAGNOSTIC
#define OFFLINE_METAL_FENCE_DIAGNOSTIC 0
#endif
int main() {
 @autoreleasepool { @try {
  mountOwnMetalAddon();require(getpid()==1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096 && MTLCreateSystemDefaultDevice()==nil,430);
  id<MTLDevice> device=offline_metal_frontend_open_service("NativeOfflineGPU");require(device && actualOriginalRenderTarget,431);
#if OFFLINE_METAL_ASYNC_DIAGNOSTIC
  void *asyncImage=dlopen(pluginBundle.executablePath.fileSystemRepresentation,RTLD_NOW|RTLD_LOCAL);require(asyncImage!=nullptr,454);
  auto enableAsync=(BOOL (*)(void))dlsym(asyncImage,"offline_metal_frontend_enable_async");auto directFinish=(void (*)(void))dlsym(asyncImage,"offline_metal_frontend_finish");require(enableAsync && directFinish && enableAsync(),455);
  NSLock *submissionGuard=[NSLock new];unsigned schedulingWaitReturned=0;NSObject *callbackGuard=[NSObject new];pthread_t mainThread=pthread_self();__block unsigned reentrantWaits=0,finishRefusals=0,callbackThreads=0;unsigned initialAsyncStatus=0;id<MTLCommandBuffer> peerCommand=nil;id<MTLTexture> peerTexture=nil;id<MTLEvent> event=[device newEvent];__block unsigned peerCallbacks=0;
  require(event && event.device==device && [(id)event conformsToProtocol:@protocol(MTLEvent)],456);
#endif
  phase=100;CAMetalLayer *layer=[CAMetalLayer layer];require(layer!=nil,432);
  phase=101;layer.device=device;
  phase=102;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
  phase=103;layer.drawableSize=CGSizeMake(64,64);
  phase=104;id<CAMetalDrawable> drawable=[layer nextDrawable];require(drawable && drawable.texture,433);
  phase=105;require(drawable.texture.width==64 && drawable.texture.height==64 && drawable.texture.pixelFormat==MTLPixelFormatBGRA8Unorm && drawable.texture.iosurface,434);
  NSError *error=nil;id<MTLLibrary> library=[device newLibraryWithFile:@"/System/Library/Frameworks/QuartzCore.framework/default.metallib" error:&error];require(library && !error,435);
  MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[library newFunctionWithName:@"read_surf_vert"];pd.fragmentFunction=[library newFunctionWithName:@"read_surf_frag"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
  id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];require(pipeline && !error,436);
  id<MTLCommandQueue> queue=[device newCommandQueue];id<MTLCommandBuffer> command=[queue commandBuffer];[command enqueue];
  MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=drawable.texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(1,1,1,1);
  id<MTLTexture> renderTarget=drawable.texture;id<MTLFence> fence=nil,unusedFence=nil;unsigned fenceRefusals=0;
#if OFFLINE_METAL_FENCE_DIAGNOSTIC
  MTLTextureDescriptor *untracked=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];untracked.storageMode=MTLStorageModePrivate;untracked.hazardTrackingMode=MTLHazardTrackingModeUntracked;untracked.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
  renderTarget=[device newTextureWithDescriptor:untracked];fence=[device newFence];unusedFence=[device newFence];
  require(renderTarget && renderTarget.hazardTrackingMode==MTLHazardTrackingModeUntracked && renderTarget.resourceOptions==(MTLResourceStorageModePrivate|MTLResourceHazardTrackingModeUntracked) && fence && unusedFence && fence.device==device && [(id)fence conformsToProtocol:@protocol(MTLFence)],448);pass.colorAttachments[0].texture=renderTarget;NSMutableString *label=[@"actual-fence" mutableCopy];fence.label=label;[label appendString:@"-changed"];require([fence.label isEqualToString:@"actual-fence"],452);fence.label=nil;require(fence.label==nil,453);
#endif
  phase=106;id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,437);
  [encoder setRenderPipelineState:pipeline];[encoder setCullMode:MTLCullModeNone];[encoder setFragmentTexture:actualOriginalRenderTarget atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
#if OFFLINE_METAL_FENCE_DIAGNOSTIC
  @try {[encoder updateFence:(id<MTLFence>)[NSNull null] afterStages:MTLRenderStageFragment];} @catch(NSException *e){if([e.name isEqualToString:@"OfflineMetalTransport"])fenceRefusals|=1;}
  @try {[encoder updateFence:fence afterStages:(MTLRenderStages)0];} @catch(NSException *e){if([e.name isEqualToString:@"OfflineMetalTransport"])fenceRefusals|=2;}
  @try {[encoder waitForFence:unusedFence beforeStages:MTLRenderStageFragment];} @catch(NSException *e){if([e.name isEqualToString:@"OfflineMetalTransport"])fenceRefusals|=4;}
  [encoder updateFence:fence afterStages:MTLRenderStageFragment];
  @try {[encoder waitForFence:fence beforeStages:MTLRenderStageFragment];} @catch(NSException *e){if([e.name isEqualToString:@"OfflineMetalTransport"])fenceRefusals|=8;}
#endif
  [encoder endEncoding];
#if OFFLINE_METAL_FENCE_DIAGNOSTIC
  @try {[encoder updateFence:fence afterStages:MTLRenderStageFragment];} @catch(NSException *e){if([e.name isEqualToString:@"OfflineMetalTransport"])fenceRefusals|=16;}
  require(fenceRefusals==31,449);id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];require(blit!=nil,450);[blit waitForFence:fence];[blit copyFromTexture:renderTarget toTexture:drawable.texture];[blit updateFence:fence];[blit endEncoding];
  pass.colorAttachments[0].texture=drawable.texture;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
  encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,451);[encoder waitForFence:fence beforeStages:MTLRenderStageVertex|MTLRenderStageFragment];[encoder setRenderPipelineState:pipeline];[encoder setCullMode:MTLCullModeNone];[encoder setFragmentTexture:renderTarget atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];[encoder endEncoding];
#endif
#if OFFLINE_METAL_ASYNC_DIAGNOSTIC
  [command encodeSignalEvent:event value:1];
#endif
  phase=107;unsigned refusals=0;
  @try {[command presentDrawable:(id<MTLDrawable>)[NSNull null]];}
  @catch(NSException *exception){if([exception.name isEqualToString:@"OfflineMetalTransport"])refusals|=1;}
  require(refusals==1,442);[command presentDrawable:drawable];
  @try {[command presentDrawable:drawable];}
  @catch(NSException *exception){if([exception.name isEqualToString:@"OfflineMetalTransport"])refusals|=2;}
  require(refusals==3,443);__block unsigned callbacks=0;
  [command addScheduledHandler:^(id<MTLCommandBuffer> actual){
#if OFFLINE_METAL_ASYNC_DIAGNOSTIC
   [submissionGuard lock];
#endif
   require(actual==command && actual.status==MTLCommandBufferStatusCompleted && callbacks==0,444);callbacks=1;
#if OFFLINE_METAL_ASYNC_DIAGNOSTIC
   [submissionGuard unlock];
#endif
  }];
  [command addCompletedHandler:^(id<MTLCommandBuffer> actual){require(actual==command && actual.status==MTLCommandBufferStatusCompleted && callbacks==1,445);
#if OFFLINE_METAL_ASYNC_DIAGNOSTIC
   require(!pthread_equal(pthread_self(),mainThread),457);[actual waitUntilCompleted];@synchronized(callbackGuard){callbackThreads|=1;reentrantWaits++;}
   @try {directFinish();}@catch(NSException *e){if([e.name isEqualToString:@"OfflineMetalTransport"])finishRefusals++;}
#endif
   callbacks=2;}];
#if OFFLINE_METAL_ASYNC_DIAGNOSTIC
  [submissionGuard lock];
#endif
  phase=108;[command commit];
#if OFFLINE_METAL_ASYNC_DIAGNOSTIC
  [command waitUntilScheduled];schedulingWaitReturned=1;[submissionGuard unlock];initialAsyncStatus=command.status;require(initialAsyncStatus>=MTLCommandBufferStatusCommitted && initialAsyncStatus<=MTLCommandBufferStatusCompleted,458);
  MTLTextureDescriptor *peerDescriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];peerDescriptor.hazardTrackingMode=MTLHazardTrackingModeUntracked;peerDescriptor.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
  peerTexture=[device newTextureWithDescriptor:peerDescriptor];require(peerTexture && peerTexture.hazardTrackingMode==MTLHazardTrackingModeUntracked,459);
  id<MTLCommandQueue> peerQueue=[device newCommandQueue];peerCommand=[peerQueue commandBuffer];[peerCommand enqueue];[peerCommand encodeWaitForEvent:event value:1];
  id<MTLBlitCommandEncoder> peerBlit=[peerCommand blitCommandEncoder];require(peerBlit!=nil,460);[peerBlit copyFromTexture:drawable.texture toTexture:peerTexture];[peerBlit endEncoding];[peerCommand encodeSignalEvent:event value:2];
  [peerCommand addScheduledHandler:^(id<MTLCommandBuffer> actual){require(actual==peerCommand && actual.status==MTLCommandBufferStatusCompleted && peerCallbacks==0,461);peerCallbacks=1;}];
  [peerCommand addCompletedHandler:^(id<MTLCommandBuffer> actual){require(actual==peerCommand && actual.status==MTLCommandBufferStatusCompleted && peerCallbacks==1 && !pthread_equal(pthread_self(),mainThread),462);[actual waitUntilCompleted];@synchronized(callbackGuard){reentrantWaits++;callbackThreads|=2;}peerCallbacks=2;}];
  [peerCommand commit];[peerCommand waitUntilCompleted];require(peerCommand.status==MTLCommandBufferStatusCompleted && !peerCommand.error && peerCallbacks==2,463);
  uint32_t peerPixels[4096];[peerTexture getBytes:peerPixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)require(peerPixels[y*64+x]==(x<32?0xffff0000:0xff0000ff),464);
#endif
  [command waitUntilCompleted];require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,438);
  phase=109;IOSurfaceRef surface=drawable.texture.iosurface;require(!IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr),439);
  @try {[command presentDrawable:drawable];}
  @catch(NSException *exception){if([exception.name isEqualToString:@"OfflineMetalTransport"])refusals|=4;}
  require(refusals==7 && callbacks==2,446);
  const uint32_t *pixels=(const uint32_t *)IOSurfaceGetBaseAddress(surface);size_t stride=IOSurfaceGetBytesPerRow(surface)/4;require(pixels && stride>=64,440);
  for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)require(pixels[y*stride+x]==(x<32?0xffff0000:0xff0000ff),441);
  require(!IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr),439);
#if OFFLINE_METAL_ASYNC_DIAGNOSTIC
  __block unsigned closingCallbacks=0,closingPixels=0;id<MTLCommandBuffer> closingWork=[queue commandBuffer];[closingWork enqueue];
  MTLRenderPassDescriptor *closingPass=[MTLRenderPassDescriptor renderPassDescriptor];closingPass.colorAttachments[0].texture=peerTexture;closingPass.colorAttachments[0].loadAction=MTLLoadActionClear;closingPass.colorAttachments[0].storeAction=MTLStoreActionStore;closingPass.colorAttachments[0].clearColor=MTLClearColorMake(0,1,0,1);
  id<MTLRenderCommandEncoder> closingEncoder=[closingWork renderCommandEncoderWithDescriptor:closingPass];require(closingEncoder!=nil,466);[closingEncoder endEncoding];closingEncoder=nil;
  [closingWork addCompletedHandler:^(id<MTLCommandBuffer> actual){require(actual==closingWork && actual.status==MTLCommandBufferStatusCompleted && !actual.error && !pthread_equal(pthread_self(),mainThread),467);uint32_t pixels[4096];[peerTexture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];for(unsigned i=0;i<4096;i++)require(pixels[i]==0xff00ff00,468);closingPixels=4096;closingCallbacks=1;}];
  [closingWork commit];closingWork=nil;
#endif
  offline_metal_frontend_finish();memset(result,0,sizeof(result));result[0]=0x43414d4c;result[1]=getpid();result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);result[4]=IOSurfaceGetID(surface);result[5]=64;result[6]=64;result[7]=drawable.drawableID;
  // Actual presentedTime may remain zero without a server-attached layer.
  // Never manufacture a display timestamp or turn GPU completion into one.
  double presented=drawable.presentedTime;memcpy(&result[8],&presented,sizeof(presented));
  Method actualPresent=class_getInstanceMethod(object_getClass(drawable),@selector(present));require(actualPresent!=nullptr,447);
  result[9]=(uintptr_t)method_getImplementation(actualPresent);result[10]=refusals;result[11]=callbacks;
#if OFFLINE_METAL_FENCE_DIAGNOSTIC
  result[12]=fenceRefusals;result[13]=4;result[14]=renderTarget.hazardTrackingMode;result[15]=renderTarget.resourceOptions;result[16]=[(id)fence conformsToProtocol:@protocol(MTLFence)];
#else
  (void)renderTarget;(void)fence;(void)unusedFence;(void)fenceRefusals;
#endif
#if OFFLINE_METAL_ASYNC_DIAGNOSTIC
  require(callbackThreads==3 && reentrantWaits==2 && finishRefusals==1,465);result[17]=initialAsyncStatus;result[18]=4096;result[19]=peerCallbacks;result[20]=[(id)event conformsToProtocol:@protocol(MTLEvent)];result[21]=callbackThreads;result[22]=reentrantWaits;result[23]=finishRefusals;require(closingCallbacks==1 && closingPixels==4096,469);result[24]=closingCallbacks;result[25]=closingPixels;require(schedulingWaitReturned==1,470);result[26]=schedulingWaitReturned;
#endif
  register const uint64_t *packet __asm__("x19")=result;register const char *name __asm__("x20")=class_getName(object_getClass(drawable));
  __asm__ volatile("brk #0" : : "r"(packet),"r"(name) : "memory");__builtin_unreachable();
 } @catch(NSException *exception){observeFailure(exception.reason);} }
}
