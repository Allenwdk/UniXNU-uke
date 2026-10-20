// SPDX-License-Identifier: MIT
// Host-only GPU feasibility instrument; never a sheng deployment backend.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CommonCrypto/CommonDigest.h>
#include <cstdio>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "OfflineGPUWire.h"
#include "HostMetalCommandServer.h"
#include "Library.h"
static void require(bool value,const char *why) {if(!value){fprintf(stderr,"%s\n",why);exit(1);}}
static NSString *digest(const void *bytes,size_t count) {
 require(count<=UINT32_MAX,"Bounded complete hash input");
 unsigned char hash[32];CC_SHA256(bytes,(CC_LONG)count,hash);
 NSMutableString *result=[NSMutableString string];for(unsigned j=0;j<32;j++)[result appendFormat:@"%02x",hash[j]];
 return result;
}
static int moduleHash(const uint8_t *p,size_t n,uint8_t out[32],void *) {
 if(n>UINT32_MAX)return 1;CC_SHA256(p,(CC_LONG)n,out);return 0;
}
static id<MTLLibrary> loadOriginal(id<MTLDevice> device,const char *path,NSString *expected,NSMutableArray *rows) {
 NSError *error=nil;
 NSData *bytes=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path] options:NSDataReadingMappedIfSafe error:&error];
 require(bytes && bytes.length<=AIR_LIBRARY_MAX_BYTES,"Bounded complete original library required");
 require([digest(bytes.bytes,bytes.length) isEqualToString:expected],"Complete original library identity differs");
 air_library_view view={};require(!air_library_open((const uint8_t *)bytes.bytes,bytes.length,moduleHash,nullptr,&view),"Original AIR records/module hashes invalid");
 // Pass only the unchanged, independently selected AIR-bearing slice. The
 // original QuartzCore fat container also has a native GPU Mach-O slice;
 // that native slice is deliberately excluded from this compilation test.
 const void *selected=(const uint8_t *)bytes.bytes+view.slice_offset;
 dispatch_data_t data=dispatch_data_create(selected,view.slice_bytes,dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{(void)[bytes length];});
 id<MTLLibrary> library=[device newLibraryWithData:data error:&error];
 if(!library){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);exit(4);}
 NSSet *names=[NSSet setWithArray:library.functionNames];require(names.count==view.count,"Actual original function-name inventory differs");
 for(uint32_t i=0;i<view.count;i++) {
  air_library_function f={};require(!air_library_get(&view,i,&f),"Actual original function record");
  NSString *name=[[NSString alloc] initWithBytes:f.name length:f.name_bytes encoding:NSUTF8StringEncoding];
  require([names containsObject:name],"Host library omitted an original function identity");
 }
 [rows addObject:@{@"input_sha256":expected,@"input_bytes":@(bytes.length),@"air_slice_offset":@(view.slice_offset),
  @"air_slice_bytes":@(view.slice_bytes),@"air_slice_sha256":digest(selected,view.slice_bytes),@"function_count":@(view.count),
  @"all_names_matched":@YES,@"all_module_hashes_checked":@YES,@"air_slice_only":@YES}];
 return library;
}
static NSDictionary *pass(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLRenderPipelineState> pipeline,bool draw,std::vector<uint32_t> *pixels=nullptr,bool bgra=false) {
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bgra?MTLPixelFormatBGRA8Unorm:MTLPixelFormatR32Float width:64 height:64 mipmapped:NO];
 td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
 id<MTLTexture> texture=[device newTextureWithDescriptor:td];require(texture!=nil,"Actual texture allocation failed");
 MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
 unsigned attachment=bgra?0:2;
 rp.colorAttachments[attachment].texture=texture;rp.colorAttachments[attachment].loadAction=MTLLoadActionClear;
 rp.colorAttachments[attachment].storeAction=MTLStoreActionStore;rp.colorAttachments[attachment].clearColor=bgra?MTLClearColorMake(0.2,0.4,0.6,1):MTLClearColorMake(1,0,0,0);
 id<MTLCommandBuffer> buffer=[queue commandBuffer];require(buffer!=nil,"Actual command buffer allocation failed");
 id<MTLRenderCommandEncoder> encoder=[buffer renderCommandEncoderWithDescriptor:rp];require(encoder!=nil,"Render encoder failed");
 if(draw){[encoder setRenderPipelineState:pipeline];[encoder setCullMode:MTLCullModeNone];[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];}
 [encoder endEncoding];[buffer commit];[buffer waitUntilCompleted];
 require(buffer.status==MTLCommandBufferStatusCompleted && buffer.error==nil,"Actual GPU command failed");
 std::vector<uint32_t> words(64*64,0x6b6b6b6b);
 [texture getBytes:words.data() bytesPerRow:64*4 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
 uint32_t expected=draw?0:(bgra?0xff336699:0x3f800000);for(uint32_t value:words)require(value==expected,"Actual texture pixels differ");
 unsigned char hash[32];CC_SHA256(words.data(),(CC_LONG)(words.size()*4),hash);
 NSMutableString *digest=[NSMutableString string];for(unsigned j=0;j<32;j++)[digest appendFormat:@"%02x",hash[j]];
 size_t verifiedPixels=words.size();if(pixels)*pixels=std::move(words);
 return @{@"original_shader_draw":@(draw),@"verified_pixels":@(verifiedPixels),@"expected_pixel_bits":@(expected),@"readback_sha256":digest,
  @"command_status":@(buffer.status),@"gpu_start":@(buffer.GPUStartTime),@"gpu_end":@(buffer.GPUEndTime)};
}
static bool readExact(int fd,void *data,size_t bytes) {
 unsigned char *p=(unsigned char *)data;
 while(bytes){ssize_t n=read(fd,p,bytes);if(n<0 && errno==EINTR)continue;if(n<=0)return false;p+=n;bytes-=n;}return true;
}
static bool writeExact(int fd,const void *data,size_t bytes) {
 const unsigned char *p=(const unsigned char *)data;
 while(bytes){ssize_t n=write(fd,p,bytes);if(n<0 && errno==EINTR)continue;if(n<=0)return false;p+=n;bytes-=n;}return true;
}
static NSArray *serve(const char *path,id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLRenderPipelineState> pipeline) {
 require(strlen(path)<sizeof(((sockaddr_un *)nullptr)->sun_path),"Private Unix socket path too long");
 int listener=socket(AF_UNIX,SOCK_STREAM,0);require(listener>=0,"Socket creation failed");
 sockaddr_un address={};address.sun_family=AF_UNIX;strcpy(address.sun_path,path);
 // Refuse existing paths; the caller owns a fresh private temporary directory.
 require(bind(listener,(sockaddr *)&address,sizeof(address))==0 && listen(listener,1)==0,"Fresh private socket bind/listen failed");
 fprintf(stderr,"OFFLINE_GPU_READY\n");fflush(stderr);
 int fd=accept(listener,nullptr,nullptr);require(fd>=0,"QEMU connection failed");close(listener);
 timeval timeout={90,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
 int noSigPipe=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&noSigPipe,sizeof(noSigPipe));
 NSMutableArray *observations=[NSMutableArray array];unsigned bootBytes=0;
 for(uint32_t request=1;request<=2;request++) {
  GPUWireHeader header={};
  if(request==1) {
   // UEFI may use standard port zero as its console before XNU resets the
   // device. Consume at most 64 KiB of that genuine boot-console prefix.
   uint32_t prefix=0;unsigned count=0;
   do {unsigned char byte=0;require(readExact(fd,&byte,1),"No framed native GPU request after boot console");
       prefix=(prefix>>8)|((uint32_t)byte<<24);count++;require(count<=65536,"Bounded boot-console prefix exceeded");
   } while(count<4 || prefix!=GPU_WIRE_MAGIC);
   bootBytes=count-4;header.magic=prefix;
   require(readExact(fd,(unsigned char *)&header+4,sizeof(header)-4),"Incomplete native GPU header");
  } else require(readExact(fd,&header,sizeof(header)),"Incomplete real guest request");
  uint16_t operation=request==1?GPU_WIRE_CLEAR:GPU_WIRE_ORIGINAL_DRAW;
  require(header.magic==GPU_WIRE_MAGIC && header.version==GPU_WIRE_VERSION && header.operation==operation && !header.bytes && header.request==request,"Unknown or out-of-order offline GPU request");
  std::vector<uint32_t> pixels;NSDictionary *observation=pass(device,queue,pipeline,operation==GPU_WIRE_ORIGINAL_DRAW,&pixels);
  require(pixels.size()*sizeof(uint32_t)==GPU_WIRE_PIXEL_BYTES,"Complete actual GPU readback required");
  double start=[observation[@"gpu_start"] doubleValue],end=[observation[@"gpu_end"] doubleValue];
  require(start>0 && end>start,"Actual GPU timing required");
  GPUWireResult result={GPU_WIRE_WIDTH,GPU_WIRE_HEIGHT,GPU_WIRE_R32_FLOAT,4,(uint64_t)(start*1e9),(uint64_t)(end*1e9)};
  header.operation|=GPU_WIRE_RESPONSE;header.bytes=sizeof(result)+GPU_WIRE_PIXEL_BYTES;
  require(writeExact(fd,&header,sizeof(header)) && writeExact(fd,&result,sizeof(result)) && writeExact(fd,pixels.data(),GPU_WIRE_PIXEL_BYTES),"Actual GPU reply failed");
  [observations addObject:observation];
 }
 fprintf(stderr,"UEFI console prefix bytes: %u\n",bootBytes);
 close(fd);unlink(path);return observations;
}
int main(int argc,char **argv) {
 @autoreleasepool {
  bool bgra=argc==4 && !strcmp(argv[3],"--bgra");
  require(argc==3 || bgra || (argc==5 && (!strcmp(argv[3],"--socket") || !strcmp(argv[3],"--metal-socket") || (!strcmp(argv[3],"--metal-session-socket") || !strcmp(argv[3],"--metal-session-socket-one")))),"Pass both unchanged original system libraries and optional private socket/BGRA qualification");
  id<MTLDevice> device=MTLCreateSystemDefaultDevice();require(device!=nil,"No host Metal device");NSError *error=nil;
  NSMutableArray *rows=[NSMutableArray array];
  id<MTLLibrary> library=loadOriginal(device,argv[1],@"606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9",rows);
  id<MTLLibrary> renderbox=loadOriginal(device,argv[2],@"d76883f626b1527c36fb0940cf27dcade503a55cf2fa05c8f7a247a40ccebec4",rows);
  if(argc==5 && (!strcmp(argv[3],"--metal-socket") || (!strcmp(argv[3],"--metal-session-socket") || !strcmp(argv[3],"--metal-session-socket-one")))) {
   NSDictionary *report=ServeOfflineMetalCommands(argv[4],device,@[library,renderbox],rows,!strcmp(argv[3],"--metal-session-socket-one")?1:!strcmp(argv[3],"--metal-session-socket")?2:0);
   require(report!=nil,"Actual offline Metal command server failed");
   NSData *out=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];fwrite(out.bytes,1,out.length,stdout);puts("");
   return [report[@"transport_healthy"] boolValue] && [report[@"session_finished"] boolValue]?0:2;
  }
  id<MTLFunction> vs=[library newFunctionWithName:@"read_surf_vert"],fs=[library newFunctionWithName:bgra?@"attachment_clear_frag_lpf":@"path_clear_frag_lpf"];
  require(vs && fs && vs.functionType==MTLFunctionTypeVertex && fs.functionType==MTLFunctionTypeFragment,"Actual original functions unavailable");
  MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=vs;pd.fragmentFunction=fs;pd.colorAttachments[bgra?0:2].pixelFormat=bgra?MTLPixelFormatBGRA8Unorm:MTLPixelFormatR32Float;
  id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
  if(!pipeline){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 4;}
  id<MTLCommandQueue> queue=[device newCommandQueue];require(queue!=nil,"Actual queue failed");
  if(argc==5) {
   NSArray *observations=serve(argv[4],device,queue,pipeline);
   NSDictionary *report=@{@"host_device":device.name,@"original_libraries":rows,@"transport_observations":observations,
      @"actual_gpu_pixel_bytes_sent":@(GPU_WIRE_PIXEL_BYTES*2),@"cpu_pixel_renderer":@NO,
      @"guest_metal_device":@NO,@"adreno_execution":@NO,@"original_desktop":@NO};
   NSData *out=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];fwrite(out.bytes,1,out.length,stdout);puts("");return 0;
  }
  NSDictionary *control=pass(device,queue,pipeline,false,nullptr,bgra),*draw=pass(device,queue,pipeline,true,nullptr,bgra);
  NSDictionary *report=@{@"host_device":device.name,@"original_libraries":rows,@"cpu_pixel_renderer":@NO,@"unchanged_original_functions":@[vs.name,fs.name],@"color_attachment":@(bgra?0:2),@"bgra8":@(bgra),
   @"clear_only_control":control,@"original_draw":draw,@"guest_device_created":@NO,@"adreno_execution":@NO,@"original_desktop":@NO};
  NSData *out=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];fwrite(out.bytes,1,out.length,stdout);puts("");
 }
 return 0;
}
