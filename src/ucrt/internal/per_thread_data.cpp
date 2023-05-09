//
// per_thread_data.cpp
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// Per-Thread Data (PTD) used by the AppCRT.
//
#include <corecrt_internal.h>
#include <stddef.h>


struct __acrt_ptd_km : public __acrt_ptd
{
    void*       tid;
    long long   uid;
};

static NPAGED_LOOKASIDE_LIST __acrt_startup_ptd_pools;
static RTL_AVL_TABLE         __acrt_startup_ptd_table;
static KSPIN_LOCK            __acrt_startup_ptd_table_lock;

static long long __get_thread_uid(_In_ PETHREAD thread)
{
    CLIENT_ID id = PsGetThreadClientId(thread);
    size_t high  = reinterpret_cast<size_t>(id.UniqueProcess) >> 2;
    size_t low   = reinterpret_cast<size_t>(id.UniqueThread ) >> 2;
    return (static_cast<long long>(high & ~uint32_t(0)) << 32) | (static_cast<long long>(low & ~uint32_t(0)));
}

RTL_GENERIC_COMPARE_RESULTS NTAPI __acrt_ptd_table_compare(
    _In_ RTL_AVL_TABLE* /*table*/,
    _In_ PVOID first,
    _In_ PVOID second
)
{
    auto ptd1 = static_cast<__acrt_ptd_km*>(first);
    auto ptd2 = static_cast<__acrt_ptd_km*>(second);

    return
        (ptd1->tid < ptd2->tid) ? GenericLessThan :
        (ptd1->tid > ptd2->tid) ? GenericGreaterThan : GenericEqual;
}

PVOID NTAPI __acrt_ptd_table_allocate(
    _In_ RTL_AVL_TABLE* /*table*/,
    _In_ CLONG /*size*/
)
{
    return ExAllocateFromNPagedLookasideList(&__acrt_startup_ptd_pools);
}

VOID NTAPI __acrt_ptd_table_free(
    _In_ RTL_AVL_TABLE* /*table*/,
    _In_ __drv_freesMem(Mem) _Post_invalid_ PVOID buffer
)
{
    auto ptd = reinterpret_cast<__acrt_ptd*>(static_cast<uint8_t*>(buffer) + sizeof(RTL_BALANCED_LINKS));
    free(ptd->_strerror_buffer);
    free(ptd->_wcserror_buffer);

    return ExFreeToNPagedLookasideList(&__acrt_startup_ptd_pools, buffer);
}

static __acrt_ptd* __cdecl store_and_initialize_ptd(__acrt_ptd* const ptd)
{
    BOOLEAN inserted = false;

    __acrt_ptd* const new_ptd = static_cast<__acrt_ptd*>(RtlInsertElementGenericTableAvl(
        &__acrt_startup_ptd_table, ptd, sizeof __acrt_ptd_km, &inserted));
    if (!new_ptd)
    {
        return nullptr;
    }

    // reuse outdated ptd. (tid == new_tid && uid != new_uid)
    if (__get_thread_uid(PsGetCurrentThread()) != static_cast<__acrt_ptd_km*>(new_ptd)->uid)
    {
        inserted = true;
        RtlSecureZeroMemory(new_ptd, sizeof __acrt_ptd); // not reset pid/uid
    }

    return new_ptd;
}

extern "C" bool __cdecl __acrt_initialize_ptd()
{
    constexpr auto size = ROUND_TO_SIZE(sizeof __acrt_ptd_km + sizeof RTL_BALANCED_LINKS, sizeof(void*));

    ExInitializeNPagedLookasideList(&__acrt_startup_ptd_pools, nullptr, nullptr,
        POOL_NX_ALLOCATION, size, __ucxxrt_tag, 0);

    KeInitializeSpinLock(&__acrt_startup_ptd_table_lock);

    RtlInitializeGenericTableAvl(&__acrt_startup_ptd_table, &__acrt_ptd_table_compare,
        &__acrt_ptd_table_allocate, &__acrt_ptd_table_free, &__acrt_startup_ptd_pools);

    __acrt_ptd_km ptd{};
    ptd.tid = PsGetCurrentThreadId();
    ptd.uid = __get_thread_uid(PsGetCurrentThread());

    if (!store_and_initialize_ptd(&ptd))
    {
        __acrt_uninitialize_ptd(false);
        return false;
    }

    return true;
}

extern "C" bool __cdecl __acrt_uninitialize_ptd(bool)
{
    auto ptd = static_cast<__acrt_ptd*>(RtlGetElementGenericTableAvl(&__acrt_startup_ptd_table, 0));

    while (ptd) {
        RtlDeleteElementGenericTableAvl(&__acrt_startup_ptd_table, ptd);
        ptd = static_cast<__acrt_ptd*>(RtlGetElementGenericTableAvl(&__acrt_startup_ptd_table, 0));
    }

    ExDeleteNPagedLookasideList(&__acrt_startup_ptd_pools);

    return true;
}

extern "C" __acrt_ptd* __cdecl __acrt_getptd_noexit()
{
    __acrt_ptd* existing_ptd;

    __acrt_ptd_km ptd{};
    ptd.tid = PsGetCurrentThreadId();
    ptd.uid = __get_thread_uid(PsGetCurrentThread());

    KLOCK_QUEUE_HANDLE lock_state{};
    KeAcquireInStackQueuedSpinLock(&__acrt_startup_ptd_table_lock, &lock_state);
    {
        existing_ptd = static_cast<__acrt_ptd*>(RtlLookupElementGenericTableAvl(&__acrt_startup_ptd_table, &ptd));
        if (existing_ptd == nullptr) {
            // No per-thread data for this thread yet. Try to create one:
            existing_ptd = store_and_initialize_ptd(&ptd);
        }
    }
    KeReleaseInStackQueuedSpinLock(&lock_state);

    return existing_ptd;
}

extern "C" __acrt_ptd* __cdecl __acrt_getptd()
{
    __acrt_ptd* const ptd = __acrt_getptd_noexit();
    if (ptd == nullptr)
    {
        abort();
    }

    return ptd;
}

extern "C" void __cdecl __acrt_freeptd()
{
    __acrt_ptd_km current_ptd{};
    current_ptd.tid = PsGetCurrentThreadId();
    current_ptd.uid = __get_thread_uid(PsGetCurrentThread());


    __acrt_ptd* const block_to_free = &current_ptd;

    KLOCK_QUEUE_HANDLE lock_state{};
    KeAcquireInStackQueuedSpinLock(&__acrt_startup_ptd_table_lock, &lock_state);
    {
        RtlDeleteElementGenericTableAvl(&__acrt_startup_ptd_table, block_to_free);
    }
    KeReleaseInStackQueuedSpinLock(&lock_state);
}



// These functions are simply wrappers around the Windows API functions.
extern "C" unsigned long __cdecl __threadid()
{
    return HandleToLong(PsGetCurrentThreadId());
}

extern "C" uintptr_t __cdecl __threadhandle()
{
    return reinterpret_cast<uintptr_t>(PsGetCurrentThread());
}
