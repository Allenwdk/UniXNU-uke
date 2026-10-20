// SPDX-License-Identifier: MIT
// Own diagnostic: genuine original IOSurface Mach rights and two real GPU clients.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <mach/mach.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
@interface NSObject (OriginalMetalCommandSubmission)
- (BOOL)commitAndWaitUntilSubmitted;
@end
extern "C" id<MTLDevice> offline_metal_frontend_open_service(const char *);
extern "C" void offline_metal_frontend_finish(void);
extern char **environ;
static uint64_t result[32];
static int workerOutput=-1;
static void require(bool ok,int code) {
 if(ok)return;
 if(getpid()==1) {
  result[0]=0x53484641;result[1]=1;result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);result[22]=code;result[23]=errno;
  register const uint64_t *pointer __asm__("x19")=result;__asm__ volatile("brk #0" : : "r"(pointer) : "memory");__builtin_unreachable();
 }
 if(workerOutput>=0){uint64_t failure[8]={0x53484641,(uint64_t)getpid(),(uint64_t)code,(uint64_t)errno,0,0,0,0};(void)write(workerOutput,failure,sizeof(failure));}
 _exit(code);
}
static void transfer(int fd,void *data,size_t size,bool writing) {
 auto p=(unsigned char *)data;
 while(size){ssize_t n=writing?write(fd,p,size):read(fd,p,size);if(n<0 && errno==EINTR)continue;require(n>0,200);p+=n;size-=n;}
}
struct PortMessage {
 mach_msg_header_t header;
 mach_msg_body_t body;
 mach_msg_port_descriptor_t port;
 uint64_t ordinal,pid,surface;
};
static mach_port_t newPort() {
 mach_port_t p=MACH_PORT_NULL;
 require(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&p)==KERN_SUCCESS &&
         mach_port_insert_right(mach_task_self(),p,p,MACH_MSG_TYPE_MAKE_SEND)==KERN_SUCCESS,201);
 return p;
}
static void sendPort(mach_port_t destination,mach_port_t port,uint64_t ordinal,uint64_t surface,bool move=false) {
 PortMessage m={};m.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0)|MACH_MSGH_BITS_COMPLEX;
 m.header.msgh_size=sizeof(m);m.header.msgh_remote_port=destination;m.header.msgh_id=0x534852;
 m.body.msgh_descriptor_count=1;m.port.name=port;m.port.disposition=move?MACH_MSG_TYPE_MOVE_SEND:MACH_MSG_TYPE_COPY_SEND;
 m.port.type=MACH_MSG_PORT_DESCRIPTOR;m.ordinal=ordinal;m.pid=getpid();m.surface=surface;
 require(mach_msg(&m.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(m),0,MACH_PORT_NULL,10000,MACH_PORT_NULL)==MACH_MSG_SUCCESS,202);
}
static PortMessage receivePort(mach_port_t port) {
 struct {PortMessage m;mach_msg_max_trailer_t trailer;} storage={};
 require(mach_msg(&storage.m.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(storage),port,10000,MACH_PORT_NULL)==MACH_MSG_SUCCESS,203);
 PortMessage m=storage.m;
 require(m.header.msgh_size==sizeof(m) && m.header.msgh_id==0x534852 && (m.header.msgh_bits&MACH_MSGH_BITS_COMPLEX) &&
         m.body.msgh_descriptor_count==1 && m.port.type==MACH_MSG_PORT_DESCRIPTOR && m.port.disposition==MACH_MSG_TYPE_PORT_SEND &&
         MACH_PORT_VALID(m.port.name),204);
 return m;
}
static mach_port_t inheritedParentPort() {
 mach_port_array_t ports=nullptr;mach_msg_type_number_t count=0;
 require(mach_ports_lookup(mach_task_self(),&ports,&count)==KERN_SUCCESS && count==3 && MACH_PORT_VALID(ports[2]),205);
 mach_port_t parent=ports[2];
 for(unsigned i=0;i<count;i++)if(i!=2 && MACH_PORT_VALID(ports[i]))mach_port_deallocate(mach_task_self(),ports[i]);
 require(vm_deallocate(mach_task_self(),(vm_address_t)ports,count*sizeof(mach_port_t))==KERN_SUCCESS,206);
 return parent;
}
static uint32_t checkPixels(IOSurfaceRef surface,uint32_t expected) {
 uint32_t seed=0;require(!IOSurfaceLock(surface,kIOSurfaceLockReadOnly,&seed),207);
 auto pixels=(const uint32_t *)IOSurfaceGetBaseAddress(surface);require(pixels!=nullptr,208);
 for(unsigned i=0;i<4096;i++)require(pixels[i]==expected,209);
 require(!IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,nullptr),207);return seed;
}
static int worker(unsigned ordinal,int input,int output) {
 for(int fd=3;fd<64;fd++)if(fd!=input && fd!=output)close(fd);workerOutput=output;
 @autoreleasepool { @try {
  require(getpid()>1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096 && MTLCreateSystemDefaultDevice()==nil,210);
  mach_port_t parent=inheritedParentPort(),surfacePort=MACH_PORT_NULL,reply=MACH_PORT_NULL;
  id<MTLDevice> device=offline_metal_frontend_open_service("NativeOfflineGPU");require(device!=nil,211);
  id<MTLCommandQueue> queue=[device newCommandQueue];require(queue!=nil,212);
  IOSurfaceRef surface=nullptr;uint64_t refused=0;
  if(!ordinal) {
   NSDictionary *properties=@{(__bridge NSString *)kIOSurfaceWidth:@64,(__bridge NSString *)kIOSurfaceHeight:@64,
    (__bridge NSString *)kIOSurfaceBytesPerElement:@4,(__bridge NSString *)kIOSurfaceBytesPerRow:@256,
    (__bridge NSString *)kIOSurfaceAllocSize:@16384,(__bridge NSString *)kIOSurfacePixelFormat:@(0x52474241)};
   surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);require(surface!=nullptr,213);
  } else {
   reply=newPort();sendPort(parent,reply,1,0);
   PortMessage invalid=receivePort(reply);require(invalid.ordinal==2 && invalid.pid==1 && !invalid.surface,214);
   IOSurfaceRef rejected=IOSurfaceLookupFromMachPort(invalid.port.name);require(rejected==nullptr,215);
   require(mach_port_deallocate(mach_task_self(),invalid.port.name)==KERN_SUCCESS,216);refused=1;
   PortMessage authorized=receivePort(reply);require(authorized.ordinal==3 && authorized.pid==1 && authorized.surface,217);
   surfacePort=authorized.port.name;surface=IOSurfaceLookupFromMachPort(surfacePort);
   require(surface && IOSurfaceGetID(surface)==authorized.surface && IOSurfaceGetWidth(surface)==64 && IOSurfaceGetHeight(surface)==64 &&
     IOSurfaceGetBytesPerRow(surface)==256 && IOSurfaceGetAllocSize(surface)==16384 && IOSurfaceGetPixelFormat(surface)==0x52474241,218);
   checkPixels(surface,0xff996633);
  }
  MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
  td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
  id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:surface plane:0];require(texture!=nil && texture.iosurface==surface,219);
  auto render=[&](bool load,MTLClearColor color,uint32_t expected){
   MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
   pass.colorAttachments[0].loadAction=load?MTLLoadActionLoad:MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=color;
   id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,220);
   __block unsigned callbacks=0;
   [command enqueue];require(command.status==MTLCommandBufferStatusEnqueued && command.GPUStartTime==0 && command.GPUEndTime==0,253);
   [command addScheduledHandler:^(id<MTLCommandBuffer> actual){
    require(actual==command && actual.status==MTLCommandBufferStatusCompleted && actual.GPUStartTime>0 && actual.GPUEndTime>actual.GPUStartTime && callbacks==0,254);callbacks=1;
   }];
   [command addCompletedHandler:^(id<MTLCommandBuffer> actual){
    require(actual==command && actual.status==MTLCommandBufferStatusCompleted && actual.GPUStartTime>0 && actual.GPUEndTime>actual.GPUStartTime && callbacks==1,255);callbacks=2;
   }];
   [encoder endEncoding];
   if(ordinal)require([(id)command commitAndWaitUntilSubmitted],257);else [command commit];
   [command waitUntilCompleted];
   [command waitUntilScheduled];require(callbacks==2,256);
   require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,221);
   return checkPixels(surface,expected);
  };
  if(!ordinal) {
   render(false,MTLClearColorMake(.2,.4,.6,1),0xff996633);
   surfacePort=IOSurfaceCreateMachPort(surface);require(MACH_PORT_VALID(surfacePort),222);
   sendPort(parent,surfacePort,0,IOSurfaceGetID(surface));
  } else {
   uint32_t pixels[4096];[texture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
   for(unsigned i=0;i<4096;i++)require(pixels[i]==0xff996633,223);
  }
  uint64_t state[8]={(uint64_t)getpid(),(uint64_t)getpagesize(),(uint64_t)sysconf(_SC_PAGESIZE),
   [[(NSObject *)texture valueForKey:@"handle"] unsignedLongLongValue],IOSurfaceGetID(surface),IOSurfaceGetSeed(surface),refused,1};
  require(state[3] && state[4],224);transfer(output,state,sizeof(state),true);
  uint64_t command=0;transfer(input,&command,sizeof(command),false);require(command==1,225);
  uint32_t before=(uint32_t)state[5];
  if(ordinal)state[5]=render(false,MTLClearColorMake(.8,.4,.2,1),0xff3366cc);
  else {
   // The existing creator texture still has its previous remote pixels.
   // A real Load/Store GPU pass must import the changed original seed.
   require(checkPixels(surface,0xff3366cc)!=before,226);
   state[5]=render(true,MTLClearColorMake(0,0,0,0),0xff3366cc);
  }
  require(state[5]!=before,227);state[7]=2;transfer(output,state,sizeof(state),true);
  transfer(input,&command,sizeof(command),false);require(command==2,228);
  if(ordinal) {
   require(checkPixels(surface,0xff3366cc)==(uint32_t)IOSurfaceGetSeed(surface),229);
   state[5]=render(false,MTLClearColorMake(1,0,1,1),0xffff00ff);state[7]=3;
  }
  texture=nil;queue=nil;CFRelease(surface);require(mach_port_deallocate(mach_task_self(),surfacePort)==KERN_SUCCESS,230);
  if(reply!=MACH_PORT_NULL)require(mach_port_destroy(mach_task_self(),reply)==KERN_SUCCESS,231);
  require(mach_port_deallocate(mach_task_self(),parent)==KERN_SUCCESS,232);
  offline_metal_frontend_finish();device=nil;state[7]=4;transfer(output,state,sizeof(state),true);return 0;
 } @catch(NSException *exception) {
  uint64_t failure[8]={0x53484641,(uint64_t)getpid(),233,(uint64_t)errno,0,0,0,0};
  const char *why=exception.reason.UTF8String;if(why){size_t size=strlen(why);if(size>32)why+=size-32;memcpy(failure+4,why,MIN(size,(size_t)32));}
  transfer(output,failure,sizeof(failure),true);return 233;
 } }
}
int main(int argc,char **argv) {
 if(argc==5 && !strcmp(argv[1],"worker"))return worker((unsigned)strtoul(argv[4],nullptr,10),(int)strtol(argv[2],nullptr,10),(int)strtol(argv[3],nullptr,10));
 require(argc==1 && getpid()==1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096,234);
 mach_port_t incoming=newPort();mach_port_array_t saved=nullptr;mach_msg_type_number_t count=0;
 require(mach_ports_lookup(mach_task_self(),&saved,&count)==KERN_SUCCESS && count==3 && saved[2]==MACH_PORT_NULL,235);
 mach_port_t registered[3]={saved[0],saved[1],incoming};require(mach_ports_register(mach_task_self(),registered,3)==KERN_SUCCESS,236);
 int commands[2][2],replies[2][2];pid_t children[2]={};
 for(unsigned i=0;i<2;i++)require(!pipe(commands[i]) && !pipe(replies[i]),237);
 for(unsigned i=0;i<2;i++) {
  char in[16],out[16],ordinal[16];snprintf(in,sizeof(in),"%d",commands[i][0]);snprintf(out,sizeof(out),"%d",replies[i][1]);snprintf(ordinal,sizeof(ordinal),"%u",i);
  char *arguments[]={(char *)"air-native",(char *)"worker",in,out,ordinal,nullptr};
  int status=posix_spawn(&children[i],"/private/var/tmp/air-native",nullptr,nullptr,arguments,environ);result[24]=status;result[25]=i;require(!status,238);
 }
 require(mach_ports_register(mach_task_self(),saved,count)==KERN_SUCCESS,239);
 for(unsigned i=0;i<count;i++)if(MACH_PORT_VALID(saved[i]))mach_port_deallocate(mach_task_self(),saved[i]);
 require(vm_deallocate(mach_task_self(),(vm_address_t)saved,count*sizeof(mach_port_t))==KERN_SUCCESS,240);
 for(unsigned i=0;i<2;i++){close(commands[i][0]);close(replies[i][1]);}
 PortMessage source={},peer={};
 for(unsigned i=0;i<2;i++) {
  PortMessage m=receivePort(incoming);
  require(m.ordinal<2 && m.pid==(uint64_t)children[m.ordinal],241);
  if(m.ordinal==0){require(source.port.name==MACH_PORT_NULL && m.surface,242);source=m;}
  else {require(peer.port.name==MACH_PORT_NULL && !m.surface,243);peer=m;}
 }
 require(MACH_PORT_VALID(source.port.name) && MACH_PORT_VALID(peer.port.name),244);
 // An unrelated, genuine Mach send right is not an IOSurface authorization.
 sendPort(peer.port.name,peer.port.name,2,0);
 sendPort(peer.port.name,source.port.name,3,source.surface,true);
 require(mach_port_deallocate(mach_task_self(),peer.port.name)==KERN_SUCCESS && mach_port_destroy(mach_task_self(),incoming)==KERN_SUCCESS,245);
 uint64_t first[8]={},second[8]={};transfer(replies[0][0],first,sizeof(first),false);memcpy(result+4,first,sizeof(first));
 transfer(replies[1][0],second,sizeof(second),false);memcpy(result+12,second,sizeof(second));
 require(first[0]==(uint64_t)children[0] && second[0]==(uint64_t)children[1] && first[0]!=second[0] && first[0]>1 && second[0]>1 &&
   first[1]==4096 && first[2]==4096 && second[1]==4096 && second[2]==4096 && first[3] && second[3] && first[3]!=second[3] &&
   first[4]==source.surface && second[4]==source.surface && first[5]==second[5] && !first[6] && second[6]==1 && first[7]==1 && second[7]==1,246);
 result[26]=first[5];uint64_t command=1;
 transfer(commands[1][1],&command,sizeof(command),true);transfer(replies[1][0],second,sizeof(second),false);
 require(second[7]==2 && second[5]!=result[26],247);result[27]=second[5];
 transfer(commands[0][1],&command,sizeof(command),true);transfer(replies[0][0],first,sizeof(first),false);
 require(first[7]==2 && first[5]!=result[26] && first[5]!=result[27],248);result[28]=first[5];command=2;
 transfer(commands[0][1],&command,sizeof(command),true);transfer(replies[0][0],first,sizeof(first),false);require(first[7]==4,249);
 int status1=-1,status2=-1;require(waitpid(children[0],&status1,0)==children[0] && WIFEXITED(status1) && !WEXITSTATUS(status1),250);
 transfer(commands[1][1],&command,sizeof(command),true);transfer(replies[1][0],second,sizeof(second),false);
 require(second[7]==4 && second[5]!=result[28],251);
 require(waitpid(children[1],&status2,0)==children[1] && WIFEXITED(status2) && !WEXITSTATUS(status2),252);
 result[0]=0x53485250;result[1]=1;result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);
 memcpy(result+4,first,sizeof(first));memcpy(result+12,second,sizeof(second));result[20]=status1;result[21]=status2;result[22]=1;
 register const uint64_t *pointer __asm__("x19")=result;__asm__ volatile("brk #0" : : "r"(pointer) : "memory");__builtin_unreachable();
}
