// SPDX-License-Identifier: Apache-2.0
// Chip model + real firmware image (when present): framing, identification, header parse, host download, frames.
#include "../NvtProtocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SIZE 0x200000
static uint8_t mem[MEM_SIZE];
static uint32_t page;
static unsigned auto_copy_polls, boot_rdy_writes, bus_calls;
static int crc_enabled, bootloader_resets, fail_boot_rdy, flip_write_number, flip_counter, bus_fault_at;
static uint32_t ilm_des,dlm_des,ilm_len,dlm_len,g_ilm,g_dlm;
static uint8_t id_bytes[6]={0x0e,0xaa,0xbb,0x32,0x65,0x03};
static unsigned eng_resets, id_writes;
static unsigned delays,cmd_busy_polls,seq_log_n;static uint32_t seq_log[8];

static uint32_t rd3(uint32_t a){return mem[a]|(mem[a+1]<<8)|(mem[a+2]<<16);}
static uint32_t rd4(uint32_t a){return rd3(a)|((uint32_t)mem[a+3]<<24);}
static void model_reset(void){
    memset(mem,0,sizeof(mem));eng_resets=0;id_writes=0;page=0;auto_copy_polls=0;crc_enabled=0;bootloader_resets=0;bus_calls=0;delays=0;
    fail_boot_rdy=0;flip_write_number=0;flip_counter=0;bus_fault_at=-1;
    memcpy(mem+0x1FB104,id_bytes,6);mem[0x1FB12C]=0x00;   // ENB_CASC clear = Cascade Chip (the sheng panel's reading)
}
static void on_write(uint32_t addr,const uint8_t *d,size_t n){
    for(size_t i=0;i<n;++i){
        if(addr+i>=0x1FB104 && addr+i<0x1FB10A){++id_writes;continue;}   // the chip-id registers are read-only
        mem[addr+i]=d[i];
    }
    if(addr<=0x1FB43E && 0x1FB43E<addr+n && mem[0x1FB43E]==0x69){++bootloader_resets;crc_enabled=0;mem[0x125800+0x60]=0;mem[0x11C400+0x60]=0;}
    if(addr<=0x1FC925 && 0x1FC925<addr+n && mem[0x1FC925]==0x56)auto_copy_polls=3;
    if(addr==0x11C400+0x50 && d[0]==0xae)crc_enabled=1;
    if(addr==0x11C400+0x50 && n==5 && d[0]==0xbf){mem[addr]=cmd_busy_polls?0xbf:0;if(!cmd_busy_polls&&seq_log_n<8)seq_log[seq_log_n++]=d[1];}
    if(addr==0x113718&&seq_log_n<8)seq_log[seq_log_n++]=addr;
    if(addr<=0x1FB50D && 0x1FB50D<addr+n && mem[0x1FB50D]==1) {
        ++boot_rdy_writes;
        ilm_des=rd3(0x1FB528);ilm_len=rd3(0x1FB518);dlm_des=rd3(0x1FB52C);dlm_len=rd3(0x1FB530);g_ilm=rd4(0x1FB500);g_dlm=rd4(0x1FB504);
        // The boot ROM verifies ILM and DLM against the golden checksums with the same CRC engine.
        int ok=crc_enabled && !fail_boot_rdy
            && nvt_crc32_words(mem+ilm_des,ilm_len,MEM_SIZE-ilm_des)==g_ilm
            && nvt_crc32_words(mem+dlm_des,dlm_len,MEM_SIZE-dlm_des)==g_dlm;
        if(ok){mem[0x11C400+0x60]=0xa0;mem[0x125800+0x60]=0xa0;}
    }
}
static int xfer(void *ctx,const uint8_t *tx,uint8_t *rx,size_t len){
    (void)ctx;++bus_calls;
    if(bus_fault_at>=0 && (int)bus_calls==bus_fault_at) return -1;
    assert(len>=2);
    if(tx[0]==0xff && len==3){page=((uint32_t)tx[1]<<8)|tx[2];return 0;}
    uint32_t addr=(page<<7)|(tx[0]&0x7f);
    if(tx[0]&0x80) {
        assert(rx==NULL);
        if(addr==0x7FFF80){assert(len==2 && tx[1]==0x5a);++eng_resets;return 0;}   // ENG_RST_ADDR lies outside the modelled memory
        uint8_t *copy=malloc(len-1);memcpy(copy,tx+1,len-1);
        if(flip_write_number && len>1000 && ++flip_counter==flip_write_number)copy[len/2]^=0x10;
        assert(addr+len-1<=MEM_SIZE);on_write(addr,copy,len-1);free(copy);return 0;
    }
    assert(rx && len>=3 && tx[1]==0);   // dummy byte after the address phase
    rx[0]=rx[1]=0;
    for(size_t k=0;k+2<len;++k){
        uint32_t a=addr+(uint32_t)k;uint8_t v=mem[a];
        if(a==0x1FC925 && auto_copy_polls){--auto_copy_polls;v=0x56;}
        if(a==0x1FC925 && !auto_copy_polls && v==0x56){} 
        if(a==0x11C400+0x50 && v==0xbf){if(cmd_busy_polls){--cmd_busy_polls;}else{mem[a]=0;v=0;}}
        rx[2+k]=v;
    }
    if(addr<=0x1FC925 && 0x1FC925<addr+len-2 && !auto_copy_polls)mem[0x1FC925]=0;
    return 0;
}
static void delay_ms(void *ctx,unsigned ms){(void)ctx;(void)ms;++delays;}

static uint8_t txbuf[NVT_TRANSFER_LEN+8],rxbuf[NVT_READ_MAX+8];
static const struct nvt_bus bus={NULL,xfer,delay_ms};
static struct nvt_chip chip={.bus=&bus,.scratch=txbuf,.rxbuf=rxbuf};

static uint8_t *load(const char *path,size_t *size){
    FILE *f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);*size=(size_t)ftell(f);fseek(f,0,SEEK_SET);
    uint8_t *d=malloc(*size);assert(fread(d,1,*size,f)==*size);fclose(f);return d;
}
static uint32_t le32(const uint8_t *p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}

int main(int argc,char **argv){
    // 1. Framing: write sets the page then the offset byte with bit 7; read skips address and dummy bytes.
    model_reset();
    uint8_t pattern[300];for(unsigned i=0;i<sizeof(pattern);++i)pattern[i]=(uint8_t)(i*7+3);
    assert(!nvt_write(&chip,0x123456,pattern,sizeof(pattern)));
    assert(!memcmp(mem+0x123456,pattern,sizeof(pattern)));   // burst crosses 128-byte page boundaries in the chip
    uint8_t back[300];assert(!nvt_read(&chip,0x123456,back,sizeof(back)));assert(!memcmp(back,pattern,sizeof(back)));
    assert(nvt_write(&chip,0x10,pattern,0)==NVT_ERR_PARAM&&nvt_read(&chip,0x10,back,NVT_READ_MAX+1)==NVT_ERR_PARAM);
    // 2. Identification: NT36532E id with ENB_CASC clear selects the NT36532E cascade map (vendor: set = single chip).
    model_reset();assert(!nvt_identify(&chip));assert(chip.cascade&&chip.map->event_buf==0x11C400&&bootloader_resets==1);
    assert(eng_resets==1 && id_writes==6);   // engine reset once; the 7-byte address write targets the six read-only id bytes
    model_reset();mem[0x1FB12C]=1;assert(!nvt_identify(&chip));assert(!chip.cascade&&chip.map->event_buf==0x125800);   // bit set = Single Chip
    model_reset();mem[0x1FB104]=0x11;assert(!nvt_identify(&chip));assert(chip.cascade&&chip.map->event_buf==0x125800);
    model_reset();mem[0x1FB107]=0x00;assert(nvt_identify(&chip)==NVT_ERR_CHIP);assert(bootloader_resets==5);   // five attempts, as the vendor loop
    model_reset();bus_fault_at=3;assert(nvt_identify(&chip)==NVT_ERR_BUS);
    // 3. Frame read and matrix extraction.
    model_reset();assert(!nvt_identify(&chip));
    uint8_t frame[NVT_THP_FRAME_LEN],got[NVT_THP_FRAME_LEN];
    for(unsigned i=0;i<sizeof(frame);++i)frame[i]=(uint8_t)(i*13+5);
    frame[NVT_THP_HEADER_LEN+24]=4;frame[NVT_THP_HEADER_LEN+2]=0x34;frame[NVT_THP_HEADER_LEN+3]=0x12;
    frame[0]=0;   // index 0 is the offset byte of the vendor buffer; the chip's bytes start at index 1
    memcpy(mem+chip.map->event_buf,frame+1,sizeof(frame)-1);
    assert(!nvt_read_frame(&chip,got));assert(!memcmp(got,frame,sizeof(frame)));
    assert(nvt_frame_type(got)==4&&nvt_frame_counter(got)==0x1234);
    {   // peek: the header window of the same frame
        uint8_t pk[NVT_THP_PEEK_LEN];assert(!nvt_peek_frame_header(&chip,pk));
        assert(!memcmp(pk,frame+NVT_THP_HEADER_LEN,NVT_THP_PEEK_LEN));
        assert(nvt_peek_counter(pk)==0x1234&&nvt_peek_type(pk)==4);
    }
    int16_t matrix[2400];nvt_frame_matrix(got,matrix);
    for(int i=0;i<2400;++i)assert((uint16_t)matrix[i]==(uint16_t)(frame[NVT_THP_MATRIX_OFFSET+i*2]|(frame[NVT_THP_MATRIX_OFFSET+i*2+1]<<8)));
    // 3b. Post-download sequence: fw info, custom commands with status handshake, doze delay, header check.
    model_reset();assert(!nvt_identify(&chip));
    {
        uint32_t ev=chip.map->event_buf;
        mem[ev+0x78]=0x2a;mem[ev+0x79]=0xd5;mem[ev+0x7a]=60;mem[ev+0x7b]=40;mem[ev+0x7c]=0x0a;mem[ev+0x7d]=0x00;mem[ev+0x7e]=0x08;mem[ev+0x7f]=0x00;
        mem[ev+0x78+13]=0x07;mem[ev+0x78+34]=0x34;mem[ev+0x78+35]=0x12;
        struct nvt_fw_info info;assert(!nvt_get_fw_info(&chip,&info));
        assert(info.fw_ver==0x2a&&info.x_num==60&&info.y_num==40&&info.abs_x_max==0x0a00&&info.abs_y_max==0x0800&&info.fw_type==7&&info.pid==0x1234);
        mem[ev+0x79]=0x00;assert(nvt_get_fw_info(&chip,&info)==NVT_ERR_FW&&info.defaulted);mem[ev+0x79]=0xd5;
        cmd_busy_polls=0;assert(!nvt_set_custom_cmd(&chip,0x19,0x0000));
        assert(mem[ev+0x51]==0x19&&mem[ev+0x52]==0&&mem[ev+0x53]==0&&mem[ev+0x54]==0);
        cmd_busy_polls=2;assert(!nvt_set_custom_cmd(&chip,0x01,0x0102));assert(mem[ev+0x51]==0x01&&mem[ev+0x53]==0x02&&mem[ev+0x54]==0x01);
        cmd_busy_polls=9;assert(nvt_set_custom_cmd(&chip,0x04,1)==NVT_ERR_TIMEOUT);cmd_busy_polls=0;
        assert(!nvt_set_doze_delay(&chip,120));assert(mem[0x113718]==120&&mem[0x113719]==0);
        unsigned d0=delays;cmd_busy_polls=0;seq_log_n=0;assert(!nvt_start_scanning(&chip,&info));
        assert(seq_log_n==3&&seq_log[0]==0x19&&seq_log[1]==0x113718&&seq_log[2]==0x01&&delays>d0);
        uint8_t f[NVT_THP_FRAME_LEN]={0};uint8_t *h=f+NVT_THP_HEADER_LEN;
        h[4]=0x34;h[5]=0x12;h[12]=0xcb;h[13]=0xed;h[8]=0x4e;h[9]=0x54;h[10]=0x50;h[11]=0x31;h[16]=0xb1;h[17]=0xab;h[18]=0xaf;h[19]=0xce;
        assert(nvt_frame_header_valid(f));h[17]^=1;assert(!nvt_frame_header_valid(f));
    }
    // 4. Real firmware image, if available: parser, download into the model, and failure handling.
    const char *path=argc>1?argv[1]:getenv("NVT_FIRMWARE");
    if(!path||!*path){puts("PASS (framing/identify/frame tests); real-firmware tests skipped: pass the firmware path");return 0;}
    size_t size;uint8_t *fwdata=load(path,&size);
    if(!fwdata){puts("PASS (framing/identify/frame tests); firmware file not found, real-firmware tests skipped");return 0;}
    struct nvt_fw fw;assert(!nvt_fw_parse(&fw,fwdata,size));
    // Independent expectation straight from the first six header words.
    assert(fw.part[0].bin_addr==le32(fwdata)&&fw.part[0].sram_addr==le32(fwdata+4)&&fw.part[0].size==le32(fwdata+8));
    assert(fw.part[1].bin_addr==le32(fwdata+12)&&fw.part[1].sram_addr==le32(fwdata+16)&&fw.part[1].size==le32(fwdata+20));
    assert(fw.cascade_2nd_header==((fwdata[0x20]>>1)&1)&&fw.count>=2&&fw.count<=NVT_MAX_PARTITIONS);
    for(unsigned i=0;i<fw.count;++i)assert((uint64_t)fw.part[i].bin_addr+fw.part[i].size<=size);
    assert(!strcmp(fw.part[0].name,"ILM")&&!strcmp(fw.part[1].name,"DLM"));
    printf("firmware: %zu bytes, %u partitions, cascade header %d, ILM %u@0x%x DLM %u@0x%x\n",size,fw.count,fw.cascade_2nd_header,
        fw.part[0].size,fw.part[0].sram_addr,fw.part[1].size,fw.part[1].sram_addr);
    model_reset();assert(!nvt_identify(&chip));
    assert(!nvt_download_firmware(&chip,&fw));
    assert(mem[0x11C400+0x60]==0xa0&&boot_rdy_writes==1&&crc_enabled);
    assert(!memcmp(mem+fw.part[0].sram_addr,fwdata+fw.part[0].bin_addr,fw.part[0].size));
    assert(!memcmp(mem+fw.part[1].sram_addr,fwdata+fw.part[1].bin_addr,fw.part[1].size));
    for(unsigned i=0;i<fw.count;++i)if(fw.part[i].size)assert(!memcmp(mem+fw.part[i].sram_addr,fwdata+fw.part[i].bin_addr,fw.part[i].size));
    assert(mem[0x1FC925]==0);   // auto-copy flag was polled to completion
    // A corrupted SRAM write is caught by the boot ROM's CRC: the first attempt fails, the retry succeeds.
    model_reset();assert(!nvt_identify(&chip));flip_write_number=1;boot_rdy_writes=0;int before_reset=bootloader_resets;
    assert(!nvt_download_firmware(&chip,&fw));
    assert(bootloader_resets-before_reset==2&&boot_rdy_writes==2&&mem[0x11C400+0x60]==0xa0);
    // Persistent failure (boot ROM never reports ready) times out after three attempts.
    model_reset();assert(!nvt_identify(&chip));fail_boot_rdy=1;int before=bootloader_resets;boot_rdy_writes=0;
    assert(nvt_download_firmware(&chip,&fw)==NVT_ERR_TIMEOUT&&bootloader_resets-before==3);
    // Truncated or malformed images are refused without reading out of bounds.
    struct nvt_fw bad;assert(nvt_fw_parse(&bad,fwdata,0x20)==NVT_ERR_FW);
    assert(nvt_fw_parse(&bad,fwdata,size/2)==NVT_ERR_FW);
    uint8_t *copy=malloc(size);memcpy(copy,fwdata,size);copy[0]=0xff;copy[1]=0xff;copy[2]=0xff;copy[3]=0x7f;
    assert(nvt_fw_parse(&bad,copy,size)==NVT_ERR_FW);free(copy);free(fwdata);
    puts("PASS: NvtProtocol framing, identification, real-firmware parse and model download with CRC, retries, failures; model only, no hardware");
    return 0;
}
