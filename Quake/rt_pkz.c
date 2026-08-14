// .pkz (zip) archive support for the Q2RTX material system (phase 4.5).

#include "quakedef.h"
#include "rt_pkz.h"
#include "sys.h"
#include "miniz.h"

#define RT_PKZ_MAX_ARCHIVES 8
#define RT_PKZ_MAX_STREAMS  64
#define RT_PKZ_HANDLE_BASE  100000
#define RT_PKZ_MAX_TEMP     64

typedef struct {
    char path[MAX_OSPATH];
    byte *data;          // whole archive read into memory
    size_t dataSize;
    mz_zip_archive zip;
    qboolean valid;
} rt_pkz_archive_t;

// memory streams returned by RT_PKZ_OpenFile
typedef struct {
    qboolean inUse;
    int archive;         // index into rt_pkz_archives
    int fileIndex;       // mz_zip file index
    byte *buf;           // lazily decompressed
    size_t size;
    size_t pos;
} rt_pkz_stream_t;

static rt_pkz_archive_t rt_pkz_archives[RT_PKZ_MAX_ARCHIVES];
static int rt_pkz_count = 0;
static rt_pkz_stream_t rt_pkz_streams[RT_PKZ_MAX_STREAMS];
static char rt_pkz_temp_files[RT_PKZ_MAX_TEMP][MAX_OSPATH];
static int rt_pkz_temp_count = 0;
static searchpath_t *rt_pkz_searchpaths[RT_PKZ_MAX_ARCHIVES];
static int rt_pkz_searchpath_count = 0;

// miniz 3.x (single header, MINIZ_NO_STDIO) reads through user callbacks.
// We give it the whole archive already loaded into memory.
static void *pkz_alloc(void *opaque, size_t items, size_t size)
{
    (void)opaque;
    return Mem_Alloc(items * size);
}

static void pkz_free(void *opaque, void *address)
{
    (void)opaque;
    Mem_Free(address);
}

static void *pkz_realloc(void *opaque, void *address, size_t items, size_t size)
{
    (void)opaque;
    return Mem_Realloc(address, items * size);
}

static size_t pkz_read(void *opaque, mz_uint64 file_ofs, void *pBuf, size_t n)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)opaque;
    if (file_ofs + n > a->dataSize)
    {
        return 0;
    }
    memcpy(pBuf, a->data + file_ofs, n);
    return n;
}

void RT_PKZ_Init(void)
{
    if (rt_pkz_count > 0)
    {
        return;
    }

    memset(rt_pkz_archives, 0, sizeof(rt_pkz_archives));
    rt_pkz_count = 0;

    // find *.pkz files directly in the game directory
    char pattern[MAX_OSPATH];
    q_snprintf(pattern, sizeof(pattern), "%s/*.pkz", com_gamedir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        if (rt_pkz_count >= RT_PKZ_MAX_ARCHIVES)
        {
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            continue;
        }

        char path[MAX_OSPATH];
        q_snprintf(path, sizeof(path), "%s/%s", com_gamedir, fd.cFileName);

        rt_pkz_archive_t *a = &rt_pkz_archives[rt_pkz_count];

        int handle;
        int size = Sys_FileOpenRead(path, &handle);
        if (size <= 0)
        {
            continue;
        }

        a->data = (byte *)Mem_Alloc(size);
        if (!a->data)
        {
            Sys_FileClose(handle);
            continue;
        }

        int rd = Sys_FileRead(handle, a->data, size);
        Sys_FileClose(handle);
        if (rd != size)
        {
            Mem_Free(a->data);
            continue;
        }

        a->dataSize = (size_t)size;
        memset(&a->zip, 0, sizeof(a->zip));
        a->zip.m_pAlloc = pkz_alloc;
        a->zip.m_pFree = pkz_free;
        a->zip.m_pRealloc = pkz_realloc;
        a->zip.m_pAlloc_opaque = a;
        a->zip.m_pRead = pkz_read;
        a->zip.m_pIO_opaque = a;
        a->zip.m_pNeeds_keepalive = NULL;
        if (mz_zip_reader_init(&a->zip, a->dataSize, 0))
        {
            q_strlcpy(a->path, path, sizeof(a->path));
            a->valid = true;
            rt_pkz_count++;
            Con_Printf("RT: mounted pkz %s (%u files)\n", path, (unsigned)a->zip.m_total_files);
        }
        else
        {
            Mem_Free(a->data);
            a->data = NULL;
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);

    // expose every mounted archive through the engine search path (like PAK)
    for (int i = 0; i < rt_pkz_count && rt_pkz_searchpath_count < RT_PKZ_MAX_ARCHIVES; i++)
    {
        rt_pkz_archive_t *a = &rt_pkz_archives[i];
        if (!a->valid)
        {
            continue;
        }

        searchpath_t *s = (searchpath_t *)Mem_Alloc(sizeof(searchpath_t));
        memset(s, 0, sizeof(*s));
        if (com_searchpaths)
        {
            s->path_id = com_searchpaths->path_id << 1;
        }
        else
        {
            s->path_id = 1U;
        }
        q_strlcpy(s->filename, a->path, sizeof(s->filename));
        s->rt_pkz = a;
        s->next = com_searchpaths;
        com_searchpaths = s;

        rt_pkz_searchpaths[rt_pkz_searchpath_count++] = s;
    }
}

void RT_PKZ_Shutdown(void)
{
    // remove pkz searchpaths from the engine search path
    for (int i = 0; i < rt_pkz_searchpath_count; i++)
    {
        searchpath_t *target = rt_pkz_searchpaths[i];
        searchpath_t **link;
        for (link = &com_searchpaths; *link; link = &(*link)->next)
        {
            if (*link == target)
            {
                *link = target->next;
                break;
            }
        }
        Mem_Free(target);
    }
    rt_pkz_searchpath_count = 0;

    for (int i = 0; i < rt_pkz_count; i++)
    {
        rt_pkz_archive_t *a = &rt_pkz_archives[i];
        if (a->valid)
        {
            mz_zip_reader_end(&a->zip);
        }
        if (a->data)
        {
            Mem_Free(a->data);
        }
    }
    rt_pkz_count = 0;

    for (int i = 0; i < RT_PKZ_MAX_STREAMS; i++)
    {
        rt_pkz_streams[i].inUse = false;
        if (rt_pkz_streams[i].buf)
        {
            Mem_Free(rt_pkz_streams[i].buf);
            rt_pkz_streams[i].buf = NULL;
        }
    }

    // delete extracted temp files
    for (int i = 0; i < rt_pkz_temp_count; i++)
    {
        remove(rt_pkz_temp_files[i]);
    }
    rt_pkz_temp_count = 0;
}

qboolean RT_PKZ_Active(void)
{
    return rt_pkz_count > 0;
}

qboolean RT_PKZ_Exists(const char *name)
{
    for (int i = 0; i < rt_pkz_count; i++)
    {
        rt_pkz_archive_t *a = &rt_pkz_archives[i];
        if (!a->valid)
        {
            continue;
        }
        mz_uint32 file_index;
        if (mz_zip_reader_locate_file_v2(&a->zip, name, NULL, 0, &file_index))
        {
            return true;
        }
    }
    return false;
}

byte *RT_PKZ_LoadFile(const char *name, int *outLen)
{
    for (int i = 0; i < rt_pkz_count; i++)
    {
        rt_pkz_archive_t *a = &rt_pkz_archives[i];
        if (!a->valid)
        {
            continue;
        }

        size_t len = 0;
        void *buf = mz_zip_reader_extract_file_to_heap(&a->zip, name, &len, 0);
        if (buf)
        {
            if (outLen)
            {
                *outLen = (int)len;
            }
            return (byte *)buf;
        }
    }

    if (outLen)
    {
        *outLen = 0;
    }
    return NULL;
}

int RT_PKZ_ListFiles(const char *dir, const char *ext,
                     int (*cb)(const char *name, void *ctx), void *ctx)
{
    int count = 0;
    size_t dirLen = strlen(dir);
    size_t extLen = strlen(ext);

    for (int i = 0; i < rt_pkz_count; i++)
    {
        rt_pkz_archive_t *a = &rt_pkz_archives[i];
        if (!a->valid)
        {
            continue;
        }

        mz_uint num = (mz_uint)a->zip.m_total_files;
        for (mz_uint f = 0; f < num; f++)
        {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&a->zip, f, &st))
            {
                continue;
            }

            const char *name = st.m_filename;
            size_t nameLen = strlen(name);
            if (nameLen < dirLen + extLen)
            {
                continue;
            }
            if (q_strncasecmp(name, dir, dirLen) != 0)
            {
                continue;
            }
            if (q_strcasecmp(name + nameLen - extLen, ext) != 0)
            {
                continue;
            }

            count++;
            if (cb && cb(name, ctx))
            {
                return count;
            }
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// native searchpath integration (read like .PAK)
// ---------------------------------------------------------------------------

static int rt_pkz_find_index(rt_pkz_archive_t *a, const char *name, int *outSize)
{
    if (!a || !a->valid || !name || !name[0])
    {
        return -1;
    }

    mz_uint32 file_index;
    if (!mz_zip_reader_locate_file_v2(&a->zip, name, NULL, 0, &file_index))
    {
        return -1;
    }

    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&a->zip, file_index, &st))
    {
        return -1;
    }

    if (outSize)
    {
        *outSize = (int)st.m_uncomp_size;
    }
    return (int)file_index;
}

int RT_PKZ_FindFile(const void *varchive, const char *name, int *outSize)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    int size = 0;
    if (rt_pkz_find_index(a, name, &size) < 0)
    {
        return -1;
    }
    if (outSize)
    {
        *outSize = size;
    }
    return size;
}

int RT_PKZ_OpenFile(const void *varchive, const char *name, int *outSize)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    int size = 0;
    int fi = rt_pkz_find_index(a, name, &size);
    if (fi < 0)
    {
        return -1;
    }

    for (int i = 0; i < RT_PKZ_MAX_STREAMS; i++)
    {
        rt_pkz_stream_t *s = &rt_pkz_streams[i];
        if (s->inUse)
        {
            continue;
        }

        s->inUse = true;
        s->archive = (int)(a - rt_pkz_archives);
        s->fileIndex = fi;
        s->buf = NULL;
        s->size = (size_t)size;
        s->pos = 0;
        if (outSize)
        {
            *outSize = size;
        }
        return RT_PKZ_HANDLE_BASE + i;
    }
    return -1;
}

qboolean RT_PKZ_IsHandle(int handle)
{
    return handle >= RT_PKZ_HANDLE_BASE && handle < RT_PKZ_HANDLE_BASE + RT_PKZ_MAX_STREAMS;
}

static rt_pkz_stream_t *rt_pkz_get_stream(int handle)
{
    if (!RT_PKZ_IsHandle(handle))
    {
        return NULL;
    }
    rt_pkz_stream_t *s = &rt_pkz_streams[handle - RT_PKZ_HANDLE_BASE];
    return s->inUse ? s : NULL;
}

int RT_PKZ_Read(int handle, void *dest, int count)
{
    rt_pkz_stream_t *s = rt_pkz_get_stream(handle);
    if (!s || count <= 0)
    {
        return 0;
    }

    if (!s->buf)
    {
        rt_pkz_archive_t *a = &rt_pkz_archives[s->archive];
        size_t len = 0;
        s->buf = (byte *)mz_zip_reader_extract_to_heap(&a->zip, (mz_uint)s->fileIndex, &len, 0);
        if (!s->buf)
        {
            return 0;
        }
        // trust the actual extracted size, not the central-directory stat
        s->size = len;
    }

    size_t avail = s->size - s->pos;
    if ((size_t)count > avail)
    {
        count = (int)avail;
    }
    if (count <= 0)
    {
        return 0;
    }

    memcpy(dest, s->buf + s->pos, (size_t)count);
    s->pos += (size_t)count;
    return count;
}

void RT_PKZ_Seek(int handle, int position)
{
    rt_pkz_stream_t *s = rt_pkz_get_stream(handle);
    if (!s)
    {
        return;
    }
    if (position < 0)
    {
        position = 0;
    }
    s->pos = ((size_t)position < s->size) ? (size_t)position : s->size;
}

void RT_PKZ_Close(int handle)
{
    rt_pkz_stream_t *s = rt_pkz_get_stream(handle);
    if (!s)
    {
        return;
    }
    if (s->buf)
    {
        Mem_Free(s->buf);
        s->buf = NULL;
    }
    s->inUse = false;
}

FILE *RT_PKZ_OpenFileAsFILE(const void *varchive, const char *name)
{
    rt_pkz_archive_t *a = (rt_pkz_archive_t *)varchive;
    int fi = rt_pkz_find_index(a, name, NULL);
    if (fi < 0)
    {
        return NULL;
    }

    size_t len = 0;
    void *buf = mz_zip_reader_extract_to_heap(&a->zip, (mz_uint)fi, &len, 0);
    if (!buf)
    {
        return NULL;
    }

    if (rt_pkz_temp_count >= RT_PKZ_MAX_TEMP)
    {
        Mem_Free(buf);
        return NULL;
    }

    char tmppath[MAX_OSPATH];
    char tmpfile[MAX_OSPATH];
    if (!GetTempPathA(sizeof(tmppath), tmppath))
    {
        Mem_Free(buf);
        return NULL;
    }
    GetTempFileNameA(tmppath, "rtp", 0, tmpfile);

    FILE *f = fopen(tmpfile, "wb");
    if (!f)
    {
        Mem_Free(buf);
        return NULL;
    }
    if (len > 0)
    {
        fwrite(buf, 1, len, f);
    }
    fclose(f);
    Mem_Free(buf);

    q_strlcpy(rt_pkz_temp_files[rt_pkz_temp_count], tmpfile, sizeof(rt_pkz_temp_files[0]));
    rt_pkz_temp_count++;

    return fopen(tmpfile, "rb");
}
