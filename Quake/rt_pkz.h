// .pkz (zip) archive support for the Q2RTX material system (phase 4.5).
// Every *.pkz found in the game directory is mounted with miniz and files can
// be loaded from them by name or listed (for materials/*.mat discovery).
// The host's miniz is built with MINIZ_NO_STDIO, so the archives are read into
// memory and opened with mz_zip_reader_init.

#ifndef RT_PKZ_H
#define RT_PKZ_H

#include "quakedef.h"

// Mounts every *.pkz found in com_gamedir (idempotent).
void RT_PKZ_Init(void);
// Closes all mounted archives.
void RT_PKZ_Shutdown(void);

// True if at least one pkz is mounted.
qboolean RT_PKZ_Active(void);

// Cheap existence check: searches the central directory (no decompression).
qboolean RT_PKZ_Exists(const char *name);

// Loads a file (case-insensitive, e.g. "textures/foo_base.tga") from any
// mounted archive, first match wins. Returns a heap buffer (free with
// Mem_Free) and its length, or NULL if not found.
byte *RT_PKZ_LoadFile(const char *name, int *outLen);

// Calls cb(name, ctx) for every entry whose name starts with dir (no leading
// slash) and ends with ext (e.g. ".mat"). Stops early if cb returns non-zero.
// Returns the number of matches found.
int RT_PKZ_ListFiles(const char *dir, const char *ext,
                     int (*cb)(const char *name, void *ctx), void *ctx);

// ---------------------------------------------------------------------------
// Native searchpath integration: mounted .pkz archives are exposed through the
// engine's file system (COM_OpenFile / COM_LoadFile / COM_FOpenFile), exactly
// like .PAK files. A mounted archive is referenced by an opaque void* that the
// host stores in searchpath_t::rt_pkz.
// ---------------------------------------------------------------------------

// Returns the uncompressed size of a file in one archive, or -1.
int RT_PKZ_FindFile(const void *archive, const char *name, int *outSize);

// Opens a file in one archive as a memory stream. Returns a handle that must
// be used with RT_PKZ_Read/Seek/Close (and is recognized by Sys_FileRead/
// Sys_FileSeek/Sys_FileClose via RT_PKZ_IsHandle). Returns -1 if not found.
int RT_PKZ_OpenFile(const void *archive, const char *name, int *outSize);

// Extracts a file to a temporary file and returns a FILE* (used by
// COM_FOpenFile which cannot read from memory). The temp file is deleted at
// RT_PKZ_Shutdown. Returns NULL if not found.
FILE *RT_PKZ_OpenFileAsFILE(const void *archive, const char *name);

qboolean RT_PKZ_IsHandle(int handle);
int RT_PKZ_Read(int handle, void *dest, int count);
void RT_PKZ_Seek(int handle, int position);
void RT_PKZ_Close(int handle);

#endif // RT_PKZ_H
