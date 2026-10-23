#ifndef SCOS_CAT_H
#define SCOS_CAT_H
#include "scos.h"
#define CAT_HEADER 128u
#define CAT_SOURCE_MAX 65536u
#define CAT_PERMISSION_THEME 1u
struct cat_info { char id[32],title[40]; u32 width,height,permissions; const char *source; u32 length; };
int cat_validate(const void *,u32,struct cat_info *,char *,size_t);
int cat_project_save(const char *,const struct cat_info *,char *,size_t);
int cat_project_load(const void *,u32,struct cat_info *,char *,size_t);
int cat_build(const char *,const struct cat_info *,char *,size_t);
int lua_source_check(const char *,u32,char *,size_t);
#endif
