// SPDX-License-Identifier: MIT
// Own native resource test; unchanged original shader pair, actual GPU tests.
#define OFFLINE_RENDERBOX_DRAW 1
#include "OfflineRenderBoxDevice.mm"
@interface NSObject (OriginalMetalOptionalArchive)
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error;
@end
static void qualifyOriginalRenderBoxDrawing(id<MTLDevice> device,id rb) {
 (void)rb;phase=90;NSError *error=nil;
 require(![(id)device newPipelineLibraryWithFilePath:@"/System/Library/PrivateFrameworks/RenderBox.framework/default.pipelinelib" error:&error] &&
     error && error.code==-4 && [error.domain isEqualToString:@"org.blu-sharky.offline-metal"],370);
 NSString *path=@"/System/Library/Frameworks/QuartzCore.framework/default.metallib";
 id<MTLLibrary> library=(getpid()&1)?[device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error]:[device newLibraryWithFile:path error:&error];
 require(library && !error && library.functionNames.count==167,351);
 require(![device newLibraryWithURL:[NSURL URLWithString:@"data:text/plain,not-a-library"] error:&error] && error,371);
 require(![device newLibraryWithFile:@"/System/Library/Frameworks/QuartzCore.framework/__native_missing__.metallib" error:&error] && error,372);
 require(![device newLibraryWithFile:@"/System/Library/Frameworks/QuartzCore.framework/Info.plist" error:&error] && error && error.code==-4,373);
 NSBundle *bundle=[NSBundle bundleWithPath:@"/System/Library/Frameworks/QuartzCore.framework"];require(bundle!=nil,374);
 id<MTLLibrary> defaultLibrary=[device newDefaultLibraryWithBundle:bundle error:&error];require(defaultLibrary && !error && defaultLibrary.functionNames.count==167,375);
 id<MTLLibrary> rbLibrary=[device newLibraryWithFile:@"/System/Library/PrivateFrameworks/RenderBox.framework/default.metallib" error:&error];require(rbLibrary && !error && rbLibrary.functionNames.count==73,376);
 MTLFunctionConstantValues *constants=[MTLFunctionConstantValues new];uint32_t state[4]={2,0,5502022,4};
 [constants setConstantValue:state type:MTLDataTypeUInt4 withName:@"RB::Shader::Constant::_shader_state"];
 NSString *before=constants.description;
 id<MTLFunction> named=[rbLibrary newFunctionWithName:@"plane_color_fragment" constantValues:constants error:&error];
 require(named && !error && named.functionType==MTLFunctionTypeFragment && [named.name isEqualToString:@"plane_color_fragment"] && [before isEqualToString:constants.description],377);
 require(![rbLibrary newFunctionWithName:@"__native_missing__" constantValues:constants error:&error] && error && error.code==-4,378);
 [constants reset];float unsupported[4]={1,2,3,0};[constants setConstantValue:unsupported type:MTLDataTypeFloat3 atIndex:0];
 require(![rbLibrary newFunctionWithName:@"plane__vertex" constantValues:constants error:&error] && error && error.code==-4,379);
 [constants reset];[constants setConstantValue:state type:MTLDataTypeUInt4 atIndex:0];before=constants.description;
 id<MTLFunction> indexed=[rbLibrary newFunctionWithName:@"plane__vertex" constantValues:constants error:&error];
 require(indexed && !error && indexed.functionType==MTLFunctionTypeVertex && [before isEqualToString:constants.description],380);
 id<MTLFunction> incomplete=[rbLibrary newFunctionWithName:@"plane_color_fragment" constantValues:[MTLFunctionConstantValues new] error:&error];require(incomplete && !error,381);
 MTLRenderPipelineDescriptor *missing=[MTLRenderPipelineDescriptor new];missing.vertexFunction=indexed;missing.fragmentFunction=incomplete;
 missing.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;missing.colorAttachments[1].pixelFormat=MTLPixelFormatRG16Float;
 require([device newRenderPipelineStateWithDescriptor:missing error:&error] && !error,382);
 // This actual backend accepts an empty values set and its pipeline. Forward
 // the real result; it does not qualify the unspecified shader-state semantics.
 id<MTLFunction> vertex=[library newFunctionWithName:@"read_surf_vert"],fragment=[library newFunctionWithName:@"attachment_clear_frag_lpf"];require(vertex && fragment,352);
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
 td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
 id<MTLTexture> color=[device newTextureWithDescriptor:td];require(color!=nil,353);
 td.pixelFormat=MTLPixelFormatStencil8;td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;
 id<MTLTexture> stencil=[device newTextureWithDescriptor:td];require(stencil && stencil.storageMode==MTLStorageModePrivate,354);
 td.pixelFormat=MTLPixelFormatDepth32Float;id<MTLTexture> depth=[device newTextureWithDescriptor:td];require(depth && depth.storageMode==MTLStorageModePrivate,355);
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=vertex;pd.fragmentFunction=fragment;
 pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;pd.stencilAttachmentPixelFormat=MTLPixelFormatStencil8;pd.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
 id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];require(pipeline && !error,356);
 MTLDepthStencilDescriptor *ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=NO;
 MTLStencilDescriptor *face=[MTLStencilDescriptor new];face.stencilCompareFunction=MTLCompareFunctionEqual;
 face.stencilFailureOperation=MTLStencilOperationKeep;face.depthFailureOperation=MTLStencilOperationKeep;face.depthStencilPassOperation=MTLStencilOperationKeep;
 face.readMask=255;face.writeMask=0;ds.frontFaceStencil=face;ds.backFaceStencil=face;
 id<MTLDepthStencilState> always=[device newDepthStencilStateWithDescriptor:ds];require(always && always.device==device,357);
 ds.depthCompareFunction=MTLCompareFunctionLess;id<MTLDepthStencilState> less=[device newDepthStencilStateWithDescriptor:ds];require(less!=nil,358);
 id<MTLCommandQueue> queue=[device newCommandQueue];require(queue!=nil,359);
 for(unsigned test=0;test<4;test++) {
  phase=91+test;MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=(test&1)?MTLLoadActionLoad:MTLLoadActionClear;
  pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(.2,.4,.6,1);
  pass.stencilAttachment.texture=stencil;pass.stencilAttachment.loadAction=test?MTLLoadActionLoad:MTLLoadActionClear;
  pass.stencilAttachment.storeAction=MTLStoreActionStore;pass.stencilAttachment.clearStencil=1;
  pass.depthAttachment.texture=depth;pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=test==2?0:1;
  id<MTLCommandBuffer> command=[queue commandBuffer];[command enqueue];require(command.status==MTLCommandBufferStatusEnqueued,360);__block unsigned callbacks=0;
  [command addScheduledHandler:^(id<MTLCommandBuffer> actual){require(actual==command && actual.status==MTLCommandBufferStatusCompleted && callbacks==0,361);callbacks=1;}];
  [command addCompletedHandler:^(id<MTLCommandBuffer> actual){require(actual==command && actual.status==MTLCommandBufferStatusCompleted && actual.GPUEndTime>actual.GPUStartTime && callbacks==1,362);callbacks=2;}];
  id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,363);
  bool refused=false;@try {[encoder setDepthStencilState:(id<MTLDepthStencilState>)color];}@catch(NSException *exception){(void)exception;refused=true;}require(refused,364);
  [encoder setRenderPipelineState:pipeline];[encoder setDepthStencilState:test<2?always:less];[encoder setCullMode:MTLCullModeNone];
  if(test&1)[encoder setStencilFrontReferenceValue:1 backReferenceValue:1];else [encoder setStencilReferenceValue:test?1:2];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];[encoder endEncoding];
  if(test&1)require([(id)command commitAndWaitUntilSubmitted],365);else [command commit];[command waitUntilCompleted];
  require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime && callbacks==2,366);
  uint32_t pixels[4096];[color getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
  for(unsigned i=0;i<4096;i++)require(pixels[i]==((test&1)?0:0xff996633),367);
 }
 uint32_t pixels[4096];for(unsigned i=0;i<4096;i++)pixels[i]=0xdeadbeef;bool refused=false;
 @try {[stencil getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];}
 @catch(NSException *exception){(void)exception;refused=true;}require(refused,368);
 for(unsigned i=0;i<4096;i++)require(pixels[i]==0xdeadbeef,369);phase=95;
}
