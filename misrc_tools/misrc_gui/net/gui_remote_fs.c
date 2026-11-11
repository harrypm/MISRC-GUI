#include "gui_remote_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include <windows.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#endif
#define FS_LIST_LIMIT 8192u

static int fs_name_cmp(const void *aa,const void *bb){
    return strcmp((const char *)aa,(const char *)bb);
}
static bool fs_segment_ok(const char *s,size_t n){
    if(!n || (n==1 && s[0]=='.') || (n==2 && s[0]=='.'&&s[1]=='.'))return false;
    for(size_t i=0;i<n;i++){
        if((unsigned char)s[i]<32 || (unsigned char)s[i]==127)return false;
#ifdef _WIN32
        if(s[i]==':'||s[i]=='*'||s[i]=='?'||s[i]=='"'||s[i]=='<'||s[i]=='>'||s[i]=='|')return false;
#else
        if(s[i]=='\\')return false;
#endif
    }
    return true;
}
#ifdef _WIN32
static bool fs_wide(const char *s,wchar_t *w,size_t cap){
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,w,(int)cap)>0;
}
static bool fs_utf8(const wchar_t *w,char *s,size_t cap){
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w,-1,s,(int)cap,NULL,NULL)>0;
}
static bool fs_dir(const char *path){
    wchar_t w[GUI_REMOTE_FS_PATH];if(!fs_wide(path,w,GUI_REMOTE_FS_PATH))return false;
    DWORD a=GetFileAttributesW(w);
    return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);
}
/* Reject relative/device paths, traversal, reparse points including ancestors.
 * UNC share roots are supported; drive roots are validated normally. */
static bool fs_valid_path(char *path){
    size_t n=strlen(path);
    if(n<3 || n>=GUI_REMOTE_FS_PATH)return false;
    for(size_t i=0;i<n;i++)if(path[i]=='/')path[i]='\\';
    if(!strncmp(path,"\\\\?\\",4)||!strncmp(path,"\\\\.\\",4))return false;
    size_t pos=0;
    if(((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z'))&&path[1]==':'&&path[2]=='\\'){
        pos=3;
    }else if(path[0]=='\\'&&path[1]=='\\'){
        /* UNC needs a nonempty host and share, never a bare server name. */
        const char *host=path+2,*slash=strchr(host,'\\');
        if(!slash||!fs_segment_ok(host,(size_t)(slash-host)))return false;
        const char *share=slash+1,*end=strchr(share,'\\');
        size_t len=end?(size_t)(end-share):strlen(share);
        if(!fs_segment_ok(share,len))return false;
        pos=end?(size_t)(end-path)+1:n;
    }else return false;
    char segment[GUI_REMOTE_FS_PATH];
    for(size_t start=pos;start<n;){
        size_t end=start;while(end<n&&path[end]!='\\')end++;
        if(!fs_segment_ok(path+start,end-start))return false;
        start=end+1;
        if(end==n)break;
        if(start==n)break;
    }
    /* Check every existing component; do not follow reparse-point traversal. */
    char copy[GUI_REMOTE_FS_PATH];snprintf(copy,sizeof(copy),"%s",path);
    size_t cur=pos;
    if(cur && copy[cur-1]=='\\')cur--;
    for(;cur<n;cur++){
        if(copy[cur]!='\\')continue;
        if(cur==0)continue;
        char save=copy[cur];copy[cur]=0;
        if(!fs_dir(copy)){copy[cur]=save;return false;}
        copy[cur]=save;
    }
    return fs_dir(path);
}
static void fs_parent(char *out,size_t cap,const char *path){
    snprintf(out,cap,"%s",path);
    size_t n=strlen(out);
    while(n>0&&out[n-1]=='\\' && !(n==3&&out[1]==':'))out[--n]=0;
    if(n==3&&out[1]==':'){out[0]=0;return;}
    char *p=strrchr(out,'\\');
    if(!p){out[0]=0;return;}
    if(p==out+2&&out[1]==':'){p[1]=0;return;}
    if(p==out+1 && out[0]=='\\'){out[0]=0;return;}
    /* UNC share root => computer */
    if(out[0]=='\\' && out[1]=='\\'){
        char *share=strchr(out+2,'\\');
        if(share==p){out[0]=0;return;}
    }
    *p=0;
}
#else
static bool fs_valid_path(char *path){
    if(path[0]!='/'||strlen(path)>=GUI_REMOTE_FS_PATH)return false;
    if(path[1]==0)return true;
    char partial[GUI_REMOTE_FS_PATH]="";
    size_t n=strlen(path),cur=1;
    while(cur<n){
        size_t end=cur;while(end<n&&path[end]!='/')end++;
        if(!fs_segment_ok(path+cur,end-cur))return false;
        size_t old=strlen(partial);
        if(snprintf(partial+old,sizeof(partial)-old,"/%.*s",(int)(end-cur),path+cur)>=(int)(sizeof(partial)-old))return false;
        struct stat st;
        if(lstat(partial,&st)!=0||!S_ISDIR(st.st_mode))return false;
        cur=end+1;
        if(end<n&&cur>=n)break; /* strip final slash */
    }
    return true;
}
static void fs_parent(char *out,size_t cap,const char *path){
    snprintf(out,cap,"%s",path);size_t n=strlen(out);
    while(n>1&&out[n-1]=='/')out[--n]=0;
    char *p=strrchr(out,'/');
    if(!p||p==out){snprintf(out,cap,"/");return;}
    *p=0;
}
#endif

static bool fs_collect(const char *path,char (*names)[GUI_REMOTE_FS_NAME],unsigned *n){
#ifdef _WIN32
    wchar_t wide[GUI_REMOTE_FS_PATH],pattern[GUI_REMOTE_FS_PATH+4];
    if(!fs_wide(path,wide,GUI_REMOTE_FS_PATH))return false;
    if(swprintf(pattern,GUI_REMOTE_FS_PATH+4,L"%ls\\*",wide)<0)return false;
    WIN32_FIND_DATAW item;HANDLE h=FindFirstFileW(pattern,&item);
    if(h==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    do{
        if(item.cFileName[0]==L'.' && (item.cFileName[1]==0||
          (item.cFileName[1]==L'.'&&item.cFileName[2]==0)))continue;
        if(!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||
           (item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;
        if(*n<FS_LIST_LIMIT && fs_utf8(item.cFileName,names[*n],GUI_REMOTE_FS_NAME))(*n)++;
    }while(FindNextFileW(h,&item));
    FindClose(h);
#else
    DIR *d=opendir(path);if(!d)return false;
    struct dirent *ent;
    while((ent=readdir(d))!=NULL){
        if(!strcmp(ent->d_name,".")||!strcmp(ent->d_name,".."))continue;
        if(strlen(ent->d_name)>=GUI_REMOTE_FS_NAME)continue;
        char child[GUI_REMOTE_FS_PATH];
        if(snprintf(child,sizeof(child),"%s%s%s",path,strcmp(path,"/")?"/":"",ent->d_name)>=(int)sizeof(child))continue;
        struct stat st;
        if(lstat(child,&st)!=0||!S_ISDIR(st.st_mode))continue;
        if(*n<FS_LIST_LIMIT)snprintf(names[(*n)++],GUI_REMOTE_FS_NAME,"%s",ent->d_name);
    }
    closedir(d);
#endif
    return true;
}

bool gui_remote_fs_browse(const char *requested,unsigned page,gui_remote_fs_result_t *out){
    if(!out||page>100000)return false;
    memset(out,0,sizeof(*out));out->page=page;
    char target[GUI_REMOTE_FS_PATH];
    const char *input=requested?requested:"";
    if(!strcmp(input,"~")){
#ifdef _WIN32
        input=getenv("USERPROFILE");
#else
        input=getenv("HOME");
#endif
        if(!input)input="";
    }
#ifdef _WIN32
    if(!*input){out->computer=true;out->path[0]=out->parent[0]=0;}
    else{
        snprintf(target,sizeof(target),"%s",input);
        if(!fs_valid_path(target)){
            out->recovered=true;out->computer=true;
        }else{
            snprintf(out->path,sizeof(out->path),"%s",target);
            fs_parent(out->parent,sizeof(out->parent),target);
        }
    }
#else
    if(!*input)input="/";
    snprintf(target,sizeof(target),"%s",input);
    if(!fs_valid_path(target)){
        out->recovered=true;snprintf(target,sizeof(target),"/");
    }
    snprintf(out->path,sizeof(out->path),"%s",target);
    fs_parent(out->parent,sizeof(out->parent),target);
#endif
    char (*all)[GUI_REMOTE_FS_NAME]=calloc(FS_LIST_LIMIT,sizeof(*all));
    if(!all)return false;
    unsigned n=0;bool ok=true;
#ifdef _WIN32
    if(out->computer){
        DWORD mask=GetLogicalDrives();
        for(int i=0;i<26;i++){
            if(!(mask&(1UL<<i)))continue;
            wchar_t drive[4]={(wchar_t)(L'A'+i),L':',L'\\',0};
            if(GetDriveTypeW(drive)==DRIVE_NO_ROOT_DIR)continue;
            snprintf(all[n++],GUI_REMOTE_FS_NAME,"%c:\\",'A'+i);
        }
    }else ok=fs_collect(out->path,all,&n);
#else
    ok=fs_collect(out->path,all,&n);
#endif
    if(ok){
        qsort(all,n,sizeof(*all),fs_name_cmp);
        size_t from=(size_t)page*GUI_REMOTE_FS_PAGE;
        if(from<n){
            size_t count=n-from;if(count>GUI_REMOTE_FS_PAGE)count=GUI_REMOTE_FS_PAGE;
            out->count=(unsigned)count;
            for(unsigned i=0;i<out->count;i++)snprintf(out->names[i],GUI_REMOTE_FS_NAME,"%s",all[from+i]);
            out->has_more=from+count<n;
        }
    }
    free(all);return ok;
}
