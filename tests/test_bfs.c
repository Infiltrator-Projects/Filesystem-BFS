/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define SIZE (2u*1024u*1024u)
#define B 512u
#define N 8u
#define PAY (12u*B)
static unsigned char disk[SIZE], stable[SIZE], baseline[SIZE], data[N][PAY];
static unsigned events, fail_at;
static int cut(void){return ++events==fail_at;}
static int rd(void *ctx,uint64_t off,void *p,uint32_t n){
    (void)ctx;assert(!(off%B)&&!(n%B)&&off+n<=SIZE);memcpy(p,disk+off,n);return 0;
}
static int wr(void *ctx,uint64_t off,const void *p,uint32_t n){
    (void)ctx;assert(!(off%B)&&!(n%B)&&off+n<=SIZE);if(cut())return -1;
    memcpy(disk+off,p,n);return 0;
}
static int flush(void *ctx){(void)ctx;if(cut())return -1;memcpy(stable,disk,SIZE);return 0;}
static void *alloc(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
static IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof h,
    .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,.volume_bytes=SIZE,
    .logical_sector_bytes=B,.physical_sector_bytes=B,.maximum_transfer_bytes=4096,
    .read_at=rd,.write_at=wr,.flush=flush,.allocate=alloc,.release=release};
static const IfsFsCoreOpsV2 *core;
static const IfsFsVolumeOpsV2 *vol;
static const IfsFsDefragOpsV2 *df;
static void *mount(void){
    void *v=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
    assert(core->mount(&h,&v,&info)==0);assert(v&&info.block_bytes==B);return v;
}
static void path(char p[20],unsigned i){snprintf(p,20,"/F%02u.BIN",i);}
static uint32_t le32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t le16(const unsigned char *p){return p[0]|(uint32_t)p[1]<<8;}
static void put32(unsigned char *p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(x>>(8*i));}
static void validate(void *v,uint64_t pos[N],unsigned policy){
    unsigned char buf[PAY];uint64_t errors=99;assert(vol->check(v,0,&errors)==0&&!errors);
    assert(le32(disk)==0x1badfaceu);
    for(unsigned i=0;i<N;i++){
        char p[20];path(p,i);memset(buf,0,sizeof buf);
        assert(core->read(v,p,0,buf,sizeof buf)==0&&!memcmp(buf,data[i],sizeof buf));
        IfsFsExtentV2 e={0};uint32_t count=0;
        assert(df->map(v,p,0,&e,1,&count)==0&&count==1&&e.length==PAY&&e.logical_offset==0);
        assert(e.physical_offset%B==0&&e.physical_offset+PAY<=SIZE);
        assert(!memcmp(disk+e.physical_offset,data[i],PAY));pos[i]=e.physical_offset;
        assert(df->map(v,p,1,&e,1,&count)==IFS_FS_END&&!count);
        /* Independent disk-format oracle: root directory -> inode -> extent. */
        uint32_t root=le32(disk+B+4)*B,rs=le32(disk+B+12)+1-root,ino=0;
        for(uint32_t x=0;x<rs;x+=16){
            unsigned char *ent=disk+root+x;
            if(le16(ent)&&!strncmp((char*)ent+2,p+1,14)){ino=le16(ent);break;}
        }
        assert(ino>=3);unsigned char *raw=disk+B+(ino-2)*64;
        assert(le16(raw)==ino&&le32(raw+4)*B==pos[i]);
        assert(le32(raw+12)+1-pos[i]==PAY);
    }
    uint64_t root_end=(uint64_t)(le32(disk+B+8)+1)*B;
    for(unsigned i=0;i<N;i++){
        uint64_t end=pos[i]+PAY,slack=policy?2u*B:0;
        assert(pos[i]>=root_end&&end+slack<=SIZE);
        for(unsigned j=0;j<N;j++)if(i!=j)assert(pos[j]>=end+slack||pos[j]+PAY<=pos[i]);
    }
}
static void corpus(void){
    memset(disk,0,SIZE);memset(stable,0,SIZE);events=fail_at=0;
    assert(vol->format(&h,1,"BFS",0)==0);void *v=mount();
    unsigned char filler[4*B]={0};
    assert(core->create(v,"/GAP.BIN",1)==0&&core->write(v,"/GAP.BIN",0,filler,sizeof filler)==0);
    for(unsigned i=0;i<N;i++){
        char p[20];path(p,i);for(unsigned j=0;j<PAY;j++)data[i][j]=(unsigned char)(i*37+j*13+(j>>8));
        assert(core->create(v,p,1)==0&&core->write(v,p,0,data[i],PAY)==0);
    }
    assert(core->remove(v,"/GAP.BIN")==0);
    assert(core->rename(v,"/F00.BIN","/RENAMED")==0&&core->rename(v,"/RENAMED","/F00.BIN")==0);
    assert(core->create(v,"/TEMP",1)==0&&core->truncate(v,"/TEMP",513)==0);
    unsigned char z[513];memset(z,1,sizeof z);assert(core->read(v,"/TEMP",0,z,sizeof z)==0);
    for(unsigned i=0;i<sizeof z;i++)assert(!z[i]);
    assert(core->truncate(v,"/TEMP",0)==0&&core->remove(v,"/TEMP")==0);
    uint64_t pos[N];validate(v,pos,0);
    assert(pos[0]>(uint64_t)(le32(disk+B+8)+1)*B);
    assert(core->unmount(v)==0);memcpy(baseline,stable,SIZE);
}
static void run(unsigned policy){
    corpus();void *v=mount(),*p=NULL;uint64_t moved=0,pos[N];
    assert(df->plan(v,"/",policy,&p)==0&&p);
    assert(df->execute(v,p,&moved)==0&&moved>0);
    assert(df->verify(v,p)==0);df->release_plan(v,p);
    validate(v,pos,policy);assert(core->unmount(v)==0);
    if(!policy){
        uint64_t start=(uint64_t)(le32(disk+B+8)+1)*B;
        for(unsigned i=0;i<N;i++)assert(pos[i]==start+i*PAY);
    }
    v=mount();validate(v,pos,policy);assert(core->unmount(v)==0);
    printf("BFS policy %u: actual contiguous extent placement and payloads verified\n",policy);
}
static void fault(void){
    corpus();void *v=mount(),*p=NULL;uint64_t moved=0,pos[N];events=0;
    assert(df->plan(v,"/F00.BIN",10,&p)==0&&df->execute(v,p,&moved)==0);
    unsigned limit=events;df->release_plan(v,p);assert(core->unmount(v)==0);
    assert(limit>4);
    for(unsigned n=1;n<=limit;n++){
        memcpy(disk,baseline,SIZE);memcpy(stable,baseline,SIZE);events=0;fail_at=n;
        v=mount();assert(df->plan(v,"/F00.BIN",10,&p)==0);
        assert(df->execute(v,p,&moved)!=0);df->release_plan(v,p);assert(core->unmount(v)==0);
        memcpy(disk,stable,SIZE);fail_at=0;v=mount();uint64_t repaired=0;
        assert(df->recover(v,0,&repaired)==0);validate(v,pos,0);assert(core->unmount(v)==0);
    }
    printf("BFS recovered every one of %u durable write/flush fault points\n",limit);
}
int main(int argc,char **argv){
    assert(argc==2||argc==3);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){puts(dlerror());return 1;}
    const IfsFsModuleApiV2 *(*api)(void)=NULL;void *sym=dlsym(lib,IFS_FS_COMMON_EXPORT_V2);
    assert(sym);memcpy(&api,&sym,sizeof api);const IfsFsModuleApiV2 *a=api();
    assert(a->abi_version==2&&!strcmp(a->format_id,"bfs"));
    core=(const void*)a->query_group(IFS_FS_GROUP_CORE,2,2);
    vol=(const void*)a->query_group(IFS_FS_GROUP_VOLUME,2,2);
    df=(const void*)a->query_group(IFS_FS_GROUP_DEFRAG,2,2);
    assert(core&&vol&&df&&df->header.capabilities==IFS_FS_DEFRAG_ALL);
    run(0);run(10);fault();
    /* An independently detected cross-linked inode must fail closed. */
    corpus();put32(disk+B+3*64+4,le32(disk+B+2*64+4));
    put32(disk+B+3*64+8,le32(disk+B+2*64+8));put32(disk+B+3*64+12,le32(disk+B+2*64+12));
    void *bad=NULL;IfsFsVolumeInfoV2 info={.structure_bytes=sizeof info};
    assert(core->mount(&h,&bad,&info)==IFS_FS_CORRUPT&&!bad);
    if(argc==3){
        FILE *fp=fopen(argv[2],"rb");assert(fp&&fread(disk,1,SIZE,fp)==SIZE);fclose(fp);
        IfsFsHostRuntimeV2 ro=h;ro.capabilities=IFS_FS_HOST_READ_ONLY;ro.write_at=NULL;ro.flush=NULL;
        void *v=NULL;assert(core->mount(&ro,&v,&info)==0);
        assert(core->create(v,"/NO",1)==IFS_FS_ACCESS);assert(core->unmount(v)==0);
        puts("BFS independent mkfs.bfs image mounted read-only");
    }
    dlclose(lib);puts("BFS qualification passed");return 0;
}