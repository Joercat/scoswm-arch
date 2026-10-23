/* CAT1: bounded, checksummed source package. Never accept native Lua bytecode.
 * Integrity is not authenticity: CRC does not make a downloaded app trusted. */
#include "cat.h"
static int error(char *out,size_t cap,const char *s){if(cap){strncpy(out,s,cap-1);out[cap-1]=0;}return 0;}
static u32 get32(const u8 *p){return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static void put32(u8 *p,u32 v){for(int i=0;i<4;i++)p[i]=(u8)(v>>(8*i));}
static u32 checksum(const u8 *p,u32 n){u32 c=~0u;for(u32 i=0;i<n;i++){c^=(i>=16&&i<20)?0:p[i];for(int b=0;b<8;b++)c=(c>>1)^(0xedb88320u&-(c&1));}return ~c;}
static int name_ok(const char *s){size_t n=strlen(s);if(!n||n>30)return 0;for(size_t i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='-'))return 0;return 1;}
static int decode(const void *bytes,u32 size,struct cat_info *m,char *err,size_t cap,int project){
    const u8 *p=bytes;
    if(!p||size<CAT_HEADER||size>CAT_HEADER+CAT_SOURCE_MAX)return error(err,cap,"Invalid CAT package size");
    if(memcmp(p,project?"SCOSPRJ1":"SCOSCAT1",8)||get32(p+8)!=CAT_HEADER||get32(p+20)!=2)return error(err,cap,"Unsupported CAT format or API version");
    u32 n=get32(p+12);if((!n&&!project)||n!=size-CAT_HEADER)return error(err,cap,"Invalid CAT payload length");
    if(checksum(p,size)!=get32(p+16))return error(err,cap,"CAT checksum mismatch");
    for(int i=108;i<128;i++)if(p[i])return error(err,cap,"Unsupported CAT reserved fields");
    if(p[67]||p[107])return error(err,cap,"Unterminated CAT metadata");
    memset(m,0,sizeof(*m));memcpy(m->id,p+36,32);memcpy(m->title,p+68,40);
    if(!name_ok(m->id)||!m->title[0])return error(err,cap,"Invalid app ID or title");
    for(const char *s=m->title;*s;s++)if((u8)*s<32||(u8)*s>126)return error(err,cap,"Title must be printable ASCII");
    m->permissions=get32(p+24);m->width=get32(p+28);m->height=get32(p+32);
    if(m->permissions&~CAT_PERMISSION_THEME)return error(err,cap,"Unsupported CAT permission");
    if(m->width<320||m->width>2048||m->height<200||m->height>2048)return error(err,cap,"Invalid window dimensions");
    m->source=(const char *)p+CAT_HEADER;m->length=n;
    if(n&&(u8)m->source[0]==27)return error(err,cap,"Binary Lua chunks are not allowed");
    for(u32 i=0;i<n;i++)if(!m->source[i])return error(err,cap,"NUL in CAT source");
    if(cap)err[0]=0;
    return 1;
}
int cat_validate(const void *p,u32 n,struct cat_info *m,char *err,size_t cap){return decode(p,n,m,err,cap,0);}
int cat_project_load(const void *p,u32 n,struct cat_info *m,char *err,size_t cap){return decode(p,n,m,err,cap,1);}
static int write_package(const char *path,const struct cat_info *m,char *err,size_t cap,int project){
    size_t pl=path?strlen(path):0;
    if(pl<(project?9u:5u)||pl>=192||strcmp(path+pl-(project?8:4),project?".project":".cat"))return error(err,cap,"Output must be a .cat path shorter than 192 bytes");
    if(!name_ok(m->id)||!m->title[0]||strlen(m->title)>39||!m->source||(!m->length&&!project)||m->length>CAT_SOURCE_MAX)return error(err,cap,"Invalid project metadata or source size");
    if(!project&&!lua_source_check(m->source,m->length,err,cap))return 0;
    u32 size=CAT_HEADER+m->length;u8 *p=palloc(size);if(!p)return error(err,cap,"Out of memory building package");
    memset(p,0,CAT_HEADER);memcpy(p,project?"SCOSPRJ1":"SCOSCAT1",8);put32(p+8,CAT_HEADER);put32(p+12,m->length);put32(p+20,2);put32(p+24,m->permissions);put32(p+28,m->width);put32(p+32,m->height);
    strcpy((char *)p+36,m->id);strcpy((char *)p+68,m->title);memcpy(p+CAT_HEADER,m->source,m->length);put32(p+16,checksum(p,size));
    struct cat_info check;int ok=decode(p,size,&check,err,cap,project);
    if(ok&&!vfs_write(path,(const char *)p,size))ok=error(err,cap,"Cannot write package (check folder and free RAM)");
    pfree(p,size);if(ok&&cap){strncpy(err,project?"Project saved in RAM":"Build succeeded: verified CAT package saved in RAM",cap-1);err[cap-1]=0;}return ok;
}

int cat_build(const char *p,const struct cat_info *m,char *err,size_t n){return write_package(p,m,err,n,0);}
int cat_project_save(const char *p,const struct cat_info *m,char *err,size_t n){return write_package(p,m,err,n,1);}
