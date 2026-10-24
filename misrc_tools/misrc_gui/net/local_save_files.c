#include "local_save_files.h"
#include "../../common/file_utils.h"
#include <string.h>
#include <errno.h>
#include <stdint.h>
#if defined(_WIN32)
#include <wchar.h>
#include <windows.h>
#define mls_fseek _fseeki64
static int mls_rename_utf8(const char *oldp,const char *newp){
 wchar_t a[MLS_PATH_MAX],b[MLS_PATH_MAX];
 if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,oldp,-1,a,MLS_PATH_MAX)||
    !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,newp,-1,b,MLS_PATH_MAX))return -1;
 return _wrename(a,b);
}
#else
#include <sys/types.h>
#define mls_fseek fseeko
static int mls_rename_utf8(const char *oldp,const char *newp){return rename(oldp,newp);}
#endif
static int path_exists(const char *p){return file_path_info_utf8(p,NULL,NULL)?1:0;}
int mls_receiver_init(mls_receiver_t *r,const char *directory,bool overwrite){
 if(!r||!directory||!*directory||strlen(directory)>=MLS_PATH_MAX)return -1;
 bool is_dir=false;
 if(!file_path_info_utf8(directory,NULL,&is_dir)||!is_dir)return -1;
 memset(r,0,sizeof(*r));strcpy(r->directory,directory);r->overwrite=overwrite;return 0;
}
static int mls_open(mls_receiver_t *r,unsigned id,const void *data,size_t n){
 mls_file_t *f=&r->files[id];
 if(f->fp||f->was_opened||!data||!n||n>255||memchr(data,0,n)||!mls_safe_filename(data,n))return -1;
 char name[256];memcpy(name,data,n);name[n]=0;
 size_t dlen=strlen(r->directory);
 char sep=(strchr(r->directory,'\\')|| (dlen>1 && r->directory[1]==':'))?'\\':'/';
 const char *slash=(dlen && (r->directory[dlen-1]=='/'||r->directory[dlen-1]=='\\'))?"":(sep=='\\'?"\\":"/");
 int rc=snprintf(f->final_path,sizeof(f->final_path),"%s%s%s",r->directory,slash,name);
 if(rc<0||(size_t)rc>=sizeof(f->final_path))return -1;
 if(strlen(f->final_path)+sizeof(".partial")>sizeof(f->incomplete_path))return -1;
 strcpy(f->incomplete_path,f->final_path);
 strcat(f->incomplete_path,".partial");
 rc=(int)strlen(f->incomplete_path);
 if(rc<0||(size_t)rc>=sizeof(f->incomplete_path))return -1;
 if(path_exists(f->incomplete_path)||(!r->overwrite&&path_exists(f->final_path)))return -1;
 f->fp=file_open_utf8(f->incomplete_path,"wb+");
 if(f->fp)f->was_opened=true;
 return f->fp?0:-1;
}
static int mls_write(mls_receiver_t *r,unsigned id,uint64_t offset,const void *data,size_t len,bool flush_now){
 mls_file_t *f=&r->files[id];
 if(!f->fp||f->bad||!data||offset>INT64_MAX||len>(uint64_t)INT64_MAX-offset)return -1;
 /* fseek on every sequential block forces a buffered-stream flush and
  * adds unnecessary disk work. Keep exact offsets for FLAC/WAV header
  * rewrites, but skip fseek while streaming forward. */
 if(offset!=f->next_offset && mls_fseek(f->fp,(int64_t)offset,SEEK_SET)!=0)return -1;
 if(fwrite(data,1,len,f->fp)!=len)return -1;
 f->next_offset=offset+len;
 /* STREAM batches flush at MLS_SYNC, which is the ACK boundary. */
 return !flush_now || fflush(f->fp)==0?0:-1;
}
static int mls_seek(mls_receiver_t *r,unsigned id,uint64_t offset){
 mls_file_t *f=&r->files[id];
 if(!f->fp||f->bad||offset>INT64_MAX)return -1;
 int rc=mls_fseek(f->fp,(int64_t)offset,SEEK_SET);
 if(rc==0) f->next_offset=offset;
 return rc;
}
static int mls_close(mls_receiver_t *r,unsigned id){
 mls_file_t *f=&r->files[id];
 if(!f->fp||f->bad)return -1;
 int rc=fflush(f->fp);
 if(fclose(f->fp)!=0)rc=-1;
 f->fp=NULL;
 if(rc==0)f->was_closed=true;
 if(rc!=0)f->bad=true;
 return rc;
}
/* A recording is a multi-file transaction. CLOSE leaves .partial in place;
 * COMMIT renames only after ALL files have closed without errors. */
static int mls_commit(mls_receiver_t *r){
 unsigned opened=0;
 for(unsigned id=0;id<MLS_MAX_FILES;id++){
   mls_file_t *f=&r->files[id];
   if(!f->was_opened)continue;
   opened++;
   if(f->fp || !f->was_closed || f->bad ||
      (!r->overwrite && path_exists(f->final_path)))return -1;
 }
 if(!opened)return -1;
 /* Rename completed files only once all streams have closed correctly.
  * On mid-transaction rename failure roll previously moved files back to
  * .partial where possible, and return failure to the capture server. */
 for(unsigned id=0;id<MLS_MAX_FILES;id++){
   mls_file_t *f=&r->files[id];
   if(f->was_opened && mls_rename_utf8(f->incomplete_path,f->final_path)!=0){
     for(unsigned j=0;j<id;j++) {
       mls_file_t *earlier=&r->files[j];
       if(earlier->was_opened)
         (void)mls_rename_utf8(earlier->final_path,earlier->incomplete_path);
     }
     return -1;
   }
 }
 return 0;
}
int mls_receiver_apply(mls_receiver_t *r,const mls_frame_t *f,const void *payload){
 if(!r||!f||r->had_error||f->file_id>=MLS_MAX_FILES||f->length>MLS_MAX_PAYLOAD)return -1;
 if(f->length&&(!payload||mls_crc32(payload,f->length)!=f->crc)){r->had_error=true;return -1;}
 int rc=-1;
 switch(f->type){
 case MLS_OPEN:rc=mls_open(r,f->file_id,payload,f->length);break;
 case MLS_WRITE:rc=mls_write(r,f->file_id,f->offset,payload,f->length,true);break;
 case MLS_WRITE_STREAM:rc=mls_write(r,f->file_id,f->offset,payload,f->length,false);break;
 case MLS_SYNC:
   rc=f->length==0?0:-1;
   if(rc==0)for(unsigned i=0;i<MLS_MAX_FILES;i++) {
     if(r->files[i].fp && fflush(r->files[i].fp)!=0){rc=-1;break;}
   }
   break;
 case MLS_SEEK:rc=(f->length==0)?mls_seek(r,f->file_id,f->offset):-1;break;
 case MLS_CLOSE:rc=(f->length==0)?mls_close(r,f->file_id):-1;break;
 case MLS_ABORT:mls_receiver_abort(r);r->had_error=true;return 0;
 case MLS_COMMIT:rc=f->length==0?mls_commit(r):-1;break;
 default:break;
 }
 if(rc!=0)r->had_error=true;
 return rc;
}
void mls_receiver_abort(mls_receiver_t *r){
 if(!r)return;
 for(unsigned i=0;i<MLS_MAX_FILES;i++){
     if(r->files[i].fp){fclose(r->files[i].fp);r->files[i].fp=NULL;}
 }
}
