/*
 * Copyright 2019 Nikolay Sivov for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define NONAMELESSUNION

#undef INITGUID
#include <guiddef.h>
#include "mfapi.h"
#include "mfidl.h"
#include "mferror.h"

#include "wine/heap.h"
#include "wine/debug.h"

struct attribute
{
    GUID key;
    PROPVARIANT value;
};

struct attributes
{
    IMFAttributes IMFAttributes_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    struct attribute *attributes;
    size_t capacity;
    size_t count;
};

extern HRESULT init_attributes_object(struct attributes *object, UINT32 size) DECLSPEC_HIDDEN;
extern void clear_attributes_object(struct attributes *object) DECLSPEC_HIDDEN;
extern const char *debugstr_attr(const GUID *guid) DECLSPEC_HIDDEN;
extern const char *debugstr_mf_guid(const GUID *guid) DECLSPEC_HIDDEN;

extern HRESULT attributes_GetItem(struct attributes *object, REFGUID key, PROPVARIANT *value) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetItemType(struct attributes *object, REFGUID key, MF_ATTRIBUTE_TYPE *type) DECLSPEC_HIDDEN;
extern HRESULT attributes_CompareItem(struct attributes *object, REFGUID key, REFPROPVARIANT value,
        BOOL *result) DECLSPEC_HIDDEN;
extern HRESULT attributes_Compare(struct attributes *object, IMFAttributes *theirs,
        MF_ATTRIBUTES_MATCH_TYPE match_type, BOOL *ret) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetUINT32(struct attributes *object, REFGUID key, UINT32 *value) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetUINT64(struct attributes *object, REFGUID key, UINT64 *value) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetDouble(struct attributes *object, REFGUID key, double *value) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetGUID(struct attributes *object, REFGUID key, GUID *value) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetStringLength(struct attributes *object, REFGUID key, UINT32 *length) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetString(struct attributes *object, REFGUID key, WCHAR *value, UINT32 size,
        UINT32 *length) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetAllocatedString(struct attributes *object, REFGUID key, WCHAR **value,
        UINT32 *length) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetBlobSize(struct attributes *object, REFGUID key, UINT32 *size) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetBlob(struct attributes *object, REFGUID key, UINT8 *buf, UINT32 bufsize,
        UINT32 *blobsize) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetAllocatedBlob(struct attributes *object, REFGUID key, UINT8 **buf,
        UINT32 *size) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetUnknown(struct attributes *object, REFGUID key, REFIID riid, void **out) DECLSPEC_HIDDEN;
extern HRESULT attributes_SetItem(struct attributes *object, REFGUID key, REFPROPVARIANT value) DECLSPEC_HIDDEN;
extern HRESULT attributes_DeleteItem(struct attributes *object, REFGUID key) DECLSPEC_HIDDEN;
extern HRESULT attributes_DeleteAllItems(struct attributes *object) DECLSPEC_HIDDEN;
extern HRESULT attributes_SetUINT32(struct attributes *object, REFGUID key, UINT32 value) DECLSPEC_HIDDEN;
extern HRESULT attributes_SetUINT64(struct attributes *object, REFGUID key, UINT64 value) DECLSPEC_HIDDEN;
extern HRESULT attributes_SetDouble(struct attributes *object, REFGUID key, double value) DECLSPEC_HIDDEN;
extern HRESULT attributes_SetGUID(struct attributes *object, REFGUID key, REFGUID value) DECLSPEC_HIDDEN;
extern HRESULT attributes_SetString(struct attributes *object, REFGUID key, const WCHAR *value) DECLSPEC_HIDDEN;
extern HRESULT attributes_SetBlob(struct attributes *object, REFGUID key, const UINT8 *buf,
        UINT32 size) DECLSPEC_HIDDEN;
extern HRESULT attributes_SetUnknown(struct attributes *object, REFGUID key, IUnknown *unknown) DECLSPEC_HIDDEN;
extern HRESULT attributes_LockStore(struct attributes *object) DECLSPEC_HIDDEN;
extern HRESULT attributes_UnlockStore(struct attributes *object) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetCount(struct attributes *object, UINT32 *items) DECLSPEC_HIDDEN;
extern HRESULT attributes_GetItemByIndex(struct attributes *object, UINT32 index, GUID *key,
        PROPVARIANT *value) DECLSPEC_HIDDEN;
extern HRESULT attributes_CopyAllItems(struct attributes *object, IMFAttributes *dest) DECLSPEC_HIDDEN;

static inline BOOL mf_array_reserve(void **elements, size_t *capacity, size_t count, size_t size)
{
    size_t new_capacity, max_capacity;
    void *new_elements;

    if (count <= *capacity)
        return TRUE;

    max_capacity = ~(SIZE_T)0 / size;
    if (count > max_capacity)
        return FALSE;

    new_capacity = max(4, *capacity);
    while (new_capacity < count && new_capacity <= max_capacity / 2)
        new_capacity *= 2;
    if (new_capacity < count)
        new_capacity = max_capacity;

    if (!(new_elements = heap_realloc(*elements, new_capacity * size)))
        return FALSE;

    *elements = new_elements;
    *capacity = new_capacity;

    return TRUE;
}

extern unsigned int mf_format_get_bpp(const GUID *subtype, BOOL *is_yuv) DECLSPEC_HIDDEN;

static inline const char *debugstr_propvar(const PROPVARIANT *v)
{
    if (!v)
        return "(null)";

    switch (v->vt)
    {
        case VT_EMPTY:
            return wine_dbg_sprintf("%p {VT_EMPTY}", v);
        case VT_NULL:
            return wine_dbg_sprintf("%p {VT_NULL}", v);
        case VT_UI4:
            return wine_dbg_sprintf("%p {VT_UI4: %d}", v, v->u.ulVal);
        case VT_UI8:
            return wine_dbg_sprintf("%p {VT_UI8: %s}", v, wine_dbgstr_longlong(v->u.uhVal.QuadPart));
        case VT_R8:
            return wine_dbg_sprintf("%p {VT_R8: %lf}", v, v->u.dblVal);
        case VT_CLSID:
            return wine_dbg_sprintf("%p {VT_CLSID: %s}", v, debugstr_mf_guid(v->u.puuid));
        case VT_LPWSTR:
            return wine_dbg_sprintf("%p {VT_LPWSTR: %s}", v, wine_dbgstr_w(v->u.pwszVal));
        case VT_VECTOR | VT_UI1:
            return wine_dbg_sprintf("%p {VT_VECTOR|VT_UI1: %p}", v, v->u.caub.pElems);
        case VT_UNKNOWN:
            return wine_dbg_sprintf("%p {VT_UNKNOWN: %p}", v, v->u.punkVal);
        default:
            return wine_dbg_sprintf("%p {vt %#x}", v, v->vt);
    }
}
