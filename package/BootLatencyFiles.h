/* SPDX-License-Identifier: MIT */
#ifndef BOOT_LATENCY_FILES_H
#define BOOT_LATENCY_FILES_H
#include <string.h>
static inline int bl_latency_contains(const char *text,size_t size,const char *term) {
 size_t length=strlen(term);
 for(size_t at=0;at+length<=size;++at)if(!memcmp(text+at,term,length))return 1;
 return 0;
}
static const char *const bl_crash_processes[]={"STExtractionService","dasd","WidgetRenderer","FitnessWidget","CalendarWidget",
 "MobileSafari","com.apple.WebKit.WebContent","com.apple.WebKit.Networking","com.apple.WebKit.GPU","backboardd","SpringBoard"};
static inline int bl_crash_process(const char *path) {
 const char *name=path;for(const char *at=path;*at;++at)if(*at=='/')name=at+1;
 for(unsigned i=0;i<sizeof bl_crash_processes/sizeof bl_crash_processes[0];++i) {
  size_t n=strlen(bl_crash_processes[i]);
  if(!strncmp(name,bl_crash_processes[i],n) && (name[n]=='-' || name[n]=='_' || !strncmp(name+n,"Extension-",10)))return (int)i;
 }
 return -1;
}
static inline int bl_frame_trace(const char *path) {
 static const char *const dirs[]={"/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/",
  "/private/var/root/Library/Caches/com.apple.backboardd/m2v/","/private/var/tmp/NativeAdrenoMetal.trace/"};
 for(unsigned i=0;i<sizeof dirs/sizeof dirs[0];++i) {
  size_t n=strlen(dirs[i]);
  if(!strncmp(path,dirs[i],n) && (!strcmp(path+n,"frames.trace") || !strcmp(path+n,"frames.previous.trace")))return 1;
 }
 return 0;
}
static inline int bl_latency_file(const char *path) {
 return bl_frame_trace(path) ||
        bl_latency_contains(path,strlen(path),"/Library/Caches/com.apple.backboardd/m2v/pipelines-") ||
        (bl_latency_contains(path,strlen(path),"/Library/Logs/CrashReporter/") && bl_crash_process(path)>=0);
}
static inline int bl_latency_message(const char *line,size_t size) {
 static const char *const terms[]={"panic","EXC_","memorystatus:","watermark","STExtractionService",
  "dasd","WidgetRenderer","FitnessWidget","CalendarWidget","GPU fault","GPU hang","PAC exception"};
 for(unsigned i=0;i<sizeof terms/sizeof terms[0];++i) {
  if(bl_latency_contains(line,size,terms[i]))return 1;
 }
 return 0;
}
static inline unsigned bl_latency_messages(char *text,unsigned bytes) {
 unsigned out=0;
 for(unsigned at=0;at<bytes;) {
  unsigned end=at;while(end<bytes && text[end]!='\n')++end;
  unsigned size=end-at+(end<bytes);
  if(bl_latency_message(text+at,size)){memmove(text+out,text+at,size);out+=size;}
  at+=size;
 }
 return out;
}
#endif
