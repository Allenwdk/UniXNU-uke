// SPDX-License-Identifier: MIT
// Own instrument: genuine target shared-handle construction and Mach rights.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <mach/mach.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <poll.h>
#include <dlfcn.h>
#include <xpc/xpc.h>
#ifndef OFFLINE_SHARED_CROSS_PROCESS
#define OFFLINE_SHARED_CROSS_PROCESS 0
#endif
#include <atomic>
#include <pthread.h>
#ifndef OFFLINE_SHARED_CODING
#define OFFLINE_SHARED_CODING 0
#endif
#ifndef OFFLINE_SHARED_NOTIFICATIONS
#define OFFLINE_SHARED_NOTIFICATIONS 0
#endif
extern "C" void offline_metal_frontend_notification_counts(uint64_t *);
extern "C" id<MTLDevice> offline_metal_frontend_open_service(const char *);
extern "C" BOOL offline_metal_frontend_enable_async(void);
extern "C" void offline_metal_frontend_finish(void);
extern "C" int offline_metal_frontend_raw_shared_import_control(uint64_t);
extern "C" int offline_metal_frontend_stale_shared_port_control(mach_port_t);
extern char **environ;
@interface MTLSharedEventHandle (OriginalHandleAccess)
- (instancetype)initWithSharedEvent:(id)event;
- (mach_port_t)eventPort;
- (uint64_t)labelTraceID;
@end
// Explicit received-right adapter for the genuine original constructor. This
// instrument does not qualify NSXPC/NSSecureCoding serialization.
@interface ReceivedEventRight : NSObject
@property(nonatomic) mach_port_t eventPort;
@property(nonatomic,copy) NSString *label;
@property(nonatomic) uint64_t labelTraceID;
@end
@implementation ReceivedEventRight
@end
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
#if OFFLINE_SHARED_NOTIFICATIONS
@interface NotificationObservation : NSObject {
@public std::atomic<uint64_t> delivered,futureCalls,finishRefusals,blockedCalls;
}
@property(nonatomic,strong) id<MTLSharedEvent> event;
@property(nonatomic,strong) MTLSharedEventListener *listener;
@property(nonatomic,strong) dispatch_queue_t queue;
@property(nonatomic,strong) dispatch_semaphore_t done,blocked;
@property(nonatomic) pthread_t caller;
@end
@implementation NotificationObservation
- (instancetype)init {self=[super init];if(self){delivered=0;futureCalls=0;finishRefusals=0;blockedCalls=0;}return self;}
@end
static char notificationQueueKey;
static NotificationObservation *observe(id<MTLSharedEvent> event,uint64_t value) {
 NotificationObservation *observation=[NotificationObservation new];observation.event=event;observation.caller=pthread_self();
 observation.queue=dispatch_queue_create("org.blu-sharky.original-shared-listener",DISPATCH_QUEUE_SERIAL);
 dispatch_queue_set_specific(observation.queue,&notificationQueueKey,(__bridge void *)observation,nullptr);
 observation.listener=[[MTLSharedEventListener alloc] initWithDispatchQueue:observation.queue];observation.done=dispatch_semaphore_create(0);
 require(object_getClass(observation.listener)==NSClassFromString(@"MTLSharedEventListenerInternal") && observation.listener.dispatchQueue==observation.queue,320);
 [event notifyListener:observation.listener atValue:value block:^(id<MTLSharedEvent> actual,uint64_t actualValue){
  require(actual==observation.event && actualValue>=value && actual.signaledValue>=value &&
    dispatch_get_specific(&notificationQueueKey)==(__bridge void *)observation && !pthread_equal(observation.caller,pthread_self()),321);
  @try {offline_metal_frontend_finish();require(false,322);}@catch(NSException *exception){require([exception.reason isEqualToString:@"Finish from an active event callback is unsupported"],323);observation->finishRefusals++;}
  observation->delivered++;dispatch_semaphore_signal(observation.done);
 }];
 [event notifyListener:observation.listener atValue:99 block:^(id<MTLSharedEvent> actual,uint64_t actualValue){(void)actual;(void)actualValue;observation->futureCalls++;require(false,324);}];
 return observation;
}
static void delivered(NotificationObservation *observation) {
 require(dispatch_semaphore_wait(observation.done,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC))==0,325);
 // Drain the real queue so the callback's return and bookkeeping are observed.
 dispatch_sync(observation.queue,^{ });require(observation->delivered==1 && !observation->futureCalls && observation->finishRefusals==1,326);
}
static NotificationObservation *blockedObservation(id<MTLSharedEvent> event) {
 NotificationObservation *observation=[NotificationObservation new];observation.event=event;
 observation.queue=dispatch_queue_create("org.blu-sharky.blocked-original-listener",DISPATCH_QUEUE_SERIAL);
 observation.blocked=dispatch_semaphore_create(0);observation.done=dispatch_semaphore_create(0);
 observation.listener=[[MTLSharedEventListener alloc] initWithDispatchQueue:observation.queue];require(observation.listener.dispatchQueue==observation.queue,327);
 dispatch_async(observation.queue,^{dispatch_semaphore_signal(observation.done);require(dispatch_semaphore_wait(observation.blocked,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC))==0,328);});
 require(dispatch_semaphore_wait(observation.done,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC))==0,329);
 [event notifyListener:observation.listener atValue:3 block:^(id<MTLSharedEvent> actual,uint64_t value){(void)actual;(void)value;observation->blockedCalls++;require(false,330);}];
 return observation;
}
static void finishNotifications(uint64_t state[8],NotificationObservation *observation,NotificationObservation *blocked) {
 uint64_t counts[8]={};if(blocked){for(unsigned i=0;i<10000;i++){offline_metal_frontend_notification_counts(counts);if(counts[5])break;usleep(1000);}require(counts[5]==1 && counts[6]==0,331);}
 offline_metal_frontend_finish();offline_metal_frontend_notification_counts(counts);
 require(counts[0]==(blocked?3:2) && counts[1]==1 && counts[2]==(blocked?2:1) && !counts[3] && !counts[4] && observation->delivered==1 && !observation->futureCalls && observation->finishRefusals==1,332);
 if(blocked){dispatch_semaphore_signal(blocked.blocked);dispatch_sync(blocked.queue,^{ });require(!blocked->blockedCalls,333);}
 state[6]|=(counts[0]<<8)|(counts[1]<<16)|(counts[2]<<24)|(observation->finishRefusals.load()<<32);
}
#endif
#if OFFLINE_SHARED_CODING
@protocol OriginalSharedEventCoding
- (void)receiveHandle:(MTLSharedEventHandle *)handle reply:(void (^)(uint64_t,NSString *))reply;
@end
@interface OriginalSharedEventCodingReceiver : NSObject<NSXPCListenerDelegate,OriginalSharedEventCoding>
@property(nonatomic,strong) id<MTLDevice> device;
@property(nonatomic,strong) id<MTLSharedEvent> imported;
@property(nonatomic,strong) NSMutableArray<NSXPCConnection *> *connections;
@property(nonatomic,strong) NSLock *lock;
@property(nonatomic) pthread_t caller;
@property(nonatomic) pid_t expectedPID;
@end
static NSXPCInterface *codingInterface() {
 NSXPCInterface *interface=[NSXPCInterface interfaceWithProtocol:@protocol(OriginalSharedEventCoding)];
 [interface setClasses:[NSSet setWithObject:[MTLSharedEventHandle class]] forSelector:@selector(receiveHandle:reply:) argumentIndex:0 ofReply:NO];return interface;
}
@implementation OriginalSharedEventCodingReceiver
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection {
 (void)listener;require(connection.processIdentifier==self.expectedPID,340);connection.exportedInterface=codingInterface();connection.exportedObject=self;
 [self.lock lock];[self.connections addObject:connection];[self.lock unlock];[connection resume];return YES;
}
- (void)receiveHandle:(MTLSharedEventHandle *)handle reply:(void (^)(uint64_t,NSString *))reply {
 require(!pthread_equal(self.caller,pthread_self()) && object_getClass(handle)==[MTLSharedEventHandle class] && [handle.label isEqualToString:@"shared GPU timeline"] && !handle.labelTraceID,341);
 mach_port_type_t type=0;require(mach_port_type(mach_task_self(),handle.eventPort,&type)==KERN_SUCCESS && (type&MACH_PORT_TYPE_SEND),342);
 self.imported=[self.device newSharedEventWithHandle:handle];require(self.imported && self.imported.signaledValue==1 && [self.imported waitUntilSignaledValue:1 timeoutMS:0],343);
 reply(self.imported.signaledValue,self.imported.label);
}
@end
static void originalCoding(id<MTLDevice> device,MTLSharedEventHandle *handle,uint64_t state[8]) {
 // Public NSXPC APIs create the actual original NSXPCCoder. No stand-in coder,
 // original method replacement or integer serialization of a right is used.
 OriginalSharedEventCodingReceiver *receiver=[OriginalSharedEventCodingReceiver new];receiver.device=device;receiver.caller=pthread_self();receiver.expectedPID=getpid();receiver.connections=[NSMutableArray array];receiver.lock=[NSLock new];
 NSXPCListener *listener=[NSXPCListener anonymousListener];require(listener!=nil,344);listener.delegate=receiver;[listener resume];
 NSXPCConnection *connection=[[NSXPCConnection alloc] initWithListenerEndpoint:listener.endpoint];require(connection!=nil,345);connection.remoteObjectInterface=codingInterface();[connection resume];
 dispatch_semaphore_t done=dispatch_semaphore_create(0);__block uint64_t observed=0;__block NSString *label=nil;__block NSError *failure=nil;
 id<OriginalSharedEventCoding> proxy=[connection remoteObjectProxyWithErrorHandler:^(NSError *error){failure=error;dispatch_semaphore_signal(done);}];
 [proxy receiveHandle:handle reply:^(uint64_t value,NSString *actual){observed=value;label=actual;dispatch_semaphore_signal(done);}];
 require(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC))==0,346);
 require(!failure && observed==1 && [label isEqualToString:@"shared GPU timeline"] && receiver.imported!=nil,347);
 [connection invalidate];[listener invalidate];[receiver.lock lock];for(NSXPCConnection *peer in receiver.connections)[peer invalidate];[receiver.connections removeAllObjects];[receiver.lock unlock];
 receiver.imported=nil;receiver.device=nil;state[6]|=1ull<<40;
 BOOL refused=NO;@try {NSError *error=nil;(void)[NSKeyedArchiver archivedDataWithRootObject:handle requiringSecureCoding:YES error:&error];refused=error!=nil;}@catch(NSException *exception){require([exception.name isEqualToString:NSInvalidArgumentException],348);refused=YES;}
 require(refused,349);state[6]|=1ull<<41;
}
#endif
#if OFFLINE_SHARED_CROSS_PROCESS
@interface NSXPCListenerEndpoint (OriginalEndpointAccess)
- (xpc_object_t)_endpoint;
- (void)_setEndpoint:(xpc_object_t)endpoint;
@end
static OriginalSharedEventCodingReceiver *crossReceiver;
static NSXPCListener *crossListener;
static mach_port_t crossPipe;
static void retirePrivateRight(mach_port_t port) {
 mach_port_urefs_t refs=0;kern_return_t status=mach_port_get_refs(mach_task_self(),port,MACH_PORT_RIGHT_SEND,&refs);
 require(status==KERN_SUCCESS || status==KERN_INVALID_NAME,350);
 if(status==KERN_SUCCESS && refs)require(mach_port_mod_refs(mach_task_self(),port,MACH_PORT_RIGHT_SEND,-(mach_port_delta_t)refs)==KERN_SUCCESS,351);
}
static void sendCrossEndpoint(mach_port_t reply,id<MTLDevice> device) {
 PortMessage bridge=receivePort(reply);require(bridge.ordinal==4 && bridge.pid==1 && bridge.surface>1 && bridge.surface!=(uint64_t)getpid(),352);
 crossReceiver=[OriginalSharedEventCodingReceiver new];crossReceiver.device=device;crossReceiver.caller=pthread_self();crossReceiver.expectedPID=(pid_t)bridge.surface;crossReceiver.connections=[NSMutableArray array];crossReceiver.lock=[NSLock new];
 crossListener=[NSXPCListener anonymousListener];require(crossListener!=nil,353);crossListener.delegate=crossReceiver;[crossListener resume];
 NSXPCListenerEndpoint *endpoint=crossListener.endpoint;require(object_getClass(endpoint)==[NSXPCListenerEndpoint class] && class_getInstanceSize(object_getClass(endpoint))==24,354);
 xpc_object_t actual=[endpoint _endpoint];require(actual && xpc_get_type(actual)==XPC_TYPE_ENDPOINT,355);
 auto create=(void *(*)(mach_port_t,uint64_t))dlsym(RTLD_DEFAULT,"xpc_pipe_create_from_port");auto send=(int (*)(xpc_object_t,xpc_object_t))dlsym(RTLD_DEFAULT,"xpc_pipe_simpleroutine");require(create && send,356);
 xpc_object_t pipe=(__bridge_transfer xpc_object_t)create(bridge.port.name,0);require(pipe!=nil,357);
 xpc_object_t message=xpc_dictionary_create(nullptr,nullptr,0);xpc_dictionary_set_value(message,"endpoint",actual);xpc_dictionary_set_uint64(message,"peer_pid",getpid());
 require(send(pipe,message)==0,358);pipe=nil;retirePrivateRight(bridge.port.name);
}
static void crossCoding(MTLSharedEventHandle *handle,uint64_t state[8]) {
 // This target has a genuine third flags argument. A historical two-argument
 // receive declaration would leave an uninitialized hardware register here.
 auto receive=(int (*)(mach_port_t,void **,uint64_t))dlsym(RTLD_DEFAULT,"xpc_pipe_receive");require(receive && crossPipe,359);
 void *raw=nullptr;require(receive(crossPipe,&raw,0)==0 && raw,360);xpc_object_t message=(__bridge_transfer xpc_object_t)raw;
 require(xpc_get_type(message)==XPC_TYPE_DICTIONARY,361);xpc_object_t endpointObject=xpc_dictionary_get_value(message,"endpoint");
 uint64_t peerPID=xpc_dictionary_get_uint64(message,"peer_pid");require(endpointObject && xpc_get_type(endpointObject)==XPC_TYPE_ENDPOINT && peerPID>1 && peerPID!=(uint64_t)getpid(),362);
 NSXPCListenerEndpoint *endpoint=[NSXPCListenerEndpoint new];require(endpoint!=nil,363);[endpoint _setEndpoint:endpointObject];require([endpoint _endpoint]==endpointObject,364);
 NSXPCConnection *connection=[[NSXPCConnection alloc] initWithListenerEndpoint:endpoint];require(connection!=nil,365);connection.remoteObjectInterface=codingInterface();[connection resume];
 dispatch_semaphore_t done=dispatch_semaphore_create(0);__block uint64_t observed=0;__block NSString *label=nil;__block NSError *failure=nil;
 id<OriginalSharedEventCoding> proxy=[connection remoteObjectProxyWithErrorHandler:^(NSError *error){failure=error;dispatch_semaphore_signal(done);}];
 [proxy receiveHandle:handle reply:^(uint64_t value,NSString *actual){observed=value;label=actual;dispatch_semaphore_signal(done);}];
 require(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC))==0,366);
 require(!failure && observed==1 && [label isEqualToString:@"shared GPU timeline"] && connection.processIdentifier==(pid_t)peerPID,367);
 [connection invalidate];require(mach_port_destroy(mach_task_self(),crossPipe)==KERN_SUCCESS,368);crossPipe=0;state[6]|=1ull<<42;
}
static id<MTLSharedEvent> stopCrossListener(uint64_t state[8]) {
 require(crossReceiver && crossReceiver.imported && crossReceiver.imported.signaledValue==1,369);
 [crossListener invalidate];[crossReceiver.lock lock];for(NSXPCConnection *connection in crossReceiver.connections)[connection invalidate];[crossReceiver.connections removeAllObjects];[crossReceiver.lock unlock];
 id<MTLSharedEvent> imported=crossReceiver.imported;crossReceiver.imported=nil;crossReceiver.device=nil;crossReceiver=nil;crossListener=nil;state[6]|=1ull<<42;return imported;
}
#endif
static MTLSharedEventHandle *receivedHandle(mach_port_t port) {
 ReceivedEventRight *source=[ReceivedEventRight new];source.eventPort=port;source.label=@"shared GPU timeline";source.labelTraceID=0;
 return [[MTLSharedEventHandle alloc] initWithSharedEvent:source];
}
static uint32_t render(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLSharedEvent> event,uint64_t wait,uint64_t signal,MTLClearColor color,uint32_t pixel) {
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
 td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;
 id<MTLTexture> texture=[device newTextureWithDescriptor:td];require(texture!=nil,270);
 id<MTLCommandBuffer> command=[queue commandBuffer];require(command!=nil,271);
 if(wait)[command encodeWaitForEvent:event value:wait];
 MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
 pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=color;
 id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];require(encoder!=nil,272);[encoder endEncoding];
 [command encodeSignalEvent:event value:signal];[command commit];[command waitUntilCompleted];
 require(command.status==MTLCommandBufferStatusCompleted && !command.error && command.GPUStartTime>0 && command.GPUEndTime>command.GPUStartTime,273);
 uint32_t pixels[4096];[texture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
 for(unsigned i=0;i<4096;i++)require(pixels[i]==pixel,274);
 require(event.signaledValue>=signal && [event waitUntilSignaledValue:signal timeoutMS:0],275);return 4096;
}
static int worker(unsigned ordinal,int input,int output) {
 for(int fd=3;fd<64;fd++)if(fd!=input && fd!=output)close(fd);workerOutput=output;
 @autoreleasepool { @try {
  require(getpid()>1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096 && MTLCreateSystemDefaultDevice()==nil,276);
  mach_port_t parent=inheritedParentPort();id<MTLDevice> device=offline_metal_frontend_open_service("NativeOfflineGPU");require(device && offline_metal_frontend_enable_async(),277);
  id<MTLCommandQueue> queue=[device newCommandQueue];require(queue!=nil,278);
  uint64_t state[8]={(uint64_t)getpid(),4096,4096,0,0,0,0,0};
  require(offline_metal_frontend_raw_shared_import_control(1)==(int)0xe00002c2,279);state[6]=1;
  if(!ordinal) {
#if OFFLINE_SHARED_CROSS_PROCESS
   crossPipe=newPort();sendPort(parent,crossPipe,4,0);
#endif
   id<MTLSharedEvent> event=[device newSharedEvent];require(event && [event conformsToProtocol:@protocol(MTLSharedEvent)] && (!event.device || event.device==device) && !event.signaledValue,280);
   NSMutableString *label=[NSMutableString stringWithString:@"shared GPU timeline"];event.label=label;[label appendString:@" changed"];
   require([event.label isEqualToString:@"shared GPU timeline"],281);
   #if OFFLINE_SHARED_NOTIFICATIONS
   NotificationObservation *notification=observe(event,1);
#endif
   state[3]=render(device,queue,event,0,1,MTLClearColorMake(1,0,0,1),0xff0000ff);
#if OFFLINE_SHARED_NOTIFICATIONS
   delivered(notification);
#endif
   @autoreleasepool {
    MTLSharedEventHandle *handle=[event newSharedEventHandle];require(object_getClass(handle)==[MTLSharedEventHandle class] && class_getInstanceSize(object_getClass(handle))==16 && [MTLSharedEventHandle supportsSecureCoding] && [handle.label isEqualToString:event.label] && !handle.labelTraceID,282);
    mach_port_urefs_t refs=0;require(mach_port_get_refs(mach_task_self(),handle.eventPort,MACH_PORT_RIGHT_SEND,&refs)==KERN_SUCCESS && refs==2,283);
    @autoreleasepool {MTLSharedEventHandle *second=[event newSharedEventHandle];require(second.eventPort==handle.eventPort && mach_port_get_refs(mach_task_self(),handle.eventPort,MACH_PORT_RIGHT_SEND,&refs)==KERN_SUCCESS && refs==3,284);second=nil;}
    require(mach_port_get_refs(mach_task_self(),handle.eventPort,MACH_PORT_RIGHT_SEND,&refs)==KERN_SUCCESS && refs==2,285);state[4]=refs;
    #if OFFLINE_SHARED_CODING
    originalCoding(device,handle,state);
#endif
#if OFFLINE_SHARED_CROSS_PROCESS
    crossCoding(handle,state);
#endif
    sendPort(parent,handle.eventPort,0,1);state[5]=(uint64_t)class_getMethodImplementation([MTLSharedEventHandle class],@selector(initWithSharedEvent:));handle=nil;
   }
   event=nil;queue=nil;
#if OFFLINE_SHARED_NOTIFICATIONS
   // Release the observation's event before exporting the creator lifetime.
   notification.event=nil;finishNotifications(state,notification,nil);notification=nil;
#else
   offline_metal_frontend_finish();
#endif
   device=nil;state[7]=4;transfer(output,state,sizeof(state),true);
  } else {
   mach_port_t reply=newPort();sendPort(parent,reply,1,0);
#if OFFLINE_SHARED_CROSS_PROCESS
   sendCrossEndpoint(reply,device);
#endif
   PortMessage bad=receivePort(reply);require(bad.ordinal==2 && bad.pid==1,286);
   @autoreleasepool {MTLSharedEventHandle *wrong=receivedHandle(bad.port.name);require(wrong && [device newSharedEventWithHandle:wrong]==nil,287);wrong=nil;}
   require(mach_port_deallocate(mach_task_self(),bad.port.name)==KERN_SUCCESS,288);state[6]++;
   PortMessage authorized=receivePort(reply);require(authorized.ordinal==3 && authorized.pid==1 && authorized.surface==1,289);
   id<MTLSharedEvent> event=nil;mach_port_t stale=0;
#if OFFLINE_SHARED_CROSS_PROCESS
   id<MTLSharedEvent> coded=stopCrossListener(state);
#endif
#if OFFLINE_SHARED_NOTIFICATIONS
   NotificationObservation *notification=nil,*blocked=nil;
#endif
   @autoreleasepool {
    MTLSharedEventHandle *handle=receivedHandle(authorized.port.name);require(handle && [handle.label isEqualToString:@"shared GPU timeline"] && !handle.labelTraceID,290);
    require(mach_port_deallocate(mach_task_self(),authorized.port.name)==KERN_SUCCESS,291);
    event=[device newSharedEventWithHandle:handle];require(event && (!event.device || event.device==device) && event.signaledValue==1 && [event.label isEqualToString:handle.label],292);
    require([event waitUntilSignaledValue:1 timeoutMS:0] && ![event waitUntilSignaledValue:99 timeoutMS:0],293);
    #if OFFLINE_SHARED_NOTIFICATIONS
    notification=observe(event,2);
#endif
    #if OFFLINE_SHARED_CROSS_PROCESS
    state[3]=render(device,queue,coded,1,2,MTLClearColorMake(0,0,1,1),0xffff0000);require(event.signaledValue==2,370);coded=nil;
#else
    state[3]=render(device,queue,event,1,2,MTLClearColorMake(0,0,1,1),0xffff0000);
#endif
#if OFFLINE_SHARED_NOTIFICATIONS
    delivered(notification);
#endif
    stale=handle.eventPort;handle=nil;
   }
   mach_port_type_t type=0;require(mach_port_type(mach_task_self(),stale,&type)==KERN_INVALID_NAME,294);state[4]=1;
   require(offline_metal_frontend_stale_shared_port_control(stale)==(int)0xe00002c2,295);state[6]++;
   event.label=@"peer GPU timeline";require([event.label isEqualToString:@"peer GPU timeline"],296);
   #if OFFLINE_SHARED_NOTIFICATIONS
   blocked=blockedObservation(event);
#endif
   state[3]+=render(device,queue,event,2,3,MTLClearColorMake(0,1,0,1),0xff00ff00);state[5]=event.signaledValue;
   event=nil;queue=nil;require(mach_port_destroy(mach_task_self(),reply)==KERN_SUCCESS,297);
   #if OFFLINE_SHARED_NOTIFICATIONS
   notification.event=nil;blocked.event=nil;finishNotifications(state,notification,blocked);notification=nil;blocked=nil;
#else
   offline_metal_frontend_finish();
#endif
   device=nil;state[7]=4;transfer(output,state,sizeof(state),true);
  }
  require(mach_port_deallocate(mach_task_self(),parent)==KERN_SUCCESS,298);return 0;
 } @catch(NSException *exception) {
  uint64_t failure[8]={0x53484641,(uint64_t)getpid(),299,(uint64_t)errno,0,0,0,0};const char *why=exception.reason.UTF8String;
  if(why){size_t size=strlen(why);if(size>32)why+=size-32;memcpy(failure+4,why,MIN(size,(size_t)32));}transfer(output,failure,sizeof(failure),true);return 299;
 } }
}
int main(int argc,char **argv) {
 if(argc==5 && !strcmp(argv[1],"worker"))return worker((unsigned)strtoul(argv[4],nullptr,10),(int)strtol(argv[2],nullptr,10),(int)strtol(argv[3],nullptr,10));
 require(argc==1 && getpid()==1 && getpagesize()==4096 && sysconf(_SC_PAGESIZE)==4096,300);
 mach_port_t incoming=newPort();mach_port_array_t saved=nullptr;mach_msg_type_number_t count=0;
 require(mach_ports_lookup(mach_task_self(),&saved,&count)==KERN_SUCCESS && count==3 && saved[2]==MACH_PORT_NULL,301);
 mach_port_t registered[3]={saved[0],saved[1],incoming};require(mach_ports_register(mach_task_self(),registered,3)==KERN_SUCCESS,302);
 int commands[2][2],replies[2][2];pid_t children[2]={};for(unsigned i=0;i<2;i++)require(!pipe(commands[i]) && !pipe(replies[i]),303);
 for(unsigned i=0;i<2;i++) {
  char in[16],out[16],ordinal[16];snprintf(in,sizeof(in),"%d",commands[i][0]);snprintf(out,sizeof(out),"%d",replies[i][1]);snprintf(ordinal,sizeof(ordinal),"%u",i);
  char *arguments[]={(char *)"air-native",(char *)"worker",in,out,ordinal,nullptr};int status=posix_spawn(&children[i],"/private/var/tmp/air-native",nullptr,nullptr,arguments,environ);
  result[24]=status;result[25]=i;require(!status,304);
 }
 require(mach_ports_register(mach_task_self(),saved,count)==KERN_SUCCESS,305);
 for(unsigned i=0;i<count;i++)if(MACH_PORT_VALID(saved[i]))mach_port_deallocate(mach_task_self(),saved[i]);
 require(vm_deallocate(mach_task_self(),(vm_address_t)saved,count*sizeof(mach_port_t))==KERN_SUCCESS,306);
 for(unsigned i=0;i<2;i++){close(commands[i][0]);close(replies[i][1]);}
 PortMessage source={},peer={};
#if OFFLINE_SHARED_CROSS_PROCESS
 PortMessage bridge={};for(unsigned i=0;i<2;i++){PortMessage m=receivePort(incoming);if(m.ordinal==4){require(m.pid==(uint64_t)children[0] && !bridge.port.name,371);bridge=m;}else {require(m.ordinal==1 && m.pid==(uint64_t)children[1] && !peer.port.name,372);peer=m;}}
 require(MACH_PORT_VALID(bridge.port.name) && MACH_PORT_VALID(peer.port.name),373);sendPort(peer.port.name,bridge.port.name,4,children[0],true);
#endif
 uint64_t first[8]={},second[8]={};
#if OFFLINE_SHARED_CROSS_PROCESS
 struct pollfd descriptors[2]={{replies[0][0],POLLIN,0},{replies[1][0],POLLIN,0}};require(poll(descriptors,2,10000)>0,374);
 if(descriptors[1].revents){transfer(replies[1][0],second,sizeof(second),false);memcpy(result+12,second,sizeof(second));require(false,375);}
 require(descriptors[0].revents&POLLIN,376);
#endif
 transfer(replies[0][0],first,sizeof(first),false);memcpy(result+4,first,sizeof(first));require(first[7]==4,313);
 for(unsigned i=0;i<(OFFLINE_SHARED_CROSS_PROCESS?1u:2u);i++) {PortMessage m=receivePort(incoming);require(m.ordinal<2 && m.pid==(uint64_t)children[m.ordinal],307);if(!m.ordinal)source=m;else peer=m;}
 require(MACH_PORT_VALID(source.port.name) && MACH_PORT_VALID(peer.port.name) && source.surface==1,308);
 int status1=-1,status2=-1;require(waitpid(children[0],&status1,0)==children[0] && WIFEXITED(status1) && !WEXITSTATUS(status1) && first[7]==4,309);
 // Transfer only after the creator process really exited. No other task has a
 // reference to its event proxy or userclient; this genuine right is authority.
 sendPort(peer.port.name,peer.port.name,2,0);sendPort(peer.port.name,source.port.name,3,1,true);
 require(mach_port_deallocate(mach_task_self(),peer.port.name)==KERN_SUCCESS && mach_port_destroy(mach_task_self(),incoming)==KERN_SUCCESS,310);
 transfer(replies[1][0],second,sizeof(second),false);memcpy(result+12,second,sizeof(second));
 require(waitpid(children[1],&status2,0)==children[1] && WIFEXITED(status2) && !WEXITSTATUS(status2) && second[7]==4,311);
 require(first[0]==(uint64_t)children[0] && second[0]==(uint64_t)children[1] && first[3]==4096 && second[3]==8192 && first[4]==2 && second[4]==1 && second[5]==3 && (first[6]&255)==1 && (second[6]&255)==3,312);
 result[0]=0x53484556;result[1]=1;result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);result[20]=status1;result[21]=status2;result[22]=1;
 register const uint64_t *pointer __asm__("x19")=result;__asm__ volatile("brk #0" : : "r"(pointer) : "memory");__builtin_unreachable();
}
