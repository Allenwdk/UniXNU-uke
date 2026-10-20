// SPDX-License-Identifier: MIT
// Own resource qualification: actual private RG16Float render and GPU blit.
#define OFFLINE_RENDERBOX_DRAW 1
#include "OfflineRenderBoxDevice.mm"
static void qualifyOriginalRenderBoxDrawing(id<MTLDevice> device,id rb) {
 (void)rb;phase=80;
 MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float width:64 height:64 mipmapped:NO];
 descriptor.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;descriptor.storageMode=MTLStorageModePrivate;
 id<MTLTexture> source=[device newTextureWithDescriptor:descriptor];require(source && source.storageMode==MTLStorageModePrivate,330);
 descriptor.storageMode=MTLStorageModeShared;id<MTLTexture> destination=[device newTextureWithDescriptor:descriptor];require(destination && destination.storageMode==MTLStorageModeShared,331);
 id<MTLCommandQueue> queue=[device newCommandQueue];id<MTLCommandBuffer> command=[queue commandBuffer];require(queue && command,332);
 [command enqueue];require(command.status==MTLCommandBufferStatusEnqueued,333);__block unsigned callbacks=0;
 [command addScheduledHandler:^(id<MTLCommandBuffer> actual){require(actual==command && actual.status==MTLCommandBufferStatusCompleted && callbacks==0,334);callbacks=1;}];
 [command addCompletedHandler:^(id<MTLCommandBuffer> actual){require(actual==command && actual.status==MTLCommandBufferStatusCompleted && actual.GPUEndTime>actual.GPUStartTime && callbacks==1,335);callbacks=2;}];
 for(unsigned i=0;i<2;i++) {
  MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=i?destination:source;
  pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
  pass.colorAttachments[0].clearColor=i?MTLClearColorMake(.75,.25,0,1):MTLClearColorMake(.25,.75,0,1);
  id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,336);[encoder endEncoding];
 }
 id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];require(blit!=nil,337);
 bool refused=false;@try {
  [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(63,0,0) sourceSize:MTLSizeMake(64,64,1)
      toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
 } @catch(NSException *exception){(void)exception;refused=true;}require(refused,338);
 [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1)
     toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];
 require([(id)command commitAndWaitUntilSubmitted],339);[command waitUntilCompleted];
 require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime && callbacks==2,340);
 uint32_t pixels[4096];for(unsigned i=0;i<4096;i++)pixels[i]=0xdeadbeef;
 [destination getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
 for(unsigned i=0;i<4096;i++)require(pixels[i]==0x3a003400,341); // actual half(.25),half(.75)
 for(unsigned i=0;i<4096;i++)pixels[i]=0xdeadbeef;
 refused=false;@try {[source getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];}
 @catch(NSException *exception){(void)exception;refused=true;}require(refused,342);
 for(unsigned i=0;i<4096;i++)require(pixels[i]==0xdeadbeef,343);phase=81;
}
