// SPDX-License-Identifier: MIT
// Actual original Sandbox token issuance/consumption/release and IOKit opens.
// Own issuer and consumer have identical instructions, distinct signed scopes.
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
static uint64_t result[32];static int output=-1;extern char **environ;
static void transfer(int fd,void *bytes,size_t size,bool writing) {
 auto p=(unsigned char *)bytes;
 while(size){ssize_t n=writing?write(fd,p,size):read(fd,p,size);if(n<0 && errno==EINTR)continue;if(n<=0)_exit(90);p+=n;size-=n;}
}
static void stop(unsigned phase) {
 result[30]=phase;result[31]=errno;
 if(output>=0){transfer(output,result,sizeof(result),true);_exit(phase?91:0);}
 register const uint64_t *packet __asm__("x19")=result;
 __asm__ volatile("brk #0" : : "r"(packet) : "memory");__builtin_unreachable();
}
static void require(bool value,unsigned phase){if(!value)stop(phase);}
int main(int argc,char **argv) {
 @autoreleasepool {
  result[0]=0x5342494f;result[1]=getpid();result[2]=getpagesize();result[3]=sysconf(_SC_PAGESIZE);result[4]=getuid();result[5]=geteuid();
  bool worker=argc==4 && !strcmp(argv[1],"consumer");if(worker)output=atoi(argv[3]);
  require(result[2]==4096 && result[3]==4096,1);
  void *sandbox=dlopen("/usr/lib/system/libsystem_sandbox.dylib",RTLD_NOW|RTLD_LOCAL);require(sandbox!=nullptr,2);
  auto issue=(char *(*)(const char *,const char *,uint32_t))dlsym(sandbox,"sandbox_extension_issue_iokit_user_client_class");
  auto consume=(int64_t (*)(const char *))dlsym(sandbox,"sandbox_extension_consume");
  auto release=(int (*)(int64_t))dlsym(sandbox,"sandbox_extension_release");require(issue && consume && release,3);
  if(worker) {
   require(getpid()>1,4);int input=atoi(argv[2]);
   void *iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);require(iokit!=nullptr,5);
   auto matching=(void *(*)(const char *))dlsym(iokit,"IOServiceMatching");
   auto service=(unsigned (*)(unsigned,void *))dlsym(iokit,"IOServiceGetMatchingService");
   auto open=(int (*)(unsigned,unsigned,unsigned,unsigned *))dlsym(iokit,"IOServiceOpen");
   auto close=(int (*)(unsigned))dlsym(iokit,"IOServiceClose");
   auto releaseObject=(int (*)(unsigned))dlsym(iokit,"IOObjectRelease");require(matching && service && open && close && releaseObject,6);
   unsigned root=service(0,matching("IOSurfaceRoot"));require(root,7);result[6]=root;
   auto attempt=[&](unsigned index){unsigned client=0;int status=open(root,mach_task_self(),0,&client);result[index]=(uint32_t)status;result[27]+=(client!=0);if(client){int closed=close(client);require(!status && !closed,8);}return status;};
   require(attempt(7)!=0,9);
   for(unsigned ordinal=0;ordinal<2;ordinal++) {
    uint64_t size=0;transfer(input,&size,sizeof(size),false);require(size>0 && size<8192,10);char token[8192]={};transfer(input,token,size,false);require(token[size-1]==0,11);
    int64_t handle=consume(token);memset(token,0,sizeof(token));result[8+ordinal*5]=(uint64_t)handle;require(handle>=0,12);
    int opened=attempt(9+ordinal*5);if(!ordinal)require(opened!=0,13);
    result[10+ordinal*5]=(uint32_t)release(handle);require(!result[10+ordinal*5],14);
    require(attempt(11+ordinal*5)!=0,15);
   }
   require(!releaseObject(root),16);result[20]=(uintptr_t)consume;result[21]=(uintptr_t)release;stop(0);
  }
  require(argc==1 && getpid()==1,17);
  // A real entitled issuer open is an independent driver/type control.
  void *iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);require(iokit!=nullptr,24);
  auto matching=(void *(*)(const char *))dlsym(iokit,"IOServiceMatching");auto service=(unsigned (*)(unsigned,void *))dlsym(iokit,"IOServiceGetMatchingService");
  auto open=(int (*)(unsigned,unsigned,unsigned,unsigned *))dlsym(iokit,"IOServiceOpen");auto close=(int (*)(unsigned))dlsym(iokit,"IOServiceClose");auto releaseObject=(int (*)(unsigned))dlsym(iokit,"IOObjectRelease");
  require(matching && service && open && close && releaseObject,25);unsigned root=service(0,matching("IOSurfaceRoot")),client=0;require(root,26);
  result[6]=(uint32_t)open(root,mach_task_self(),0,&client);require(!result[6] && client && !close(client) && !releaseObject(root),27);
  // Actual issue routines use the real original Sandbox; no token is fabricated.
  char *tokens[2]={issue("com.apple.app-sandbox.iokit-client","NativeOfflineGPUClient",0),issue("com.apple.app-sandbox.iokit-client","IOSurfaceRootUserClient",0)};
  result[7]=(tokens[0]?1:0)|(tokens[1]?2:0);require(tokens[0] && tokens[1],18);
  int input[2],reply[2];require(!pipe(input) && !pipe(reply),19);char in[16],out[16];snprintf(in,sizeof(in),"%d",input[0]);snprintf(out,sizeof(out),"%d",reply[1]);
  char *arguments[]={(char *)"sandbox-consumer",(char *)"consumer",in,out,nullptr};pid_t child=0;
  int spawned=posix_spawn(&child,"/private/var/tmp/sandbox-consumer",nullptr,nullptr,arguments,environ);result[8]=spawned;require(!spawned,20);close(input[0]);close(reply[1]);
  for(char *token:tokens){uint64_t size=strlen(token)+1;require(size<8192,21);transfer(input[1],&size,sizeof(size),true);transfer(input[1],token,size,true);memset(token,0,size);free(token);}
  close(input[1]);uint64_t actual[32]={};transfer(reply[0],actual,sizeof(actual),false);close(reply[0]);int status=-1;require(waitpid(child,&status,0)==child,22);
  memcpy(result+9,actual+6,16*sizeof(uint64_t));result[25]=(uint32_t)status;result[26]=actual[30];result[27]=actual[27];result[28]=child;result[29]=actual[1];require(!status && !actual[30] && actual[1]==(uint64_t)child && actual[2]==4096 && actual[3]==4096,23);stop(0);
 }
}
