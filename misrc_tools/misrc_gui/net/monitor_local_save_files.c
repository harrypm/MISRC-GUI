#include "monitor_local_save_files.h"
#include "../../common/file_utils.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

static bool mls_join_path(char out[MLS_PATH_MAX], const char *dir, const char *name) {
 size_t n = strlen(dir), m = strlen(name);
#if defined(_WIN32)
 const char sep = '\\';
 bool add_separator = n && dir[n-1] != '\\' && dir[n-1] != '/';
#else
 const char sep = '/';
 bool add_separator = n && dir[n-1] != '/';
#endif
 if (n + add_separator + m >= MLS_PATH_MAX) return false;
 memcpy(out, dir, n);
 if (add_separator) out[n++] = sep;
 memcpy(out + n, name, m + 1);
 return true;
}

#if defined(_WIN32)
#include <io.h>
#include <wchar.h>
#include <windows.h>
#define mls_fseek _fseeki64

static bool mls_open_destination(mls_receiver_t *r) {
 if (r->directory_handle) return true;
 wchar_t selection[MLS_PATH_MAX], physical[MLS_PATH_MAX];
 if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, r->directory, -1, selection, MLS_PATH_MAX))
   return false;
 HANDLE directory = CreateFileW(selection, FILE_READ_ATTRIBUTES,
     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
 if (directory == INVALID_HANDLE_VALUE) return false;
 DWORD length = GetFinalPathNameByHandleW(directory, physical, MLS_PATH_MAX, FILE_NAME_NORMALIZED);
 if (!length) length = GetFinalPathNameByHandleW(directory, physical, MLS_PATH_MAX, FILE_NAME_OPENED);
 if (!length || length >= MLS_PATH_MAX ||
     !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, physical, -1, r->physical_directory,
                          MLS_PATH_MAX, NULL, NULL)) {
   CloseHandle(directory);
   return false;
 }
 r->directory_handle = (uintptr_t)directory;
 return true;
}

static bool mls_open_stage(mls_receiver_t *r, mls_file_t *f) {
 char partial[sizeof(f->name) + sizeof(".partial")];
 snprintf(partial, sizeof(partial), "%s.partial", f->name);
 if (!mls_open_destination(r) ||
     !mls_join_path(f->final_path, r->physical_directory, f->name) ||
     !mls_join_path(f->incomplete_path, r->physical_directory, partial) ||
     (!r->overwrite && file_path_info_utf8(f->final_path, NULL, NULL)))
   return false;

 wchar_t wide[MLS_PATH_MAX];
 if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, f->incomplete_path, -1, wide, MLS_PATH_MAX))
   return false;

 // This native handle owns the exact stage and its sharing reservation
 // through the COMMIT acknowledgement. The FILE owns a duplicate.
 HANDLE stage = CreateFileW(wide, GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                            NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
 if (stage == INVALID_HANDLE_VALUE) return false;
 HANDLE writer;
 if (DuplicateHandle(GetCurrentProcess(), stage, GetCurrentProcess(), &writer,
                     0, FALSE, DUPLICATE_SAME_ACCESS)) {
   int fd = _open_osfhandle((intptr_t)writer, _O_WRONLY | _O_BINARY);
   if (fd >= 0) {
     f->fp = _fdopen(fd, "wb");
     if (f->fp) {
       f->stage_handle = (uintptr_t)stage;
       strcpy(r->recovery_path, r->physical_directory);
       return true;
     }
     _close(fd);
   } else {
     CloseHandle(writer);
   }
 }

 FILE_DISPOSITION_INFO disposition = {.DeleteFile = TRUE};
 if (!SetFileInformationByHandle(stage, FileDispositionInfo, &disposition, sizeof(disposition)))
   fprintf(stderr, "[Local Save] Could not remove unused staging file %s\n", f->incomplete_path);
 CloseHandle(stage);
 return false;
}

static int mls_move_stage(const mls_receiver_t *r, const mls_file_t *f,
                          const char *name, bool overwrite) {
 HANDLE handle = (HANDLE)f->stage_handle;
 if (!handle || !r->directory_handle) return -1;

 // Win32 FileRenameInfo requires an absolute name on some Windows
 // versions. Build it from the pinned physical directory, never from
 // the selected alias, which may have been redirected since OPEN.
 char destination[MLS_PATH_MAX];
 wchar_t wide[MLS_PATH_MAX];
 if (!mls_join_path(destination, r->physical_directory, name)) return -1;
 int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, destination, -1, wide, MLS_PATH_MAX);
 if (!length) return -1;
 size_t bytes = offsetof(FILE_RENAME_INFO, FileName) + (size_t)length * sizeof(wchar_t);
 FILE_RENAME_INFO *info = calloc(1, bytes);
 if (!info) return -1;
 info->ReplaceIfExists = overwrite;
 info->FileNameLength = (length - 1) * sizeof(wchar_t);
 memcpy(info->FileName, wide, (size_t)length * sizeof(wchar_t));
 bool moved = SetFileInformationByHandle(handle, FileRenameInfo, info, (DWORD)bytes) != 0;
 free(info);
 return moved ? 0 : -1;
}

static int mls_publish_stage(const mls_receiver_t *r, const mls_file_t *f) {
 return mls_move_stage(r, f, f->name, r->overwrite);
}

static void mls_rollback_stages(mls_receiver_t *r, unsigned limit) {
 for (unsigned id = 0; id < limit; ++id) {
   mls_file_t *f = &r->files[id];
   if (!f->was_opened) continue;
   char partial[sizeof(f->name) + sizeof(".partial")];
   snprintf(partial, sizeof(partial), "%s.partial", f->name);
   if (mls_move_stage(r, f, partial, false) != 0)
     fprintf(stderr, "[Local Save] Could not roll back published file %s\n", f->final_path);
 }
}

static int mls_publish(mls_receiver_t *r, const mls_file_t *first, unsigned count) {
 for (unsigned id = 0; id < MLS_MAX_FILES; ++id) {
   mls_file_t *f = &r->files[id];
   if (!f->was_opened) continue;
   if (mls_publish_stage(r, f) != 0) {
     mls_rollback_stages(r, id);
     return -1;
   }
 }
 strcpy(r->published_path, count == 1 ? first->final_path : r->physical_directory);
 return 0;
}

static void mls_release_stage(mls_file_t *f) {
 if (!f->stage_handle) return;
 CloseHandle((HANDLE)f->stage_handle);
 f->stage_handle = 0;
}

static void mls_release_destination(mls_receiver_t *r, bool remove_work) {
 (void)remove_work;
 if (!r->directory_handle) return;
 CloseHandle((HANDLE)r->directory_handle);
 r->directory_handle = 0;
}

#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#if defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif
#endif
#define mls_fseek fseeko

static void mls_release_destination(mls_receiver_t *r, bool remove_work) {
 if (remove_work && r->work_fd >= 0) {
   (void)unlinkat(r->work_fd, "payload", AT_REMOVEDIR);
   struct stat work, named;
   if (fstat(r->work_fd, &work) == 0 &&
       fstatat(r->directory_fd, r->work_name, &named, AT_SYMLINK_NOFOLLOW) == 0 &&
       work.st_dev == named.st_dev && work.st_ino == named.st_ino)
     (void)unlinkat(r->directory_fd, r->work_name, AT_REMOVEDIR);
 }
 if (r->payload_fd >= 0) close(r->payload_fd);
 if (r->work_fd >= 0) close(r->work_fd);
 if (r->directory_fd >= 0) close(r->directory_fd);
 r->payload_fd = r->work_fd = r->directory_fd = -1;
}

static bool mls_open_destination(mls_receiver_t *r) {
 if (r->payload_fd >= 0) return true;
 r->directory_fd = open(r->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
 if (r->directory_fd < 0) return false;
 char *physical = realpath(r->directory, NULL);
 struct stat opened, resolved;
 bool same_directory = physical && strlen(physical) < sizeof(r->physical_directory) &&
     fstat(r->directory_fd, &opened) == 0 && stat(physical, &resolved) == 0 &&
     opened.st_dev == resolved.st_dev && opened.st_ino == resolved.st_ino;
 if (same_directory) strcpy(r->physical_directory, physical);
 free(physical);
 if (!same_directory) {
   mls_release_destination(r, false);
   return false;
 }

 // Keep a private parent around the payload so neither publication nor
 // rollback depends on resolving the selected directory pathname again.
 for (unsigned attempt = 0; attempt < 1024; ++attempt) {
   snprintf(r->group_name, sizeof(r->group_name), "misrc-recording-%lld-%ld-%u",
            (long long)time(NULL), (long)getpid(), attempt);
   snprintf(r->work_name, sizeof(r->work_name), ".%s.partial", r->group_name);
   if (!mls_join_path(r->recovery_path, r->physical_directory, r->work_name)) break;
   struct stat existing;
   if (fstatat(r->directory_fd, r->group_name, &existing, AT_SYMLINK_NOFOLLOW) == 0)
     continue;
   if (errno != ENOENT) break;
   if (mkdirat(r->directory_fd, r->work_name, 0700) != 0) {
     if (errno == EEXIST) continue;
     break;
   }
   r->work_fd = openat(r->directory_fd, r->work_name,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (r->work_fd >= 0 && mkdirat(r->work_fd, "payload", 0777) == 0) {
     r->payload_fd = openat(r->work_fd, "payload", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
     if (r->payload_fd >= 0) return true;
   }
   break;
 }
 mls_release_destination(r, true);
 r->recovery_path[0] = 0;
 return false;
}

static bool mls_open_stage(mls_receiver_t *r, mls_file_t *f) {
 if (!mls_open_destination(r) || !mls_join_path(f->final_path, r->physical_directory, f->name))
   return false;
 int fd = openat(r->payload_fd, f->name,
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
 if (fd < 0) return false;
 struct stat info;
 if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
   f->stage_fd = dup(fd);
   if (f->stage_fd >= 0) {
     f->fp = fdopen(fd, "wb");
     if (f->fp) {
       f->stage_device = (uint64_t)info.st_dev;
       f->stage_object = (uint64_t)info.st_ino;
       return true;
     }
     close(f->stage_fd);
   }
 }
 close(fd);
 (void)unlinkat(r->payload_fd, f->name, 0);
 return false;
}

static bool mls_stage_matches(const mls_receiver_t *r, const mls_file_t *f) {
 struct stat info;
 return fstatat(r->payload_fd, f->name, &info, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(info.st_mode) && f->stage_device == (uint64_t)info.st_dev &&
        f->stage_object == (uint64_t)info.st_ino;
}

static bool mls_payload_matches(const mls_receiver_t *r, int parent, const char *name) {
 struct stat opened, named;
 return fstat(r->payload_fd, &opened) == 0 &&
        fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(named.st_mode) && opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
}

static int mls_move_noreplace(int from_fd, const char *from,
                              int to_fd, const char *to, bool directory) {
#if defined(__linux__) && defined(SYS_renameat2)
 if (syscall(SYS_renameat2, from_fd, from, to_fd, to, RENAME_NOREPLACE) == 0) return 0;
 if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return -1;
#elif defined(__APPLE__) && defined(RENAME_EXCL)
 if (renameatx_np(from_fd, from, to_fd, to, RENAME_EXCL) == 0) return 0;
 if (errno != ENOTSUP && errno != EINVAL) return -1;
#endif
 // A file can fall back to an atomic exclusive hard link. A directory
 // requires an atomic no-replace rename or stays staged for recovery.
 if (directory) {
   errno = ENOTSUP;
   return -1;
 }
 if (linkat(from_fd, from, to_fd, to, 0) != 0) return -1;
 (void)unlinkat(from_fd, from, 0);
 return 0;
}

static int mls_publish(mls_receiver_t *r, const mls_file_t *first, unsigned count) {
 if (count == 1) {
   if (!mls_stage_matches(r, first)) return -1;
   int rc = r->overwrite ? renameat(r->payload_fd, first->name, r->directory_fd, first->name) :
       mls_move_noreplace(r->payload_fd, first->name, r->directory_fd, first->name, false);
   if (rc != 0) return -1;
   strcpy(r->published_path, first->final_path);
 } else {
   char path[MLS_PATH_MAX];
   const char publishing[] = ".publishing";
   if (!mls_join_path(path, r->physical_directory, r->group_name) ||
       !mls_payload_matches(r, r->work_fd, "payload") ||
       mls_move_noreplace(r->work_fd, "payload", r->work_fd, publishing, true) != 0)
     return -1;

   // Verify the directory actually moved by the first rename while it is
   // still private. A swap of the recovery name cannot publish a foreign group.
   bool valid = mls_payload_matches(r, r->work_fd, publishing);
   for (unsigned id = 0; valid && id < MLS_MAX_FILES; ++id)
     if (r->files[id].was_opened) valid = mls_stage_matches(r, &r->files[id]);
   if (!valid || mls_move_noreplace(r->work_fd, publishing,
                                   r->directory_fd, r->group_name, true) != 0) {
     (void)mls_move_noreplace(r->work_fd, publishing, r->work_fd, "payload", true);
     return -1;
   }
   if (!mls_payload_matches(r, r->directory_fd, r->group_name)) return -1;
   strcpy(r->published_path, path);
 }
 return 0;
}

static void mls_release_stage(mls_file_t *f) {
 if (f->was_opened && f->stage_fd >= 0) {
   close(f->stage_fd);
   f->stage_fd = -1;
 }
}
#endif

int mls_receiver_init(mls_receiver_t *r,const char *directory,bool overwrite){
 if(!r||!directory||!*directory||strlen(directory)>=MLS_PATH_MAX)return -1;
 bool is_dir=false;
 if(!file_path_info_utf8(directory,NULL,&is_dir)||!is_dir)return -1;
 memset(r,0,sizeof(*r));
 strcpy(r->directory,directory);
 r->overwrite=overwrite;
#if !defined(_WIN32)
 r->directory_fd = r->work_fd = r->payload_fd = -1;
#endif
 return 0;
}
static int mls_open(mls_receiver_t *r,unsigned id,const void *data,size_t n){
 mls_file_t *f=&r->files[id];
 if(f->fp||f->was_opened||!data||!n||n>=sizeof(f->name)||memchr(data,0,n)||!mls_safe_filename(data,n))
   return -1;
 memcpy(f->name, data, n);
 f->name[n] = '\0';
 if (!mls_open_stage(r, f)) return -1;
 f->was_opened = true;
 return 0;
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
static int mls_commit(mls_receiver_t *r) {
 const mls_file_t *first = NULL;
 unsigned count = 0;
 for (unsigned id = 0; id < MLS_MAX_FILES; ++id) {
   mls_file_t *f = &r->files[id];
   if (!f->was_opened) continue;
   if (f->fp || !f->was_closed || f->bad) return -1;
#if defined(_WIN32)
   if (!r->overwrite && file_path_info_utf8(f->final_path, NULL, NULL)) return -1;
#endif
   if (!first) first = f;
   ++count;
 }
 return first ? mls_publish(r, first, count) : -1;
}

void mls_receiver_finish_commit(mls_receiver_t *r) {
 for (unsigned id = 0; id < MLS_MAX_FILES; ++id) mls_release_stage(&r->files[id]);
 mls_release_destination(r, true);
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
 case MLS_ABORT:r->had_error=true;return 0;
 case MLS_COMMIT:rc=f->length==0?mls_commit(r):-1;break;
 default:break;
 }
 if(rc!=0)r->had_error=true;
 return rc;
}
void mls_receiver_abort(mls_receiver_t *r){
 if(!r)return;
 bool has_output = false;
 for(unsigned i=0;i<MLS_MAX_FILES;i++){
   mls_file_t *f = &r->files[i];
   if(f->fp){fclose(f->fp);f->fp=NULL;}
   mls_release_stage(f);
   has_output |= f->was_opened;
 }
 if (has_output && !r->published_path[0] && r->recovery_path[0])
   fprintf(stderr, "[M-LOCAL] Unfinished recording files retained in %s\n", r->recovery_path);
 mls_release_destination(r, !has_output || r->published_path[0]);
}
