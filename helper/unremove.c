// SPDX-License-Identifier: GPL-3.0-or-later
// Uninstaller v0.1.18: single-use local action broker for existing elfldr.
// NO shared writable request folder, NO arbitrary paths or system-dir unlink.
// The helper prints a random capability only to its elfldr stdout connection,
// then accepts one confirmed operation over 127.0.0.1:9176 for <=15 seconds.
#include <ps5/kernel.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define AUTHID_INSTALLER 0x4800000000000006ull
#define PORT 9176
// Use direct AppInstUtil SDK imports, consistent with tested PS5 title-remover.
// The Sony uninstall API takes a single title ID argument.
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppUnInstall(const char *title_id);

static int strong_random(char out[33]){
  unsigned char bytes[16];
  int f=open("/dev/urandom",O_RDONLY);
  if(f<0)return 0;
  size_t done=0;
  while(done<sizeof bytes){
    ssize_t n=read(f,bytes+done,sizeof bytes-done);
    if(n<=0)break;
    done+=(size_t)n;
  }
  close(f);
  if(done!=sizeof bytes)return 0;
  for(unsigned i=0;i<16;i++)snprintf(out+2*i,3,"%02x",(unsigned)bytes[i]);
  out[32]=0;
  return 1;
}
static int title_ok(const char* id){
  if(strlen(id)!=9||!strcmp(id,"PPSA99986"))return 0;
  if(strncmp(id,"PPSA",4)&&strncmp(id,"CUSA",4)&&
     strncmp(id,"FAKE",4)&&strncmp(id,"LAPY",4))return 0;
  for(int k=4;k<9;k++)if(id[k]<'0'||id[k]>'9')return 0;
  return 1;
}
static int installed(const char* id){
  char p[190];struct stat st;
  snprintf(p,sizeof p,"/user/app/%s",id);
  if(lstat(p,&st)||!S_ISDIR(st.st_mode))return 0;
  const char* parts[]={"/eboot.bin","/sce_sys/param.json","/sce_sys/param.sfo"};
  for(unsigned i=0;i<3;i++){
    char sub[220];snprintf(sub,sizeof sub,"%s%s",p,parts[i]);
    if(lstat(sub,&st)==0&&S_ISREG(st.st_mode))return 1;
  }
  return 0;
}
static int safe_path(const char* path){
  size_t n=strlen(path);
  if(n<18||n>690||strncmp(path,"/mnt/usb",8)||
     path[8]<'0'||path[8]>'7'||path[9]!='/'||
     strstr(path,"//")||strstr(path,"/../")||strstr(path,"/./")||
     strchr(path,'\\')||path[n-1]=='/')return 0;
  for(size_t i=0;i<n;i++)
    if((unsigned char)path[i]<32||(unsigned char)path[i]==127)return 0;
  return 1;
}
static int pkg_ok(const char* path){
  if(!safe_path(path))return 0;
  size_t n=strlen(path);
  if(n<5||(strcmp(path+n-4,".pkg")&&strcmp(path+n-4,".PKG")))return 0;
  const char* name=strrchr(path,'/');
  if(!name||name[1]=='.')return 0;
  struct stat st;
  return lstat(path,&st)==0&&S_ISREG(st.st_mode)&&st.st_nlink==1;
}
static int homebrew_ok(const char* path){
  if(!safe_path(path))return 0;
  const char* first=strchr(path+10,'/');
  if(!first||strncmp(path+10,"homebrew/",9)!=0)return 0;
  const char* name=path+19;
  if(!*name||strchr(name,'/')||name[0]=='.'||!strcmp(name,".."))return 0;
  struct stat st;
  return lstat(path,&st)==0&&S_ISDIR(st.st_mode);
}
static int progress_fd=-1;
static void progress_ping(unsigned count,int applying){
  if(progress_fd>=0 && count>0 && count%512==0){
    char text[98];
    snprintf(text,sizeof text,"UREM2_STAGE %s_%u\n",
             applying?"removing_files":"checking_files",count);
    send(progress_fd,text,strlen(text),0);
  }
}
// Validate the *entire* tree before any deletion. Refuse symlinks and
// special nodes to avoid unexpected traversal. Bounded 15000 entries.
static int tree_walk(const char* path,int apply,unsigned depth,unsigned* count){
  if(depth>64||*count>=500000)return E2BIG;
  struct stat st;
  if(lstat(path,&st))return errno;
  if(S_ISLNK(st.st_mode))return EPERM;
  if(S_ISREG(st.st_mode)){
    (*count)++;
    progress_ping(*count,apply);
    if(st.st_nlink!=1)return EPERM;
    if(!apply)return 0;
    return unlink(path)==0?0:errno;
  }
  if(!S_ISDIR(st.st_mode))return EPERM;
  DIR* dir=opendir(path);
  if(!dir)return errno;
  int error=0;struct dirent* entry;
  while((entry=readdir(dir))!=NULL){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
    if(strchr(entry->d_name,'/')||strlen(entry->d_name)>250){error=EINVAL;break;}
    char child[1000];
    int n=snprintf(child,sizeof child,"%s/%s",path,entry->d_name);
    if(n<=0||(size_t)n>=sizeof child){error=ENAMETOOLONG;break;}
    error=tree_walk(child,apply,depth+1,count);
    if(error)break;
  }
  closedir(dir);
  if(error)return error;
  (*count)++;
  progress_ping(*count,apply);
  if(!apply)return 0;
  return rmdir(path)==0?0:errno;
}
static void report_stage(int client,const char* phase){
  char msg[96];
  snprintf(msg,sizeof msg,"UREM2_STAGE %s\n",phase);
  if(client>=0)send(client,msg,strlen(msg),0);
  if(client>=0)printf("UREM2_STAGE %s\n",phase);
}

// ShadowMount's raw USB source folders are not registered PKG installs.
// Only top-level /mnt/usbN/<title_id>-app folders may be considered, and
// only when their contents look like a game. Never delete app.db or mount
// directories under /user/app or /system_ex/app.
static int source_syntax(const char* path,const char* title){
  char expected[100];
  if(!title_ok(title)||!safe_path(path))return 0;
  if(path[9]!='/'||strchr(path+10,'/'))return 0;
  if(snprintf(expected,sizeof expected,"%s-app",title)>= (int)sizeof expected)
    return 0;
  if(!strcmp(path+10,expected))return 1;
  if(snprintf(expected,sizeof expected,"%s-app0",title)>= (int)sizeof expected)
    return 0;
  return !strcmp(path+10,expected);
}
static int marker_exists(const char* base,const char* tail){
  char file[960];struct stat st;
  if(snprintf(file,sizeof file,"%s/%s",base,tail)>=(int)sizeof file)
    return 0;
  return lstat(file,&st)==0 && S_ISREG(st.st_mode) && st.st_nlink==1;
}
static int title_tracker_matches(const char* path,const char* id){
  char tracker[128];snprintf(tracker,sizeof tracker,"/user/app/%s/mount.lnk",id);
  int fd=open(tracker,O_RDONLY|O_NOFOLLOW);
  if(fd<0)return errno==ENOENT?1:0; // source not registered / no tracker
  char line[960]={0};ssize_t n=read(fd,line,sizeof line-1);
  close(fd);
  if(n<=0)return 0;
  line[n]=0;
  size_t len=strcspn(line,"\r\n");
  line[len]=0;
  size_t p=strlen(path);
  // ShadowMount may store an app contents subdirectory instead of base.
  return !strcmp(line,path) || (strncmp(line,path,p)==0 && line[p]=='/');
}
static int source_ok(const char* path,const char* title){
  if(!source_syntax(path,title))return 0;
  struct stat st;
  if(lstat(path,&st)||!S_ISDIR(st.st_mode))return 0;
  if(!title_tracker_matches(path,title))return 0;
  if(marker_exists(path,"eboot.bin")||
     marker_exists(path,"sce_sys/param.json")||
     marker_exists(path,"sce_sys/param.sfo")||
     marker_exists(path,"contents/eboot.bin")||
     marker_exists(path,"contents/sce_sys/param.json"))return 1;
  return 0;
}
static void diagnostic(int client,const char* type,const char* detail){
  // Diagnostics are read-only and must not modify or traverse source roots.
  char clean[660]={0};size_t n=0;
  for(const char* p=detail;p&&*p&&n+1<sizeof clean;p++){
    unsigned char ch=(unsigned char)*p;
    clean[n++]=(ch<32||ch>126)?'_':(char)ch;
  }
  char message[800];
  snprintf(message,sizeof message,"UREM2_DIAG %s %s\n",type,clean);
  send(client,message,strlen(message),0);
}
static void diagnose_tracker(const char* title,int client){
  char tracker[130];
  snprintf(tracker,sizeof tracker,"/user/app/%s/mount.lnk",title);
  int fd=open(tracker,O_RDONLY|O_NOFOLLOW);
  if(fd>=0){
    char line[700]={0};
    ssize_t n=read(fd,line,sizeof line-1);close(fd);
    if(n>0){
      line[n]=0;line[strcspn(line,"\r\n")]=0;
      diagnostic(client,"tracker",line);
    }else diagnostic(client,"tracker","unreadable");
  }else if(errno!=ENOENT)diagnostic(client,"tracker","inaccessible");
  else diagnostic(client,"tracker","missing");
  snprintf(tracker,sizeof tracker,"/user/app/%s/mount_img.lnk",title);
  fd=open(tracker,O_RDONLY|O_NOFOLLOW);
  if(fd>=0){
    char line[700]={0};
    ssize_t n=read(fd,line,sizeof line-1);close(fd);
    if(n>0){
      line[n]=0;line[strcspn(line,"\r\n")]=0;
      diagnostic(client,"image_tracker",line);
    }
  }
}
static int detect_source(const char* title,char out[700],int client){
  out[0]=0;
  if(!title_ok(title))return EINVAL;
  diagnose_tracker(title,client);
  unsigned found=0;
  for(unsigned i=0;i<8;i++)for(unsigned form=0;form<2;form++){
    char candidate[700];
    snprintf(candidate,sizeof candidate,form?
             "/mnt/usb%u/%s-app0":"/mnt/usb%u/%s-app",i,title);
    struct stat st;
    if(lstat(candidate,&st)!=0)continue;
    if(source_ok(candidate,title)){
      diagnostic(client,"matched_source",candidate);
      if(++found>1){out[0]=0;return EEXIST;}
      strcpy(out,candidate);
    }else{
      // Explain why a concrete matching folder was refused, rather than
      // telling the user "none" when game bytes remain on the USB drive.
      diagnostic(client,!S_ISDIR(st.st_mode)?"rejected_not_directory":
                 !title_tracker_matches(candidate,title)?"rejected_tracker_mismatch":
                 "rejected_missing_game_files",candidate);
    }
  }
  if(!found)diagnostic(client,"result","no_verified_usb_game_directory");
  return 0;
}
static int do_system_uninstall(const char* title,int client){
  if(!installed(title))return ENOENT;
  report_stage(client,"appinst_initialize");
  int rc=sceAppInstUtilInitialize();
  if(rc!=0){report_stage(client,"appinst_init_rejected");return rc;}
  report_stage(client,"installer_authid");
  uint64_t saved=kernel_get_ucred_authid(-1);
  kernel_set_ucred_authid(-1,AUTHID_INSTALLER);
  if(kernel_get_ucred_authid(-1)!=AUTHID_INSTALLER){
    report_stage(client,"installer_authid_rejected");
    kernel_set_ucred_authid(-1,saved);
    return EPERM;
  }
  report_stage(client,"appinst_uninstall");
  rc=sceAppInstUtilAppUnInstall(title);
  report_stage(client,"appinst_uninstall_returned");
  kernel_set_ucred_authid(-1,saved);
  return rc;
}
static void send_source(int client,const char* source){
  char payload[850];
  snprintf(payload,sizeof payload,"UREM2_SOURCE %s\n",source&&source[0]?source:"-");
  send(client,payload,strlen(payload),0);
}
static int remove_game_source(const char* src,const char* id,int client){
  if(!source_ok(src,id))return EPERM;
  char staged[850];
  // Remove from ShadowMount's scan roots atomically before recursive
  // cleanup so it cannot immediately re-add a half-removed game.
  char nonce[33];
  if(!strong_random(nonce))return EIO;
  char prefix[32];
  memcpy(prefix,nonce,12);prefix[12]=0;
  if(snprintf(staged,sizeof staged,"%.*s/.uninstaller-%s-%s",
       9,src,id,prefix)>= (int)sizeof staged)return ENAMETOOLONG;
  // /mnt/usb0 is length 9; /mnt/usb1 etc are the only allowed roots.
  struct stat exists;
  if(lstat(staged,&exists)==0)return EEXIST;
  report_stage(client,"source_quarantine");
  if(rename(src,staged)!=0)return errno;
  report_stage(client,"source_delete");
  unsigned inspected=0,removed=0;
  // Pre-flight whole renamed folder; aborts on symlinks and special files.
  progress_fd=client;
  int err=tree_walk(staged,0,0,&inspected);
  if(err==0)err=tree_walk(staged,1,0,&removed);
  progress_fd=-1;
  if(err!=0){
    char error[900];
    snprintf(error,sizeof error,"UREM2_STAGED %s\n",staged);
    send(client,error,strlen(error),0);
    return err; // remains hidden for manual recovery, never claim success
  }
  return 0;
}


// DEFERRED SHADOWMOUNT REMOVAL: after a one-shot authenticated request, the
// separate elfldr payload stays alive as the native PPSA99986 application quits.
// It must never recursively delete a managed source itself.
static char g_defer_title[16],g_defer_source[700];
static void defer_log(const char* event,const char* detail){
  char line[1060];
  snprintf(line,sizeof line,"[Uninstaller v0.1.18] %s %s\n",event,detail?detail:"");
  // Detached work must not write to elfldr stdout after the GUI closes
  // the launch socket: SIGPIPE would abort an otherwise valid deletion.
  mkdir("/data/Uninstaller",0755);
  const char* paths[]={"/data/Uninstaller/uninstaller.log",
                       "/data/Uninstaller/deferred-shadow.log"};
  for(unsigned i=0;i<2;i++){
    int fd=open(paths[i],O_CREAT|O_WRONLY|O_APPEND,0644);
    if(fd>=0){write(fd,line,strlen(line));close(fd);}
  }
}
static int defer_safe_source(const char* path){
  size_t n=strlen(path);
  if(n<18||n>690||path[n-1]=='/'||strchr(path,'\\')||
     strstr(path,"//")||strstr(path,"/../")||strstr(path,"/./"))return 0;
  if(n>=3&&(!strcmp(path+n-3,"/..")||!strcmp(path+n-2,"/.")))return 0;
  int root=0;
  if(!strncmp(path,"/mnt/usb",8)||!strncmp(path,"/mnt/ext",8)){
    if((path[8]<'0'||path[8]>'7')||path[9]!='/')return 0;
    root=1;
  }else if(!strncmp(path,"/data/",6))root=1;
  if(!root)return 0;
  for(size_t i=0;i<n;i++)
    if((unsigned char)path[i]<32||(unsigned char)path[i]==127)return 0;
  return 1;
}
static int defer_queue(const char* target){
  const char* sep=strchr(target,'|');
  if(!sep||sep-target!=9||strlen(sep+1)>690)return EINVAL;
  char id[16]={0};memcpy(id,target,9);
  if(!title_ok(id)||!defer_safe_source(sep+1))return EPERM;
  snprintf(g_defer_title,sizeof g_defer_title,"%s",id);
  snprintf(g_defer_source,sizeof g_defer_source,"%s",sep+1);
  return 0;
}
static const char* defer_json_key(const char* json,const char* key){
  static char pattern[75];
  snprintf(pattern,sizeof pattern,"\"%s\"",key);
  const char* p=strstr(json,pattern);
  if(!p)return NULL;
  p=strchr(p+strlen(pattern),':');
  if(!p)return NULL;
  while(*++p==' '||*p=='\t'||*p=='\r'||*p=='\n'){}
  return p;
}
static int defer_json_number(const char* json,const char* key,int fallback){
  const char* p=defer_json_key(json,key);
  if(!p||*p<'0'||*p>'9')return fallback;
  long result=strtol(p,NULL,10);
  return result>=0&&result<=2147483647?(int)result:fallback;
}
static int defer_json_bool(const char* json,const char* key){
  const char* p=defer_json_key(json,key);
  return p&&strncmp(p,"true",4)==0;
}
static int defer_json_string(const char* json,const char* key,char* out,size_t cap){
  out[0]=0;
  const char* p=defer_json_key(json,key);
  if(!p||*p!='"')return 0;
  p++;size_t n=0;
  while(*p&&*p!='"'){
    if(n+1>=cap)return 0;
    if(*p=='\\'){
      ++p;
      if(*p!='"'&&*p!='\\'&&*p!='/')return 0;
    }
    if((unsigned char)*p<32)return 0;
    out[n++]=*p++;
  }
  if(*p!='"')return 0;
  out[n]=0;return 1;
}
struct defer_reply {int http;char body[16384];};
static struct defer_reply defer_post(const char* endpoint,const char* json){
  struct defer_reply result={0};
  int fd=socket(AF_INET,SOCK_STREAM,0);
  if(fd<0)return result;
  struct timeval timeout={5,0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
  struct sockaddr_in addr={0};
  addr.sin_family=AF_INET;addr.sin_port=htons(10101);
  addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(connect(fd,(struct sockaddr*)&addr,sizeof addr)!=0){close(fd);return result;}
  char request[1500];
  int n=snprintf(request,sizeof request,
      "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Content-Type: application/json\r\nConnection: close\r\n"
      "Content-Length: %zu\r\n\r\n%s",endpoint,strlen(json),json);
  if(n<=0||(size_t)n>=sizeof request){close(fd);return result;}
  size_t pos=0;
  while(pos<(size_t)n){
    ssize_t written=send(fd,request+pos,(size_t)n-pos,0);
    if(written<=0){close(fd);return result;}
    pos+=(size_t)written;
  }
  char response[16800];
  size_t used=0;
  while(used+1<sizeof response){
    ssize_t readn=recv(fd,response+used,sizeof response-1-used,0);
    if(readn==0)break;
    if(readn<0){close(fd);return result;}
    used+=(size_t)readn;
  }
  close(fd);response[used]=0;
  if(sscanf(response,"HTTP/%*d.%*d %d",&result.http)!=1){
    result.http=0;return result;
  }
  const char* body=strstr(response,"\r\n\r\n");
  if(!body){result.http=0;return result;}
  body+=4;
  size_t size=strlen(body);
  if(size>=sizeof result.body){result.http=0;return result;}
  memcpy(result.body,body,size+1);
  return result;
}
static int defer_inspect(void){
  char body[128];
  snprintf(body,sizeof body,"{\"title_id\":\"%s\"}",g_defer_title);
  struct defer_reply r=defer_post("/api/v1/games/info",body);
  if(r.http!=200||defer_json_number(r.body,"status",-1)!=0)return 0;
  char id[24],path[720],type[32];
  if(!defer_json_string(r.body,"title_id",id,sizeof id)||
     !defer_json_string(r.body,"path",path,sizeof path)||
     !defer_json_string(r.body,"source_type",type,sizeof type)||
     strcmp(id,g_defer_title)||strcmp(path,g_defer_source)||
     (strcmp(type,"folder")&&strcmp(type,"image"))||
     !defer_json_bool(r.body,"can_manage_source")||
     !defer_json_bool(r.body,"source_available"))return 0;
  return 1;
}
static int defer_busy(const char* json){
  char reason[80]={0};
  if(!defer_json_string(json,"error_reason",reason,sizeof reason))return 0;
  const char* valid[]={"game_active","runtime_prepared","runtime_busy",
     "scanner_busy","storage_busy","title_unmount_busy","image_unmount_busy",
     "install_pending"};
  for(unsigned i=0;i<sizeof valid/sizeof valid[0];i++)
    if(!strcmp(reason,valid[i]))return 1;
  return 0;
}
static int defer_run(void){
  defer_log("DEFER_START",g_defer_title);
  // The confirmation has already been acknowledged. The GUI will call
  // sceSystemServiceLoadExec("exit") via hui::sys::quit(). Give it time to
  // leave the foreground and let ShadowMount observe the exit.
  sleep(4);
  char body[128];
  snprintf(body,sizeof body,"{\"title_id\":\"%s\",\"confirm\":true}",g_defer_title);
  int job=-1,source_absent=0;
  for(int try_number=0;try_number<90;try_number++){
    if(!defer_inspect()){
      defer_log("DEFER_FAIL","Source changed, no longer manageable, or API unavailable");
      return 1; // never bypass this check with direct filesystem deletion
    }
    struct defer_reply r=defer_post("/api/v1/games/delete",body);
    if(r.http==202&&defer_json_number(r.body,"status",-1)==0){
      job=defer_json_number(r.body,"job_id",-1);
      if(job<=0){defer_log("DEFER_FAIL","Missing storage job id");return 1;}
      char tmp[95];snprintf(tmp,sizeof tmp,"job=%d title=%s",job,g_defer_title);
      defer_log("DEFER_QUEUED",tmp);break;
    }
    if(r.http==200&&defer_json_number(r.body,"status",-1)==0&&
       defer_json_bool(r.body,"source_missing")){
      source_absent=1;defer_log("DEFER_SOURCE_ABSENT",g_defer_title);break;
    }
    if(r.http==409&&defer_busy(r.body)){
      if(try_number==0||try_number%10==0){
        char reason[80]={0};defer_json_string(r.body,"error_reason",reason,sizeof reason);
        defer_log("DEFER_WAIT",reason);
      }
      sleep(3);continue;
    }
    char message[240]={0};
    defer_json_string(r.body,"error",message,sizeof message);
    defer_log("DEFER_FAIL",message[0]?message:"ShadowMount refused delete");
    return 1;
  }
  if(job<=0&&!source_absent){
    defer_log("DEFER_FAIL","Timed out waiting for safe game exit");return 1;
  }
  if(job>0){
    int progress=-1,errors=0;
    for(int attempt=0;attempt<5400;attempt++){
      char query[80];snprintf(query,sizeof query,"{\"job_id\":%d}",job);
      struct defer_reply r=defer_post("/api/v1/games/storage/status",query);
      if(r.http!=200||defer_json_number(r.body,"status",-1)!=0){
        if(++errors>=5){
          defer_log("DEFER_UNCERTAIN","Job status unavailable; never retry deletion blindly");
          return 1;
        }
        sleep(2);continue;
      }
      errors=0;
      char title[24],source[720],operation[30],state[35];
      if(defer_json_number(r.body,"job_id",-1)!=job||
         !defer_json_string(r.body,"title_id",title,sizeof title)||
         !defer_json_string(r.body,"source",source,sizeof source)||
         !defer_json_string(r.body,"operation",operation,sizeof operation)||
         !defer_json_string(r.body,"state",state,sizeof state)||
         strcmp(title,g_defer_title)||strcmp(source,g_defer_source)||
         strcmp(operation,"delete")){
        defer_log("DEFER_UNCERTAIN","Storage job identity changed");return 1;
      }
      int current=defer_json_number(r.body,"progress_percent",0);
      if(current>=0&&current<=100&&current/10!=progress/10){
        progress=current;
        char line[96];snprintf(line,sizeof line,"%s %d%%",g_defer_title,current);
        defer_log("DEFER_PROGRESS",line);
      }
      if(!strcmp(state,"completed")){
        if(defer_json_number(r.body,"result_status",-1)!=0){
          defer_log("DEFER_FAIL","Storage job completed with error");return 1;
        }
        defer_log("DEFER_SOURCE_REMOVED",g_defer_source);break;
      }
      if(!strcmp(state,"failed")||!strcmp(state,"cancelled")){
        char error[160]={0};
        defer_json_string(r.body,"result_error",error,sizeof error);
        defer_log("DEFER_FAIL",error[0]?error:state);return 1;
      }
      if(attempt==5399){
        defer_log("DEFER_UNCERTAIN","Deletion job still running after status limit");
        return 1;
      }
      sleep(2);
    }
  }
  if(installed(g_defer_title)){
    int native=do_system_uninstall(g_defer_title,-1);
    if(native!=0){
      char msg[100];snprintf(msg,sizeof msg,"%s native_code=0x%08x",
                  g_defer_title,(unsigned)native);
      defer_log("DEFER_NATIVE_PENDING",msg);
    }else defer_log("DEFER_NATIVE_ACCEPTED",g_defer_title);
  }
  defer_log("DEFER_SOURCE_DONE_TITLE_RECHECK",g_defer_title);
  return 0;
}

static void perform(const char* kind,const char* target,unsigned* code,unsigned* state,int client){
  *code=EINVAL;*state=0;
  if(!strcmp(kind,"SHADOWDEFER")){
    *code=(unsigned)defer_queue(target);
    if(*code==0)*state=5; // authenticated dispatch accepted, deletion NOT done
    return;
  }
  if(!strcmp(kind,"TITLEPROBE")||!strcmp(kind,"TITLEAUTO")){
    report_stage(client,"source_inspect");
    char id[16]={0}, expected[700]={0}, source[700]={0};
    const char* split=strchr(target,'|');
    if(split){
      size_t n=(size_t)(split-target);
      if(n>=sizeof id){*code=EINVAL;return;}
      memcpy(id,target,n);id[n]=0;
      snprintf(expected,sizeof expected,"%s",split+1);
    }else snprintf(id,sizeof id,"%s",target);
    if(!title_ok(id)){*code=EINVAL;return;}
    int detected=detect_source(id,source,client);
    if(detected){*code=(unsigned)detected;return;}
    if(!strcmp(kind,"TITLEPROBE")){
      send_source(client,source);
      *code=0;*state=3;return;
    }
    // A source path selected in the UI must remain identical on disk.
    if(strcmp(expected,source)) { *code=ESTALE;return; }
    if(source[0])send_source(client,source);
    // Try normal PS5 registration removal; ShadowMount-only sources can
    // outlive this API, so a failed/missing registered entry is not fatal
    // to removing a separately confirmed backing folder.
    int native=ENOENT;
    if(installed(id))native=do_system_uninstall(id,client);
    if(source[0]){
      int sourceError=remove_game_source(source,id,client);
      *code=(unsigned)sourceError;
      *state=sourceError==0?(native==0?2:4):0;
      if(sourceError==0 && native!=0)
        report_stage(client,"source_deleted_title_cleanup_pending");
      return;
    }
    *code=(unsigned)native;
    *state=native==0?1:0;
    return;
  }
  if(!strcmp(kind,"TITLE")){
    report_stage(client,"title_validate");
    if(!title_ok(target)||!installed(target)){
      report_stage(client,"invalid_installed_title");
      return;
    }
    report_stage(client,"appinst_initialize");
    int rc=sceAppInstUtilInitialize();
    if(rc!=0){
      report_stage(client,"appinst_init_rejected");
      *code=(unsigned)rc;
      return;
    }
    // Initialize AppInstUtil before switching to the installer authid.
    report_stage(client,"installer_authid");
    uint64_t saved=kernel_get_ucred_authid(-1);
    kernel_set_ucred_authid(-1,AUTHID_INSTALLER);
    if(kernel_get_ucred_authid(-1)!=AUTHID_INSTALLER){
      report_stage(client,"installer_authid_rejected");
      *code=EPERM;
      kernel_set_ucred_authid(-1,saved);
      return;
    }
    report_stage(client,"appinst_uninstall");
    rc=sceAppInstUtilAppUnInstall(target);
    report_stage(client,"appinst_uninstall_returned");
    kernel_set_ucred_authid(-1,saved);
    *code=(unsigned)rc;
    if(rc==0)*state=1; // accepted; asynchronous native operation may remain
  }else if(!strcmp(kind,"USBPKG")){
    if(!pkg_ok(target))return;
    *code=unlink(target)==0?0:(unsigned)errno;
    if(!*code)*state=2;
  }else if(!strcmp(kind,"USBHOME")){
    if(!homebrew_ok(target))return;
    unsigned inspected=0,removed=0;
    int rc=tree_walk(target,0,0,&inspected);
    if(rc==0)rc=tree_walk(target,1,0,&removed);
    *code=(unsigned)rc;
    if(rc==0)*state=2;
  }
}
static int send_full(int fd,const char* s){
  size_t n=strlen(s);
  while(n){ssize_t v=send(fd,s,n,0);if(v<=0)return 0;s+=v;n-=(size_t)v;}
  return 1;
}
int main(void){
  setvbuf(stdout,NULL,_IONBF,0);
  char token[33]={0};
  if(!strong_random(token)){puts("UREM2_ERROR no_random");return 2;}
  int server=socket(AF_INET,SOCK_STREAM,0);
  if(server<0){puts("UREM2_ERROR socket");return 2;}
  int reuse=1;
  setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
  struct sockaddr_in addr={0};
  addr.sin_family=AF_INET;
  addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  addr.sin_port=htons(PORT);
  if(bind(server,(struct sockaddr*)&addr,sizeof addr)!=0||listen(server,1)!=0){
    puts("UREM2_ERROR bind");close(server);return 3;
  }
  printf("UREM2_READY %s\n",token);
  fd_set readfds;FD_ZERO(&readfds);FD_SET(server,&readfds);
  struct timeval wait={15,0};
  if(select(server+1,&readfds,NULL,NULL,&wait)<=0){
    puts("UREM2_ERROR timeout");close(server);return 4;
  }
  int client=accept(server,NULL,NULL);
  close(server);
  if(client<0)return 5;
  struct timeval shortwait={5,0};
  setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&shortwait,sizeof shortwait);
  setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&shortwait,sizeof shortwait);
  char command[800]={0};size_t used=0;char ch=0;
  while(used+1<sizeof command && recv(client,&ch,1,0)==1){
    if(ch=='\n')break;
    command[used++]=ch;
  }
  if(ch!='\n'||strncmp(command,"ACT ",4)!=0||
     strlen(command)<4+32+1+5+1||
     strncmp(command+4,token,32)!=0||command[36]!=' '){
    send_full(client,"UREM2_ERROR unauthorized\n");close(client);return 6;
  }
  char* kind=command+37;char* sep=strchr(kind,' ');
  if(!sep){send_full(client,"UREM2_ERROR format\n");close(client);return 7;}
  *sep=0;const char* target=sep+1;
  unsigned code=0,state=0;
  perform(kind,target,&code,&state,client);
  char report[160];
  snprintf(report,sizeof report,"UREM2_RESULT %s %08x %u\n",token,code,state);
  send_full(client,report);
  close(client);
  if(code==0&&state==5){
    // Keep this independent elfldr payload alive after native UI exit.
    return defer_run()==0?0:8;
  }
  printf("UREM2_DONE code=%08x state=%u\n",code,state);
  return code==0?0:8;
}
