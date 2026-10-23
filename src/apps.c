/* Application registry, independent of compositor implementation and app list.
 * Built-ins self-register at link time; file-backed clients register at runtime. */
#include "cat.h"
#define APP_LIMIT 64
static struct app *registry[APP_LIMIT];
static int reg_count;
extern struct app * const __scos_apps_start[], * const __scos_apps_end[];
int app_register(struct app *a)
{
    if(!a||!a->id||!a->title||!a->id[0]||strlen(a->id)>31||strlen(a->title)>39||(a->desktop_label&&strlen(a->desktop_label)>39)||reg_count>=APP_LIMIT)return 0;
    for(int i=0;i<reg_count;i++)if(!strcmp(registry[i]->id,a->id))return registry[i]==a;
    registry[reg_count++]=a;return 1;
}
int app_unregister(struct app *a){for(int i=0;i<reg_count;i++)if(registry[i]==a){for(int j=i;j<reg_count-1;j++)registry[j]=registry[j+1];reg_count--;return 1;}return 0;}
static char uninstall_id[32];
static void uninstall_answer(int ok,const char *text,void *ud){(void)text;(void)ud;if(!ok)return;char message[256];int done=app_uninstall(uninstall_id,message,sizeof(message));wm_notify(done?"Application removed":"Uninstall failed",message,!done);}
void app_request_uninstall(const char *id){
    if(!id||strlen(id)>30||wm_dialog_active())return;
    struct app *a=app_find(id);if(!a||!a->external){wm_notify("Cannot uninstall","Built-in system applications cannot be uninstalled.",1);return;}
    strcpy(uninstall_id,id);char prompt[256];strcpy(prompt,"Uninstall ");strcat(prompt,id);strcat(prompt,"?\nCloses its windows; removes package/shortcuts.\nProject source and app data are kept.");
    if(fs_image_available()){strcat(prompt,"\nSaves current filesystem to:\n");strncat(prompt,fs_image_target(),40);}else strcat(prompt,"\nSession RAM only; no disk will be written.");
    wm_dialog("Uninstall application",prompt,NULL,uninstall_answer,NULL);
}
void apps_register_all(void)
{
    reg_count=0;
    for(struct app * const *p=__scos_apps_start;p<__scos_apps_end;p++)
        if(!app_register(*p))panic("invalid native app registration");
    lua_apps_refresh();
}
struct app *app_find(const char *id)
{
    if(!id)return NULL;
    for(int i=0;i<reg_count;i++)if(!strcmp(registry[i]->id,id))return registry[i];
    return lua_app_install(id);
}
int app_count(void){return reg_count;}
struct app *app_at(int i){return i>=0&&i<reg_count?registry[i]:NULL;}

/* One native modal at a time; copy the path rather than retaining a caller pointer. */
static char install_path[192];
static void install_answer(int ok,const char *text,void *ud)
{
    (void)text;(void)ud;if(!ok)return;
    u32 n=0;char *bytes=vfs_read(install_path,&n),err[256];struct cat_info m;
    if(!cat_validate(bytes,n,&m,err,sizeof(err))||!lua_source_check(m.source,m.length,err,sizeof(err))){wm_error_popup(err);return;}
    char dest[96];strcpy(dest,"home/apps/");strcat(dest,m.id);strcat(dest,".cat");
    for(int i=0;i<reg_count;i++)if(!strcmp(registry[i]->id,m.id)){wm_error_popup("Application ID already registered.\nNo files were replaced.");return;}
    if(vfs_lookup(dest)){wm_error_popup("Package already exists in home/apps.\nOpen that copy; no files replaced.");return;}
    if(!vfs_lookup("home/apps"))vfs_mkdir("home/apps");
    if(!vfs_write(dest,bytes,n)){wm_error_popup("Package install failed: cannot write RAM file.");return;}
    struct app *a=lua_app_install(dest);
    if(!a){vfs_delete(dest);wm_error_popup("Package registration failed. Install rolled back.");return;}
    wm_desktop_install(a->id);
    int persistent=fs_image_available(),saved=persistent?fs_image_save():0;
    wm_notify(saved?"Application installed":"Session installation",saved?"Installed and saved to supported disk.\nOpen it from Desktop or the launcher.":persistent?"Installed in RAM, but disk save FAILED.\nUse Terminal save before restarting.":"Installed in RAM: Desktop and launcher.\nNo supported persistence disk.\nReboot will lose this installation.",persistent&&!saved);
}

struct window *app_open_document(const char *path)
{
    size_t n=strlen(path);
    if(n>=4&&!strcmp(path+n-4,".cat")){
        const char *relative=path[0]=='/'?path+1:path;
        if(strncmp(relative,"home/apps/",10)){
            if(wm_dialog_active())return NULL;
            if(strlen(path)>=sizeof(install_path)){wm_error_popup("Package path too long");return NULL;}
            u32 size=0;char *bytes=vfs_read(path,&size),err[256];struct cat_info m;
            if(!cat_validate(bytes,size,&m,err,sizeof(err))){wm_error_popup(err);return NULL;}
            strcpy(install_path,path);
            char prompt[256];strcpy(prompt,fs_image_available()?"Install package and desktop shortcut?":"Install for this session?");
            if(fs_image_available()){strcat(prompt,"\nSaves current filesystem to:\n");strncat(prompt,fs_image_target(),48);strcat(prompt,"\nMay differ from boot USB.");}
            else strcat(prompt,"\nTemporary RAM package and shortcuts.\nNothing is saved to disk; lost on reboot.");
            strcat(prompt,"\nOnly install trusted packages.");
            wm_dialog("Install CAT application",prompt,NULL,install_answer,NULL);
            return NULL;
        }
        struct app *a=lua_app_install(path);
        if(a)return wm_open_app(a->id,NULL);
        wm_error_popup("Cannot open CAT package.\nInvalid format, conflicting ID,\nor application registry full.");return NULL;
    }
    for(int i=0;i<reg_count;i++)if(registry[i]->file_suffix){
        size_t len=strlen(registry[i]->file_suffix);
        if(n>=len&&!strcmp(path+n-len,registry[i]->file_suffix))return wm_open_app(registry[i]->id,(void *)path);
    }
    for(int i=0;i<reg_count;i++)if(registry[i]->file_editor)
        return wm_open_app(registry[i]->id,(void *)path);
    return NULL;
}
