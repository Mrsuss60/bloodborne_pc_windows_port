/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define ftruncate(fd, size) _chsize_s(fd, size)
#else
#include <ftw.h>
#endif

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define SLOTS 16

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");

static HostMutex lock = HOST_MUTEX_INIT;
static int initialized;
static char title_id[16]="UNKNOWN";
static struct { int used; char host[700], meta[700]; } slots[SLOTS];
static char memory_path[720];
static uint64_t memory_size;
static size_t mounts_done, memory_writes;

/* Bloodborne "sound hack" (rainvmaker, from the Diegolix29 shadPS4 fork): the game data
 * save carries a flag at 0x204E of userdata0010; with it clear, parts of the game's audio
 * (e.g. the player's weapon sounds) never play. It is set before the game reads the save.
 * BB_SOUND_HACK=0 leaves saves untouched. */
static void bloodborne_sound_hack(void) {
    static const char *const ids[]={"CUSA00207","CUSA00208","CUSA00299","CUSA00900","CUSA01363","CUSA03014","CUSA03023","CUSA03173"};
    const char *env=getenv("BB_SOUND_HACK");
    if (env && env[0]=='0') return;
    int bloodborne=0;
    for (size_t i=0;i<sizeof(ids)/sizeof(*ids);++i) if (!strcmp(title_id,ids[i])) bloodborne=1;
    if (!bloodborne) return;
    char users[700];
    snprintf(users,sizeof(users),"%s/savedata",runtime_file_user_dir());
    DIR *dir=opendir(users);
    if (!dir) return;
    for (struct dirent *e; (e=readdir(dir));) {
        if (e->d_name[0]=='.') continue;
        char path[1100];
        snprintf(path,sizeof(path),"%s/%s/%s/SPRJ0005/userdata0010",users,e->d_name,title_id);
        FILE *f=fopen(path,"r+b");
        if (!f) continue;
        if (!fseek(f,0x204e,SEEK_SET)) {
            unsigned char byte=0;
            if (fread(&byte,1,1,f)==1 && !(byte & 1)) {
                byte|=1;
                fseek(f,0x204e,SEEK_SET);
                if (fwrite(&byte,1,1,f)==1) printf("Runtime: applied Bloodborne sound hack to %s\n",path);
            }
        }
        fclose(f);
    }
    closedir(dir);
}

void runtime_savedata_configure(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
    bloodborne_sound_hack();
}

static int make_dirs(const char *path) {
    char buf[1024]; snprintf(buf,sizeof(buf),"%s",path);
    for (char *p=buf+1;*p;++p) if (*p=='/' || *p=='\\') {
        char old = *p;
        *p=0;
#ifdef _WIN32
        runtime_win_mkdir(buf, 0777);
#else
        mkdir(buf,0777);
#endif
        *p=old;
    }
#ifdef _WIN32
    return (runtime_win_mkdir(buf, 0777) && errno!=EEXIST) ? -1 : 0;
#else
    return (mkdir(buf,0777) && errno!=EEXIST) ? -1 : 0;
#endif
}

static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out,size,"%s/savedata/%d/%s",runtime_file_user_dir(),user,title && *title ? title : title_id);
}

static void target_paths(int32_t user, const char *title, const DirName *dir, char *host, char *meta, size_t size) {
    char base[700]; root(user,title,base,sizeof(base));
    snprintf(host,size,"%s/%s",base,dir->data);
    snprintf(meta,size,"%s/%s.sce_sys",base,dir->data);
}

static int write_param(const char *meta, const Param *p) {
    if (make_dirs(meta)) return -1;
    char path[800]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    FILE *f=fopen(path,"wb");
    if (!f) return -1;
    size_t n=fwrite(p,sizeof(*p),1,f);
    fclose(f);
    return n==1 ? 0 : -1;
}

static int read_param(const char *meta, Param *p) {
    memset(p,0,sizeof(*p));
    char path[800]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f);
    fclose(f);
    struct stat st;
    if (!stat(path,&st)) p->mtime=st.st_mtime;
    return n==1 ? 0 : -1;
}

#ifndef _WIN32
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}
#endif

static ABI int32_t save_initialize(const void *param) { (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    host_lock(&lock);
    for (int i=0;i<SLOTS;++i) if (slots[i].used) {
        char point[16]; snprintf(point,sizeof(point),"/savedata%d",i);
        runtime_file_unmount(point);
        slots[i].used=0;
    }
    initialized=0;
    host_unlock(&lock);
    return 0;
}

static int32_t mount(int32_t user, const char *title, const DirName *dir, uint32_t mode, MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0 || !dir || !result) return ERR_PARAMETER;
    char host[700], meta[700];
    target_paths(user,title,dir,host,meta,sizeof(host));
    struct stat st;
    int exists=!stat(host,&st) && S_ISDIR(st.st_mode);
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    host_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { host_unlock(&lock); return ERR_BAD_MOUNTED; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { host_unlock(&lock); return ERR_MOUNT_FULL; }
    if (!exists && (make_dirs(host) || make_dirs(meta))) { host_unlock(&lock); return ERR_INTERNAL; }
    if (!exists) { Param empty={0}; write_param(meta,&empty); }
    slots[slot].used=1;
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    snprintf(result->point.data,sizeof(result->point.data),"/savedata%d",slot);
    result->required_blocks=0;
    result->status=exists ? 0 : 1; /* CREATED */
    runtime_file_mount(result->point.data,host);
    ++mounts_done;
    host_unlock(&lock);

    static char s_last_mounted[700] = {0};
    if (strcmp(s_last_mounted, host) != 0) {
        snprintf(s_last_mounted, sizeof(s_last_mounted), "%s", host);
        printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
               exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    }
    return 0;
}

static ABI int32_t save_mount1(const Mount1 *m, MountResult *r) {
    return mount(m ? m->user : -1, m && m->title ? m->title->data : NULL, m ? m->dir : NULL, m ? m->mode : 0, r);
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *r) {
    return mount(m ? m->user : -1, NULL, m ? m->dir : NULL, m ? m->mode : 0, r);
}

static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    char *end=NULL; long n=strtol(point->data+9,&end,10);
    return end && !*end && n>=0 && n<SLOTS && slots[n].used ? (int)n : -1;
}

static ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    host_lock(&lock);
    int slot=slot_of(point);
    if (slot>=0) { runtime_file_unmount(point->data); slots[slot].used=0; }
    host_unlock(&lock);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}

static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    host_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { host_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: if (size<sizeof(p.title)) goto bad; memcpy(p.title,buffer,size); break;
    case 2: if (size<sizeof(p.subtitle)) goto bad; memcpy(p.subtitle,buffer,size); break;
    case 3: if (size<sizeof(p.detail)) goto bad; memcpy(p.detail,buffer,size); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    write_param(slots[slot].meta,&p);
    host_unlock(&lock);
    return 0;
bad:
    host_unlock(&lock);
    return ERR_PARAMETER;
}

static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    host_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char path[800]; snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        FILE *f=fopen(path,"wb");
        r=f && fwrite(icon->buffer,1,icon->data_size,f)==icon->data_size ? 0 : ERR_INTERNAL;
        if (f) fclose(f);
    }
    host_unlock(&lock);
    return r;
}

static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || d->user<0 || !d->dir) return ERR_PARAMETER;
    char base[700], host[700], meta[700];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    struct stat st;
    if (stat(host,&st)) return ERR_NOT_FOUND;
#ifdef _WIN32
    runtime_win_remove_tree(host);
    runtime_win_remove_tree(meta);
#else
    nftw(host,remove_entry,16,FTW_DEPTH|FTW_PHYS);
    nftw(meta,remove_entry,16,FTW_DEPTH|FTW_PHYS);
#endif
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}

static int name_matches(const char *pattern, const char *name) {
    if (!strcmp(pattern,"*")) return 1;
    size_t n=strlen(pattern);
    return pattern[n-1]=='*' ? !strncmp(pattern,name,n-1) : !strcmp(pattern,name);
}

typedef struct { DirName name; Param param; } Candidate;
static int compare_candidates(const void *a, const void *b) {
    const Candidate *ca=a, *cb=b;
    return ca->param.mtime<cb->param.mtime ? 1 : ca->param.mtime>cb->param.mtime ? -1 : 0;
}

static ABI int32_t save_search(const SearchCond *cond, SearchResult *res) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!cond || cond->user<0 || !cond->dir || !res) return ERR_PARAMETER;
    char base[700]; root(cond->user,cond->title ? cond->title->data : NULL,base,sizeof(base));
    DIR *dir=opendir(base);
    if (!dir) { res->hits=0; return 0; }
    Candidate list[128]; uint32_t total=0;
    for (struct dirent *e; (e=readdir(dir));) {
        if (e->d_name[0]=='.' || strstr(e->d_name,".sce_sys") || strstr(e->d_name,".memory")) continue;
        if (!name_matches(cond->dir->data,e->d_name)) continue;
        if (total<sizeof(list)/sizeof(*list)) {
            snprintf(list[total].name.data,sizeof(list[total].name.data),"%s",e->d_name);
            char meta[800]; snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,e->d_name);
            read_param(meta,&list[total].param);
        }
        ++total;
    }
    closedir(dir);
    uint32_t in_list=total<sizeof(list)/sizeof(*list) ? total : (uint32_t)(sizeof(list)/sizeof(*list));
    qsort(list,in_list,sizeof(Candidate),compare_candidates);
    uint32_t copy=in_list<res->names_capacity ? in_list : res->names_capacity;
    for (uint32_t i=0;i<copy;++i) {
        if (res->names) res->names[i]=list[i].name;
        if (res->params) res->params[i]=list[i].param;
        if (res->infos) res->infos[i]=(SearchInfo){.blocks=32768,.free_blocks=16384};
    }
    res->hits=total;
    return 0;
}

static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param *param) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0 || !size) return ERR_PARAMETER;
    char base[700], dir[750];
    root(user,NULL,base,sizeof(base));
    snprintf(dir,sizeof(dir),"%s.memory",base);
    if (make_dirs(dir)) return ERR_INTERNAL;
    host_lock(&lock);
    snprintf(memory_path,sizeof(memory_path),"%s/memory.dat",dir);
    FILE *f=fopen(memory_path,"r+b");
    if (!f) f=fopen(memory_path,"w+b");
    int32_t r=ERR_INTERNAL;
    if (f) {
        r=!ftruncate(fileno(f),(off_t)size) ? 0 : ERR_INTERNAL;
        memory_size=r ? 0 : size;
        fclose(f);
    }
    if (!r && param) write_param(dir,param);
    host_unlock(&lock);
    if (!r) printf("Runtime: save data memory ready (%llu bytes)\n",(unsigned long long)size);
    return r;
}

static int32_t memory_io(void *buffer, uint64_t size, int64_t offset, int write) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer || offset<0) return ERR_PARAMETER;
    host_lock(&lock);
    if (!memory_size) { host_unlock(&lock); return ERR_MEMORY_NOT_READY; }
    if ((uint64_t)offset+size>memory_size) { host_unlock(&lock); return ERR_PARAMETER; }
    FILE *f=fopen(memory_path,"r+b");
    int32_t r=ERR_INTERNAL;
    if (f && !fseek(f,offset,SEEK_SET) &&
        (write ? fwrite(buffer,1,size,f) : fread(buffer,1,size,f))==size) r=0;
    if (f) fclose(f);
    if (!r && write) ++memory_writes;
    host_unlock(&lock);
    return r;
}

static ABI int32_t memory_get(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(buffer,size,offset,0); }
static ABI int32_t memory_set(int32_t user, const void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io((void *)buffer,size,offset,1); }

static const RuntimeExport exports[]={
    /* Bloodborne #O#P NIDs (from import_names.inc / EBOOT) */
    {"ZkZhskCPXFw#O#P",save_initialize},
    {"yKDy8S5yLA0#O#P",save_terminate},
    {"32HQAQdwM2o#O#P",save_mount1},
    {"BMR4F-Uek3E#O#P",save_umount},
    {"85zul--eGXs#O#P",save_set_param},
    {"c88Yy54Mx0w#O#P",save_icon},
    {"S1GkePI17zQ#O#P",save_delete},
    {"dyIhnXq-0SM#O#P",save_search},
    {"v7AAAMo0Lz4#O#P",memory_setup},
    {"7Bt5pBC-Aco#O#P",memory_get},
    {"h3YURzXGSVQ#O#P",memory_set},

    /* Standard symbol names */
    {"sceSaveDataInitialize",save_initialize},
    {"sceSaveDataInitialize2",save_initialize},
    {"sceSaveDataInitialize3",save_initialize},
    {"sceSaveDataTerminate",save_terminate},
    {"sceSaveDataMount",save_mount1},
    {"sceSaveDataMount2",save_mount2},
    {"sceSaveDataUmount",save_umount},
    {"sceSaveDataSetParam",save_set_param},
    {"sceSaveDataSaveIcon",save_icon},
    {"sceSaveDataDelete",save_delete},
    {"sceSaveDataDirNameSearch",save_search},
    {"sceSaveDataSetupSaveDataMemory",memory_setup},
    {"sceSaveDataGetSaveDataMemory",memory_get},
    {"sceSaveDataSetSaveDataMemory",memory_set},
    {"sceSaveDataMemoryInit",memory_setup},
    {"sceSaveDataMemoryGetData",memory_get},
    {"sceSaveDataMemorySetData",memory_set},

    /* Legacy / alternate #p#J NIDs */
    {"Kz6960K8y5k#p#J",save_initialize},
    {"y807Z3XQv0U#p#J",save_terminate},
    {"8wK8R7w5-3I#p#J",save_mount1},
    {"Xk1J7F4j-Yg#p#J",save_mount2},
    {"h2nS5F1R+nI#p#J",save_umount},
    {"J+ZJ-XbV2r4#p#J",save_set_param},
    {"c2Q3q1N0x9Y#p#J",save_icon},
    {"q8K7Y1V-n2E#p#J",save_delete},
    {"k-r1L1R0r1E#p#J",save_search},
};

uintptr_t runtime_savedata_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu\n",mounts_done,memory_writes); }
