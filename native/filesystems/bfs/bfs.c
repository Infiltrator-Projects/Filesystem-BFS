/* SPDX-License-Identifier: GPL-3.0-or-later
 * SCO/UnixWare Boot File System (BFS), NOT BeFS.
 * Native Common V2 implementation of the fixed little-endian disk
 * structures. BFS has contiguous file allocations by design: ordinary
 * defrag compacts inter-file holes; growth reserves adjacent free blocks.
 * The formatter marks its reserved superblock padding for a redo journal.
 * Foreign media can be read but are never modified by this implementation.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/* GCC may lower aggregate initialization to these routines even in
 * a freestanding LTO build. Keep the implementations private and
 * independent of libc, including when native optimizations are enabled. */
__attribute__((visibility("hidden"),used,externally_visible))
void *memset(void *dst,int value,size_t n)
{ volatile unsigned char *p=dst;while(n--)*p++=(unsigned char)value;return dst; }
__attribute__((visibility("hidden"),used,externally_visible))
void *memcpy(void *dst,const void *src,size_t n)
{ volatile unsigned char *p=dst;const volatile unsigned char *q=src;while(n--)*p++=*q++;return dst; }

enum { B=512, I=64, D=16, ROOT=2, MAXINO=513, MAXNAME=14,
       NATIVE_INODES=512, ROOT_BLOCKS=16, DIR_LIMIT=4096,
       JOFF=64, JLEN=224 };
#define MAGIC UINT32_C(0x1badface)
#define NONE UINT32_MAX
static const uint8_t jmagic[8]={'I','F','S','B','F','S','2',0};

static void copy(void *d,const void *s,size_t n)
{ uint8_t *a=d;const uint8_t *b=s;while(n--)*a++=*b++; }
static void zero(void *d,size_t n)
{ uint8_t *p=d;while(n--)*p++=0; }
static int eq(const void *a,const void *b,size_t n)
{ const uint8_t *x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1; }
static uint16_t get16(const uint8_t *p)
{ return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)get16(p)|((uint32_t)get16(p+2)<<16); }
static uint64_t get64(const uint8_t *p)
{ return (uint64_t)get32(p)|((uint64_t)get32(p+4)<<32); }
static void put16(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p,uint32_t v)
{ put16(p,v);put16(p+2,v>>16); }
static void put64(uint8_t *p,uint64_t v)
{ put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32)); }
static uint32_t namelen(const uint8_t *p,uint32_t max)
{ uint32_t n=0;while(n<max && p[n])++n;return n; }

typedef struct { uint32_t data,total,maxino; uint64_t bytes; } Geometry;
typedef struct { uint32_t ino,type,first,last,size,links; } Node;
typedef struct {
    uint32_t inode,slot;
    uint8_t old_inode[I],new_inode[I],old_dir[D],new_dir[D];
    uint64_t digest;
} Intent;
typedef struct { uint32_t ino,from,to,count; } Step;
typedef struct { uint32_t ino,target,count,size,reserve; } Expected;
typedef struct {
    uint32_t steps,files,policy,executed;
    Step step[2u*MAXINO];Expected expected[MAXINO];
} Plan;
typedef struct {
    IfsFsHostRuntimeV2 h;Geometry g;Node root;
    uintptr_t token,generation,plan_token,plan_generation;
    unsigned journal;uint64_t recovered,free_blocks;
    Plan *plan;
} Session;
static Session s;
static atomic_flag gate=ATOMIC_FLAG_INIT;
static int enter(void)
{ return !atomic_flag_test_and_set_explicit(&gate,memory_order_acquire); }
static int leave(int rc)
{ atomic_flag_clear_explicit(&gate,memory_order_release);return rc; }
static int live(void *v)
{ return s.token && (uintptr_t)v==s.token; }
static int cancelled(const IfsFsHostRuntimeV2 *h)
{ return h->cancelled && h->cancelled(h->context); }
static int writable(const IfsFsHostRuntimeV2 *h)
{
    return !(h->capabilities&IFS_FS_HOST_READ_ONLY) &&
        (h->capabilities&IFS_FS_HOST_EXCLUSIVE) &&
        (h->capabilities&IFS_FS_HOST_DURABLE) && h->write_at && h->flush;
}
static int rd(const IfsFsHostRuntimeV2 *h,uint64_t at,void *p,uint32_t n)
{ return ifs_fs_host_read_bytes_v2(h,at,p,n); }
static int wr(const IfsFsHostRuntimeV2 *h,uint64_t at,const void *p,uint32_t n)
{ return ifs_fs_host_write_bytes_v2(h,at,p,n); }
static int barrier(const IfsFsHostRuntimeV2 *h)
{ return h->flush(h->context)==0?IFS_FS_OK:IFS_FS_IO_ERROR; }
static int durable(const IfsFsHostRuntimeV2 *h,uint64_t at,const void *p,uint32_t n)
{ int rc=wr(h,at,p,n);return rc==IFS_FS_OK?barrier(h):rc; }

static int geometry(const IfsFsHostRuntimeV2 *h,uint8_t raw[B],Geometry *g)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(h->volume_bytes<3u*B)return IFS_FS_NOT_FOUND;
    int rc=rd(h,0,raw,B);if(rc!=IFS_FS_OK)return rc;
    if(get32(raw)!=MAGIC)return IFS_FS_NOT_FOUND;
    const uint32_t start=get32(raw+4),end=get32(raw+8);
    const uint64_t bytes=(uint64_t)end+1u;
    if(start<B+I || (start-B)%I || bytes% B || bytes>h->volume_bytes ||
       bytes<=start)return IFS_FS_CORRUPT;
    uint32_t data=(uint32_t)(((uint64_t)start+B-1u)/B);
    uint32_t total=(uint32_t)(bytes/B),maxino=(start-B)/I+ROOT-1u;
    if(data>=total || maxino<ROOT || maxino>MAXINO)return IFS_FS_CORRUPT;
    *g=(Geometry){data,total,maxino,bytes};return IFS_FS_OK;
}
static uint64_t inode_at(uint32_t ino)
{ return B+(uint64_t)(ino-ROOT)*I; }
static int decode(const Geometry *g,uint32_t ino,const uint8_t raw[I],Node *n)
{
    zero(n,sizeof *n);
    if(!get16(raw))return IFS_FS_NOT_FOUND;
    if(get16(raw)!=ino || ino<ROOT || ino>g->maxino)return IFS_FS_CORRUPT;
    n->ino=ino;n->first=get32(raw+4);n->last=get32(raw+8);
    n->type=get32(raw+16);n->links=get32(raw+32);
    const uint32_t end=get32(raw+12);
    if(n->type!=(ino==ROOT?2u:1u) || !n->links)return IFS_FS_CORRUPT;
    if(!n->first) {
        if(n->last || ino==ROOT)return IFS_FS_CORRUPT;
        return IFS_FS_OK;
    }
    if(n->first<g->data || n->last<n->first || n->last>=g->total ||
       (uint64_t)end<(uint64_t)n->first*B ||
       (uint64_t)end>=((uint64_t)n->last+1u)*B)return IFS_FS_CORRUPT;
    uint64_t size=(uint64_t)end+1u-(uint64_t)n->first*B;
    if(size>UINT32_MAX)return IFS_FS_CORRUPT;
    n->size=(uint32_t)size;
    if(ino==ROOT && (n->size%D || n->size/D>DIR_LIMIT))
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int node_read(const IfsFsHostRuntimeV2 *h,const Geometry *g,
                     uint32_t ino,Node *n,uint8_t raw[I])
{
    if(ino<ROOT || ino>g->maxino)return IFS_FS_INVALID;
    int rc=rd(h,inode_at(ino),raw,I);
    return rc==IFS_FS_OK?decode(g,ino,raw,n):rc;
}
static uint32_t blocks(const Node *n)
{ return n->first?n->last-n->first+1u:0u; }
static uint64_t dir_at(const Node *root,uint32_t index)
{ return (uint64_t)root->first*B+(uint64_t)index*D; }
static int valid_name(const uint8_t *p,uint32_t n)
{
    if(!n || n>MAXNAME || (n==1u && p[0]=='.') ||
       (n==2u && p[0]=='.' && p[1]=='.'))return 0;
    for(uint32_t i=0;i<n;++i)
        if(p[i]<32u || p[i]==127u || p[i]=='/' || p[i]=='\\')return 0;
    return 1;
}
static int path_name(const char *path,const uint8_t **p,uint32_t *n)
{
    if(!path || path[0]!='/')return IFS_FS_INVALID;
    *p=(const uint8_t *)path+1;*n=0;
    while(*n<=MAXNAME && (*p)[*n])++*n;
    return valid_name(*p,*n)?IFS_FS_OK:IFS_FS_INVALID;
}
static int root_path(const char *p)
{ return p && p[0]=='/' && !p[1]; }
static int bit(const uint8_t *map,uint32_t b)
{ return (map[b/8u]&(uint8_t)(1u<<(b%8u)))!=0; }
static void setbit(uint8_t *map,uint32_t b,int used)
{
    uint8_t mask=(uint8_t)(1u<<(b%8u));
    if(used)map[b/8u]|=mask;else map[b/8u]&=(uint8_t)~mask;
}
static void range_set(uint8_t *map,uint32_t start,uint32_t n,int used)
{ for(uint32_t i=0;i<n;++i)setbit(map,start+i,used); }
static int range_free(const Geometry *g,const uint8_t *map,uint32_t start,uint32_t n)
{
    if(start<g->data || start>=g->total || n>g->total-start)return 0;
    for(uint32_t i=0;i<n;++i)if(bit(map,start+i))return 0;
    return 1;
}
static uint32_t find_run(const Geometry *g,const uint8_t *map,uint32_t first,uint32_t n)
{
    uint32_t run=0;
    if(first<g->data)first=g->data;
    for(uint32_t i=first;i<g->total;++i) {
        if(bit(map,i))run=0;
        else if(++run==n)return i-run+1u;
    }
    return 0;
}
/* Full inode ownership and root namespace validation. Hard links are
 * counted, not treated as duplicated physical allocations. Reject
 * overlapping file extents, unreferenced inodes, duplicate names and
 * out-of-range directory entries before exposing a writable session. */
static int scan(const IfsFsHostRuntimeV2 *h,const Geometry *g,Node *nodes,
                uint8_t **map_out,uint64_t *free_out)
{
    uint32_t refs[MAXINO+1u]={0};
    const uint32_t mb=g->total/8u+1u;
    uint8_t *map=h->allocate(h->context,mb),*entries=NULL;
    if(!map)return IFS_FS_NO_MEMORY;
    zero(map,mb);zero(nodes,(MAXINO+1u)*sizeof *nodes);
    range_set(map,0,g->data,1);
    uint64_t used=0u;int rc=IFS_FS_OK;
    for(uint32_t ino=ROOT;ino<=g->maxino;++ino) {
        if(cancelled(h)){rc=IFS_FS_BUSY;goto done;}
        uint8_t raw[I];Node n;
        rc=node_read(h,g,ino,&n,raw);
        if(rc==IFS_FS_NOT_FOUND){if(ino==ROOT){rc=IFS_FS_CORRUPT;goto done;}continue;}
        if(rc!=IFS_FS_OK)goto done;
        nodes[ino]=n;
        uint32_t count=blocks(&n);
        if(count && !range_free(g,map,n.first,count)){rc=IFS_FS_CORRUPT;goto done;}
        if(count)range_set(map,n.first,count,1);
        used+=count;
    }
    const Node *root=&nodes[ROOT];
    entries=h->allocate(h->context,root->size);
    if(!entries){rc=IFS_FS_NO_MEMORY;goto done;}
    rc=rd(h,(uint64_t)root->first*B,entries,root->size);
    if(rc!=IFS_FS_OK)goto done;
    unsigned dot=0u,dotdot=0u;
    for(uint32_t i=0;i<root->size/D;++i) {
        const uint8_t *e=entries+(uint64_t)i*D;const uint32_t ino=get16(e);
        if(!ino)continue;
        const uint32_t n=namelen(e+2,MAXNAME);
        if(ino<ROOT || ino>g->maxino || !nodes[ino].ino){rc=IFS_FS_CORRUPT;goto done;}
        if(ino==ROOT) {
            if(n==1u && e[2]=='.')++dot;
            else if(n==2u && e[2]=='.' && e[3]=='.')++dotdot;
            else {rc=IFS_FS_CORRUPT;goto done;}
        } else if(!valid_name(e+2,n)){rc=IFS_FS_CORRUPT;goto done;}
        for(uint32_t j=0;j<i;++j) {
            const uint8_t *prev=entries+(uint64_t)j*D;
            if(get16(prev) && namelen(prev+2,MAXNAME)==n && eq(prev+2,e+2,n)){
                rc=IFS_FS_CORRUPT;goto done;
            }
        }
        ++refs[ino];
    }
    if(dot!=1u || dotdot!=1u){rc=IFS_FS_CORRUPT;goto done;}
    for(uint32_t ino=ROOT;ino<=g->maxino;++ino)
        if(nodes[ino].ino && refs[ino]!=nodes[ino].links){
            rc=IFS_FS_CORRUPT;goto done;
        }
    if(used>g->total-g->data){rc=IFS_FS_CORRUPT;goto done;}
    *free_out=g->total-g->data-used;
    *map_out=map;map=NULL;rc=IFS_FS_OK;
done:
    if(entries)h->release(h->context,entries);
    if(map)h->release(h->context,map);
    return rc;
}
static int lookup(const IfsFsHostRuntimeV2 *h,const Geometry *g,const Node *root,
                  const char *path,Node *node,uint32_t *slot,uint8_t raw[I])
{
    if(root_path(path)){if(slot)*slot=NONE;return node_read(h,g,ROOT,node,raw);}
    const uint8_t *name;uint32_t n;
    int rc=path_name(path,&name,&n);if(rc!=IFS_FS_OK)return rc;
    for(uint32_t j=0;j<root->size/D;++j) {
        uint8_t e[D];rc=rd(h,dir_at(root,j),e,D);if(rc!=IFS_FS_OK)return rc;
        if(get16(e) && namelen(e+2,MAXNAME)==n && eq(e+2,name,n)) {
            if(slot)*slot=j;
            return node_read(h,g,get16(e),node,raw);
        }
    }
    return IFS_FS_NOT_FOUND;
}
static uint32_t checksum(const uint8_t *p)
{
    uint32_t v=UINT32_C(2166136261);
    for(uint32_t i=0;i<JLEN-4u;++i)v=(v^p[i])*UINT32_C(16777619);
    return v;
}
static void encode_journal(uint8_t raw[B],const Geometry *g,const Intent *j)
{
    uint8_t *p=raw+JOFF;zero(p,JLEN);copy(p,jmagic,sizeof jmagic);
    put32(p+8,g->data);put32(p+12,g->total);
    if(j) {
        put32(p+16,1u);put32(p+20,j->inode);put32(p+24,j->slot);
        copy(p+28,j->old_inode,I);copy(p+92,j->new_inode,I);
        copy(p+156,j->old_dir,D);copy(p+172,j->new_dir,D);
        put64(p+188,j->digest);
        put32(raw+12,get32(j->old_inode+4));put32(raw+16,get32(j->new_inode+4));
        put32(raw+20,get32(j->old_inode+4));put32(raw+24,get32(j->new_inode+4));
    } else {
        for(uint32_t i=12u;i<=24u;i+=4u)put32(raw+i,NONE);
    }
    put32(p+JLEN-4u,checksum(p));
}
static int decode_journal(const uint8_t raw[B],const Geometry *g,Intent *j,unsigned *state)
{
    const uint8_t *p=raw+JOFF;*state=0;
    const int clean=get32(raw+12)==NONE && get32(raw+16)==NONE;
    if(!eq(p,jmagic,sizeof jmagic))return clean?IFS_FS_OK:IFS_FS_CORRUPT;
    if(get32(p+JLEN-4u)!=checksum(p) || get32(p+8)!=g->data ||
       get32(p+12)!=g->total || get32(p+16)>1u)return IFS_FS_CORRUPT;
    *state=get32(p+16)?2u:1u;
    if((*state==1u)!=clean)return IFS_FS_CORRUPT;
    if(*state==2u) {
        zero(j,sizeof *j);j->inode=get32(p+20);j->slot=get32(p+24);
        copy(j->old_inode,p+28,I);copy(j->new_inode,p+92,I);
        copy(j->old_dir,p+156,D);copy(j->new_dir,p+172,D);
        j->digest=get64(p+188);
        if(j->inode<=ROOT || j->inode>g->maxino)return IFS_FS_CORRUPT;
    }
    return IFS_FS_OK;
}
static int write_journal(const IfsFsHostRuntimeV2 *h,const Geometry *g,const Intent *j)
{
    uint8_t raw[B];Geometry current;
    int rc=geometry(h,raw,&current);if(rc!=IFS_FS_OK)return rc;
    if(current.data!=g->data || current.total!=g->total || current.maxino!=g->maxino)
        return IFS_FS_CORRUPT;
    encode_journal(raw,g,j);return durable(h,0,raw,B);
}
static int hash_data(const IfsFsHostRuntimeV2 *h,const Node *n,uint64_t *digest)
{
    uint8_t p[4096];uint64_t v=UINT64_C(14695981039346656037),at=0;
    while(at<n->size) {
        uint32_t take=n->size-at<sizeof p?(uint32_t)(n->size-at):sizeof p;
        int rc=rd(h,(uint64_t)n->first*B+at,p,take);if(rc!=IFS_FS_OK)return rc;
        for(uint32_t i=0;i<take;++i)v=(v^p[i])*UINT64_C(1099511628211);
        at+=take;
    }
    *digest=v;return IFS_FS_OK;
}
/* The intent contains the actual prior and desired inode records and,
 * for namespace mutations, the exact prior and desired directory slot.
 * A different current record or a new extent owned by another inode
 * makes replay unsafe. Data is verified before any metadata publication.
 */
static int replay(const IfsFsHostRuntimeV2 *h,const Geometry *g,const Intent *j)
{
    uint8_t raw[I],dir[D];Node old,newnode,root;
    int rc=rd(h,inode_at(j->inode),raw,I);if(rc!=IFS_FS_OK)return rc;
    if(!eq(raw,j->old_inode,I) && !eq(raw,j->new_inode,I))return IFS_FS_CORRUPT;
    rc=decode(g,j->inode,j->old_inode,&old);
    if(rc!=IFS_FS_OK && rc!=IFS_FS_NOT_FOUND)return rc;
    rc=decode(g,j->inode,j->new_inode,&newnode);
    if(rc!=IFS_FS_OK && rc!=IFS_FS_NOT_FOUND)return rc;
    if(!old.ino && !newnode.ino)return IFS_FS_CORRUPT;
    if(newnode.ino) {
        for(uint32_t ino=ROOT;ino<=g->maxino;++ino) {
            if(ino==j->inode)continue;
            Node peer;uint8_t peerraw[I];
            rc=node_read(h,g,ino,&peer,peerraw);
            if(rc==IFS_FS_NOT_FOUND)continue;
            if(rc!=IFS_FS_OK)return rc;
            if(peer.first && newnode.first &&
               peer.first<=newnode.last && newnode.first<=peer.last)
                return IFS_FS_CORRUPT;
        }
        uint64_t digest=0;rc=hash_data(h,&newnode,&digest);
        if(rc!=IFS_FS_OK)return rc;
        if(digest!=j->digest)return IFS_FS_CORRUPT;
    }
    if(j->slot!=NONE) {
        rc=node_read(h,g,ROOT,&root,raw);if(rc!=IFS_FS_OK)return rc;
        if(j->slot>=root.size/D)return IFS_FS_CORRUPT;
        rc=rd(h,dir_at(&root,j->slot),dir,D);if(rc!=IFS_FS_OK)return rc;
        if(!eq(dir,j->old_dir,D) && !eq(dir,j->new_dir,D))return IFS_FS_CORRUPT;
        uint32_t a=get16(j->old_dir),b=get16(j->new_dir);
        if((a && a!=j->inode) || (b && b!=j->inode) || (!a && !b) ||
           (b && !valid_name(j->new_dir+2,namelen(j->new_dir+2,MAXNAME))))
            return IFS_FS_CORRUPT;
    } else if(!old.ino || !newnode.ino)return IFS_FS_CORRUPT;
    if(!newnode.ino) {
        rc=durable(h,dir_at(&root,j->slot),j->new_dir,D);
        if(rc!=IFS_FS_OK)return rc;
    }
    rc=durable(h,inode_at(j->inode),j->new_inode,I);
    if(rc!=IFS_FS_OK)return rc;
    if(newnode.ino && j->slot!=NONE) {
        rc=durable(h,dir_at(&root,j->slot),j->new_dir,D);
        if(rc!=IFS_FS_OK)return rc;
    }
    return write_journal(h,g,NULL);
}
static void info(IfsFsVolumeInfoV2 *o)
{
    o->variant=1u;o->max_name_bytes=MAXNAME;o->block_bytes=B;
    o->total_bytes=(uint64_t)(s.g.total-s.g.data)*B;
    o->free_bytes=s.free_blocks*B;
}
static void drop_plan(void)
{
    if(s.plan)s.h.release(s.h.context,s.plan);
    s.plan=NULL;s.plan_token=0;
}
static int refresh(void)
{
    Node *nodes=s.h.allocate(s.h.context,(MAXINO+1u)*sizeof *nodes);
    if(!nodes)return IFS_FS_NO_MEMORY;
    uint8_t *map=NULL;int rc=scan(&s.h,&s.g,nodes,&map,&s.free_blocks);
    if(rc==IFS_FS_OK)s.root=nodes[ROOT];
    if(map)s.h.release(s.h.context,map);
    s.h.release(s.h.context,nodes);return rc;
}
static int commit(Intent *j)
{
    Node n;int rc=decode(&s.g,j->inode,j->new_inode,&n);
    if(rc==IFS_FS_OK){rc=hash_data(&s.h,&n,&j->digest);if(rc!=IFS_FS_OK)return rc;}
    else if(rc!=IFS_FS_NOT_FOUND)return rc;
    s.journal=2u;
    rc=write_journal(&s.h,&s.g,j);
    if(rc==IFS_FS_OK)rc=replay(&s.h,&s.g,j);
    if(rc==IFS_FS_OK)rc=refresh();
    if(rc==IFS_FS_OK)s.journal=1u;
    return rc;
}
static int probe_impl(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{
    if(!variant)return IFS_FS_INVALID;
    *variant=0;
    uint8_t raw[B];Geometry g;Intent j;unsigned state;
    int rc=geometry(h,raw,&g);
    if(rc==IFS_FS_OK)rc=decode_journal(raw,&g,&j,&state);
    if(rc==IFS_FS_OK)*variant=1u;
    return rc;
}
static int mount_impl(const IfsFsHostRuntimeV2 *h,void **v,IfsFsVolumeInfoV2 *o)
{
    if(!v || !o || o->structure_bytes<sizeof *o)return IFS_FS_INVALID;
    *v=NULL;
    if(s.token || s.generation==UINTPTR_MAX)return IFS_FS_BUSY;
    uint8_t raw[B];Geometry g;Intent j;unsigned state;
    int rc=geometry(h,raw,&g);if(rc!=IFS_FS_OK)return rc;
    rc=decode_journal(raw,&g,&j,&state);if(rc!=IFS_FS_OK)return rc;
    s.recovered=0u;
    if(state==2u) {
        if(!writable(h))return IFS_FS_ACCESS;
        if(h->logical_sector_bytes!=B)return IFS_FS_UNSUPPORTED;
        rc=replay(h,&g,&j);if(rc!=IFS_FS_OK)return rc;
        state=1u;s.recovered=1u;
    }
    zero(&s.h,sizeof s.h);
    size_t bytes=h->structure_bytes<sizeof s.h?h->structure_bytes:sizeof s.h;
    copy(&s.h,h,bytes);s.h.structure_bytes=(uint32_t)bytes;
    s.g=g;s.journal=state;
    rc=refresh();if(rc!=IFS_FS_OK){zero(&s.h,sizeof s.h);return rc;}
    s.token=++s.generation;info(o);*v=(void *)s.token;return IFS_FS_OK;
}
static int unmount_impl(void *v)
{
    if(!live(v))return IFS_FS_INVALID;
    drop_plan();s.token=0;zero(&s.h,sizeof s.h);return IFS_FS_OK;
}
static int usable(void *v)
{
    if(!live(v))return IFS_FS_INVALID;
    return s.journal==2u?IFS_FS_IO_ERROR:IFS_FS_OK;
}
static int mutate(void *v,int plan)
{
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    if(!writable(&s.h))return IFS_FS_ACCESS;
    if(s.h.logical_sector_bytes!=B)return IFS_FS_UNSUPPORTED;
    if(s.journal!=1u)return IFS_FS_UNSUPPORTED;
    if(s.plan && !plan)return IFS_FS_BUSY;
    return IFS_FS_OK;
}
static int entry(const Node *n,const uint8_t *name,uint32_t len,IfsFsEntryV2 *o)
{
    if(!o || o->structure_bytes<sizeof *o)return IFS_FS_INVALID;
    o->name_bytes=0u;
    if(!o->name || o->name_capacity<=len)return IFS_FS_NO_SPACE;
    copy(o->name,name,len);o->name[len]=0;
    o->name_bytes=len;o->type=n->type==2u?IFS_FS_ENTRY_DIRECTORY:IFS_FS_ENTRY_FILE;
    o->size_bytes=n->size;o->attributes=0;return IFS_FS_OK;
}
static int stat_impl(void *v,const char *path,IfsFsEntryV2 *o)
{
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    Node n;uint8_t raw[I];rc=lookup(&s.h,&s.g,&s.root,path,&n,NULL,raw);
    if(rc!=IFS_FS_OK)return rc;
    const uint8_t *name=(const uint8_t *)path;uint32_t len=1;
    if(!root_path(path)){rc=path_name(path,&name,&len);if(rc!=IFS_FS_OK)return rc;}
    return entry(&n,name,len,o);
}
static int list_impl(void *v,const char *path,uint32_t nth,IfsFsEntryV2 *o)
{
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    if(!root_path(path))return IFS_FS_INVALID;
    for(uint32_t j=0;j<s.root.size/D;++j) {
        uint8_t e[D],raw[I];rc=rd(&s.h,dir_at(&s.root,j),e,D);
        if(rc!=IFS_FS_OK)return rc;
        uint32_t ino=get16(e);
        if(!ino || ino==ROOT)continue;
        if(nth--)continue;
        Node n;rc=node_read(&s.h,&s.g,ino,&n,raw);
        return rc==IFS_FS_OK?entry(&n,e+2,namelen(e+2,MAXNAME),o):rc;
    }
    return IFS_FS_END;
}
static int read_impl(void *v,const char *path,uint64_t at,void *p,uint32_t n)
{
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    Node f;uint8_t raw[I];rc=lookup(&s.h,&s.g,&s.root,path,&f,NULL,raw);
    if(rc!=IFS_FS_OK)return rc;
    if(f.type!=1u || (!p && n) || at>f.size || n>f.size-at)return IFS_FS_INVALID;
    return n?rd(&s.h,(uint64_t)f.first*B+at,p,n):IFS_FS_OK;
}
static uint32_t timestamp(void)
{
    uint64_t t=s.h.realtime_ns?s.h.realtime_ns(s.h.context)/UINT64_C(1000000000):0u;
    return t>UINT32_MAX?UINT32_MAX:(uint32_t)t;
}
static void new_extent(uint8_t raw[I],uint32_t start,uint32_t size)
{
    put32(raw+4,start);
    put32(raw+8,size?start+(size-1u)/B:0u);
    put32(raw+12,size?(uint32_t)((uint64_t)start*B+size-1u):NONE);
    put32(raw+40,timestamp());put32(raw+44,timestamp());
}
static int create_impl(void *v,const char *path,uint32_t type)
{
    int rc=mutate(v,0);if(rc!=IFS_FS_OK)return rc;
    if(type!=IFS_FS_ENTRY_FILE)return IFS_FS_UNSUPPORTED;
    const uint8_t *name;uint32_t len;
    rc=path_name(path,&name,&len);if(rc!=IFS_FS_OK)return rc;
    Node f;uint8_t raw[I];
    rc=lookup(&s.h,&s.g,&s.root,path,&f,NULL,raw);
    if(rc==IFS_FS_OK)return IFS_FS_EXISTS;
    if(rc!=IFS_FS_NOT_FOUND)return rc;
    Intent j={0};j.slot=NONE;
    for(uint32_t i=ROOT+1u;i<=s.g.maxino;++i) {
        rc=rd(&s.h,inode_at(i),j.old_inode,I);if(rc!=IFS_FS_OK)return rc;
        if(!get16(j.old_inode)){j.inode=i;break;}
    }
    if(!j.inode)return IFS_FS_NO_SPACE;
    for(uint32_t i=0;i<s.root.size/D;++i) {
        rc=rd(&s.h,dir_at(&s.root,i),j.old_dir,D);if(rc!=IFS_FS_OK)return rc;
        if(!get16(j.old_dir)){j.slot=i;break;}
    }
    if(j.slot==NONE)return IFS_FS_NO_SPACE;
    put16(j.new_inode,j.inode);put32(j.new_inode+16,1u);
    put32(j.new_inode+20,0100644u);put32(j.new_inode+32,1u);
    new_extent(j.new_inode,0,0);
    put16(j.new_dir,j.inode);copy(j.new_dir+2,name,len);
    return commit(&j);
}
static int remove_impl(void *v,const char *path)
{
    int rc=mutate(v,0);if(rc!=IFS_FS_OK)return rc;
    if(root_path(path))return IFS_FS_ACCESS;
    Intent j={0};Node f;
    rc=lookup(&s.h,&s.g,&s.root,path,&f,&j.slot,j.old_inode);
    if(rc!=IFS_FS_OK)return rc;
    j.inode=f.ino;
    rc=rd(&s.h,dir_at(&s.root,j.slot),j.old_dir,D);if(rc!=IFS_FS_OK)return rc;
    if(f.links>1u){copy(j.new_inode,j.old_inode,I);put32(j.new_inode+32,f.links-1u);}
    return commit(&j);
}
static int rename_impl(void *v,const char *oldpath,const char *newpath)
{
    int rc=mutate(v,0);if(rc!=IFS_FS_OK)return rc;
    if(root_path(oldpath))return IFS_FS_ACCESS;
    const uint8_t *name;uint32_t len;
    rc=path_name(newpath,&name,&len);if(rc!=IFS_FS_OK)return rc;
    Node f;uint8_t raw[I];rc=lookup(&s.h,&s.g,&s.root,newpath,&f,NULL,raw);
    if(rc==IFS_FS_OK)return IFS_FS_EXISTS;
    if(rc!=IFS_FS_NOT_FOUND)return rc;
    Intent j={0};
    rc=lookup(&s.h,&s.g,&s.root,oldpath,&f,&j.slot,j.old_inode);
    if(rc!=IFS_FS_OK)return rc;
    j.inode=f.ino;copy(j.new_inode,j.old_inode,I);
    rc=rd(&s.h,dir_at(&s.root,j.slot),j.old_dir,D);if(rc!=IFS_FS_OK)return rc;
    put16(j.new_dir,j.inode);copy(j.new_dir+2,name,len);
    return commit(&j);
}
static int load_map(uint8_t **map)
{
    Node *nodes=s.h.allocate(s.h.context,(MAXINO+1u)*sizeof *nodes);
    if(!nodes)return IFS_FS_NO_MEMORY;
    uint64_t free_count=0;
    int rc=scan(&s.h,&s.g,nodes,map,&free_count);
    s.h.release(s.h.context,nodes);return rc;
}
static int copy_payload(const Node *f,uint32_t target)
{
    uint8_t data[4096],verify[4096];uint64_t at=0;
    while(at<f->size) {
        if(cancelled(&s.h))return IFS_FS_BUSY;
        uint32_t take=f->size-at<sizeof data?(uint32_t)(f->size-at):sizeof data;
        int rc=rd(&s.h,(uint64_t)f->first*B+at,data,take);
        if(rc==IFS_FS_OK)rc=wr(&s.h,(uint64_t)target*B+at,data,take);
        if(rc==IFS_FS_OK)rc=rd(&s.h,(uint64_t)target*B+at,verify,take);
        if(rc!=IFS_FS_OK)return rc;
        if(!eq(data,verify,take))return IFS_FS_CORRUPT;
        at+=take;
    }
    return IFS_FS_OK;
}
/* Ordinary overwrite uses copy-on-write. Appending into the existing
 * extent is safe only when every newly required block is unowned.
 * The old logical payload is never overwritten before journal commit. */
static int write_impl(void *v,const char *path,uint64_t at,const void *p,
                      uint32_t n,int zeros)
{
    int rc=mutate(v,0);if(rc!=IFS_FS_OK)return rc;
    Node f;Intent j={0};j.slot=NONE;
    rc=lookup(&s.h,&s.g,&s.root,path,&f,NULL,j.old_inode);
    if(rc!=IFS_FS_OK)return rc;
    if(f.type!=1u || (!p && n && !zeros) || at>f.size || at>UINT32_MAX-n)
        return IFS_FS_INVALID;
    if(!n)return IFS_FS_OK;
    uint32_t end=(uint32_t)(at+n),size=end>f.size?end:f.size;
    uint32_t count=(size-1u)/B+1u,target=0u;
    uint8_t *map=NULL;rc=load_map(&map);if(rc!=IFS_FS_OK)return rc;
    const uint32_t oldcount=blocks(&f);
    if(at==f.size && f.first && count>=oldcount &&
       (count==oldcount || range_free(&s.g,map,f.first+oldcount,count-oldcount)))
        target=f.first;
    else target=find_run(&s.g,map,s.g.data,count);
    s.h.release(s.h.context,map);
    if(!target)return IFS_FS_NO_SPACE;
    if((uint64_t)target*B+size-1u>UINT32_MAX)return IFS_FS_NO_SPACE;
    if(f.first && target!=f.first){rc=copy_payload(&f,target);if(rc!=IFS_FS_OK)return rc;}
    if(zeros) {
        uint8_t buf[4096]={0};uint32_t left=n;uint64_t pos=at;
        while(left) {
            if(cancelled(&s.h))return IFS_FS_BUSY;
            uint32_t take=left<sizeof buf?left:sizeof buf;
            rc=wr(&s.h,(uint64_t)target*B+pos,buf,take);
            if(rc!=IFS_FS_OK)return rc;
            left-=take;pos+=take;
        }
    } else {
        rc=wr(&s.h,(uint64_t)target*B+at,p,n);if(rc!=IFS_FS_OK)return rc;
    }
    rc=barrier(&s.h);if(rc!=IFS_FS_OK)return rc;
    j.inode=f.ino;copy(j.new_inode,j.old_inode,I);new_extent(j.new_inode,target,size);
    return commit(&j);
}
static int truncate_impl(void *v,const char *path,uint64_t bytes)
{
    int rc=mutate(v,0);if(rc!=IFS_FS_OK)return rc;
    if(bytes>UINT32_MAX)return IFS_FS_INVALID;
    Node f;Intent j={0};j.slot=NONE;
    rc=lookup(&s.h,&s.g,&s.root,path,&f,NULL,j.old_inode);
    if(rc!=IFS_FS_OK)return rc;
    if(f.type!=1u)return IFS_FS_INVALID;
    if(bytes==f.size)return IFS_FS_OK;
    if(bytes>f.size)return write_impl(v,path,f.size,NULL,(uint32_t)(bytes-f.size),1);
    j.inode=f.ino;copy(j.new_inode,j.old_inode,I);
    new_extent(j.new_inode,bytes?f.first:0u,(uint32_t)bytes);
    return commit(&j);
}
static int map_impl(void *v,const char *path,uint64_t first,IfsFsExtentV2 *out,
                    uint32_t cap,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0;
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    if(!out || !cap)return IFS_FS_INVALID;
    Node f;uint8_t raw[I];rc=lookup(&s.h,&s.g,&s.root,path,&f,NULL,raw);
    if(rc!=IFS_FS_OK)return rc;
    if(first || !f.size)return IFS_FS_END;
    *out=(IfsFsExtentV2){.physical_offset=(uint64_t)f.first*B,.length=f.size};
    *written=1u;return IFS_FS_OK;
}
/* BFS has no separate free-space bitmap: its native allocator validates
 * the complete inode table and namespace, and builds the exact ownership
 * map from the format's contiguous inode extents. Reuse that same checked
 * inventory rather than assuming holes between visible regular files. */
static int allocation_impl(void *v,uint64_t first,IfsFsAllocationRangeV2 *out,
                           uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    if(!out || !capacity || capacity>64u || first>s.h.volume_bytes)
        return IFS_FS_INVALID;
    Node *nodes=s.h.allocate(s.h.context,(MAXINO+1u)*sizeof *nodes);
    if(!nodes)return IFS_FS_NO_MEMORY;
    uint8_t *bitmap=NULL;uint64_t free_blocks=0u;
    rc=scan(&s.h,&s.g,nodes,&bitmap,&free_blocks);
    if(rc!=IFS_FS_OK)goto done;
    if(free_blocks!=s.free_blocks){rc=IFS_FS_CORRUPT;goto done;}
    if(first==s.h.volume_bytes){rc=IFS_FS_END;goto done;}
    IfsFsAllocationRangeV2 page[64]={{0}};
    uint64_t at=first;uint32_t count=0u,scanned=0u;
    while(at<s.h.volume_bytes && scanned<65536u) {
        if(cancelled(&s.h)){rc=IFS_FS_BUSY;goto done;}
        uint64_t block=at/B,end;uint32_t state;
        if(block<s.g.data){state=IFS_FS_SPACE_RESERVED;end=(uint64_t)s.g.data*B;}
        else if(block>=s.g.total){state=IFS_FS_SPACE_RESERVED;end=s.h.volume_bytes;}
        else {
            state=bit(bitmap,(uint32_t)block)?IFS_FS_SPACE_ALLOCATED:IFS_FS_SPACE_FREE;
            end=(block+1u)*B;
        }
        if(count && page[count-1u].state==state)page[count-1u].length+=end-at;
        else {
            if(count==capacity)break;
            page[count++]=(IfsFsAllocationRangeV2){
                .physical_offset=at,.length=end-at,.state=state};
        }
        at=end;++scanned;
    }
    copy(out,page,count*sizeof *page);*written=count;rc=IFS_FS_OK;
done:
    if(bitmap)s.h.release(s.h.context,bitmap);
    s.h.release(s.h.context,nodes);
    return rc;
}
static int allocation(void *v,uint64_t first,IfsFsAllocationRangeV2 *out,
                      uint32_t capacity,uint32_t *written)
{
    if(written)*written=0u;
    if(!enter())return IFS_FS_BUSY;
    return leave(allocation_impl(v,first,out,capacity,written));
}
static int plan_impl(void *v,const char *path,uint32_t policy,void **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    if(!path || (policy!=0u && policy!=10u))return IFS_FS_INVALID;
    if(s.journal!=1u)return IFS_FS_UNSUPPORTED;
    if(s.plan || s.plan_generation==UINTPTR_MAX)return IFS_FS_BUSY;
    if(s.root.first!=s.g.data)return IFS_FS_UNSUPPORTED;
    Node *nodes=s.h.allocate(s.h.context,(MAXINO+1u)*sizeof *nodes);
    Plan *p=s.h.allocate(s.h.context,sizeof *p);uint8_t *map=NULL;
    if(!nodes || !p){rc=IFS_FS_NO_MEMORY;goto done;}
    zero(p,sizeof *p);p->policy=policy;
    uint64_t free_count;
    rc=scan(&s.h,&s.g,nodes,&map,&free_count);if(rc!=IFS_FS_OK)goto done;
    uint32_t selected=0u;
    if(!root_path(path)) {
        Node n;uint8_t raw[I];rc=lookup(&s.h,&s.g,&s.root,path,&n,NULL,raw);
        if(rc!=IFS_FS_OK)goto done;
        if(n.type!=1u){rc=IFS_FS_INVALID;goto done;}
        selected=n.ino;
    }
    uint32_t order[MAXINO],nfiles=0;
    for(uint32_t ino=ROOT+1u;ino<=s.g.maxino;++ino) {
        Node *n=&nodes[ino];
        if(!n->ino || !n->size || (selected && selected!=ino))continue;
        if(blocks(n)!=(n->size-1u)/B+1u){rc=IFS_FS_UNSUPPORTED;goto done;}
        uint32_t j=nfiles++;
        while(j && nodes[order[j-1u]].first>n->first){order[j]=order[j-1u];--j;}
        order[j]=ino;
    }
    uint32_t cursor=s.root.last+1u;
    for(uint32_t i=0;i<nfiles;++i) {
        if(cancelled(&s.h)){rc=IFS_FS_BUSY;goto done;}
        Node *n=&nodes[order[i]];uint32_t count=blocks(n),target=n->first;
        uint32_t reserve=policy?(count+9u)/10u:0u;
        if(policy) {
            target=find_run(&s.g,map,cursor,count+reserve);
            if(!target){rc=IFS_FS_NO_SPACE;goto done;}
            p->step[p->steps++]=(Step){n->ino,n->first,target,count};
            range_set(map,n->first,count,0);range_set(map,target,count+reserve,1);
            cursor=target+count+reserve;
        } else if(!selected) {
            target=cursor;
            if(target>n->first || count>s.g.total-target){rc=IFS_FS_CORRUPT;goto done;}
            if(target!=n->first) {
                if(!range_free(&s.g,map,target,count)) {
                    /* A small preceding hole overlaps the source. Stage
                     * through a fully free extent, then move into the
                     * now free compacted target. Every step is durable. */
                    uint32_t scratch=find_run(&s.g,map,n->last+1u,count);
                    if(!scratch){rc=IFS_FS_NO_SPACE;goto done;}
                    p->step[p->steps++]=(Step){n->ino,n->first,scratch,count};
                    range_set(map,n->first,count,0);range_set(map,scratch,count,1);
                    if(!range_free(&s.g,map,target,count)){rc=IFS_FS_CORRUPT;goto done;}
                    p->step[p->steps++]=(Step){n->ino,scratch,target,count};
                    range_set(map,scratch,count,0);range_set(map,target,count,1);
                } else {
                    p->step[p->steps++]=(Step){n->ino,n->first,target,count};
                    range_set(map,n->first,count,0);range_set(map,target,count,1);
                }
            }
            cursor=target+count;
        }
        /* A single ordinary file is intrinsically contiguous. Whole
         * volume ordinary policy is what eliminates inter-file gaps. */
        p->expected[p->files++]=(Expected){n->ino,target,count,n->size,reserve};
    }
    s.plan=p;s.plan_token=++s.plan_generation;
    *out=(void *)s.plan_token;p=NULL;rc=IFS_FS_OK;
done:
    if(map)s.h.release(s.h.context,map);
    if(nodes)s.h.release(s.h.context,nodes);
    if(p)s.h.release(s.h.context,p);
    return rc;
}
static Plan *getplan(void *v,void *p)
{ return live(v) && p && (uintptr_t)p==s.plan_token?s.plan:NULL; }
static int execute_impl(void *v,void *token,uint64_t *moved)
{
    if(!moved)return IFS_FS_INVALID;
    *moved=0;
    int rc=mutate(v,1);if(rc!=IFS_FS_OK)return rc;
    Plan *p=getplan(v,token);if(!p || p->executed)return IFS_FS_INVALID;
    for(uint32_t i=0;i<p->steps;++i) {
        if(cancelled(&s.h))return IFS_FS_BUSY;
        const Step *step=&p->step[i];Node f;Intent j={0};j.slot=NONE;
        rc=node_read(&s.h,&s.g,step->ino,&f,j.old_inode);
        if(rc!=IFS_FS_OK)return rc;
        if(f.first!=step->from || blocks(&f)!=step->count)return IFS_FS_CORRUPT;
        uint8_t *map=NULL;rc=load_map(&map);if(rc!=IFS_FS_OK)return rc;
        int free=range_free(&s.g,map,step->to,step->count);
        s.h.release(s.h.context,map);
        if(!free)return IFS_FS_CORRUPT;
        rc=copy_payload(&f,step->to);
        if(rc==IFS_FS_OK)rc=barrier(&s.h);
        if(rc!=IFS_FS_OK)return rc;
        j.inode=f.ino;copy(j.new_inode,j.old_inode,I);
        new_extent(j.new_inode,step->to,f.size);
        rc=commit(&j);if(rc!=IFS_FS_OK)return rc;
        *moved+=f.size;
    }
    p->executed=1u;return IFS_FS_OK;
}
static int verify_impl(void *v,void *token)
{
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    Plan *p=getplan(v,token);if(!p || !p->executed)return IFS_FS_INVALID;
    uint8_t *map=NULL;rc=load_map(&map);if(rc!=IFS_FS_OK)return rc;
    for(uint32_t i=0;i<p->files;++i) {
        const Expected *e=&p->expected[i];Node f;uint8_t raw[I];
        rc=node_read(&s.h,&s.g,e->ino,&f,raw);
        if(rc!=IFS_FS_OK)break;
        if(f.first!=e->target || f.size!=e->size || blocks(&f)!=e->count ||
           (e->reserve && !range_free(&s.g,map,e->target+e->count,e->reserve))){
            rc=IFS_FS_CORRUPT;break;
        }
    }
    s.h.release(s.h.context,map);return rc;
}
static int recover_impl(void *v,uint32_t flags,uint64_t *repaired)
{
    if(!repaired)return IFS_FS_INVALID;
    *repaired=0;
    if(!live(v) || flags)return IFS_FS_INVALID;
    if(!writable(&s.h))return IFS_FS_ACCESS;
    if(s.h.logical_sector_bytes!=B)return IFS_FS_UNSUPPORTED;
    if(s.plan)return IFS_FS_BUSY;
    uint8_t raw[B];Geometry g;Intent j;unsigned state;
    int rc=geometry(&s.h,raw,&g);if(rc!=IFS_FS_OK)return rc;
    rc=decode_journal(raw,&g,&j,&state);if(rc!=IFS_FS_OK)return rc;
    if(!state)return IFS_FS_UNSUPPORTED;
    if(state==2u) {
        rc=replay(&s.h,&g,&j);if(rc!=IFS_FS_OK)return rc;
        ++s.recovered;
    }
    rc=refresh();if(rc!=IFS_FS_OK)return rc;
    s.journal=1u;*repaired=s.recovered;s.recovered=0;return IFS_FS_OK;
}
static int query_impl(void *v,IfsFsVolumeInfoV2 *o)
{
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    if(!o || o->structure_bytes<sizeof *o)return IFS_FS_INVALID;
    info(o);return IFS_FS_OK;
}
static int label_impl(void *v,char *label,uint32_t cap,uint32_t *n)
{
    if(!n)return IFS_FS_INVALID;
    *n=0;
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    uint8_t raw[B];rc=rd(&s.h,0,raw,B);if(rc!=IFS_FS_OK)return rc;
    uint32_t len=namelen(raw+34,6u);
    if(!label || cap<=len)return IFS_FS_NO_SPACE;
    copy(label,raw+34,len);label[len]=0;*n=len;return IFS_FS_OK;
}
static int check_impl(void *v,uint32_t flags,uint64_t *errors)
{
    if(!errors)return IFS_FS_INVALID;
    *errors=0;
    int rc=usable(v);if(rc!=IFS_FS_OK)return rc;
    if(flags)return IFS_FS_INVALID;
    uint8_t *map=NULL;rc=load_map(&map);
    if(map)s.h.release(s.h.context,map);
    if(rc==IFS_FS_CORRUPT){*errors=1u;return IFS_FS_OK;}
    return rc;
}
static int format_impl(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                       const char *label,uint32_t flags)
{
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(s.token)return IFS_FS_BUSY;
    if(!writable(h))return IFS_FS_ACCESS;
    if(h->logical_sector_bytes!=B)return IFS_FS_UNSUPPORTED;
    if((variant!=0u && variant!=1u) || flags)return IFS_FS_UNSUPPORTED;
    uint32_t len=0u;
    if(label){while(len<=6u && label[len])++len;if(len>6u)return IFS_FS_INVALID;}
    const uint32_t data=1u+NATIVE_INODES*I/B;
    if(h->volume_bytes%B || h->volume_bytes>UINT64_C(0x100000000) ||
       h->volume_bytes/B<=data+ROOT_BLOCKS)return IFS_FS_UNSUPPORTED;
    Geometry g={data,(uint32_t)(h->volume_bytes/B),MAXINO,h->volume_bytes};
    uint8_t raw[B]={0};
    int rc=durable(h,0,raw,B);if(rc!=IFS_FS_OK)return rc;
    for(uint32_t i=1u;i<data+ROOT_BLOCKS;++i) {
        if(cancelled(h))return IFS_FS_BUSY;
        zero(raw,B);
        if(i==1u) {
            put16(raw,ROOT);put32(raw+4,data);
            put32(raw+8,data+ROOT_BLOCKS-1u);
            put32(raw+12,(data+ROOT_BLOCKS)*B-1u);
            put32(raw+16,2u);put32(raw+20,0040755u);put32(raw+32,2u);
        } else if(i==data) {
            put16(raw,ROOT);raw[2]='.';
            put16(raw+D,ROOT);raw[D+2]='.';raw[D+3]='.';
        }
        rc=wr(h,(uint64_t)i*B,raw,B);if(rc!=IFS_FS_OK)return rc;
    }
    rc=barrier(h);if(rc!=IFS_FS_OK)return rc;
    zero(raw,B);put32(raw,MAGIC);put32(raw+4,data*B);
    put32(raw+8,(uint32_t)(g.bytes-1u));
    copy(raw+28,"BFS",3u);if(label)copy(raw+34,label,len);
    encode_journal(raw,&g,NULL);
    rc=durable(h,0,raw,B);if(rc!=IFS_FS_OK)return rc;
    uint32_t observed=0;
    return probe_impl(h,&observed)==IFS_FS_OK && observed==1u?IFS_FS_OK:IFS_FS_CORRUPT;
}

/* All exported callbacks share the same non-reentrant operation gate.
 * The Common host is the sole authority for bounded physical I/O. */
#define WRAP(name,args,call) static int name args {if(!enter())return IFS_FS_BUSY;return leave(call);}
WRAP(probe,(const IfsFsHostRuntimeV2 *h,uint32_t *n),probe_impl(h,n))
WRAP(mount,(const IfsFsHostRuntimeV2 *h,void **v,IfsFsVolumeInfoV2 *o),mount_impl(h,v,o))
WRAP(unmount,(void *v),unmount_impl(v))
WRAP(stat,(void *v,const char *p,IfsFsEntryV2 *o),stat_impl(v,p,o))
WRAP(list,(void *v,const char *p,uint32_t n,IfsFsEntryV2 *o),list_impl(v,p,n,o))
WRAP(read,(void *v,const char *p,uint64_t a,void *d,uint32_t n),read_impl(v,p,a,d,n))
WRAP(write,(void *v,const char *p,uint64_t a,const void *d,uint32_t n),write_impl(v,p,a,d,n,0))
WRAP(create,(void *v,const char *p,uint32_t t),create_impl(v,p,t))
WRAP(remove_file,(void *v,const char *p),remove_impl(v,p))
WRAP(rename_file,(void *v,const char *a,const char *b),rename_impl(v,a,b))
WRAP(truncate_file,(void *v,const char *p,uint64_t n),truncate_impl(v,p,n))
WRAP(query,(void *v,IfsFsVolumeInfoV2 *o),query_impl(v,o))
WRAP(label,(void *v,char *p,uint32_t cap,uint32_t *n),label_impl(v,p,cap,n))
WRAP(check,(void *v,uint32_t flags,uint64_t *n),check_impl(v,flags,n))
WRAP(format,(const IfsFsHostRuntimeV2 *h,uint32_t v,const char *p,uint32_t f),format_impl(h,v,p,f))
WRAP(map,(void *v,const char *p,uint64_t f,IfsFsExtentV2 *o,uint32_t c,uint32_t *n),map_impl(v,p,f,o,c,n))
WRAP(plan,(void *v,const char *p,uint32_t policy,void **o),plan_impl(v,p,policy,o))
WRAP(execute,(void *v,void *p,uint64_t *n),execute_impl(v,p,n))
WRAP(verify,(void *v,void *p),verify_impl(v,p))
WRAP(recover,(void *v,uint32_t flags,uint64_t *n),recover_impl(v,flags,n))
static int sync_volume(void *v)
{
    if(!enter())return IFS_FS_BUSY;
    int rc=usable(v);
    if(rc==IFS_FS_OK && writable(&s.h))rc=barrier(&s.h);
    return leave(rc);
}
static void release_plan(void *v,void *p)
{
    if(!enter())return;
    if(getplan(v,p))drop_plan();
    (void)leave(IFS_FS_OK);
}
static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),IFS_FS_GROUP_CORE,0,IFS_FS_CORE_ALL},
    .probe=probe,.mount=mount,.unmount=unmount,.stat=stat,.directory_entry=list,
    .read=read,.write=write,.create=create,.remove=remove_file,.rename=rename_file,.truncate=truncate_file};
static const IfsFsVolumeOpsV2 volume={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),IFS_FS_GROUP_VOLUME,0,
        IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET|IFS_FS_VOLUME_SYNC|
        IFS_FS_VOLUME_FORMAT|IFS_FS_VOLUME_CHECK},
    .query=query,.label=label,.sync=sync_volume,.format=format,.check=check};
static const IfsFsAllocationOpsV2 allocation_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsAllocationOpsV2),IFS_FS_GROUP_ALLOCATION,0,IFS_FS_ALLOCATION_MAP},
    .map=allocation};
static const IfsFsDefragOpsV2 defrag={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsDefragOpsV2),IFS_FS_GROUP_DEFRAG,0,IFS_FS_DEFRAG_ALL},
    .map=map,.plan=plan,.execute=execute,.verify=verify,.recover=recover,.release_plan=release_plan};
static const IfsFsGroupHeaderV2 *group(uint32_t id,uint32_t min,uint32_t max)
{
    if(min>IFS_FS_GROUP_ABI_V2 || max<IFS_FS_GROUP_ABI_V2)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume.header;
    if(id==IFS_FS_GROUP_DEFRAG)return &defrag.header;
    if(id==IFS_FS_GROUP_ALLOCATION)return &allocation_ops.header;
    return NULL;
}
__attribute__((used,section(".ifsmeta")))static const char identity[]="IFS-MODULE:bfs:common2";
static const IfsFsModuleApiV2 api={
    .abi_version=IFS_FS_COMMON_ABI_V2,.structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="bfs",.runtime_profiles=IFS_FS_PROFILE_HOSTED|IFS_FS_PROFILE_FREESTANDING,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION),.query_group=group};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
