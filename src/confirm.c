/* Shared, default-no shell confirmations. State belongs to each shell/tab. */
#include "scos.h"

int confirm_answer(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char a[8]; int n = 0;
    while (*s && *s != ' ' && *s != '\t' && n < 7) {
        char c = *s++; a[n++] = c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    a[n] = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s) return -1;
    if (!n || !strcmp(a,"n") || !strcmp(a,"no")) return 0;
    if (!strcmp(a,"y") || !strcmp(a,"yes")) return 1;
    return -1;
}

static int has_option(const char *line, const char *option)
{
    unsigned n = strlen(option);
    while (*line) {
        while (*line == ' ') line++;
        if (!strncmp(line,option,n) && (!line[n] || line[n]==' ')) return 1;
        while (*line && *line!=' ') line++;
    }
    return 0;
}

int confirm_command(struct shell_confirm *c, char *line, unsigned cap,
                    char *message)
{
    if (c->command[0]) {
        int answer = confirm_answer(line);
        if (answer < 0) { strcpy(message,"Please answer y or n (Enter cancels)."); return 1; }
        if (!answer) { c->command[0] = 0; strcpy(message,"Cancelled."); return 1; }
        if (strlen(c->command) >= cap) {
            c->command[0] = 0; strcpy(message,"Confirmation command is too long."); return 1;
        }
        strcpy(line,c->command); c->command[0] = 0;
        return 0;
    }
    /* Power actions, privileged deletions and system stops require consent. */
    char normalized[256]; unsigned ni = 0; int space = 0;
    for (const char *p = line; *p && ni < sizeof(normalized)-1; p++) {
        if (*p == ' ' || *p == '\t') { if (ni) space = 1; continue; }
        if (space && ni < sizeof(normalized)-1) normalized[ni++] = ' ';
        space = 0;
        if (ni < sizeof(normalized)-1) normalized[ni++] = *p;
    }
    normalized[ni] = 0;
    const char *action = NULL;
    char storage_action[160];
    if (!strcmp(normalized,"save") && fs_image_available()) {
        strcpy(storage_action,"Overwrite the saved tree in the snapshot file: ");
        strncat(storage_action,fs_image_target(),40);
        strcat(storage_action,"\nThis may differ from the boot USB.");
        action=storage_action;
    }
    if (!strncmp(normalized,"kill --system ",14)) action = "Stopping scwm closes all GUI apps. Unsaved edits will be lost.";
    else if ((!strcmp(normalized,"reboot --confirm") || !strcmp(normalized,"reboot"))) action = "Reboot now? Unsaved changes may be lost.";
    else if ((!strcmp(normalized,"shutdown --confirm") || !strcmp(normalized,"poweroff --confirm") || !strcmp(normalized,"shutdown") || !strcmp(normalized,"poweroff"))) action = "Power off now? Unsaved changes may be lost.";
    else if (!strncmp(normalized,"rm ",3) &&
             (has_option(normalized,"-s") || has_option(normalized,"-f") || has_option(normalized,"-i")))
        action = "Delete this RAM-filesystem path? Unsaved data will be lost.";
    int removal=0;
    if(!strncmp(normalized,"appuninstall ",13)){
        const char *id=normalized+13;int valid=*id&&strlen(id)<=30;
        for(const char *p=id;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'))valid=0;
        struct app *a=valid?app_find(id):NULL;removal=a&&a->external;
    }
    if(removal){
        strcpy(storage_action,"Uninstall this app and close its windows? Project/data files stay.");
        if(fs_image_available()){strcat(storage_action,"\nSaves filesystem to: ");strncat(storage_action,fs_image_target(),36);}
        else strcat(storage_action,"\nSession RAM only; no disk write.");
        action=storage_action;
    }
    if (!action) return 0;
    if (strlen(line) >= sizeof(c->command)) {
        strcpy(message,"Command too long to confirm."); return 1;
    }
    strcpy(c->command,line);
    strcpy(message,action); strcat(message,"\nContinue? [y/N]");
    return 1;
}

int parse_pid(const char *s, int *pid)
{
    unsigned v = 0;
    if (!s || !*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9' || v > 100000) return 0;
        v = v * 10 + (*s++ - '0');
    }
    *pid = (int)v;
    return 1;
}
