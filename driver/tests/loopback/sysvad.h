// 用户态测试桩：只提供 aivcloopback.cpp 用到的内核 API。
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <stdlib.h>
#include <string.h>

typedef LONG NTSTATUS;
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
typedef ULONG_PTR KSPIN_LOCK;
typedef UCHAR KIRQL;
#define POOL_FLAG_NON_PAGED 0x40
#define PAGED_CODE()
#define IsEqualGUIDAligned(a, b) IsEqualGUID(a, b)
inline void KeInitializeSpinLock(KSPIN_LOCK *l) { *l = 0; }
inline void KeAcquireSpinLock(KSPIN_LOCK *, KIRQL *o) { *o = 0; }
inline void KeReleaseSpinLock(KSPIN_LOCK *, KIRQL) {}
inline void *ExAllocatePool2(ULONGLONG, SIZE_T n, ULONG) { return calloc(1, n); }
inline void ExFreePoolWithTag(void *p, ULONG) { free(p); }
#ifndef __forceinline
#define __forceinline inline
#endif
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
