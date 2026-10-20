#!/usr/bin/env python3
"""Execute the production scratch-Data policy initializer with syscall stand-ins."""
import hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];out=root/'build/cpu-desktop/watchdog-policy';out.mkdir(parents=True,exist_ok=True)
s=(root/'kits/uke/BootWatchdogPolicy.c').read_text();body=s[s.index('static const char policy'):]
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#undef strlcat
#define strlcat k_strlcat
static size_t k_strlcat(char *d,const char *s,size_t n){size_t dl=strnlen(d,n),sl=strlen(s);if(dl==n)return n+sl;size_t c=sl<n-dl-1?sl:n-dl-1;memcpy(d+dl,s,c);d[dl+c]=0;return dl+sl;}
#include <fcntl.h>
#define open k_open
#define write k_write
#define mkdir k_mkdir
#define fchown k_fchown
#define fchmod k_fchmod
#define fsync k_fsync
#define printf(...) ((void)0)
#define stat64 stat
typedef void *proc_t;
struct mkdir_args {user_addr_t path;int mode;};
struct open_args {user_addr_t path;int flags,mode;};
struct write_args {int fd;user_addr_t cbuf;size_t nbyte;};
struct fchown_args {int fd,uid,gid;};struct fchmod_args {int fd,mode;};
struct fsync_args {int fd;};struct fstat64_args {int fd;user_addr_t ub;};struct close_args {int fd;};
static char jobs[200];static int enabled,calls,opens,closes,md_error,open_error,short_write,chown_error,bad_owner,close_error,marker_exists;
static unsigned char written_data[1024];static size_t written_size;
static int PE_parse_boot_argn(const char *key,void *out,int n){if(!strcmp(key,"sheng-disable-jobs")){assert(n==200);memcpy(out,jobs,200);return jobs[0]!=0;}assert(!strcmp(key,"sheng-disable-watchdogd")&&n==4);memcpy(out,&enabled,4);return enabled!=0;}
static int copyout(const void *s,uintptr_t d,size_t n){++calls;memcpy((void *)d,s,n);return 0;}
static int copyin(uintptr_t s,void *d,size_t n){++calls;memcpy(d,(void *)s,n);return 0;}
static int k_mkdir(proc_t p,struct mkdir_args *a,int32_t *r){(void)p;(void)r;++calls;assert(!strcmp((char*)a->path,"/private/var/db/com.apple.xpc.launchd")&&a->mode==0755);return md_error;}
static int k_open(proc_t p,struct open_args *a,int32_t *r){(void)p;++calls;++opens;assert(a->flags==(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW)&&a->mode==0644);if(strstr((char*)a->path,"disabled.migrated")){if(marker_exists)return EEXIST;*r=8;return 0;}assert(strstr((char*)a->path,"/disabled.plist"));if(open_error)return open_error;*r=7;return 0;}
static int k_write(proc_t p,struct write_args *a,user_ssize_t *r){(void)p;++calls;assert(a->fd==7&&a->nbyte<sizeof(written_data));written_size=a->nbyte;memcpy(written_data,(void*)a->cbuf,a->nbyte);*r=(user_ssize_t)a->nbyte-short_write;return 0;}
static int k_fchown(proc_t p,struct fchown_args *a,int32_t *r){(void)p;(void)r;++calls;assert(a->fd==7&&!a->uid&&!a->gid);return chown_error;}
static int k_fchmod(proc_t p,struct fchmod_args *a,int32_t *r){(void)p;(void)r;++calls;assert(a->fd==7&&a->mode==0644);return 0;}
static int k_fsync(proc_t p,struct fsync_args *a,int32_t *r){(void)p;(void)r;++calls;assert(a->fd==7);return 0;}
static int sys_fstat64(proc_t p,struct fstat64_args *a,int32_t *r){(void)p;(void)r;++calls;assert(a->fd==7);struct stat st={.st_uid=bad_owner?502:0,.st_gid=0,.st_mode=0100644,.st_size=(off_t)written_size};memcpy((void*)a->ub,&st,sizeof(st));return 0;}
static int sys_close(proc_t p,struct close_args *a,int32_t *r){(void)p;(void)r;++calls;++closes;assert(a->fd==7||a->fd==8);return close_error;}
'''
main=r'''
static unsigned char scratch[4096];
static void reset(void){enabled=1;jobs[0]=0;calls=opens=closes=md_error=open_error=short_write=chown_error=bad_owner=close_error=marker_exists=0;written_size=0;}
static int run(void){return sheng_boot_watchdog_policy(NULL,(uintptr_t)scratch);}
int main(void){
 reset();enabled=0;assert(!run()&&!calls);
 reset();enabled=0;strcpy(jobs,"com.apple.identityservicesd,com.apple.locationd");assert(!run()&&strstr((char*)written_data,"<key>com.apple.identityservicesd</key><true/><key>com.apple.locationd</key><true/>")&&!strstr((char*)written_data,"watchdogd"));
 reset();strcpy(jobs,"com.apple.locationd");assert(!run()&&strstr((char*)written_data,"watchdogd")&&strstr((char*)written_data,"com.apple.locationd"));
 reset();strcpy(jobs,"bad<label");assert(run()==EINVAL&&!opens);
 reset();memset(jobs,'a',199);assert(!run()&&written_size<sizeof(written_data));
 reset();memset(scratch+0x400,0x5a,0x400);assert(!run()&&!sheng_watchdog_policy_status&&opens==2&&closes==2&&written_size==strlen(policy)&&!memcmp(written_data,policy,written_size));
 for(unsigned i=0x400;i<0x800;++i)assert(scratch[i]==0x5a);
 reset();md_error=EEXIST;assert(!run()&&closes==2);
 reset();md_error=EACCES;assert(run()==EACCES&&!opens);
 reset();open_error=EEXIST;assert(run()==EEXIST&&!written_size&&!closes);
 reset();short_write=1;assert(run()==EIO&&closes==1&&opens==1);
 reset();chown_error=EPERM;assert(run()==EPERM&&closes==1&&opens==1);
 reset();bad_owner=1;assert(run()==EINVAL&&closes==1&&opens==1);
 reset();close_error=EIO;assert(run()==EIO&&closes==1&&opens==1);
 reset();marker_exists=1;assert(!run()&&opens==2&&closes==1);
 return 0;
}
'''
(out/'test.c').write_text(prefix+body+main)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True,timeout=45)
subprocess.run([str(out/'test')],check=True,timeout=10)
(out/'report.json').write_text(json.dumps(dict(passed=True,source_body_sha256=hashlib.sha256(body.encode()).hexdigest(),syscalls_substituted=True,hardware_policy_accepted=False),indent=2)+'\n')
print('PASS: production policy initializer; opt-in gate, exact content, ownership/mode checks, no overwrite, partial-write/close failures, marker and cleanup')
