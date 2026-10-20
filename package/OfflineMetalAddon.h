// SPDX-License-Identifier: MIT
// Explicit offline read-only bindfs view of own code on mutable test Data.
// This is not an immutable addon volume or production root authorization.
#include <sys/mount.h>
#include <sys/stat.h>
static void mountOwnMetalAddon() {
 NSDictionary *metadata=[NSDictionary dictionaryWithContentsOfFile:@"/private/var/tmp/native-metal-addon.plist"];
 NSString *uuid=metadata[@"volume_uuid"];require([uuid isKindOfClass:[NSString class]] && uuid.length==36,270);
 void *iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);require(iokit!=nullptr,271);
 auto matching=(CFMutableDictionaryRef (*)(const char *))dlsym(iokit,"IOServiceMatching");
 auto services=(int (*)(unsigned,CFDictionaryRef,unsigned *))dlsym(iokit,"IOServiceGetMatchingServices");
 auto next=(unsigned (*)(unsigned))dlsym(iokit,"IOIteratorNext");auto release=(int (*)(unsigned))dlsym(iokit,"IOObjectRelease");
 auto property=(CFTypeRef (*)(unsigned,CFStringRef,CFAllocatorRef,unsigned))dlsym(iokit,"IORegistryEntryCreateCFProperty");
 auto parent=(int (*)(unsigned,const char *,unsigned *))dlsym(iokit,"IORegistryEntryGetParentEntry");
 auto conforms=(int (*)(unsigned,const char *))dlsym(iokit,"IOObjectConformsTo");
 require(matching && services && next && release && property && parent && conforms,272);
 CFMutableDictionaryRef match=matching("IOMedia");require(match!=nullptr,273);
 NSDictionary *properties=@{@"UUID":uuid};CFDictionarySetValue(match,CFSTR("IOPropertyMatch"),(__bridge CFDictionaryRef)properties);
 unsigned iterator=0;require(!services(0,match,&iterator) && iterator,274);
 char device[64]={};unsigned matches=0;
 while(unsigned media=next(iterator)) {
  matches++;CFTypeRef identity=property(media,CFSTR("UUID"),kCFAllocatorDefault,0),name=property(media,CFSTR("BSD Name"),kCFAllocatorDefault,0);
  require(identity && CFGetTypeID(identity)==CFStringGetTypeID() && CFEqual(identity,(__bridge CFStringRef)uuid) &&
          name && CFGetTypeID(name)==CFStringGetTypeID(),275);
  char bsd[56]={};require(CFStringGetCString((CFStringRef)name,bsd,sizeof(bsd),kCFStringEncodingASCII) && !strncmp(bsd,"disk",4),276);
  for(const char *p=bsd;*p;p++)require((*p>='a' && *p<='z') || (*p>='0' && *p<='9'),277);
  bool backed=false;unsigned node=media;
  for(unsigned depth=0;node && depth<64;depth++) {
   if(conforms(node,"NativeRamBlock")){backed=true;break;}
   unsigned above=0;int status=parent(node,"IOService",&above);if(node!=media)release(node);node=status?0:above;
  }
  if(node && node!=media)release(node);
  require(backed,278);snprintf(device,sizeof(device),"/dev/%s",bsd);
  CFRelease(identity);CFRelease(name);release(media);
 }
 release(iterator);require(matches==1 && device[0],279);
 struct stat block={},source={},before={},after={},original={};struct statfs filesystem={};
 const char *point="/System/Library/Extensions";
 const char *directory="/private/var/tmp/native-metal-addons";
 require(!stat(device,&block) && S_ISBLK(block.st_mode) && !stat(point,&before) && !stat(directory,&source) && source.st_dev==block.st_rdev,280);
 // Existing source-built XNU bindfs accepts a path string and independently
 // requires its real bindfs-allow entitlement. Its own mount implementation
 // forces a read-only view. The covered original directory remains untouched.
 require(!mount("bindfs",point,MNT_RDONLY|MNT_UNION,(void *)directory),281);
 require(!statfs(point,&filesystem) && (filesystem.f_flags&(MNT_RDONLY|MNT_UNION))==(MNT_RDONLY|MNT_UNION) &&
         !strcmp(filesystem.f_mntfromname,directory) && !strcmp(filesystem.f_fstypename,"bindfs") && !stat(point,&after) && after.st_dev!=before.st_dev,282);
 require(!stat("/System/Library/Extensions/NativeOfflineMetal.bundle/NativeOfflineMetal",&original) && original.st_dev==after.st_dev,283);
 require(!stat("/System/Library/Extensions/AGXMetalA10.bundle/Info.plist",&original) && original.st_dev==before.st_dev,284);
 result[29]=filesystem.f_flags;result[30]=(uint32_t)after.st_dev;result[31]=(uint32_t)original.st_dev;
}
