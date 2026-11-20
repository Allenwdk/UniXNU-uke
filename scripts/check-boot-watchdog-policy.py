#!/usr/bin/env python3
"""Execute the production scratch-Data policy initializer with syscall stand-ins."""
import hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];out=root/'build/cpu-desktop/watchdog-policy';out.mkdir(parents=True,exist_ok=True)
policy_source = root/'package/BootWatchdogPolicy.c'
if not policy_source.is_file():
    policy_source = root/'kits/uke/BootWatchdogPolicy.c'
s = policy_source.read_text()
# Substitute the unrelated Gunyah watchdog backend on this host.
body = (s[s.index('static const char policy'):s.index('// The boot-log archive')] +
        '\nstatic void uke_gunyah_wdt_start(unsigned seconds) {(void)seconds;}\n' +
        s[s.index('int sheng_boot_watchdog_policy('):])
prefix=r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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
#define unlink k_unlink
#define printf(...) ((void)0)
#define stat64 stat
typedef void *proc_t;
typedef uintptr_t user_addr_t;
typedef ssize_t user_ssize_t;
struct unlink_args {user_addr_t path;};
struct mkdir_args {user_addr_t path;int mode;};
struct open_args {user_addr_t path;int flags,mode;};
struct write_args {int fd;user_addr_t cbuf;size_t nbyte;};
struct fchown_args {int fd,uid,gid;};struct fchmod_args {int fd,mode;};
struct fsync_args {int fd;};struct fstat64_args {int fd;user_addr_t ub;};struct close_args {int fd;};
static char extra_jobs[200];
static int enabled,lite,bluetooth,calls,opens,closes,md_error,open_error,short_write,chown_error,bad_owner,close_error,marker_exists;
static unsigned char written_data[2048];static size_t written_size;
static int PE_parse_boot_argn(const char *key,void *out,int n){
    if(!strcmp(key,"sheng-disable-jobs")){
        assert(n==200);memcpy(out,extra_jobs,200);return extra_jobs[0]!=0;
    }
    assert(n==4);
    const int *value=NULL;
    if(!strcmp(key,"sheng-disable-watchdogd"))value=&enabled;
    else if(!strcmp(key,"sheng-lite"))value=&lite;
    else if(!strcmp(key,"bluetooth"))value=&bluetooth;
    else assert(!strcmp(key,"sheng-setup") || !strcmp(key,"sheng-data-reset"));
    if(!value)return 0;
    memcpy(out,value,4);return 1;
}
static int k_unlink(proc_t p,struct unlink_args *a,int32_t *r){(void)p;(void)a;(void)r;assert(0);return 0;}
static int copyout(const void *s,uintptr_t d,size_t n){++calls;memcpy((void *)d,s,n);return 0;}
static int copyin(uintptr_t s,void *d,size_t n){++calls;memcpy(d,(void *)s,n);return 0;}
static int k_mkdir(proc_t p,struct mkdir_args *a,int32_t *r){(void)p;(void)r;++calls;assert(!strcmp((char*)a->path,"/private/var/db/com.apple.xpc.launchd")&&a->mode==0755);return md_error;}
static int k_open(proc_t p,struct open_args *a,int32_t *r){
    (void)p;++calls;++opens;assert(a->mode==0644);
    if(strstr((char*)a->path,"disabled.migrated")){
        assert(a->flags==(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW));
        if(marker_exists)return EEXIST;
        *r=8;return 0;
    }
    assert(strstr((char*)a->path,"/disabled.plist"));
    assert(a->flags==(O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW));
    if(open_error)return open_error;
    written_size=0;written_data[0]=0;*r=7;return 0;
}
static int k_write(proc_t p,struct write_args *a,user_ssize_t *r){
    (void)p;++calls;assert(a->fd==7 && written_size+a->nbyte<sizeof(written_data));
    memcpy(written_data+written_size,(void*)a->cbuf,a->nbyte);
    written_size+=a->nbyte;written_data[written_size]=0;
    *r=(user_ssize_t)a->nbyte-short_write;return 0;
}
static int k_fchown(proc_t p,struct fchown_args *a,int32_t *r){(void)p;(void)r;++calls;assert(a->fd==7&&!a->uid&&!a->gid);return chown_error;}
static int k_fchmod(proc_t p,struct fchmod_args *a,int32_t *r){(void)p;(void)r;++calls;assert(a->fd==7&&a->mode==0644);return 0;}
static int k_fsync(proc_t p,struct fsync_args *a,int32_t *r){(void)p;(void)r;++calls;assert(a->fd==7);return 0;}
static int sys_fstat64(proc_t p,struct fstat64_args *a,int32_t *r){(void)p;(void)r;++calls;assert(a->fd==7);struct stat st={.st_uid=bad_owner?502:0,.st_gid=0,.st_mode=0100644,.st_size=(off_t)written_size};memcpy((void*)a->ub,&st,sizeof(st));return 0;}
static int sys_close(proc_t p,struct close_args *a,int32_t *r){(void)p;(void)r;++calls;++closes;assert(a->fd==7||a->fd==8);return close_error;}
'''
main=r'''
static unsigned char scratch[4096];
static void reset(void){enabled=1;lite=bluetooth=0;extra_jobs[0]=0;calls=opens=closes=md_error=open_error=short_write=chown_error=bad_owner=close_error=marker_exists=0;written_size=0;written_data[0]=0;}
static int run(void){return sheng_boot_watchdog_policy(NULL,(uintptr_t)scratch);}
int main(void){
 reset();enabled=0;assert(!run()&&!calls);
 reset();enabled=0;strcpy(extra_jobs,"com.apple.identityservicesd,com.apple.locationd");assert(!run()&&strstr((char*)written_data,"<key>com.apple.identityservicesd</key><true/><key>com.apple.locationd</key><true/>")&&!strstr((char*)written_data,"watchdogd"));
 reset();strcpy(extra_jobs,"com.apple.locationd");assert(!run()&&strstr((char*)written_data,"watchdogd")&&strstr((char*)written_data,"com.apple.locationd"));
 reset();strcpy(extra_jobs,"bad<label");assert(run()==EINVAL&&!opens);
 reset();memset(extra_jobs,'a',199);assert(!run()&&written_size<sizeof(written_data));
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
 const char *bt="<key>com.apple.bluetoothd</key><true/>";
 reset();lite=1;assert(!run()&&strstr((char*)written_data,bt));
 // A later enabled boot rewrites the same persistent policy and clears the stale automatic disable.
 bluetooth=1;assert(!run()&&!strstr((char*)written_data,bt));
 assert(strstr((char*)written_data,"<key>com.apple.CommCenter</key><true/>"));
 assert(strstr((char*)written_data,"<key>com.apple.watchdogd</key><true/>"));
 reset();lite=bluetooth=1;strcpy(extra_jobs,"com.apple.bluetoothd,com.example.test");
 assert(!run()&&strstr((char*)written_data,bt)&&strstr((char*)written_data,"com.example.test"));
 return 0;
}
'''
(out/'test.c').write_text(prefix+body+main)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True,timeout=45)
subprocess.run([str(out/'test')],check=True,timeout=10)
(out/'report.json').write_text(json.dumps(dict(passed=True,source_body_sha256=hashlib.sha256(body.encode()).hexdigest(),syscalls_substituted=True,hardware_policy_accepted=False),indent=2)+'\n')
print('PASS: production policy initializer; opt-in gate, exact content, ownership/mode checks, persistent rewrite, Bluetooth opt-in and explicit disable, write failures, marker and cleanup')
