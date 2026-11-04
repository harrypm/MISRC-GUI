#include "gui_monitor_settings.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
typedef struct {const char *key;size_t offset,size;char type;double lo,hi;} desc_t;
static const desc_t fields[]={
    {"capture_a",offsetof(gui_settings_t,capture_a),sizeof(((gui_settings_t*)0)->capture_a),'B',0,1},
    {"capture_b",offsetof(gui_settings_t,capture_b),sizeof(((gui_settings_t*)0)->capture_b),'B',0,1},
    {"auto_names_enabled",offsetof(gui_settings_t,auto_names_enabled),sizeof(((gui_settings_t*)0)->auto_names_enabled),'B',0,1},
    {"append_timestamp_on_capture_start",offsetof(gui_settings_t,append_timestamp_on_capture_start),sizeof(((gui_settings_t*)0)->append_timestamp_on_capture_start),'B',0,1},
    {"overwrite_files",offsetof(gui_settings_t,overwrite_files),sizeof(((gui_settings_t*)0)->overwrite_files),'B',0,1},
    {"stop_on_dropout",offsetof(gui_settings_t,stop_on_dropout),sizeof(((gui_settings_t*)0)->stop_on_dropout),'B',0,1},
    {"use_flac",offsetof(gui_settings_t,use_flac),sizeof(((gui_settings_t*)0)->use_flac),'B',0,1},
    {"flac_verification",offsetof(gui_settings_t,flac_verification),sizeof(((gui_settings_t*)0)->flac_verification),'B',0,1},
    {"enable_resample_a",offsetof(gui_settings_t,enable_resample_a),sizeof(((gui_settings_t*)0)->enable_resample_a),'B',0,1},
    {"enable_resample_b",offsetof(gui_settings_t,enable_resample_b),sizeof(((gui_settings_t*)0)->enable_resample_b),'B',0,1},
    {"enable_audio_4ch",offsetof(gui_settings_t,enable_audio_4ch),sizeof(((gui_settings_t*)0)->enable_audio_4ch),'B',0,1},
    {"enable_audio_2ch_12",offsetof(gui_settings_t,enable_audio_2ch_12),sizeof(((gui_settings_t*)0)->enable_audio_2ch_12),'B',0,1},
    {"enable_audio_2ch_34",offsetof(gui_settings_t,enable_audio_2ch_34),sizeof(((gui_settings_t*)0)->enable_audio_2ch_34),'B',0,1},
    {"misrc_mode",offsetof(gui_settings_t,misrc_mode),sizeof(((gui_settings_t*)0)->misrc_mode),'B',0,1},
    {"misrc_v15_v25_ab_swap",offsetof(gui_settings_t,misrc_v15_v25_ab_swap),sizeof(((gui_settings_t*)0)->misrc_v15_v25_ab_swap),'B',0,1},
    /* Audio Mon remains in local-profile snapshots, but is explicitly
     * excluded from Monitor network synchronization in the transport. */
    {"audio_monitor_playback",offsetof(gui_settings_t,audio_monitor_playback),sizeof(((gui_settings_t*)0)->audio_monitor_playback),'B',0,1},
    {"audio_monitor_ch34",offsetof(gui_settings_t,audio_monitor_ch34),sizeof(((gui_settings_t*)0)->audio_monitor_ch34),'B',0,1},
    {"level_autostop_enabled",offsetof(gui_settings_t,level_autostop_enabled),sizeof(((gui_settings_t*)0)->level_autostop_enabled),'B',0,1},
    {"enable_audio_1ch_1",offsetof(gui_settings_t,enable_audio_1ch[0]),sizeof(((gui_settings_t*)0)->enable_audio_1ch[0]),'B',0,1},
    {"enable_audio_1ch_2",offsetof(gui_settings_t,enable_audio_1ch[1]),sizeof(((gui_settings_t*)0)->enable_audio_1ch[1]),'B',0,1},
    {"enable_audio_1ch_3",offsetof(gui_settings_t,enable_audio_1ch[2]),sizeof(((gui_settings_t*)0)->enable_audio_1ch[2]),'B',0,1},
    {"enable_audio_1ch_4",offsetof(gui_settings_t,enable_audio_1ch[3]),sizeof(((gui_settings_t*)0)->enable_audio_1ch[3]),'B',0,1},
    {"rf_bits_a",offsetof(gui_settings_t,rf_bits_a),sizeof(((gui_settings_t*)0)->rf_bits_a),'I',8,16},
    {"rf_bits_b",offsetof(gui_settings_t,rf_bits_b),sizeof(((gui_settings_t*)0)->rf_bits_b),'I',8,16},
    {"flac_level",offsetof(gui_settings_t,flac_level),sizeof(((gui_settings_t*)0)->flac_level),'I',0,8},
    {"flac_threads",offsetof(gui_settings_t,flac_threads),sizeof(((gui_settings_t*)0)->flac_threads),'I',1,32},
    {"resample_quality_a",offsetof(gui_settings_t,resample_quality_a),sizeof(((gui_settings_t*)0)->resample_quality_a),'I',0,4},
    {"resample_quality_b",offsetof(gui_settings_t,resample_quality_b),sizeof(((gui_settings_t*)0)->resample_quality_b),'I',0,4},
    {"rf_preview_budget_percent",offsetof(gui_settings_t,rf_preview_budget_percent),sizeof(((gui_settings_t*)0)->rf_preview_budget_percent),'I',50,300},
    {"capture_limit_seconds",offsetof(gui_settings_t,capture_limit_seconds),sizeof(((gui_settings_t*)0)->capture_limit_seconds),'U',0,864000},
    {"record_limit_seconds",offsetof(gui_settings_t,record_limit_seconds),sizeof(((gui_settings_t*)0)->record_limit_seconds),'U',0,864000},
    {"resample_rate_a",offsetof(gui_settings_t,resample_rate_a),sizeof(((gui_settings_t*)0)->resample_rate_a),'F',1.0,100000.0},
    {"resample_rate_b",offsetof(gui_settings_t,resample_rate_b),sizeof(((gui_settings_t*)0)->resample_rate_b),'F',1.0,100000.0},
    {"resample_gain_a",offsetof(gui_settings_t,resample_gain_a),sizeof(((gui_settings_t*)0)->resample_gain_a),'F',-48.0,48.0},
    {"resample_gain_b",offsetof(gui_settings_t,resample_gain_b),sizeof(((gui_settings_t*)0)->resample_gain_b),'F',-48.0,48.0},
    {"level_autostop_vpp",offsetof(gui_settings_t,level_autostop_vpp),sizeof(((gui_settings_t*)0)->level_autostop_vpp),'F',0.5,4.0},
    {"level_autostop_vpp_hsdaoh",offsetof(gui_settings_t,level_autostop_vpp_hsdaoh),sizeof(((gui_settings_t*)0)->level_autostop_vpp_hsdaoh),'F',0.5,4.0},
    {"output_base_name",offsetof(gui_settings_t,output_base_name),sizeof(((gui_settings_t*)0)->output_base_name),'S',0,0},
    {"output_filename_a",offsetof(gui_settings_t,output_filename_a),sizeof(((gui_settings_t*)0)->output_filename_a),'S',0,0},
    {"output_filename_b",offsetof(gui_settings_t,output_filename_b),sizeof(((gui_settings_t*)0)->output_filename_b),'S',0,0},
    {"capture_time",offsetof(gui_settings_t,capture_time),sizeof(((gui_settings_t*)0)->capture_time),'S',0,0},
    {"audio_4ch_filename",offsetof(gui_settings_t,audio_4ch_filename),sizeof(((gui_settings_t*)0)->audio_4ch_filename),'S',0,0},
    {"audio_2ch_12_filename",offsetof(gui_settings_t,audio_2ch_12_filename),sizeof(((gui_settings_t*)0)->audio_2ch_12_filename),'S',0,0},
    {"audio_2ch_34_filename",offsetof(gui_settings_t,audio_2ch_34_filename),sizeof(((gui_settings_t*)0)->audio_2ch_34_filename),'S',0,0},
    {"level_autostop_level_str",offsetof(gui_settings_t,level_autostop_level_str),sizeof(((gui_settings_t*)0)->level_autostop_level_str),'S',0,0},
    {"level_autostop_duration_str",offsetof(gui_settings_t,level_autostop_duration_str),sizeof(((gui_settings_t*)0)->level_autostop_duration_str),'S',0,0},
    {"ingest_project",offsetof(gui_settings_t,ingest_project),sizeof(((gui_settings_t*)0)->ingest_project),'S',0,0},
    {"ingest_tape_id",offsetof(gui_settings_t,ingest_tape_id),sizeof(((gui_settings_t*)0)->ingest_tape_id),'S',0,0},
    {"ingest_tape_format",offsetof(gui_settings_t,ingest_tape_format),sizeof(((gui_settings_t*)0)->ingest_tape_format),'S',0,0},
    {"ingest_tape_size",offsetof(gui_settings_t,ingest_tape_size),sizeof(((gui_settings_t*)0)->ingest_tape_size),'S',0,0},
    {"ingest_tape_speed",offsetof(gui_settings_t,ingest_tape_speed),sizeof(((gui_settings_t*)0)->ingest_tape_speed),'S',0,0},
    {"ingest_tape_condition",offsetof(gui_settings_t,ingest_tape_condition),sizeof(((gui_settings_t*)0)->ingest_tape_condition),'S',0,0},
    {"ingest_operator",offsetof(gui_settings_t,ingest_operator),sizeof(((gui_settings_t*)0)->ingest_operator),'S',0,0},
    {"ingest_location",offsetof(gui_settings_t,ingest_location),sizeof(((gui_settings_t*)0)->ingest_location),'S',0,0},
    {"ingest_notes",offsetof(gui_settings_t,ingest_notes),sizeof(((gui_settings_t*)0)->ingest_notes),'S',0,0},
    {"rf_tag_0",offsetof(gui_settings_t,rf_channel_tags[0]),sizeof(((gui_settings_t*)0)->rf_channel_tags[0]),'S',0,0},
    {"rf_tag_1",offsetof(gui_settings_t,rf_channel_tags[1]),sizeof(((gui_settings_t*)0)->rf_channel_tags[1]),'S',0,0},
    {"audio_label_0",offsetof(gui_settings_t,audio_1ch_labels[0]),sizeof(((gui_settings_t*)0)->audio_1ch_labels[0]),'S',0,0},
    {"audio_file_0",offsetof(gui_settings_t,audio_1ch_filenames[0]),sizeof(((gui_settings_t*)0)->audio_1ch_filenames[0]),'S',0,0},
    {"audio_label_1",offsetof(gui_settings_t,audio_1ch_labels[1]),sizeof(((gui_settings_t*)0)->audio_1ch_labels[1]),'S',0,0},
    {"audio_file_1",offsetof(gui_settings_t,audio_1ch_filenames[1]),sizeof(((gui_settings_t*)0)->audio_1ch_filenames[1]),'S',0,0},
    {"audio_label_2",offsetof(gui_settings_t,audio_1ch_labels[2]),sizeof(((gui_settings_t*)0)->audio_1ch_labels[2]),'S',0,0},
    {"audio_file_2",offsetof(gui_settings_t,audio_1ch_filenames[2]),sizeof(((gui_settings_t*)0)->audio_1ch_filenames[2]),'S',0,0},
    {"audio_label_3",offsetof(gui_settings_t,audio_1ch_labels[3]),sizeof(((gui_settings_t*)0)->audio_1ch_labels[3]),'S',0,0},
    {"audio_file_3",offsetof(gui_settings_t,audio_1ch_filenames[3]),sizeof(((gui_settings_t*)0)->audio_1ch_filenames[3]),'S',0,0},
    {"audio_tag_0",offsetof(gui_settings_t,audio_output_tags[0]),sizeof(((gui_settings_t*)0)->audio_output_tags[0]),'S',0,0},
    {"audio_tag_1",offsetof(gui_settings_t,audio_output_tags[1]),sizeof(((gui_settings_t*)0)->audio_output_tags[1]),'S',0,0},
    {"audio_tag_2",offsetof(gui_settings_t,audio_output_tags[2]),sizeof(((gui_settings_t*)0)->audio_output_tags[2]),'S',0,0},
};
size_t gui_monitor_settings_count(void){return sizeof(fields)/sizeof(fields[0]);}
const char *gui_monitor_settings_key(size_t i){return i<gui_monitor_settings_count()?fields[i].key:NULL;}
bool gui_monitor_settings_get(const gui_settings_t *s,size_t i,char *out,size_t cap){
 if(!s||i>=gui_monitor_settings_count()||!out||!cap)return false;
 const desc_t *d=&fields[i];const unsigned char *p=(const unsigned char *)s+d->offset;
 switch(d->type){
 case 'B':snprintf(out,cap,"%u",*(const bool*)p?1u:0u);break;
 case 'I':{
   unsigned n=0;if(d->size==1)n=*p;else if(d->size==sizeof(int))n=(unsigned)*(const int*)p;
   snprintf(out,cap,"%u",n);break;}
 case 'U':snprintf(out,cap,"%u",*(const uint32_t*)p);break;
 case 'F':snprintf(out,cap,"%.7g",(double)*(const float*)p);break;
 case 'S':{
   size_t n=strnlen((const char*)p,d->size);
   if(n>=d->size||n>=cap)return false;
   memcpy(out,p,n);out[n]=0;break;}
 default:return false;
 }
 return strlen(out)<cap;
}
bool gui_monitor_settings_set(gui_settings_t *s,const char *key,const char *value){
 if(!s||!key||!value)return false;
 const desc_t *d=NULL;
 for(size_t i=0;i<gui_monitor_settings_count();i++)if(strcmp(fields[i].key,key)==0){d=&fields[i];break;}
 if(!d)return false;
 unsigned char *p=(unsigned char*)s+d->offset;
 if(d->type=='S'){
   size_t n=strlen(value);
   if(n>=d->size)return false;
   for(size_t i=0;i<n;i++)if((unsigned char)value[i]<32||value[i]==127)return false;
   memcpy(p,value,n+1);return true;
 }
 if(!*value)return false;
 char *end=NULL;errno=0;
 double v=strtod(value,&end);
 if(errno||end==value||*end||!isfinite(v)||v<d->lo||v>d->hi)return false;
 if(d->type!='F'&&floor(v)!=v)return false;
 if((!strcmp(key,"rf_bits_a")||!strcmp(key,"rf_bits_b")) && v!=8 && v!=12 && v!=16)return false;
 switch(d->type){
 case 'B':if(v!=0&&v!=1)return false;*(bool*)p=(v!=0);return true;
 case 'I':if(d->size==1)*p=(uint8_t)v;else if(d->size==sizeof(int))*(int*)p=(int)v;else return false;return true;
 case 'U':*(uint32_t*)p=(uint32_t)v;return true;
 case 'F':*(float*)p=(float)v;return true;
 }
 return false;
}
void gui_monitor_settings_copy(gui_settings_t *dst,const gui_settings_t *src){
 if(!dst||!src)return;
 for(size_t i=0;i<gui_monitor_settings_count();i++){
   const desc_t *d=&fields[i];
   memcpy((char*)dst+d->offset,(const char*)src+d->offset,d->size);
 }
}
