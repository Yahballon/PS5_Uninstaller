// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only inventory bridge for an existing PS5 elfldr. Contains no deletion,
// write, mount, install, kernel-access or network-listener functionality.
// stdout is sent back through the existing 127.0.0.1:9021 ELF loader channel.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif

#define MAX_RECORDS 550
#define MAX_VISITS 3200
#define MAX_DEPTH 2
static unsigned records=0, visits=0;
static char icon_dirs[MAX_RECORDS][900];

static int safe(const char *s) {
    if (!s||!*s||s[0]=='.'||strlen(s)>160 || !strcmp(s,".."))return 0;
    for(const unsigned char *p=(const unsigned char*)s;*p;p++)
        if(*p<32||*p==127||*p=='/'||*p==92)return 0;
    return 1;
}
static int stat_regular(const char *p,off_t *size) {
    struct stat st;
    if(lstat(p,&st)!=0||!S_ISREG(st.st_mode))return 0;
    if(size)*size=st.st_size;
    return 1;
}
static int stat_directory(const char *p) {
    struct stat st;
    return lstat(p,&st)==0&&S_ISDIR(st.st_mode);
}
static int package(const char *name) {
    const size_t n=strlen(name);
    return n>4&&(!strcmp(name+n-4,".pkg")||!strcmp(name+n-4,".PKG"));
}
static void status(const char *root,int code,unsigned visited,unsigned found) {
    printf("R %d %u %u %zu\n",code,visited,found,strlen(root));
    fwrite(root,1,strlen(root),stdout);
}
static int join(char *dst,size_t limit,const char *root,const char *name) {
    const int n=snprintf(dst,limit,"%s/%s",root,name);
    return n>0&&(size_t)n<limit;
}
// ITEM binary payload contains path, folder name, optional param.json bytes.
// Size is valid only for raw .pkg archives (kind 'P').
static void item(char kind,const char *path,const char *name,const char *meta,
                 size_t meta_len,unsigned long long size) {
    if(records>=MAX_RECORDS)return;
    const size_t pl=strlen(path), nl=strlen(name);
    if(pl==0||pl>900||nl==0||nl>160||meta_len>8192)return;
    printf("I %c %zu %zu %zu %llu\n",kind,pl,nl,meta_len,size);
    fwrite(path,1,pl,stdout);
    fwrite(name,1,nl,stdout);
    if(meta_len)fwrite(meta,1,meta_len,stdout);
    if(kind=='H'||kind=='G')
      snprintf(icon_dirs[records],sizeof(icon_dirs[records]),"%s",path);
    records++;
}
static int read_json(const char *path,char *dest,size_t limit,size_t *out){
    *out=0;
    FILE* f=fopen(path,"rb");
    if(!f)return 0;
    *out=fread(dest,1,limit,f);
    if(ferror(f)){fclose(f);*out=0;return 0;}
    fclose(f);
    return 1;
}
static int app_folder(const char *path,const char *folder,char kind){
    char metaPath[1050],elf[1050],sfo[1050];
    if(!join(metaPath,sizeof(metaPath),path,"sce_sys/param.json") ||
       !join(elf,sizeof(elf),path,"eboot.bin") ||
       !join(sfo,sizeof(sfo),path,"sce_sys/param.sfo"))return 0;
    const int hasJson=stat_regular(metaPath,NULL);
    if(!hasJson && !stat_regular(elf,NULL) && !stat_regular(sfo,NULL))return 0;
    char metadata[8192];size_t n=0;
    if(hasJson)read_json(metaPath,metadata,sizeof(metadata),&n);
    // Filesystem discovery only. Nothing here is an uninstall authorization.
    item(kind,path,folder,metadata,n,0);
    return 1;
}
static void walk(const char* path,char kind,unsigned depth,int pkgs,
                 unsigned *localVisits,unsigned *localFound){
    if(records>=MAX_RECORDS||visits>=MAX_VISITS)return;
    DIR* dir=opendir(path);
    if(!dir){status(path,errno,*localVisits,*localFound);return;}
    struct dirent* entry;
    errno=0;
    while((entry=readdir(dir)) && visits<MAX_VISITS&&records<MAX_RECORDS){
        const char* name=entry->d_name;
        if(!safe(name))continue;
        char child[1024];if(!join(child,sizeof(child),path,name))continue;
        visits++;(*localVisits)++;
        struct stat st;
        if(lstat(child,&st)!=0||S_ISLNK(st.st_mode))continue;
        if(S_ISREG(st.st_mode)){
            if(pkgs&&package(name)){
                item('P',child,name,NULL,0,(unsigned long long)st.st_size);
                (*localFound)++;
            }
            continue;
        }
        if(!S_ISDIR(st.st_mode))continue;
        if(!pkgs){
            const unsigned before=records;
            app_folder(child,name,kind);
            if(records>before){(*localFound)++;continue;}
        }
        if(depth>=MAX_DEPTH)continue;
        if(!strcmp(name,"sce_sys") || !strcmp(name,"sce_module") ||
           !strcmp(name,"system_data") || !strcmp(name,"user") ||
           !strcmp(name,"trophy") || !strcmp(name,"patch") ||
           (pkgs && (!strcmp(name,"homebrew")||!strcmp(name,"etaHEN"))))
            continue;
        walk(child,kind,depth+1,pkgs,localVisits,localFound);
    }
    const int error=errno;
    closedir(dir);
    if(error)status(path,error,*localVisits,*localFound);
}

/* Only the previously enumerated title IDs may request an icon.
   Requests contain a decimal item index, never a filesystem path. */
static int icon_file(unsigned idx,char *out,size_t cap){
    if(idx>=records || icon_dirs[idx][0]==0)return 0;
    const char *dir=icon_dirs[idx];
    if(join(out,cap,dir,"sce_sys/icon0.png")&&stat_regular(out,NULL))return 1;
    const char *slash=strrchr(dir,'/');
    if(!slash||!safe(slash+1))return 0;
    char tid[10]={0};
    const char *name=slash+1;
    if(strlen(name)<9)return 0;
    memcpy(tid,name,9);
    int valid=1;
    for(int i=0;i<4;i++)if(tid[i]<'A'||tid[i]>'Z')valid=0;
    for(int i=4;i<9;i++)if(tid[i]<'0'||tid[i]>'9')valid=0;
    if(!valid)return 0;
    const char *roots[]={"/user/appmeta","/system_data/priv/appmeta",
                          "/user/appmeta/external","/system_data/priv/appmeta/external"};
    for(unsigned k=0;k<sizeof(roots)/sizeof(roots[0]);k++){
       if(snprintf(out,cap,"%s/%s/icon0.png",roots[k],tid)>=(int)cap)continue;
       if(stat_regular(out,NULL))return 1;
    }
    return 0;
}
static int send_all(int fd,const void* buf,size_t n){
    const char *p=(const char*)buf;
    while(n){ssize_t k=send(fd,p,n,0);if(k<=0)return 0;p+=k;n-=(size_t)k;}
    return 1;
}
static int serve_icon(int fd){
    char cmd[32]={0};
    const ssize_t n=recv(fd,cmd,sizeof(cmd)-1,0);
    if(n>=4&&memcmp(cmd,"QUIT",4)==0)return 1;
    if(n<6||strncmp(cmd,"ICON ",5)!=0){send_all(fd,"MISS\n",5);return 0;}
    char *end=NULL;unsigned long idx=strtoul(cmd+5,&end,10);
    if(end==cmd+5||(*end!='\n'&&*end!='\r'&&*end!=0) || idx>=records){
       send_all(fd,"MISS\n",5);return 0;
    }
    char path[1100]={0};
    if(!icon_file((unsigned)idx,path,sizeof(path))){send_all(fd,"MISS\n",5);return 0;}
    struct stat st;
    if(lstat(path,&st)!=0||!S_ISREG(st.st_mode)||st.st_size<32||
       st.st_size>3*1024*1024){send_all(fd,"MISS\n",5);return 0;}
    FILE* f=fopen(path,"rb");
    if(!f){send_all(fd,"MISS\n",5);return 0;}
    unsigned char magic[8];
    if(fread(magic,1,8,f)!=8 ||
       memcmp(magic,"\x89PNG\r\n\x1a\n",8)!=0){
      fclose(f);send_all(fd,"MISS\n",5);return 0;
    }
    rewind(f);
    char header[64];
    int count=snprintf(header,sizeof(header),"DATA %lld\n",(long long)st.st_size);
    if(count<=0 || !send_all(fd,header,(size_t)count)){fclose(f);return 0;}
    char buf[8192];size_t got;
    while((got=fread(buf,1,sizeof(buf),f))>0)if(!send_all(fd,buf,got))break;
    fclose(f);
    return 0;
}
static void serve_icons(void){
    signal(SIGPIPE,SIG_IGN);
    int listenfd=socket(AF_INET,SOCK_STREAM,0);
    if(listenfd<0)return;
    int enabled=1;setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled));
    struct sockaddr_in addr={0};
    addr.sin_family=AF_INET;addr.sin_port=htons(9175);
    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(listenfd,(struct sockaddr*)&addr,sizeof(addr)) || listen(listenfd,4)){
       close(listenfd);return;
    }
    // Time-bounded; no persistent daemon, no writes, never arbitrary file reads.
    for(int ticks=0;ticks<600;ticks++){
        fd_set fds;FD_ZERO(&fds);FD_SET(listenfd,&fds);
        struct timeval tv={1,0};
        if(select(listenfd+1,&fds,NULL,NULL,&tv)<=0)continue;
        int client=accept(listenfd,NULL,NULL);
        if(client<0)continue;
        struct timeval deadline={2,0};
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&deadline,sizeof(deadline));
        setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&deadline,sizeof(deadline));
        int quit=serve_icon(client);close(client);
        if(quit)break;
    }
    close(listenfd);
}
static void storage_report(const char* path){
    struct statfs info;
    uint64_t total=0,free=0;int code=0;
    if(statfs(path,&info)!=0)code=errno;
    else if(info.f_blocks<=0 || info.f_bsize<=0)code=ERANGE;
    else {
       total=(uint64_t)info.f_blocks*(uint64_t)info.f_bsize;
       free=(uint64_t)info.f_bavail*(uint64_t)info.f_bsize;
       const uint64_t gib=1024ull*1024ull*1024ull;
       // A non-mounted directory such as /mnt/ext0 can report its tiny
       // underlying memory filesystem (seen as 2.0 MiB on real hardware).
       // Never claim this is SSD or M.2 capacity.
       const uint64_t min_size=!strcmp(path,"/user")?128*gib:
           !strncmp(path,"/mnt/ext",8)?8*gib:128ull*1024*1024;
       if(total>70ull*1024ull*1024ull*1024ull || free>total || total<min_size)
           code=ERANGE;
    }
    if(code){total=0;free=0;}
    printf("S %d %zu %llu %llu\n",code,strlen(path),
           (unsigned long long)total,(unsigned long long)free);
    fwrite(path,1,strlen(path),stdout);
}
static void scan_root(const char* path,char kind,int pkgs) {
    unsigned visited=0,found=0;
    DIR* d=opendir(path);
    if(!d){status(path,errno,0,0);return;}
    closedir(d);
    walk(path,kind,0,pkgs,&visited,&found);
    status(path,0,visited,found);
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    puts("UNSCN1");
    storage_report("/user");
    for(int i=0;i<4;i++){
       char drive[40];snprintf(drive,sizeof(drive),"/mnt/ext%d",i);
       storage_report(drive);
    }
    for(int i=0;i<8;i++){
       char drive[40];snprintf(drive,sizeof(drive),"/mnt/usb%d",i);
       storage_report(drive);
    }
    // Keep in sync with /homebrew/uninstaller/native/inventory.hpp.
    scan_root("/data/homebrew",'H',0);
    scan_root("/user/app",'G',0);
    scan_root("/system_ex/app",'G',0);
    scan_root("/data/etaHEN/games",'G',0);
    scan_root("/user/data/etaHEN/games",'G',0);
    scan_root("/data/games",'G',0);
    for(int i=0;i<8;i++){
        char p[96],base[40];snprintf(base,sizeof(base),"/mnt/usb%d",i);
        snprintf(p,sizeof(p),"%s/homebrew",base);scan_root(p,'H',0);
        snprintf(p,sizeof(p),"%s/etaHEN/games",base);scan_root(p,'G',0);
        scan_root(base,'P',1);
    }
    for(int i=0;i<4;i++){
        char p[96],base[40];snprintf(base,sizeof(base),"/mnt/ext%d",i);
        snprintf(p,sizeof(p),"%s/homebrew",base);scan_root(p,'H',0);
        snprintf(p,sizeof(p),"%s/etaHEN/games",base);scan_root(p,'G',0);
        snprintf(p,sizeof(p),"%s/user/app",base);scan_root(p,'G',0);
        scan_root(base,'P',1);
    }
    printf("DONE %u %u\n",records,visits);
    serve_icons();
    return 0;
}
