/* Editable appearance data; code for system applications stays native. */
#include "scos.h"
#define CUSTOM_THEMES 8
static struct {struct theme palette;char id[32];u32 bg[3];} custom[CUSTOM_THEMES];
static int count;
int theme_custom_count(void){return count;}
const struct theme *theme_custom_get(int i){return i>=0&&i<count?&custom[i].palette:NULL;}
int theme_id_valid(const char *s){size_t n=strlen(s);if(n<6||n>30||strncmp(s,"user-",5))return 0;for(size_t i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='-'))return 0;return 1;}
int theme_custom_store(const char *id,const u32 values[10],int save){
    if(!theme_id_valid(id))return 0;
    for(int i=0;i<7;i++)if(values[i]>0xffffff)return 0;
    if(values[7]>3||values[8]<8||values[8]>256||values[9]>0xffffff)return 0;
    int i;for(i=0;i<count;i++)if(!strcmp(custom[i].id,id))break;
    if(i==CUSTOM_THEMES)return 0;
    if(save){char path[96];u8 bytes[48];memcpy(bytes,"SCOSTH1\0",8);memcpy(bytes+8,values,40);strcpy(path,"home/themes/");strcat(path,id);strcat(path,".theme");if((!vfs_lookup("home/themes")&&!vfs_mkdir("home/themes"))||!vfs_write(path,(const char *)bytes,48))return 0;}
    strcpy(custom[i].id,id);custom[i].palette=(struct theme){custom[i].id,custom[i].id,values[0],values[1],values[2],values[3],values[4],values[5],values[6]};memcpy(custom[i].bg,values+7,12);
    if(i==count)count++;
    return 1;
}
void theme_custom_load(void){
    count=0;struct vfs_node *dir=vfs_lookup("home/themes");if(!dir||!dir->is_dir)return;
    for(struct vfs_node *n=dir->child;n;n=n->sibling){size_t len=strlen(n->name);if(n->is_dir||len<7||len>36||strcmp(n->name+len-6,".theme")||n->size!=48||memcmp(n->data,"SCOSTH1\0",8))continue;char id[32];memcpy(id,n->name,len-6);id[len-6]=0;u32 v[10];memcpy(v,n->data+8,40);theme_custom_store(id,v,0);}
}
void theme_values(const struct theme *t,u32 v[10]){
    v[0]=t->main;v[1]=t->bg_top;v[2]=t->bg_bot;v[3]=t->win_bg;v[4]=t->text;v[5]=t->title_text;v[6]=t->taskbar_bg;v[7]=2;v[8]=64;v[9]=color_blend(t->bg_top,t->main,14);
    for(int i=0;i<count;i++)if(t==&custom[i].palette){memcpy(v+7,custom[i].bg,12);break;}
}
