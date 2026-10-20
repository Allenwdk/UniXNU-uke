// SPDX-License-Identifier: MIT
// Cross-task reads use the native XNU VM API, outside task/scheduler locks.
// A user-page fault can block this isolated capture worker, never the monitor.
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/simple_lock.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <arm/thread.h>
#include <vm/vm_map.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_port.h>
#include <string.h>
#include "BootUiTraceCore.h"
#include "BootUiTraceDetails.h"
static struct bt_entry samples[BT_MAX_THREADS];
#define BT_ROLES 4
static struct bt_main draft_main[BT_ROLES],published_main[BT_ROLES];
static struct bt_groups draft,published;
static unsigned trace_lock;
static char draft_names[BT_ROLES][32],published_names[BT_ROLES][32];
static unsigned draft_pid[BT_ROLES],published_pid[BT_ROLES],draft_slide[BT_ROLES],published_slide[BT_ROLES];
static unsigned draft_busy,published_busy,draft_omitted,published_omitted;
char boot_ui_trace_rows[BT_ROWS][120],boot_ui_trace_label[120];
unsigned boot_ui_trace_ready,boot_ui_trace_groups,boot_ui_trace_threads,boot_ui_trace_pages;
volatile unsigned boot_ui_trace_phase,boot_ui_trace_index;

void boot_ui_trace_begin(void) {
 memset(draft_main,0,sizeof(draft_main));
 memset(&draft,0,sizeof(draft));memset(draft_pid,0,sizeof(draft_pid));
 memset(draft_slide,0,sizeof(draft_slide));memset(draft_names,0,sizeof(draft_names));
 draft_busy=0;draft_omitted=0;boot_ui_trace_phase=1;
}
static int read_user(void *context,uint64_t address,void *bytes,unsigned size) {
 return vm_map_read_user((vm_map_t)context,address,bytes,size)!=KERN_SUCCESS;
}
static void lookup_peer(struct bt_entry *e,ipc_space_t space) {
 e->peer=-1;e->peer_code=1;
 if(e->trap!=UINT64_MAX-46)return;
 e->peer_code=2;if(!(unsigned)e->regs[3])return;
 e->peer_code=3;if(!space)return;
 ipc_port_t port=IP_NULL;e->peer_code=4;
 if(ipc_port_translate_send(space,(unsigned)e->regs[3],&port)!=KERN_SUCCESS)return;
 e->peer=ipc_port_get_receiver_task_locked(port,NULL);e->peer_code=(e->regs[1]&1)?0:5;ip_mq_unlock(port);
}
void boot_ui_trace_task(task_t task,unsigned index,int pid,const char *name,unsigned slide) {
 if(!ml_get_interrupts_enabled()){++draft_busy;return;}
 if(index<1 || index>BT_ROLES)return;
 unsigned role=index-1;
 draft_pid[role]=(unsigned)pid;draft_slide[role]=slide;
 strlcpy(draft_names[role],name,sizeof(draft_names[role]));
 if(!task_lock_try(task)){++draft_busy;return;}
 vm_map_t map=task->map;
 if(!map){task_unlock(task);++draft_busy;return;}
 vm_map_reference(map);
 ipc_space_t space=task->itk_space;if(space)is_reference(space);
 uint64_t images=task_get_all_image_info_addr(task);
 unsigned count=0,visited=0;thread_t thread;
 queue_iterate(&task->threads,thread,thread_t,task_threads) {
  if(visited++>=BT_MAX_THREADS){draft_omitted+=(unsigned)task->thread_count-BT_MAX_THREADS;break;}
  bool irq=ml_set_interrupts_enabled(false);
  if(!simple_lock_try(&thread->sched_lock,&thread_lck_grp)){++draft_busy;ml_set_interrupts_enabled(irq);continue;}
  arm_saved_state_t *state=thread->machine.upcb;
  if(state && is_saved_state64(state)) {
   struct bt_entry *e=&samples[count++];memset(e,0,sizeof(*e));
   e->index=visited;e->role=role?"BSTU"[role&3]:(!strcmp(name,"chronod")?'C':(!strcmp(name,"liveactivitiesd")?'L':'B'));
   e->pc=(uint64_t)get_saved_state_pc(state);e->lr=(uint64_t)get_saved_state_lr(state);
   e->fp=(uint64_t)get_saved_state_fp(state);
   e->trap=(uint64_t)get_saved_state_reg(state,16);
   for(unsigned reg=0;reg<8;++reg)e->regs[reg]=(uint64_t)get_saved_state_reg(state,reg);
   if((e->pc && !bt_user(e->pc,4)) || (e->lr && !bt_user(e->lr,4)))e->error=2;
  }
  thread_unlock(thread);ml_set_interrupts_enabled(irq);
 }
 task_unlock(task);
 // No thread pointers or suspended threads survive collection. The copied FP
 // can race with a running task, so malformed/changed chains are reported.
 for(unsigned i=0;i<count;++i) {
  boot_ui_trace_phase=role?5:4;boot_ui_trace_index=samples[i].index;
  lookup_peer(&samples[i],space);
  boot_ui_trace_phase=role?3:2;
  if(samples[i].index==1) {
   struct bt_main *main=&draft_main[role];main->entry=samples[i];main->pid=pid;main->slide=slide;
   if(!main->entry.error)bt_walk_n(&main->entry,read_user,map,main->frames,32);
   main->error=main->entry.error;
   uint64_t info[2]={0},image[3]={0};main->image_error=1;
   if(bt_user(images,sizeof(info)) && !read_user(map,images,info,sizeof(info)) &&
      (unsigned)(info[0]>>32) && bt_user(info[1],sizeof(image)) &&
      !read_user(map,info[1],image,sizeof(image)) && bt_user(image[0],32)) {
    main->image=image[0];main->image_error=0;
   }
  }
  if(!samples[i].error)bt_walk(&samples[i],read_user,map);
  bt_add(&draft,&samples[i]);
 }
 if(space)is_release(space);
 vm_map_deallocate(map);boot_ui_trace_phase=1;
}
void boot_ui_trace_publish(void) {
 if(!__atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE)) {
  published=draft;memcpy(published_main,draft_main,sizeof(draft_main));memcpy(published_pid,draft_pid,sizeof(draft_pid));
  memcpy(published_slide,draft_slide,sizeof(draft_slide));memcpy(published_names,draft_names,sizeof(draft_names));
  published_busy=draft_busy;published_omitted=draft_omitted;
  __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
 }
 boot_ui_trace_phase=0;
}
void boot_ui_trace_snapshot(unsigned seconds) {
 if(__atomic_exchange_n(&trace_lock,1,__ATOMIC_ACQUIRE))return;
 unsigned groups_pages=(published.count+BT_ROWS-1)/BT_ROWS;if(!groups_pages)groups_pages=1;
 unsigned main_pages=0;
 if(published_main[0].pid || published_main[1].pid)main_pages=1;
 if(published_main[2].pid || published_main[3].pid)main_pages=2;
 unsigned pages=groups_pages+main_pages;
 unsigned page=(seconds/8)%pages;
 if(page>=groups_pages)bt_main_rows(published_main,(page-groups_pages)*2,boot_ui_trace_rows);
 else bt_rows(&published,page,boot_ui_trace_rows);
 snprintf(boot_ui_trace_label,sizeof(boot_ui_trace_label),
  "STACK64 %u/%u %c=p%u+%x S=p%u+%x T=p%u U=p%u T%u G%u b%u o%u rd%u %s",
  page+1,pages,!strcmp(published_names[0],"chronod")?'C':(!strcmp(published_names[0],"liveactivitiesd")?'L':'B'),published_pid[0],published_slide[0],published_pid[1],published_slide[1],published_pid[2],published_pid[3],
  published.threads,published.count,published_busy,published_omitted,published.read_failures,
  page>=groups_pages?"MAIN32":"PC/LR/FP8; last=MAIN32");
 boot_ui_trace_ready=published.threads!=0;boot_ui_trace_groups=published.count;
 boot_ui_trace_threads=published.threads;boot_ui_trace_pages=pages;
 __atomic_store_n(&trace_lock,0,__ATOMIC_RELEASE);
}
