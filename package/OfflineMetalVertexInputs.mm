// SPDX-License-Identifier: MIT
// Original QuartzCore vertex input/MVP and real GPU coverage in each worker.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
extern "C" id<MTLDevice> offline_metal_frontend_open_service(const char *);
extern "C" id<MTLDevice> vertex_inputs_open_service(const char *);
static void qualifyVertexInputs(id<MTLDevice>);
#define offline_metal_frontend_open_service vertex_inputs_open_service
#include "OfflineMetalProcesses.mm"
#undef offline_metal_frontend_open_service

extern "C" id<MTLDevice> vertex_inputs_open_service(const char *name) {
 id<MTLDevice> device=offline_metal_frontend_open_service(name);require(device!=nil,210);
 qualifyVertexInputs(device);return device;
}
static void qualifyVertexInputs(id<MTLDevice> device) {
 NSError *error=nil;
 id<MTLLibrary> library=[device newLibraryWithFile:@"/System/Library/Frameworks/QuartzCore.framework/default.metallib" error:&error];
 require(library && !error && library.functionNames.count==167,211);
 id<MTLFunction> vertex=[library newFunctionWithName:@"std_vert0_lpf"],fragment=[library newFunctionWithName:@"attachment_clear_frag_lpf"];
 require(vertex && fragment,212);
 MTLRenderPipelineDescriptor *p=[MTLRenderPipelineDescriptor new];p.vertexFunction=vertex;p.fragmentFunction=fragment;
 p.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 MTLVertexDescriptor *v=[MTLVertexDescriptor vertexDescriptor];
 v.attributes[0].format=MTLVertexFormatFloat4;v.attributes[0].bufferIndex=0;
 v.attributes[1].format=MTLVertexFormatFloat2;v.attributes[1].offset=16;v.attributes[1].bufferIndex=0;
 v.attributes[3].format=MTLVertexFormatFloat4;v.attributes[3].offset=24;v.attributes[3].bufferIndex=0;
 v.layouts[0].stride=40;v.layouts[0].stepFunction=MTLVertexStepFunctionPerVertex;v.layouts[0].stepRate=1;p.vertexDescriptor=v;
 MTLRenderPipelineDescriptor *bad=[p copy];bad.vertexDescriptor=[v copy];bad.vertexDescriptor.attributes[0].format=(MTLVertexFormat)43;
 require(![device newRenderPipelineStateWithDescriptor:bad error:&error] && error,213);error=nil;
 bad.vertexDescriptor=[v copy];bad.vertexDescriptor.layouts[0].stride=NSUIntegerMax;
 require(![device newRenderPipelineStateWithDescriptor:bad error:&error] && error,214);error=nil;
 id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:p error:&error];require(pipeline && !error,215);
 // Descriptor mutations cannot retroactively change the genuine pipeline.
 p.vertexDescriptor.attributes[0].format=MTLVertexFormatInvalid;
 float values[4][10]={{-1,-1,0,1,0,0,0,1,0,1},{0,-1,0,1,0,0,0,1,0,1},
                       {-1,1,0,1,0,0,0,1,0,1},{0,1,0,1,0,0,0,1,0,1}};
 const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
 id<MTLBuffer> vertices=[device newBufferWithBytes:values length:sizeof(values) options:MTLResourceStorageModeShared];
 id<MTLBuffer> matrix=[device newBufferWithBytes:identity length:sizeof(identity) options:MTLResourceStorageModeShared];require(vertices && matrix,216);
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
 td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
 id<MTLTexture> texture=[device newTextureWithDescriptor:td];id<MTLCommandQueue> queue=[device newCommandQueue];require(texture && queue,217);
 for(unsigned iteration=0;iteration<2;iteration++) {
  if(iteration){for(unsigned i=0;i<4;i++)values[i][0]+=1;memcpy(vertices.contents,values,sizeof(values));}
  MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
  pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
  pass.colorAttachments[0].clearColor=MTLClearColorMake(1,0,0,1);
  id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,218);
  [encoder setRenderPipelineState:pipeline];[encoder setCullMode:MTLCullModeNone];
  [encoder setVertexBuffer:vertices offset:0 atIndex:0];[encoder setVertexBuffer:matrix offset:0 atIndex:2];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];[encoder endEncoding];
  [command commit];[command waitUntilCompleted];
  require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,219);
  uint32_t pixels[4096];[texture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
  for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)require(pixels[y*64+x]==((iteration?(x>=32):(x<32))?0:0xffff0000),220);
 }
}
