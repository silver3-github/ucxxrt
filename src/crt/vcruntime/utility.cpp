//
// utility.cpp
//
//      Copyright (c) Microsoft Corporation. All rights reserved.
//
// Common functionality shared by both the EXE and DLL startup code.
//
#include <vcstartup_internal.h>
#include <limits.h>


//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// Fatal Error Reporting
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
extern "C" void __cdecl __scrt_fastfail(unsigned const code)
{
    // This will always be availbale on ARM and is always available on Windows 8 and above.
    __fastfail(code);
}


//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// PE Image Utilities
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

// Tests whether a PE image is located at the given image base.  Returns true if
// the given image base potentially points to a loaded PE image; false otherwise.
static bool __cdecl is_potentially_valid_image_base(void* const image_base) noexcept
{
    if (!image_base)
    {
        return false;
    }

    auto const dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(image_base);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return false;
    }

    auto const nt_header_address = reinterpret_cast<PBYTE>(dos_header) + dos_header->e_lfanew;
    auto const nt_header         = reinterpret_cast<PIMAGE_NT_HEADERS>(nt_header_address);
    if (nt_header->Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }

    auto const optional_header = &nt_header->OptionalHeader;
    if (optional_header->Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
    {
        return false;
    }

    return true;
}



// Given an RVA, finds the PE section in the pointed-to image that includes the
// RVA.  Returns null if no such section exists or the section is not found.
static PIMAGE_SECTION_HEADER __cdecl find_pe_section(unsigned char* const image_base, uintptr_t const rva) noexcept
{
    auto const dos_header        = reinterpret_cast<PIMAGE_DOS_HEADER>(image_base);
    auto const nt_header_address = reinterpret_cast<PBYTE>(dos_header) + dos_header->e_lfanew;
    auto const nt_header         = reinterpret_cast<PIMAGE_NT_HEADERS>(nt_header_address);

    // Find the section holding the RVA.  We make no assumptions here about the
    // sort order of the section descriptors, though they always appear to be
    // sorted by ascending section RVA.
    PIMAGE_SECTION_HEADER const first_section = IMAGE_FIRST_SECTION(nt_header);
    PIMAGE_SECTION_HEADER const last_section  = first_section + nt_header->FileHeader.NumberOfSections;
    for (auto it = first_section; it != last_section; ++it)
    {
        if (rva >= it->VirtualAddress && rva < it->VirtualAddress + it->Misc.VirtualSize)
        {
            return it;
        }
    }

    return nullptr;
}


// Tests whether a target address is located within the current PE image (the
// image located at __ImageBase), that the target address is located in a proper
// section of the image, and that the section in which it is located is not
// writable.
extern "C" bool __cdecl __scrt_is_nonwritable_in_current_image(void const* const target)
{
    auto const target_address = reinterpret_cast<unsigned char const*>(target);
    auto const image_base     = reinterpret_cast<unsigned char*>(&__ImageBase);

    __try
    {
        // Make sure __ImageBase is the address of a valid PE image.  This is
        // likely an unnecessary check, since we should be executing in a
        // normal image, but it is fast, this routine is rarely called, and the
        // normal call is for security purposes.  If we don't have a PE image,
        // return failure:
        if (!is_potentially_valid_image_base(image_base))
        {
            return false;
        }

        // Convert the target address to an RVA within the image and find the
        // corresponding PE section.  Return failure if the target address is
        // not found within the current image:
        uintptr_t const rva_target = target_address - image_base;
        PIMAGE_SECTION_HEADER const section_header = find_pe_section(image_base, rva_target);
        if (!section_header)
        {
            return false;
        }

        // Check the section characteristics to see if the target address is
        // located within a writable section, returning a failure if it is:
        if (section_header->Characteristics & IMAGE_SCN_MEM_WRITE)
        {
            return false;
        }

        return true;
    }
    __except (GetExceptionCode() == STATUS_ACCESS_VIOLATION)
    {
        // If any of the above operations failed, assume that we do not have a
        // valid nonwritable address in the current image:
        return false;
    }
}



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// CRT Initialization
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

extern "C" void __cdecl __scrt_initialize_system();

extern "C" void __cdecl __scrt_initialize_memory()
{
    // Nx
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
}

extern "C" bool __cdecl __scrt_initialize_crt()
{
#if defined(_M_IX86) || defined(_M_X64)
    __isa_available_init();
#endif

    __scrt_initialize_memory();

    __scrt_initialize_system();

    // Notify the CRT components of the process attach, bottom-to-top:
    if (!__vcrt_initialize())
    {
        return false;
    }

    if (!__acrt_initialize())
    {
        __vcrt_uninitialize(false);
        return false;
    }

    return true;
}

extern "C" bool __cdecl __scrt_uninitialize_crt(bool const is_terminating, bool const /*from_exit*/)
{
    // Notify the CRT components of the process detach, top-to-bottom:
    __acrt_uninitialize(is_terminating);
    __vcrt_uninitialize(is_terminating);

    return true;
}



//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// On-Exit Table
//
//-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// When a module uses the Universal CRT DLL, the behavior of _onexit(), atexit(),
// and at_quick_exit() is different depending on whether the module is an EXE or
// a DLL.  If it is an EXE, then calls to these functions are passed to the
// Universal CRT DLL and the callbacks are registered with its atexit function
// tables.  This way, functions registered by the EXE are called whenever one of
// the exit() functions is called.
//
// If the module is a DLL, it has its own table of registered functions.  This
// table is executed when the DLL is unloaded (during DLL_PROCESS_DETACH).
//
// Here, we define the module-local function tables used by DLLs that use the
// Universal CRT DLL.  If this module is an EXE or if the Universal CRT is
// statically linked into this module, then these tables will have pointers
// initialized to -1.
//
// The user-callable _onexit(), atexit(), and at_quick_exit() functions then
// either use the module-local function tables or will forward calls to the
// Universal CRT DLL, as appropriate.

extern "C" _onexit_t __cdecl _onexit(_onexit_t const function)
{
    return _crt_atexit(reinterpret_cast<_PVFV>(function)) == 0
        ? function
        : nullptr;
}

extern "C" int __cdecl atexit(_PVFV const function)
{
    return _onexit(reinterpret_cast<_onexit_t>(function)) != nullptr
        ? 0
        : -1;
}

extern "C" int __cdecl at_quick_exit(_PVFV const function)
{
    return _crt_at_quick_exit(function);
}
