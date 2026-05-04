/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
*/

#ifndef cJSON__h
#define cJSON__h

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define CJSON_VERSION_MAJOR 1
#define CJSON_VERSION_MINOR 7
#define CJSON_VERSION_PATCH 18

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;        // Deprecated
    double valuedouble;
    char *string;
} cJSON;

#define cJSON_Invalid  (0)
#define cJSON_False    (1 << 0)
#define cJSON_True     (1 << 1)
#define cJSON_NULL     (1 << 2)
#define cJSON_Number   (1 << 3)
#define cJSON_String   (1 << 4)
#define cJSON_Array    (1 << 5)
#define cJSON_Object   (1 << 6)
#define cJSON_Raw      (1 << 7)
#define cJSON_IsReference      256
#define cJSON_StringIsConst    512

#define cJSON_IsInvalid(object)  (((object)->type & 0xFF) == cJSON_Invalid)
#define cJSON_IsFalse(object)    (((object)->type & 0xFF) == cJSON_False)
#define cJSON_IsTrue(object)     (((object)->type & 0xFF) == cJSON_True)
#define cJSON_IsNull(object)     (((object)->type & 0xFF) == cJSON_NULL)
#define cJSON_IsNumber(object)   (((object)->type & 0xFF) == cJSON_Number)
#define cJSON_IsString(object)   (((object)->type & 0xFF) == cJSON_String)
#define cJSON_IsArray(object)    (((object)->type & 0xFF) == cJSON_Array)
#define cJSON_IsObject(object)   (((object)->type & 0xFF) == cJSON_Object)
#define cJSON_IsRaw(object)      (((object)->type & 0xFF) == cJSON_Raw)

extern cJSON *cJSON_Parse(const char *value);
extern char  *cJSON_Print(const cJSON *item);
extern char  *cJSON_PrintUnformatted(const cJSON *item);
extern void   cJSON_Delete(cJSON *item);
extern int    cJSON_GetArraySize(const cJSON *array);
extern cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
extern cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string);
extern cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string);
extern int    cJSON_HasObjectItem(const cJSON *object, const char *string);
extern char  *cJSON_GetErrorPtr(void);
extern char  *cJSON_GetStringValue(const cJSON * const item);
extern double cJSON_GetNumberValue(const cJSON * const item);
extern cJSON *cJSON_CreateNull(void);
extern cJSON *cJSON_CreateTrue(void);
extern cJSON *cJSON_CreateFalse(void);
extern cJSON *cJSON_CreateBool(int boolean);
extern cJSON *cJSON_CreateNumber(double num);
extern cJSON *cJSON_CreateString(const char *string);
extern cJSON *cJSON_CreateArray(void);
extern cJSON *cJSON_CreateObject(void);
extern int    cJSON_AddItemToArray(cJSON *array, cJSON *item);
extern int    cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
extern cJSON *cJSON_AddNullToObject(cJSON * const object, const char * const name);
extern cJSON *cJSON_AddTrueToObject(cJSON * const object, const char * const name);
extern cJSON *cJSON_AddFalseToObject(cJSON * const object, const char * const name);
extern cJSON *cJSON_AddBoolToObject(cJSON * const object, const char * const name, const int boolean);
extern cJSON *cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number);
extern cJSON *cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string);

#define cJSON_ArrayForEach(element, array) \
    for(element = (array != NULL) ? (array)->child : NULL; element != NULL; element = element->next)

#ifdef __cplusplus
}
#endif

#endif /* cJSON__h */

// ─── IMPLEMENTATION (inclure dans un seul .cpp) ───────────────────────────────
#ifdef CJSON_IMPLEMENTATION
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

// Implementation complète de cJSON
// Source: https://github.com/DaveGamble/cJSON (MIT License)
// Incluse ici pour simplifier la compilation avec le SDK PocketBook.
// Pour un projet complet, préférez utiliser cJSON.c séparément.

static const char *ep = NULL;
char *cJSON_GetErrorPtr(void) { return (char*)ep; }

static int cJSON_strcasecmp(const char *s1, const char *s2) {
    if (!s1) return (s1==s2)?0:1;
    if (!s2) return 1;
    for (; tolower(*(const unsigned char*)s1)==tolower(*(const unsigned char*)s2); ++s1, ++s2)
        if (*s1==0) return 0;
    return tolower(*(const unsigned char*)s1) - tolower(*(const unsigned char*)s2);
}

static void *cJSON_malloc(size_t sz)  { return malloc(sz); }
static void  cJSON_free(void *ptr)    { free(ptr); }

static char* cJSON_strdup(const char* str) {
    size_t len;
    char* copy;
    len = strlen(str) + 1;
    if (!(copy = (char*)cJSON_malloc(len))) return 0;
    memcpy(copy, str, len);
    return copy;
}

static cJSON *cJSON_New_Item(void) {
    cJSON* node = (cJSON*)cJSON_malloc(sizeof(cJSON));
    if (node) memset(node, 0, sizeof(cJSON));
    return node;
}

void cJSON_Delete(cJSON *c) {
    cJSON *next;
    while (c) {
        next = c->next;
        if (!(c->type & cJSON_IsReference) && c->child) cJSON_Delete(c->child);
        if (!(c->type & cJSON_IsReference) && c->valuestring) cJSON_free(c->valuestring);
        if (!(c->type & cJSON_StringIsConst) && c->string) cJSON_free(c->string);
        cJSON_free(c);
        c = next;
    }
}

static const char *skip(const char *in) {
    while (in && *in && (unsigned char)*in<=32) in++;
    return in;
}

static const char *parse_value(cJSON *item, const char *value);
static const char *parse_string(cJSON *item, const char *str) {
    const char *ptr = str+1; char *ptr2; char *out; int len=0;
    unsigned uc,uc2;
    if (*str!='\"') { ep=str; return 0; }
    while (*ptr!='\"' && *ptr && ++len) if (*ptr++ == '\\') ptr++;
    if (!(out=(char*)cJSON_malloc(len+1))) return 0;
    ptr=str+1; ptr2=out;
    while (*ptr!='\"' && *ptr) {
        if (*ptr!='\\') *ptr2++=*ptr++;
        else {
            ptr++;
            switch (*ptr) {
                case 'b': *ptr2++='\b'; break;
                case 'f': *ptr2++='\f'; break;
                case 'n': *ptr2++='\n'; break;
                case 'r': *ptr2++='\r'; break;
                case 't': *ptr2++='\t'; break;
                case 'u':
                    sscanf(ptr+1,"%4x",&uc); ptr+=4;
                    if ((uc>=0xDC00 && uc<=0xDFFF) || uc==0) break;
                    if (uc>=0xD800 && uc<=0xDBFF) {
                        if (ptr[1]!='\\' || ptr[2]!='u') break;
                        sscanf(ptr+3,"%4x",&uc2); ptr+=6;
                        uc=0x10000+((uc&0x3FF)<<10)+(uc2&0x3FF);
                    }
                    len=4; if (uc<0x80) len=1;
                    else if (uc<0x800) len=2;
                    else if (uc<0x10000) len=3;
                    ptr2+=len;
                    switch(len) {
                        case 4: *--ptr2=((uc|0x80)&0xBF); uc>>=6; /* fall through */
                        case 3: *--ptr2=((uc|0x80)&0xBF); uc>>=6; /* fall through */
                        case 2: *--ptr2=((uc|0x80)&0xBF); uc>>=6; /* fall through */
                        case 1: *--ptr2=(uc|"\x00\xC0\xE0\xF0"[len-1]);
                    }
                    ptr2+=len; break;
                default: *ptr2++=*ptr; break;
            }
            ptr++;
        }
    }
    *ptr2=0;
    if (*ptr=='\"') ptr++;
    item->valuestring=out;
    item->type=cJSON_String;
    return ptr;
}
static const char *parse_number(cJSON *item, const char *num) {
    double n=0,sign=1,scale=0; int subscale=0,signsubscale=1;
    if (*num=='-') sign=-1,num++;
    if (*num=='0') num++;
    else if (*num>='1' && *num<='9') { do n=(n*10.0)+(*num++ -'0'); while (*num>='0' && *num<='9'); }
    if (*num=='.' && num[1]>='0' && num[1]<='9') {
        num++;
        do { n=(n*10.0)+(*num++ -'0'); scale--; } while (*num>='0' && *num<='9');
    }
    if (*num=='e' || *num=='E') {
        num++;
        if (*num=='+') num++;
        else if (*num=='-') signsubscale=-1, num++;
        while (*num>='0' && *num<='9') subscale=(subscale*10)+(*num++ -'0');
    }
    n=sign*n*pow(10.0,(scale+subscale*signsubscale));
    item->valuedouble=n;
    item->valueint=(int)n;
    item->type=cJSON_Number;
    return num;
}
static const char *parse_array(cJSON *item, const char *value) {
    cJSON *child;
    if (*value!='[') { ep=value; return 0; }
    item->type=cJSON_Array;
    value=skip(value+1);
    if (*value==']') return value+1;
    item->child=child=cJSON_New_Item();
    if (!item->child) return 0;
    value=skip(parse_value(child,skip(value)));
    if (!value) return 0;
    while (*value==',') {
        cJSON *new_item;
        if (!(new_item=cJSON_New_Item())) return 0;
        child->next=new_item; new_item->prev=child; child=new_item;
        value=skip(parse_value(child,skip(value+1)));
        if (!value) return 0;
    }
    if (*value==']') return value+1;
    ep=value; return 0;
}
static const char *parse_object(cJSON *item, const char *value) {
    cJSON *child;
    if (*value!='{') { ep=value; return 0; }
    item->type=cJSON_Object;
    value=skip(value+1);
    if (*value=='}') return value+1;
    item->child=child=cJSON_New_Item();
    if (!item->child) return 0;
    value=skip(parse_string(child,skip(value)));
    if (!value) return 0;
    child->string=child->valuestring; child->valuestring=0;
    if (*value!=':') { ep=value; return 0; }
    value=skip(parse_value(child,skip(value+1)));
    if (!value) return 0;
    while (*value==',') {
        cJSON *new_item;
        if (!(new_item=cJSON_New_Item())) return 0;
        child->next=new_item; new_item->prev=child; child=new_item;
        value=skip(parse_string(child,skip(value+1)));
        if (!value) return 0;
        child->string=child->valuestring; child->valuestring=0;
        if (*value!=':') { ep=value; return 0; }
        value=skip(parse_value(child,skip(value+1)));
        if (!value) return 0;
    }
    if (*value=='}') return value+1;
    ep=value; return 0;
}
static const char *parse_value(cJSON *item, const char *value) {
    if (!value) return 0;
    if (!strncmp(value,"null",4)) { item->type=cJSON_NULL; return value+4; }
    if (!strncmp(value,"false",5)) { item->type=cJSON_False; return value+5; }
    if (!strncmp(value,"true",4))  { item->type=cJSON_True; item->valueint=1; return value+4; }
    if (*value=='\"') return parse_string(item,value);
    if (*value=='-' || (*value>='0' && *value<='9')) return parse_number(item,value);
    if (*value=='[') return parse_array(item,value);
    if (*value=='{') return parse_object(item,value);
    ep=value; return 0;
}
cJSON *cJSON_Parse(const char *value) {
    cJSON *c=cJSON_New_Item();
    ep=0;
    if (!c) return 0;
    if (!parse_value(c,skip(value))) { cJSON_Delete(c); return 0; }
    return c;
}
static char *print_value(const cJSON *item, int depth, int fmt);
static char *print_string_ptr(const char *str) {
    const char *ptr; char *ptr2,*out; int len=0;
    unsigned char token;
    if (!str) return cJSON_strdup("");
    ptr=str;
    while ((token=*ptr) && ++len) {
        if (strchr("\"\\\b\f\n\r\t",token)) len++;
        else if (token<32) len+=5;
        ptr++;
    }
    if (!(out=(char*)cJSON_malloc(len+3))) return 0;
    ptr2=out; ptr=str;
    *ptr2++='\"';
    while (*ptr) {
        if ((unsigned char)*ptr>31 && *ptr!='\"' && *ptr!='\\') *ptr2++=*ptr++;
        else {
            *ptr2++='\\';
            switch (token=*ptr++) {
                case '\\': *ptr2++='\\'; break;
                case '\"': *ptr2++='\"'; break;
                case '\b': *ptr2++='b'; break;
                case '\f': *ptr2++='f'; break;
                case '\n': *ptr2++='n'; break;
                case '\r': *ptr2++='r'; break;
                case '\t': *ptr2++='t'; break;
                default: sprintf(ptr2,"u%04x",token); ptr2+=5; break;
            }
        }
    }
    *ptr2++='\"'; *ptr2++=0;
    return out;
}
static char *print_number(const cJSON *item) {
    char *str=(char*)cJSON_malloc(64);
    if (str) {
        if (fabs(item->valuedouble) <= (double)INT_MAX &&
            item->valuedouble == (double)item->valueint)
            sprintf(str,"%d",item->valueint);
        else {
            if (fabs(item->valuedouble)<1e-6 || fabs(item->valuedouble)>1e9)
                sprintf(str,"%e",item->valuedouble);
            else sprintf(str,"%f",item->valuedouble);
        }
    }
    return str;
}
static char *print_object(const cJSON *item, int depth, int fmt) {
    char **entries=0,**names=0; char *out=0,*ptr,*ret,*str; int len=7,i=0,j;
    cJSON *child=item->child; int numentries=0,fail=0; size_t tmplen=0;
    while (child) numentries++,child=child->next;
    if (!numentries) { out=(char*)cJSON_malloc(fmt?depth+4:3); if(out){if(fmt){out[0]='{';for(i=0;i<depth+1;i++) out[i+1]='\n';out[depth+2]='}';out[depth+3]=0;}else strcpy(out,"{}");}return out;}
    entries=(char**)cJSON_malloc(numentries*sizeof(char*));
    if (!entries) return 0;
    names=(char**)cJSON_malloc(numentries*sizeof(char*));
    if (!names){cJSON_free(entries);return 0;}
    memset(entries,0,sizeof(char*)*numentries);
    memset(names,0,sizeof(char*)*numentries);
    child=item->child; depth++;
    while (child && !fail) {
        names[i]=str=print_string_ptr(child->string);
        entries[i++]=ret=print_value(child,depth,fmt);
        if (str && ret) len+=strlen(ret)+strlen(str)+2+(fmt?2+depth:0);
        else fail=1;
        child=child->next;
    }
    if (!fail) {
        out=(char*)cJSON_malloc(len);
        if (!out) fail=1;
    }
    if (fail) {
        for(i=0;i<numentries;i++){if(names[i])cJSON_free(names[i]);if(entries[i])cJSON_free(entries[i]);}
        cJSON_free(names);cJSON_free(entries);return 0;
    }
    *out='{'; ptr=out+1; if(fmt)*ptr++='\n'; *ptr=0;
    for (i=0;i<numentries;i++) {
        if(fmt) for(j=0;j<depth;j++) *ptr++='\t';
        tmplen=strlen(names[i]); memcpy(ptr,names[i],tmplen); ptr+=tmplen;
        *ptr++=':'; if(fmt) *ptr++='\t';
        strcpy(ptr,entries[i]); ptr+=strlen(entries[i]);
        if (i!=numentries-1) *ptr++=',';
        if(fmt) *ptr++='\n'; *ptr=0;
        cJSON_free(names[i]); cJSON_free(entries[i]);
    }
    cJSON_free(names); cJSON_free(entries);
    if(fmt) for(i=0;i<depth-1;i++) *ptr++='\t';
    *ptr++='}'; *ptr++=0;
    return out;
}
static char *print_array(const cJSON *item, int depth, int fmt) {
    char **entries; char *out=0,*ptr,*ret; int len=5; cJSON *child=item->child;
    int numentries=0,i=0,fail=0; size_t tmplen=0;
    while (child) numentries++,child=child->next;
    if (!numentries) { out=(char*)cJSON_malloc(3); if(out) strcpy(out,"[]"); return out; }
    entries=(char**)cJSON_malloc(numentries*sizeof(char*));
    if (!entries) return 0;
    memset(entries,0,numentries*sizeof(char*));
    child=item->child;
    while (child && !fail) {
        ret=print_value(child,depth+1,fmt);
        entries[i++]=ret;
        if (ret) len+=strlen(ret)+2+(fmt?1:0);
        else fail=1;
        child=child->next;
    }
    if (!fail) out=(char*)cJSON_malloc(len);
    if (!out) fail=1;
    if (fail) { for(i=0;i<numentries;i++) if(entries[i]) cJSON_free(entries[i]); cJSON_free(entries); return 0; }
    *out='['; ptr=out+1; *ptr=0;
    for (i=0;i<numentries;i++) {
        tmplen=strlen(entries[i]); memcpy(ptr,entries[i],tmplen); ptr+=tmplen;
        if (i!=numentries-1) { *ptr++=','; if(fmt) *ptr++=' '; *ptr=0; }
        cJSON_free(entries[i]);
    }
    cJSON_free(entries);
    *ptr++=']'; *ptr++=0;
    return out;
}
static char *print_value(const cJSON *item, int depth, int fmt) {
    char *out=0;
    if (!item) return 0;
    switch ((item->type)&0xFF) {
        case cJSON_NULL:   out=cJSON_strdup("null"); break;
        case cJSON_False:  out=cJSON_strdup("false"); break;
        case cJSON_True:   out=cJSON_strdup("true"); break;
        case cJSON_Number: out=print_number(item); break;
        case cJSON_String: out=print_string_ptr(item->valuestring); break;
        case cJSON_Array:  out=print_array(item,depth,fmt); break;
        case cJSON_Object: out=print_object(item,depth,fmt); break;
        default:           out=cJSON_strdup(""); break;
    }
    return out;
}
char *cJSON_Print(const cJSON *item)             { return print_value(item,0,1); }
char *cJSON_PrintUnformatted(const cJSON *item)  { return print_value(item,0,0); }
int   cJSON_GetArraySize(const cJSON *array)     { cJSON *c=array?array->child:0; int i=0; while(c) i++,c=c->next; return i; }
cJSON *cJSON_GetArrayItem(const cJSON *array,int item) {
    cJSON *c=array?array->child:0; while(c && item>0) item--,c=c->next; return c;
}
cJSON *cJSON_GetObjectItem(const cJSON * const o,const char * const s) {
    cJSON *c=o?o->child:0;
    while(c && cJSON_strcasecmp(c->string,s)) c=c->next;
    return c;
}
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON * const o,const char * const s) {
    const cJSON *c=o?o->child:0;
    while(c && strcmp(c->string,s)) c=c->next;
    return (cJSON*)c;
}
int cJSON_HasObjectItem(const cJSON *o,const char *s){ return cJSON_GetObjectItem(o,s)?1:0; }
char *cJSON_GetStringValue(const cJSON * const item) {
    if (!cJSON_IsString(item)) return NULL;
    return item->valuestring;
}
double cJSON_GetNumberValue(const cJSON * const item) {
    if (!cJSON_IsNumber(item)) return (double)0;
    return item->valuedouble;
}
static cJSON *cJSON_New_ItemTyped(int type) {
    cJSON* n=cJSON_New_Item(); if(n) n->type=type; return n;
}
cJSON *cJSON_CreateNull(void)            { return cJSON_New_ItemTyped(cJSON_NULL); }
cJSON *cJSON_CreateTrue(void)            { return cJSON_New_ItemTyped(cJSON_True); }
cJSON *cJSON_CreateFalse(void)           { return cJSON_New_ItemTyped(cJSON_False); }
cJSON *cJSON_CreateBool(int b)           { cJSON*i=cJSON_New_Item(); if(i){i->type=b?cJSON_True:cJSON_False;} return i; }
cJSON *cJSON_CreateNumber(double num)    { cJSON*i=cJSON_New_Item(); if(i){i->type=cJSON_Number;i->valuedouble=num;i->valueint=(int)num;} return i; }
cJSON *cJSON_CreateString(const char *s) { cJSON*i=cJSON_New_Item(); if(i){i->type=cJSON_String;i->valuestring=cJSON_strdup(s);if(!i->valuestring){cJSON_Delete(i);return 0;}} return i; }
cJSON *cJSON_CreateArray(void)           { return cJSON_New_ItemTyped(cJSON_Array); }
cJSON *cJSON_CreateObject(void)          { return cJSON_New_ItemTyped(cJSON_Object); }
static void cJSON_AddItemToArrayHead(cJSON *array, cJSON *item) {
    cJSON *c=array->child;
    item->next=c; if(c)c->prev=item; array->child=item;
}
int cJSON_AddItemToArray(cJSON *array,cJSON *item) {
    cJSON *c=array->child;
    if(!item) return 0;
    if(!c){array->child=item;return 1;}
    while(c&&c->next) c=c->next;
    c->next=item; item->prev=c;
    return 1;
}
int cJSON_AddItemToObject(cJSON *object,const char *string,cJSON *item) {
    if(!item) return 0;
    if(item->string) cJSON_free(item->string);
    item->string=cJSON_strdup(string);
    return cJSON_AddItemToArray(object,item);
}
cJSON *cJSON_AddNullToObject(cJSON * const o,const char * const n)    { cJSON*i=cJSON_CreateNull();    if(!cJSON_AddItemToObject(o,n,i)){cJSON_Delete(i);return NULL;} return i; }
cJSON *cJSON_AddTrueToObject(cJSON * const o,const char * const n)    { cJSON*i=cJSON_CreateTrue();    if(!cJSON_AddItemToObject(o,n,i)){cJSON_Delete(i);return NULL;} return i; }
cJSON *cJSON_AddFalseToObject(cJSON * const o,const char * const n)   { cJSON*i=cJSON_CreateFalse();   if(!cJSON_AddItemToObject(o,n,i)){cJSON_Delete(i);return NULL;} return i; }
cJSON *cJSON_AddBoolToObject(cJSON * const o,const char * const n,const int b) { cJSON*i=cJSON_CreateBool(b); if(!cJSON_AddItemToObject(o,n,i)){cJSON_Delete(i);return NULL;} return i; }
cJSON *cJSON_AddNumberToObject(cJSON * const o,const char * const n,const double num) { cJSON*i=cJSON_CreateNumber(num); if(!cJSON_AddItemToObject(o,n,i)){cJSON_Delete(i);return NULL;} return i; }
cJSON *cJSON_AddStringToObject(cJSON * const o,const char * const n,const char * const s) { cJSON*i=cJSON_CreateString(s); if(!cJSON_AddItemToObject(o,n,i)){cJSON_Delete(i);return NULL;} return i; }

#endif /* CJSON_IMPLEMENTATION */
