// SPDX-License-Identifier: MIT
// Two actual original-runtime processes, private kernel GPU sessions and
// genuine Metal/IOSurface output. No default-device or desktop substitution.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
extern "C" id<MTLDevice> offline_metal_frontend_open_service(const char *);
extern "C" void offline_metal_frontend_finish(void);
extern char **environ;
static uint64_t result[32];
static int workerOutput=-1;
static void require(bool ok,int code) {
 if(ok)return;
 if(getpid()==1) {
  result[0]=0x47504641;result[1]=1;result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);
  result[22]=(uint64_t)code;result[23]=(uint64_t)errno;
  register const uint64_t *pointer __asm__("x19")=result;__asm__ volatile("brk #0" : : "r"(pointer) : "memory");__builtin_unreachable();
 }
 if(workerOutput>=0){uint64_t failure[8]={0x47504641,(uint64_t)getpid(),(uint64_t)code,(uint64_t)errno,0,0,0,0};(void)write(workerOutput,failure,sizeof(failure));}
 _exit(code);
}
static void transfer(int fd,void *data,size_t size,bool writing) {
 auto p=(unsigned char *)data;
 while(size){ssize_t n=writing?write(fd,p,size):read(fd,p,size);if(n<0 && errno==EINTR)continue;require(n>0,170);p+=n;size-=n;}
}
static int worker(unsigned ordinal,int input,int output) {
 for(int fd=3;fd<64;fd++)if(fd!=input && fd!=output)close(fd);
 workerOutput=output;
 @autoreleasepool { @try {
  require(getpid()>1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096 && MTLCreateSystemDefaultDevice()==nil,171);
  id<MTLDevice> device=offline_metal_frontend_open_service("NativeOfflineGPU");require(device!=nil,172);
  id<MTLCommandQueue> queue=[device newCommandQueue];require(queue!=nil,173);
  NSDictionary *properties=@{(__bridge NSString *)kIOSurfaceWidth:@64,(__bridge NSString *)kIOSurfaceHeight:@64,
   (__bridge NSString *)kIOSurfaceBytesPerElement:@4,(__bridge NSString *)kIOSurfaceBytesPerRow:@256,
   (__bridge NSString *)kIOSurfaceAllocSize:@16384,(__bridge NSString *)kIOSurfacePixelFormat:@(0x42475241)};
  IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);require(surface!=nullptr,174);
  MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
  td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
  id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:surface plane:0];require(texture!=nil,175);
  uint32_t expected=ordinal?0xffcc6633:0xff336699;
  auto render=[&]{
   MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
   pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
   pass.colorAttachments[0].clearColor=ordinal?MTLClearColorMake(.8,.4,.2,1):MTLClearColorMake(.2,.4,.6,1);
   id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,176);
   [encoder endEncoding];[command commit];[command waitUntilCompleted];
   require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,177);
   require(!IOSurfaceLock(surface,kIOSurfaceLockReadOnly,nullptr),178);
   const uint32_t *pixels=(const uint32_t *)IOSurfaceGetBaseAddress(surface);require(pixels!=nullptr,179);
   for(unsigned i=0;i<4096;i++)require(pixels[i]==expected,180);
   require(!IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr),178);
  };
  render();
  uint64_t handle=[[(NSObject *)texture valueForKey:@"handle"] unsignedLongLongValue];require(handle!=0,181);
  uint64_t state[8]={(uint64_t)getpid(),(uint64_t)getpagesize(),(uint64_t)sysconf(_SC_PAGESIZE),handle,IOSurfaceGetID(surface),4096,0,1};
  transfer(output,state,sizeof(state),true);
  uint64_t command=0;transfer(input,&command,sizeof(command),false);
  if(ordinal) {
   // Mutate only this owned proxy's handle for a genuine foreign-resource
   // refusal. Its original handle is restored before any further lifetime
   // operation. No original object method or executable is changed.
   require(command && command!=handle,182);BOOL refused=NO;uint32_t pixels[4096];
   [(NSObject *)texture setValue:@(command) forKey:@"handle"];
   @try {[texture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];}
   @catch(NSException *exception){refused=[exception.name isEqualToString:@"OfflineMetalTransport"];}
   @finally {[(NSObject *)texture setValue:@(handle) forKey:@"handle"];}
   require(refused,183);
   [texture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
   for(unsigned i=0;i<4096;i++)require(pixels[i]==expected,184);
   state[6]=1;state[7]=2;transfer(output,state,sizeof(state),true);
   transfer(input,&command,sizeof(command),false);require(command==1,185);
   render();state[7]=3;
  } else require(command==1,185);
  offline_metal_frontend_finish();state[7]=4;transfer(output,state,sizeof(state),true);
  CFRelease(surface);return 0;
 } @catch(NSException *exception) {
  uint64_t failure[8]={0x47504641,(uint64_t)getpid(),186,(uint64_t)errno,0,0,0,0};
  const char *why=exception.reason.UTF8String;if(why){size_t size=strlen(why);if(size>32)why+=size-32;memcpy(failure+4,why,MIN(size,(size_t)32));}
  transfer(output,failure,sizeof(failure),true);
  [exception.reason writeToFile:@"/private/var/tmp/offline-metal-processes-error.txt" atomically:YES encoding:NSUTF8StringEncoding error:nil];return 186;
 } }
}
int main(int argc,char **argv) {
 if(argc==5 && !strcmp(argv[1],"worker"))return worker((unsigned)strtoul(argv[4],nullptr,10),(int)strtol(argv[2],nullptr,10),(int)strtol(argv[3],nullptr,10));
 require(argc==1 && getpid()==1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096,187);
 int commands[2][2],replies[2][2];pid_t children[2]={};
 for(unsigned i=0;i<2;i++)require(!pipe(commands[i]) && !pipe(replies[i]),188);
 for(unsigned i=0;i<2;i++) {
  char in[16],out[16],ordinal[16];snprintf(in,sizeof(in),"%d",commands[i][0]);snprintf(out,sizeof(out),"%d",replies[i][1]);snprintf(ordinal,sizeof(ordinal),"%u",i);
  char *arguments[]={(char *)"air-native",(char *)"worker",in,out,ordinal,nullptr};
  int status=posix_spawn(&children[i],"/private/var/tmp/air-native",nullptr,nullptr,arguments,environ);
  result[24]=status;result[25]=i;require(!status,189);
 }
 for(unsigned i=0;i<2;i++){close(commands[i][0]);close(replies[i][1]);}
 uint64_t first[8]={},second[8]={};transfer(replies[0][0],first,sizeof(first),false);memcpy(result+4,first,sizeof(first));
 transfer(replies[1][0],second,sizeof(second),false);memcpy(result+12,second,sizeof(second));
 if(first[0]==0x47504641 && second[0]==0x47504641) {
  int statuses[2]={-1,-1};
  for(unsigned i=0;i<2;i++)if(waitpid(children[i],&statuses[i],0)==children[i])result[20+i]=(uint32_t)statuses[i];
  result[26]=1;require(false,190);
 }
 require(first[0]==(uint64_t)children[0] && second[0]==(uint64_t)children[1] && first[0]!=second[0] && first[0]>1 && second[0]>1 &&
   first[1]==4096 && first[2]==4096 && second[1]==4096 && second[2]==4096 && first[3] && second[3] && first[3]!=second[3] &&
   first[4] && second[4] && first[4]!=second[4] && first[5]==4096 && second[5]==4096 && first[7]==1 && second[7]==1,190);
 transfer(commands[1][1],&first[3],sizeof(uint64_t),true);transfer(replies[1][0],second,sizeof(second),false);require(second[6]==1 && second[7]==2,191);
 uint64_t closeCommand=1;transfer(commands[0][1],&closeCommand,sizeof(closeCommand),true);transfer(replies[0][0],first,sizeof(first),false);require(first[7]==4,192);
 int status1=-1,status2=-1;require(waitpid(children[0],&status1,0)==children[0] && WIFEXITED(status1) && WEXITSTATUS(status1)==0,193);
 transfer(commands[1][1],&closeCommand,sizeof(closeCommand),true);transfer(replies[1][0],second,sizeof(second),false);require(second[6]==1 && second[7]==4,194);
 require(waitpid(children[1],&status2,0)==children[1] && WIFEXITED(status2) && WEXITSTATUS(status2)==0,195);
 result[0]=0x47505550;result[1]=1;result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);
 memcpy(result+4,first,sizeof(first));memcpy(result+12,second,sizeof(second));result[20]=status1;result[21]=status2;result[22]=1;
 register const uint64_t *pointer __asm__("x19")=result;__asm__ volatile("brk #0" : : "r"(pointer) : "memory");__builtin_unreachable();
}
