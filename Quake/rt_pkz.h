
#ifndef RT_PKZ_H
#define RT_PKZ_H

#include "quakedef.h"

void RT_PKZ_Init(void);
void RT_PKZ_Shutdown(void);

qboolean RT_PKZ_Active(void);

qboolean RT_PKZ_Exists(const char *name);

byte *RT_PKZ_LoadFile(const char *name, int *outLen);

int RT_PKZ_ListFiles(const char *dir, const char *ext,
                     int (*cb)(const char *name, void *ctx), void *ctx);

int RT_PKZ_FindFile(const void *archive, const char *name, int *outSize);

int RT_PKZ_OpenFile(const void *archive, const char *name, int *outSize);

FILE *RT_PKZ_OpenFileAsFILE(const void *archive, const char *name);

qboolean RT_PKZ_IsHandle(int handle);
int RT_PKZ_Read(int handle, void *dest, int count);
void RT_PKZ_Seek(int handle, int position);
void RT_PKZ_Close(int handle);

#endif
