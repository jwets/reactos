/*
 * Copyright (C) 1998-2005 ReactOS Team (and the authors from the programmers section)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 *
 * PROJECT:         ReactOS kernel
 * FILE:            ntoskrnl/mm/section.c
 * PURPOSE:         Implements section objects
 *
 * PROGRAMMERS:     Rex Jolliff
 *                  David Welch
 *                  Eric Kohl
 *                  Emanuele Aliberti
 *                  Eugene Ingerman
 *                  Casper Hornstrup
 *                  KJK::Hyperion
 *                  Guido de Jong
 *                  Ge van Geldorp
 *                  Royce Mitchell III
 *                  Filip Navara
 *                  Aleksey Bragin
 *                  Jason Filby
 *                  Thomas Weidenmueller
 *                  Gunnar Andre' Dalsnes
 *                  Mike Nordell
 *                  Alex Ionescu
 *                  Gregor Anich
 *                  Steven Edwards
 *                  Herve Poussineau
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <cache/newcc.h>
#include <cache/section/newmm.h>
#define NDEBUG
#include <debug.h>
#include <reactos/exeformat.h>
#include "ARM3/miarm.h"

#undef MmSetPageEntrySectionSegment
#define MmSetPageEntrySectionSegment(S,O,E) do { \
        DPRINT("SetPageEntrySectionSegment(old,%p,%x,%x)\n",(S),(O)->LowPart,E); \
        _MmSetPageEntrySectionSegment((S),(O),(E),__FILE__,__LINE__);   \
	} while (0)

extern MMSESSION MmSession;

#ifndef NEWCC
KEVENT MmWaitPageEvent;

VOID
NTAPI
_MmLockSectionSegment(PMM_SECTION_SEGMENT Segment, const char *file, int line)
{
    //DPRINT("MmLockSectionSegment(%p,%s:%d)\n", Segment, file, line);
    ExAcquireFastMutex(&Segment->Lock);
    Segment->Locked = TRUE;
}

VOID
NTAPI
_MmUnlockSectionSegment(PMM_SECTION_SEGMENT Segment, const char *file, int line)
{
    ASSERT(Segment->Locked);
    Segment->Locked = FALSE;
    ExReleaseFastMutex(&Segment->Lock);
    //DPRINT("MmUnlockSectionSegment(%p,%s:%d)\n", Segment, file, line);
}
#endif

/* Somewhat grotesque, but eh... */
PMM_IMAGE_SECTION_OBJECT ImageSectionObjectFromSegment(PMM_SECTION_SEGMENT Segment)
{
    ASSERT((Segment->SegFlags & MM_DATAFILE_SEGMENT) == 0);

    return CONTAINING_RECORD(Segment->ReferenceCount, MM_IMAGE_SECTION_OBJECT, RefCount);
}

NTSTATUS
NTAPI
MiMapViewInSystemSpace(IN PVOID Section,
                       IN PVOID Session,
                       OUT PVOID *MappedBase,
                       IN OUT PSIZE_T ViewSize,
                       IN PLARGE_INTEGER SectionOffset);

NTSTATUS
NTAPI
MmCreateArm3Section(OUT PVOID *SectionObject,
                    IN ACCESS_MASK DesiredAccess,
                    IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL,
                    IN PLARGE_INTEGER InputMaximumSize,
                    IN ULONG SectionPageProtection,
                    IN ULONG AllocationAttributes,
                    IN HANDLE FileHandle OPTIONAL,
                    IN PFILE_OBJECT FileObject OPTIONAL);

NTSTATUS
NTAPI
MmMapViewOfArm3Section(IN PVOID SectionObject,
                       IN PEPROCESS Process,
                       IN OUT PVOID *BaseAddress,
                       IN ULONG_PTR ZeroBits,
                       IN SIZE_T CommitSize,
                       IN OUT PLARGE_INTEGER SectionOffset OPTIONAL,
                       IN OUT PSIZE_T ViewSize,
                       IN SECTION_INHERIT InheritDisposition,
                       IN ULONG AllocationType,
                       IN ULONG Protect);

//
// PeFmtCreateSection depends on the following:
//
C_ASSERT(EXEFMT_LOAD_HEADER_SIZE >= sizeof(IMAGE_DOS_HEADER));
C_ASSERT(sizeof(IMAGE_NT_HEADERS32) <= sizeof(IMAGE_NT_HEADERS64));

C_ASSERT(TYPE_ALIGNMENT(IMAGE_NT_HEADERS32) == TYPE_ALIGNMENT(IMAGE_NT_HEADERS64));
C_ASSERT(RTL_SIZEOF_THROUGH_FIELD(IMAGE_NT_HEADERS32, FileHeader) == RTL_SIZEOF_THROUGH_FIELD(IMAGE_NT_HEADERS64, FileHeader));
C_ASSERT(FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader) == FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader));

C_ASSERT(PEFMT_FIELDS_EQUAL(IMAGE_OPTIONAL_HEADER32, IMAGE_OPTIONAL_HEADER64, Magic));
C_ASSERT(PEFMT_FIELDS_EQUAL(IMAGE_OPTIONAL_HEADER32, IMAGE_OPTIONAL_HEADER64, SectionAlignment));
C_ASSERT(PEFMT_FIELDS_EQUAL(IMAGE_OPTIONAL_HEADER32, IMAGE_OPTIONAL_HEADER64, FileAlignment));
C_ASSERT(PEFMT_FIELDS_EQUAL(IMAGE_OPTIONAL_HEADER32, IMAGE_OPTIONAL_HEADER64, Subsystem));
C_ASSERT(PEFMT_FIELDS_EQUAL(IMAGE_OPTIONAL_HEADER32, IMAGE_OPTIONAL_HEADER64, MinorSubsystemVersion));
C_ASSERT(PEFMT_FIELDS_EQUAL(IMAGE_OPTIONAL_HEADER32, IMAGE_OPTIONAL_HEADER64, MajorSubsystemVersion));
C_ASSERT(PEFMT_FIELDS_EQUAL(IMAGE_OPTIONAL_HEADER32, IMAGE_OPTIONAL_HEADER64, AddressOfEntryPoint));
C_ASSERT(PEFMT_FIELDS_EQUAL(IMAGE_OPTIONAL_HEADER32, IMAGE_OPTIONAL_HEADER64, SizeOfCode));
C_ASSERT(PEFMT_FIELDS_EQUAL(IMAGE_OPTIONAL_HEADER32, IMAGE_OPTIONAL_HEADER64, SizeOfHeaders));

/* TYPES *********************************************************************/

typedef struct
{
    PMEMORY_AREA MemoryArea;
    PMM_SECTION_SEGMENT Segment;
    LARGE_INTEGER Offset;
    BOOLEAN WasDirty;
    BOOLEAN Private;
    PEPROCESS CallingProcess;
    ULONG_PTR SectionEntry;
}
MM_SECTION_PAGEOUT_CONTEXT;

/* GLOBALS *******************************************************************/

POBJECT_TYPE MmSectionObjectType = NULL;

ULONG_PTR MmSubsectionBase;

static ULONG SectionCharacteristicsToProtect[16] =
{
    PAGE_NOACCESS,          /* 0 = NONE */
    PAGE_NOACCESS,          /* 1 = SHARED */
    PAGE_EXECUTE,           /* 2 = EXECUTABLE */
    PAGE_EXECUTE,           /* 3 = EXECUTABLE, SHARED */
    PAGE_READONLY,          /* 4 = READABLE */
    PAGE_READONLY,          /* 5 = READABLE, SHARED */
    PAGE_EXECUTE_READ,      /* 6 = READABLE, EXECUTABLE */
    PAGE_EXECUTE_READ,      /* 7 = READABLE, EXECUTABLE, SHARED */
    /*
     * FIXME? do we really need the WriteCopy field in segments? can't we use
     * PAGE_WRITECOPY here?
     */
    PAGE_READWRITE,         /* 8 = WRITABLE */
    PAGE_READWRITE,         /* 9 = WRITABLE, SHARED */
    PAGE_EXECUTE_READWRITE, /* 10 = WRITABLE, EXECUTABLE */
    PAGE_EXECUTE_READWRITE, /* 11 = WRITABLE, EXECUTABLE, SHARED */
    PAGE_READWRITE,         /* 12 = WRITABLE, READABLE */
    PAGE_READWRITE,         /* 13 = WRITABLE, READABLE, SHARED */
    PAGE_EXECUTE_READWRITE, /* 14 = WRITABLE, READABLE, EXECUTABLE */
    PAGE_EXECUTE_READWRITE, /* 15 = WRITABLE, READABLE, EXECUTABLE, SHARED */
};

extern ULONG MmMakeFileAccess [];
ACCESS_MASK NTAPI MiArm3GetCorrectFileAccessMask(IN ACCESS_MASK SectionPageProtection);
static GENERIC_MAPPING MmpSectionMapping =
{
    STANDARD_RIGHTS_READ | SECTION_MAP_READ | SECTION_QUERY,
    STANDARD_RIGHTS_WRITE | SECTION_MAP_WRITE,
    STANDARD_RIGHTS_EXECUTE | SECTION_MAP_EXECUTE,
    SECTION_ALL_ACCESS
};


/* FUNCTIONS *****************************************************************/



NTSTATUS
NTAPI
MiWritePage(PMM_SECTION_SEGMENT Segment,
            LONGLONG SegOffset,
            PFN_NUMBER Page)
/*
 * FUNCTION: write a page for a section backed memory area.
 * PARAMETERS:
 *       MemoryArea - Memory area to write the page for.
 *       Offset - Offset of the page to write.
 *       Page - Page which contains the data to write.
 */
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    UCHAR MdlBase[sizeof(MDL) + sizeof(PFN_NUMBER)];
    PMDL Mdl = (PMDL)MdlBase;
    PFILE_OBJECT FileObject = Segment->FileObject;
    LARGE_INTEGER FileOffset;

    FileOffset.QuadPart = Segment->Image.FileOffset + SegOffset;

    RtlZeroMemory(MdlBase, sizeof(MdlBase));
    MmInitializeMdl(Mdl, NULL, PAGE_SIZE);
    MmBuildMdlFromPages(Mdl, &Page);
    Mdl->MdlFlags |= MDL_PAGES_LOCKED;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Status = IoSynchronousPageWrite(FileObject, Mdl, &FileOffset, &Event, &IoStatus);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }
    if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA)
    {
        MmUnmapLockedPages (Mdl->MappedSystemVa, Mdl);
    }

    return Status;
}


/*
 References:
  [1] Microsoft Corporation, "Microsoft Portable Executable and Common Object
      File Format Specification", revision 6.0 (February 1999)
*/
NTSTATUS NTAPI PeFmtCreateSection(IN CONST VOID * FileHeader,
                                  IN SIZE_T FileHeaderSize,
                                  IN PVOID File,
                                  OUT PMM_IMAGE_SECTION_OBJECT ImageSectionObject,
                                  OUT PULONG Flags,
                                  IN PEXEFMT_CB_READ_FILE ReadFileCb,
                                  IN PEXEFMT_CB_ALLOCATE_SEGMENTS AllocateSegmentsCb)
{
    NTSTATUS nStatus;
    ULONG cbFileHeaderOffsetSize = 0;
    ULONG cbSectionHeadersOffset = 0;
    ULONG cbSectionHeadersSize;
    ULONG cbSectionHeadersOffsetSize = 0;
    ULONG cbOptHeaderSize;
    ULONG cbHeadersSize = 0;
    ULONG nSectionAlignment;
    ULONG nFileAlignment;
    ULONG_PTR ImageBase = 0;
    const IMAGE_DOS_HEADER * pidhDosHeader;
    const IMAGE_NT_HEADERS32 * pinhNtHeader;
    const IMAGE_OPTIONAL_HEADER32 * piohOptHeader;
    const IMAGE_SECTION_HEADER * pishSectionHeaders;
    PMM_SECTION_SEGMENT pssSegments;
    LARGE_INTEGER lnOffset;
    PVOID pBuffer;
    SIZE_T nPrevVirtualEndOfSegment = 0;
    ULONG nFileSizeOfHeaders = 0;
    ULONG i;
    ULONG AlignedLength;

    ASSERT(FileHeader);
    ASSERT(FileHeaderSize > 0);
    ASSERT(File);
    ASSERT(ImageSectionObject);
    ASSERT(ReadFileCb);
    ASSERT(AllocateSegmentsCb);

    ASSERT(Intsafe_CanOffsetPointer(FileHeader, FileHeaderSize));

    ASSERT(((UINT_PTR)FileHeader % TYPE_ALIGNMENT(IMAGE_DOS_HEADER)) == 0);

#define DIE(ARGS_) { DPRINT ARGS_; goto l_Return; }

    pBuffer = NULL;
    pidhDosHeader = FileHeader;

    /* DOS HEADER */
    nStatus = STATUS_ROS_EXEFMT_UNKNOWN_FORMAT;

    /* image too small to be an MZ executable */
    if(FileHeaderSize < sizeof(IMAGE_DOS_HEADER))
        DIE(("Too small to be an MZ executable, size is %lu\n", FileHeaderSize));

    /* no MZ signature */
    if(pidhDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        DIE(("No MZ signature found, e_magic is %hX\n", pidhDosHeader->e_magic));

    /* NT HEADER */
    nStatus = STATUS_INVALID_IMAGE_PROTECT;

    /* not a Windows executable */
    if(pidhDosHeader->e_lfanew <= 0)
        DIE(("Not a Windows executable, e_lfanew is %d\n", pidhDosHeader->e_lfanew));

    if(!Intsafe_AddULong32(&cbFileHeaderOffsetSize, pidhDosHeader->e_lfanew, RTL_SIZEOF_THROUGH_FIELD(IMAGE_NT_HEADERS32, FileHeader)))
        DIE(("The DOS stub is too large, e_lfanew is %X\n", pidhDosHeader->e_lfanew));

    if(FileHeaderSize < cbFileHeaderOffsetSize)
        pinhNtHeader = NULL;
    else
    {
        /*
         * we already know that Intsafe_CanOffsetPointer(FileHeader, FileHeaderSize),
         * and FileHeaderSize >= cbFileHeaderOffsetSize, so this holds true too
         */
        ASSERT(Intsafe_CanOffsetPointer(FileHeader, pidhDosHeader->e_lfanew));
        pinhNtHeader = (PVOID)((UINT_PTR)FileHeader + pidhDosHeader->e_lfanew);
    }

    /*
     * the buffer doesn't contain the NT file header, or the alignment is wrong: we
     * need to read the header from the file
     */
    if(FileHeaderSize < cbFileHeaderOffsetSize ||
            (UINT_PTR)pinhNtHeader % TYPE_ALIGNMENT(IMAGE_NT_HEADERS32) != 0)
    {
        ULONG cbNtHeaderSize;
        ULONG cbReadSize;
        PVOID pData;

l_ReadHeaderFromFile:
        cbNtHeaderSize = 0;
        lnOffset.QuadPart = pidhDosHeader->e_lfanew;

        /* read the header from the file */
        nStatus = ReadFileCb(File, &lnOffset, sizeof(IMAGE_NT_HEADERS64), &pData, &pBuffer, &cbReadSize);

        if(!NT_SUCCESS(nStatus))
        {
            NTSTATUS ReturnedStatus = nStatus;

            /* If it attempted to read past the end of the file, it means e_lfanew is invalid */
            if (ReturnedStatus == STATUS_END_OF_FILE) nStatus = STATUS_INVALID_IMAGE_PROTECT;

            DIE(("ReadFile failed, status %08X\n", ReturnedStatus));
        }

        ASSERT(pData);
        ASSERT(pBuffer);
        ASSERT(cbReadSize > 0);

        nStatus = STATUS_INVALID_IMAGE_FORMAT;

        /* the buffer doesn't contain the file header */
        if(cbReadSize < RTL_SIZEOF_THROUGH_FIELD(IMAGE_NT_HEADERS32, FileHeader))
            DIE(("The file doesn't contain the PE file header\n"));

        pinhNtHeader = pData;

        /* object still not aligned: copy it to the beginning of the buffer */
        if((UINT_PTR)pinhNtHeader % TYPE_ALIGNMENT(IMAGE_NT_HEADERS32) != 0)
        {
            ASSERT((UINT_PTR)pBuffer % TYPE_ALIGNMENT(IMAGE_NT_HEADERS32) == 0);
            RtlMoveMemory(pBuffer, pData, cbReadSize);
            pinhNtHeader = pBuffer;
        }

        /* invalid NT header */
        nStatus = STATUS_INVALID_IMAGE_PROTECT;

        if(pinhNtHeader->Signature != IMAGE_NT_SIGNATURE)
            DIE(("The file isn't a PE executable, Signature is %X\n", pinhNtHeader->Signature));

        nStatus = STATUS_INVALID_IMAGE_FORMAT;

        if(!Intsafe_AddULong32(&cbNtHeaderSize, pinhNtHeader->FileHeader.SizeOfOptionalHeader, FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader)))
            DIE(("The full NT header is too large\n"));

        /* the buffer doesn't contain the whole NT header */
        if(cbReadSize < cbNtHeaderSize)
            DIE(("The file doesn't contain the full NT header\n"));
    }
    else
    {
        ULONG cbOptHeaderOffsetSize = 0;

        nStatus = STATUS_INVALID_IMAGE_PROTECT;

        /* don't trust an invalid NT header */
        if(pinhNtHeader->Signature != IMAGE_NT_SIGNATURE)
            DIE(("The file isn't a PE executable, Signature is %X\n", pinhNtHeader->Signature));

        if(!Intsafe_AddULong32(&cbOptHeaderOffsetSize, pidhDosHeader->e_lfanew, FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader)))
            DIE(("The DOS stub is too large, e_lfanew is %X\n", pidhDosHeader->e_lfanew));

        nStatus = STATUS_INVALID_IMAGE_FORMAT;

        if(!Intsafe_AddULong32(&cbOptHeaderOffsetSize, cbOptHeaderOffsetSize, pinhNtHeader->FileHeader.SizeOfOptionalHeader))
            DIE(("The NT header is too large, SizeOfOptionalHeader is %X\n", pinhNtHeader->FileHeader.SizeOfOptionalHeader));

        /* the buffer doesn't contain the whole NT header: read it from the file */
        if(cbOptHeaderOffsetSize > FileHeaderSize)
            goto l_ReadHeaderFromFile;
    }

    /* read information from the NT header */
    piohOptHeader = &pinhNtHeader->OptionalHeader;
    cbOptHeaderSize = pinhNtHeader->FileHeader.SizeOfOptionalHeader;

    nStatus = STATUS_INVALID_IMAGE_FORMAT;

    if(!RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, Magic))
        DIE(("The optional header doesn't contain the Magic field, SizeOfOptionalHeader is %X\n", cbOptHeaderSize));

    /* ASSUME: RtlZeroMemory(ImageSectionObject, sizeof(*ImageSectionObject)); */

    switch(piohOptHeader->Magic)
    {
        case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
#ifndef _WIN64
            nStatus = STATUS_INVALID_IMAGE_WIN_64;
            DIE(("Win64 optional header, unsupported\n"));
#else
            // Fall through.
#endif
        case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
            break;
        default:
            DIE(("Unrecognized optional header, Magic is %X\n", piohOptHeader->Magic));
    }

    if (RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, SectionAlignment) &&
            RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, FileAlignment))
    {
        /* See [1], section 3.4.2 */
        if(piohOptHeader->SectionAlignment < PAGE_SIZE)
        {
            if(piohOptHeader->FileAlignment != piohOptHeader->SectionAlignment)
                DIE(("Sections aren't page-aligned and the file alignment isn't the same\n"));
        }
        else if(piohOptHeader->SectionAlignment < piohOptHeader->FileAlignment)
            DIE(("The section alignment is smaller than the file alignment\n"));

        nSectionAlignment = piohOptHeader->SectionAlignment;
        nFileAlignment = piohOptHeader->FileAlignment;

        if(!IsPowerOf2(nSectionAlignment) || !IsPowerOf2(nFileAlignment))
            DIE(("The section alignment (%u) and file alignment (%u) aren't both powers of 2\n", nSectionAlignment, nFileAlignment));
    }
    else
    {
        nSectionAlignment = PAGE_SIZE;
        nFileAlignment = PAGE_SIZE;
    }

    ASSERT(IsPowerOf2(nSectionAlignment));
    ASSERT(IsPowerOf2(nFileAlignment));

    switch(piohOptHeader->Magic)
    {
    /* PE32 */
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
    {
        if (RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, ImageBase))
            ImageBase = piohOptHeader->ImageBase;

        if(RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, SizeOfImage))
            ImageSectionObject->ImageInformation.ImageFileSize = piohOptHeader->SizeOfImage;

        if(RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, SizeOfStackReserve))
            ImageSectionObject->ImageInformation.MaximumStackSize = piohOptHeader->SizeOfStackReserve;

        if(RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, SizeOfStackCommit))
            ImageSectionObject->ImageInformation.CommittedStackSize = piohOptHeader->SizeOfStackCommit;

        if (RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, Subsystem))
        {
            ImageSectionObject->ImageInformation.SubSystemType = piohOptHeader->Subsystem;

            if (RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, MinorSubsystemVersion) &&
                    RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, MajorSubsystemVersion))
            {
                ImageSectionObject->ImageInformation.SubSystemMinorVersion = piohOptHeader->MinorSubsystemVersion;
                ImageSectionObject->ImageInformation.SubSystemMajorVersion = piohOptHeader->MajorSubsystemVersion;
            }
        }

        if (RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, AddressOfEntryPoint))
        {
            ImageSectionObject->ImageInformation.TransferAddress = (PVOID) (ImageBase +
                    piohOptHeader->AddressOfEntryPoint);
        }

        if (RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, SizeOfCode))
            ImageSectionObject->ImageInformation.ImageContainsCode = piohOptHeader->SizeOfCode != 0;
        else
            ImageSectionObject->ImageInformation.ImageContainsCode = TRUE;

        if (RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, AddressOfEntryPoint))
        {
            if (piohOptHeader->AddressOfEntryPoint == 0)
            {
                ImageSectionObject->ImageInformation.ImageContainsCode = FALSE;
            }
        }

        if (RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, LoaderFlags))
            ImageSectionObject->ImageInformation.LoaderFlags = piohOptHeader->LoaderFlags;

        if (RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, DllCharacteristics))
        {
            ImageSectionObject->ImageInformation.DllCharacteristics = piohOptHeader->DllCharacteristics;

            /*
             * Since we don't really implement SxS yet and LD doesn't supoprt /ALLOWISOLATION:NO, hard-code
             * this flag here, which will prevent the loader and other code from doing any .manifest or SxS
             * magic to any binary.
             *
             * This will break applications that depend on SxS when running with real Windows Kernel32/SxS/etc
             * but honestly that's not tested. It will also break them when running no ReactOS once we implement
             * the SxS support -- at which point, duh, this should be removed.
             *
             * But right now, any app depending on SxS is already broken anyway, so this flag only helps.
             */
            ImageSectionObject->ImageInformation.DllCharacteristics |= IMAGE_DLLCHARACTERISTICS_NO_ISOLATION;
        }

        break;
    }
#ifdef _WIN64
    /* PE64 */
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
    {
        const IMAGE_OPTIONAL_HEADER64 * pioh64OptHeader;

        pioh64OptHeader = (const IMAGE_OPTIONAL_HEADER64 *)piohOptHeader;

        if(RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, ImageBase))
        {
            ImageBase = pioh64OptHeader->ImageBase;
            if(pioh64OptHeader->ImageBase > MAXULONG_PTR)
                DIE(("ImageBase exceeds the address space\n"));
        }

        if(RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, SizeOfImage))
        {
            if(pioh64OptHeader->SizeOfImage > MAXULONG_PTR)
                DIE(("SizeOfImage exceeds the address space\n"));

            ImageSectionObject->ImageInformation.ImageFileSize = pioh64OptHeader->SizeOfImage;
        }

        if(RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, SizeOfStackReserve))
        {
            if(pioh64OptHeader->SizeOfStackReserve > MAXULONG_PTR)
                DIE(("SizeOfStackReserve exceeds the address space\n"));

            ImageSectionObject->ImageInformation.MaximumStackSize = (ULONG_PTR) pioh64OptHeader->SizeOfStackReserve;
        }

        if(RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, SizeOfStackCommit))
        {
            if(pioh64OptHeader->SizeOfStackCommit > MAXULONG_PTR)
                DIE(("SizeOfStackCommit exceeds the address space\n"));

            ImageSectionObject->ImageInformation.CommittedStackSize = (ULONG_PTR) pioh64OptHeader->SizeOfStackCommit;
        }

        if (RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, Subsystem))
        {
            ImageSectionObject->ImageInformation.SubSystemType = pioh64OptHeader->Subsystem;

            if (RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, MinorSubsystemVersion) &&
                    RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, MajorSubsystemVersion))
            {
                ImageSectionObject->ImageInformation.SubSystemMinorVersion = pioh64OptHeader->MinorSubsystemVersion;
                ImageSectionObject->ImageInformation.SubSystemMajorVersion = pioh64OptHeader->MajorSubsystemVersion;
            }
        }

        if (RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, AddressOfEntryPoint))
        {
            ImageSectionObject->ImageInformation.TransferAddress = (PVOID) (ImageBase +
                    pioh64OptHeader->AddressOfEntryPoint);
        }

        if (RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, SizeOfCode))
            ImageSectionObject->ImageInformation.ImageContainsCode = pioh64OptHeader->SizeOfCode != 0;
        else
            ImageSectionObject->ImageInformation.ImageContainsCode = TRUE;

        if (RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, AddressOfEntryPoint))
        {
            if (pioh64OptHeader->AddressOfEntryPoint == 0)
            {
                ImageSectionObject->ImageInformation.ImageContainsCode = FALSE;
            }
        }

        if (RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, LoaderFlags))
            ImageSectionObject->ImageInformation.LoaderFlags = pioh64OptHeader->LoaderFlags;

        if (RTL_CONTAINS_FIELD(pioh64OptHeader, cbOptHeaderSize, DllCharacteristics))
            ImageSectionObject->ImageInformation.DllCharacteristics = pioh64OptHeader->DllCharacteristics;

        break;
    }
#endif // _WIN64
    }

    /* [1], section 3.4.2 */
    if((ULONG_PTR)ImageBase % 0x10000)
        DIE(("ImageBase is not aligned on a 64KB boundary"));

    ImageSectionObject->ImageInformation.ImageCharacteristics = pinhNtHeader->FileHeader.Characteristics;
    ImageSectionObject->ImageInformation.Machine = pinhNtHeader->FileHeader.Machine;
    ImageSectionObject->ImageInformation.GpValue = 0;
    ImageSectionObject->ImageInformation.ZeroBits = 0;
    ImageSectionObject->BasedAddress = (PVOID)ImageBase;

    /* SECTION HEADERS */
    nStatus = STATUS_INVALID_IMAGE_FORMAT;

    /* see [1], section 3.3 */
    if(pinhNtHeader->FileHeader.NumberOfSections > 96)
        DIE(("Too many sections, NumberOfSections is %u\n", pinhNtHeader->FileHeader.NumberOfSections));

    /*
     * the additional segment is for the file's headers. They need to be present for
     * the benefit of the dynamic loader (to locate exports, defaults for thread
     * parameters, resources, etc.)
     */
    ImageSectionObject->NrSegments = pinhNtHeader->FileHeader.NumberOfSections + 1;

    /* file offset for the section headers */
    if(!Intsafe_AddULong32(&cbSectionHeadersOffset, pidhDosHeader->e_lfanew, FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader)))
        DIE(("Offset overflow\n"));

    if(!Intsafe_AddULong32(&cbSectionHeadersOffset, cbSectionHeadersOffset, pinhNtHeader->FileHeader.SizeOfOptionalHeader))
        DIE(("Offset overflow\n"));

    /* size of the section headers */
    ASSERT(Intsafe_CanMulULong32(pinhNtHeader->FileHeader.NumberOfSections, sizeof(IMAGE_SECTION_HEADER)));
    cbSectionHeadersSize = pinhNtHeader->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);

    if(!Intsafe_AddULong32(&cbSectionHeadersOffsetSize, cbSectionHeadersOffset, cbSectionHeadersSize))
        DIE(("Section headers too large\n"));

    /* size of the executable's headers */
    if(RTL_CONTAINS_FIELD(piohOptHeader, cbOptHeaderSize, SizeOfHeaders))
    {
//        if(!IsAligned(piohOptHeader->SizeOfHeaders, nFileAlignment))
//            DIE(("SizeOfHeaders is not aligned\n"));

        if(cbSectionHeadersSize > piohOptHeader->SizeOfHeaders)
            DIE(("The section headers overflow SizeOfHeaders\n"));

        cbHeadersSize = piohOptHeader->SizeOfHeaders;
    }
    else if(!AlignUp(&cbHeadersSize, cbSectionHeadersOffsetSize, nFileAlignment))
        DIE(("Overflow aligning the size of headers\n"));

    if(pBuffer)
    {
        ExFreePool(pBuffer);
        pBuffer = NULL;
    }
    /* WARNING: pinhNtHeader IS NO LONGER USABLE */
    /* WARNING: piohOptHeader IS NO LONGER USABLE */
    /* WARNING: pioh64OptHeader IS NO LONGER USABLE */

    if(FileHeaderSize < cbSectionHeadersOffsetSize)
        pishSectionHeaders = NULL;
    else
    {
        /*
         * we already know that Intsafe_CanOffsetPointer(FileHeader, FileHeaderSize),
         * and FileHeaderSize >= cbSectionHeadersOffsetSize, so this holds true too
         */
        ASSERT(Intsafe_CanOffsetPointer(FileHeader, cbSectionHeadersOffset));
        pishSectionHeaders = (PVOID)((UINT_PTR)FileHeader + cbSectionHeadersOffset);
    }

    /*
     * the buffer doesn't contain the section headers, or the alignment is wrong:
     * read the headers from the file
     */
    if(FileHeaderSize < cbSectionHeadersOffsetSize ||
            (UINT_PTR)pishSectionHeaders % TYPE_ALIGNMENT(IMAGE_SECTION_HEADER) != 0)
    {
        PVOID pData;
        ULONG cbReadSize;

        lnOffset.QuadPart = cbSectionHeadersOffset;

        /* read the header from the file */
        nStatus = ReadFileCb(File, &lnOffset, cbSectionHeadersSize, &pData, &pBuffer, &cbReadSize);

        if(!NT_SUCCESS(nStatus))
            DIE(("ReadFile failed with status %08X\n", nStatus));

        ASSERT(pData);
        ASSERT(pBuffer);
        ASSERT(cbReadSize > 0);

        nStatus = STATUS_INVALID_IMAGE_FORMAT;

        /* the buffer doesn't contain all the section headers */
        if(cbReadSize < cbSectionHeadersSize)
            DIE(("The file doesn't contain all of the section headers\n"));

        pishSectionHeaders = pData;

        /* object still not aligned: copy it to the beginning of the buffer */
        if((UINT_PTR)pishSectionHeaders % TYPE_ALIGNMENT(IMAGE_SECTION_HEADER) != 0)
        {
            ASSERT((UINT_PTR)pBuffer % TYPE_ALIGNMENT(IMAGE_SECTION_HEADER) == 0);
            RtlMoveMemory(pBuffer, pData, cbReadSize);
            pishSectionHeaders = pBuffer;
        }
    }

    /* SEGMENTS */
    /* allocate the segments */
    nStatus = STATUS_INSUFFICIENT_RESOURCES;
    ImageSectionObject->Segments = AllocateSegmentsCb(ImageSectionObject->NrSegments);

    if(ImageSectionObject->Segments == NULL)
        DIE(("AllocateSegments failed\n"));

    /* initialize the headers segment */
    pssSegments = ImageSectionObject->Segments;

//  ASSERT(IsAligned(cbHeadersSize, nFileAlignment));

    if(!AlignUp(&nFileSizeOfHeaders, cbHeadersSize, nFileAlignment))
        DIE(("Cannot align the size of the section headers\n"));

    nPrevVirtualEndOfSegment = ALIGN_UP_BY(cbHeadersSize, nSectionAlignment);
    if (nPrevVirtualEndOfSegment < cbHeadersSize)
        DIE(("Cannot align the size of the section headers\n"));

    pssSegments[0].Image.FileOffset = 0;
    pssSegments[0].Protection = PAGE_READONLY;
    pssSegments[0].Length.QuadPart = nPrevVirtualEndOfSegment;
    pssSegments[0].RawLength.QuadPart = nFileSizeOfHeaders;
    pssSegments[0].Image.VirtualAddress = 0;
    pssSegments[0].Image.Characteristics = 0;
    pssSegments[0].WriteCopy = TRUE;

    /* skip the headers segment */
    ++ pssSegments;

    nStatus = STATUS_INVALID_IMAGE_FORMAT;

    ASSERT(ImageSectionObject->RefCount > 0);

    /* convert the executable sections into segments. See also [1], section 4 */
    for(i = 0; i < ImageSectionObject->NrSegments - 1; ++ i)
    {
        ULONG nCharacteristics;

        /* validate the alignment */
        if(!IsAligned(pishSectionHeaders[i].VirtualAddress, nSectionAlignment))
            DIE(("Image.VirtualAddress[%u] is not aligned\n", i));

        /* sections must be contiguous, ordered by base address and non-overlapping */
        if(pishSectionHeaders[i].VirtualAddress != nPrevVirtualEndOfSegment)
            DIE(("Memory gap between section %u and the previous\n", i));

        /* ignore explicit BSS sections */
        if(pishSectionHeaders[i].SizeOfRawData != 0)
        {
            /* validate the alignment */
#if 0
            /* Yes, this should be a multiple of FileAlignment, but there's
             * stuff out there that isn't. We can cope with that
             */
            if(!IsAligned(pishSectionHeaders[i].SizeOfRawData, nFileAlignment))
                DIE(("SizeOfRawData[%u] is not aligned\n", i));
#endif

//            if(!IsAligned(pishSectionHeaders[i].PointerToRawData, nFileAlignment))
//                DIE(("PointerToRawData[%u] is not aligned\n", i));

            if(!Intsafe_CanAddULong32(pishSectionHeaders[i].PointerToRawData, pishSectionHeaders[i].SizeOfRawData))
                DIE(("SizeOfRawData[%u] too large\n", i));

            /* conversion */
            pssSegments[i].Image.FileOffset = pishSectionHeaders[i].PointerToRawData;
            pssSegments[i].RawLength.QuadPart = pishSectionHeaders[i].SizeOfRawData;
        }
        else
        {
            /* FIXME: Should reset PointerToRawData to 0 in the image mapping */
            ASSERT(pssSegments[i].Image.FileOffset == 0);
            ASSERT(pssSegments[i].RawLength.QuadPart == 0);
        }

        ASSERT(Intsafe_CanAddLong64(pssSegments[i].Image.FileOffset, pssSegments[i].RawLength.QuadPart));

        nCharacteristics = pishSectionHeaders[i].Characteristics;

        /* no explicit protection */
        if((nCharacteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE)) == 0)
        {
            if(nCharacteristics & IMAGE_SCN_CNT_CODE)
                nCharacteristics |= IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

            if(nCharacteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)
                nCharacteristics |= IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

            if(nCharacteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA)
                nCharacteristics |= IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
        }

        /* see table above */
        pssSegments[i].Protection = SectionCharacteristicsToProtect[nCharacteristics >> 28];
        pssSegments[i].WriteCopy = !(nCharacteristics & IMAGE_SCN_MEM_SHARED);

        if(pishSectionHeaders[i].Misc.VirtualSize == 0)
            pssSegments[i].Length.QuadPart = pishSectionHeaders[i].SizeOfRawData;
        else
            pssSegments[i].Length.QuadPart = pishSectionHeaders[i].Misc.VirtualSize;

        AlignedLength = ALIGN_UP_BY(pssSegments[i].Length.LowPart, nSectionAlignment);
        if(AlignedLength < pssSegments[i].Length.LowPart)
            DIE(("Cannot align the virtual size of section %u\n", i));

        pssSegments[i].Length.LowPart = AlignedLength;

        if(pssSegments[i].Length.QuadPart == 0)
            DIE(("Virtual size of section %u is null\n", i));

        pssSegments[i].Image.VirtualAddress = pishSectionHeaders[i].VirtualAddress;
        pssSegments[i].Image.Characteristics = pishSectionHeaders[i].Characteristics;

        /* ensure the memory image is no larger than 4GB */
        nPrevVirtualEndOfSegment = (ULONG_PTR)(pssSegments[i].Image.VirtualAddress + pssSegments[i].Length.QuadPart);
        if (nPrevVirtualEndOfSegment < pssSegments[i].Image.VirtualAddress)
            DIE(("The image is too large\n"));
    }

    if(nSectionAlignment >= PAGE_SIZE)
        *Flags |= EXEFMT_LOAD_ASSUME_SEGMENTS_PAGE_ALIGNED;

    /* Success */
    nStatus = STATUS_SUCCESS;// STATUS_ROS_EXEFMT_LOADED_FORMAT | EXEFMT_LOADED_PE32;

l_Return:
    if(pBuffer)
        ExFreePool(pBuffer);

    return nStatus;
}

/*
 * FUNCTION:  Waits in kernel mode indefinitely for a file object lock.
 * ARGUMENTS: PFILE_OBJECT to wait for.
 * RETURNS:   Status of the wait.
 */
NTSTATUS
MmspWaitForFileLock(PFILE_OBJECT File)
{
    return STATUS_SUCCESS;
    //return KeWaitForSingleObject(&File->Lock, 0, KernelMode, FALSE, NULL);
}



VOID
NTAPI
MmpFreePageFileSegment(PMM_SECTION_SEGMENT Segment)
{
    ULONG Length;
    LARGE_INTEGER Offset;
    ULONG_PTR Entry;
    SWAPENTRY SavedSwapEntry;
    PFN_NUMBER Page;

    Page = 0;

    MmLockSectionSegment(Segment);

    Length = PAGE_ROUND_UP(Segment->Length.QuadPart);
    for (Offset.QuadPart = 0; Offset.QuadPart < Length; Offset.QuadPart += PAGE_SIZE)
    {
        Entry = MmGetPageEntrySectionSegment(Segment, &Offset);
        if (Entry)
        {
            MmSetPageEntrySectionSegment(Segment, &Offset, 0);
            if (IS_SWAP_FROM_SSE(Entry))
            {
                MmFreeSwapPage(SWAPENTRY_FROM_SSE(Entry));
            }
            else
            {
                Page = PFN_FROM_SSE(Entry);
                SavedSwapEntry = MmGetSavedSwapEntryPage(Page);
                if (SavedSwapEntry != 0)
                {
                    MmSetSavedSwapEntryPage(Page, 0);
                    MmFreeSwapPage(SavedSwapEntry);
                }
                MmReleasePageMemoryConsumer(MC_USER, Page);
            }
        }
    }

    MmUnlockSectionSegment(Segment);
}

static
VOID
NTAPI
FreeSegmentPage(PMM_SECTION_SEGMENT Segment, PLARGE_INTEGER Offset)
{
    ULONG_PTR Entry;
    PFN_NUMBER Page;

    MmLockSectionSegment(Segment);

    Entry = MmGetPageEntrySectionSegment(Segment, Offset);

    MmUnlockSectionSegment(Segment);

    /* This must be either a valid entry or nothing */
    ASSERT(!IS_SWAP_FROM_SSE(Entry));

    /* There should be no reference anymore */
    ASSERT(SHARE_COUNT_FROM_SSE(Entry) == 0);

    Page = PFN_FROM_SSE(Entry);
    /* If there is a page, this must be because it's still dirty */
    ASSERT(Page != 0);

    /* Write the page */
    if (IS_DIRTY_SSE(Entry))
        MiWritePage(Segment, Offset->QuadPart, Page);

    MmReleasePageMemoryConsumer(MC_USER, Page);
}

VOID
NTAPI
MmDereferenceSegment(PMM_SECTION_SEGMENT Segment)
{
    KIRQL OldIrql;

    /* Lock the PFN lock because we mess around with SectionObjectPointers */
    OldIrql = MiAcquirePfnLock();

    if (InterlockedDecrementUL(Segment->ReferenceCount) > 0)
    {
        /* Nothing to do yet */
        MiReleasePfnLock(OldIrql);
        return;
    }

    *Segment->Flags |= MM_SEGMENT_INDELETE;

    MiReleasePfnLock(OldIrql);

    /* Flush the segment */
    if (*Segment->Flags & MM_DATAFILE_SEGMENT)
    {
        /* Free the page table. This will flush any remaining dirty data */
        MmFreePageTablesSectionSegment(Segment, FreeSegmentPage);

        OldIrql = MiAcquirePfnLock();
        /* Delete the pointer on the file */
        ASSERT(Segment->FileObject->SectionObjectPointer->DataSectionObject == Segment);
        Segment->FileObject->SectionObjectPointer->DataSectionObject = NULL;
        MiReleasePfnLock(OldIrql);
        ObDereferenceObject(Segment->FileObject);

        ExFreePoolWithTag(Segment, TAG_MM_SECTION_SEGMENT);
    }
    else
    {
        /* Most grotesque thing ever */
        PMM_IMAGE_SECTION_OBJECT ImageSectionObject = CONTAINING_RECORD(Segment->ReferenceCount, MM_IMAGE_SECTION_OBJECT, RefCount);
        PMM_SECTION_SEGMENT SectionSegments;
        ULONG NrSegments;
        ULONG i;

        OldIrql = MiAcquirePfnLock();
        /* Delete the pointer on the file */
        ASSERT(ImageSectionObject->FileObject->SectionObjectPointer->ImageSectionObject == ImageSectionObject);
        ImageSectionObject->FileObject->SectionObjectPointer->ImageSectionObject = NULL;
        MiReleasePfnLock(OldIrql);

        ObDereferenceObject(ImageSectionObject->FileObject);

        NrSegments = ImageSectionObject->NrSegments;
        SectionSegments = ImageSectionObject->Segments;
        for (i = 0; i < NrSegments; i++)
        {
            if (SectionSegments[i].Image.Characteristics & IMAGE_SCN_MEM_SHARED)
            {
                MmpFreePageFileSegment(&SectionSegments[i]);
            }

            MmFreePageTablesSectionSegment(&SectionSegments[i], NULL);
        }

        ExFreePoolWithTag(ImageSectionObject->Segments, TAG_MM_SECTION_SEGMENT);
        ExFreePoolWithTag(ImageSectionObject, TAG_MM_SECTION_SEGMENT);
    }
}

VOID
NTAPI
MmSharePageEntrySectionSegment(PMM_SECTION_SEGMENT Segment,
                               PLARGE_INTEGER Offset)
{
    ULONG_PTR Entry;
    BOOLEAN Dirty;

    Entry = MmGetPageEntrySectionSegment(Segment, Offset);
    if (Entry == 0)
    {
        DPRINT1("Entry == 0 for MmSharePageEntrySectionSegment\n");
        KeBugCheck(MEMORY_MANAGEMENT);
    }
    if (SHARE_COUNT_FROM_SSE(Entry) == MAX_SHARE_COUNT)
    {
        DPRINT1("Maximum share count reached\n");
        KeBugCheck(MEMORY_MANAGEMENT);
    }
    if (IS_SWAP_FROM_SSE(Entry))
    {
        KeBugCheck(MEMORY_MANAGEMENT);
    }
    Dirty = IS_DIRTY_SSE(Entry);
    Entry = MAKE_SSE(PAGE_FROM_SSE(Entry), SHARE_COUNT_FROM_SSE(Entry) + 1);
    if (Dirty)
        Entry = DIRTY_SSE(Entry);
    MmSetPageEntrySectionSegment(Segment, Offset, Entry);
}

BOOLEAN
NTAPI
MmUnsharePageEntrySectionSegment(PMEMORY_AREA MemoryArea,
                                 PMM_SECTION_SEGMENT Segment,
                                 PLARGE_INTEGER Offset,
                                 BOOLEAN Dirty,
                                 BOOLEAN PageOut,
                                 ULONG_PTR *InEntry)
{
    ULONG_PTR Entry = InEntry ? *InEntry : MmGetPageEntrySectionSegment(Segment, Offset);
    PFN_NUMBER Page = PFN_FROM_SSE(Entry);
    ULONG_PTR NewEntry = 0;
    SWAPENTRY SwapEntry;

    if (Entry == 0)
    {
        DPRINT1("Entry == 0 for MmUnsharePageEntrySectionSegment\n");
        KeBugCheck(MEMORY_MANAGEMENT);
    }
    if (SHARE_COUNT_FROM_SSE(Entry) == 0)
    {
        DPRINT1("Zero share count for unshare (Seg %p Offset %x Page %x)\n", Segment, Offset->LowPart, PFN_FROM_SSE(Entry));
        KeBugCheck(MEMORY_MANAGEMENT);
    }
    if (IS_SWAP_FROM_SSE(Entry))
    {
        KeBugCheck(MEMORY_MANAGEMENT);
    }
    Dirty = Dirty || IS_DIRTY_SSE(Entry);
    Entry = MAKE_SSE(PAGE_FROM_SSE(Entry), SHARE_COUNT_FROM_SSE(Entry) - 1);
    if (Dirty) Entry = DIRTY_SSE(Entry);

    if (SHARE_COUNT_FROM_SSE(Entry) > 0)
    {
        /* Update the page mapping in the segment and we're done */
        if (InEntry)
            *InEntry = Entry;
        else
            MmSetPageEntrySectionSegment(Segment, Offset, Entry);
        return FALSE;
    }

    if (Dirty && (MemoryArea->VadNode.u.VadFlags.VadType != VadImageMap))
    {
        ASSERT(!Segment->WriteCopy);
        ASSERT(MmGetSavedSwapEntryPage(Page) == 0);

        /* The entry must be written back to the disk, so let this in the segment, the page-out thread will take care of this */
        MmSetPageEntrySectionSegment(Segment, Offset, Entry);
        return FALSE;
    }

    /* Only valid case for shared dirty pages is shared image section */
    ASSERT(!Dirty || (Segment->Image.Characteristics & IMAGE_SCN_MEM_SHARED));

    SwapEntry = MmGetSavedSwapEntryPage(Page);
    if (Dirty && !SwapEntry)
    {
        SwapEntry = MmAllocSwapPage();
        if (!SwapEntry)
        {
            /* We can't have a swap entry for this page. Let the segment keep it */
            MmSetPageEntrySectionSegment(Segment, Offset, Entry);
            return FALSE;
        }
    }

    if (Dirty)
    {
        NTSTATUS Status = MmWriteToSwapPage(SwapEntry, Page);
        if (!NT_SUCCESS(Status))
        {
            /* We failed. Clean up */
            MmSetSavedSwapEntryPage(Page, 0);
            MmFreeSwapPage(SwapEntry);
            MmSetPageEntrySectionSegment(Segment, Offset, Entry);
            return FALSE;
        }
    }

    if (SwapEntry)
    {
        NewEntry = MAKE_SWAP_SSE(SwapEntry);
        MmSetSavedSwapEntryPage(Page, 0);
    }

    /* We can let this go */
    MmSetPageEntrySectionSegment(Segment, Offset, NewEntry);
    MmReleasePageMemoryConsumer(MC_USER, Page);
    MiSetPageEvent(NULL, NULL);
    return TRUE;
}

NTSTATUS
NTAPI
MiCopyFromUserPage(PFN_NUMBER DestPage, const VOID *SrcAddress)
{
    PEPROCESS Process;
    KIRQL Irql;
    PVOID DestAddress;

    Process = PsGetCurrentProcess();
    DestAddress = MiMapPageInHyperSpace(Process, DestPage, &Irql);
    if (DestAddress == NULL)
    {
        return(STATUS_NO_MEMORY);
    }
    ASSERT((ULONG_PTR)DestAddress % PAGE_SIZE == 0);
    ASSERT((ULONG_PTR)SrcAddress % PAGE_SIZE == 0);
    RtlCopyMemory(DestAddress, SrcAddress, PAGE_SIZE);
    MiUnmapPageInHyperSpace(Process, DestAddress, Irql);
    return(STATUS_SUCCESS);
}

#ifndef NEWCC
NTSTATUS
NTAPI
MiReadPage(PMEMORY_AREA MemoryArea,
           LONGLONG SegOffset,
           PPFN_NUMBER Page)
/*
 * FUNCTION: Read a page for a section backed memory area.
 * PARAMETERS:
 *       MemoryArea - Memory area to read the page for.
 *       Offset - Offset of the page to read.
 *       Page - Variable that receives a page contains the read data.
 */
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    UCHAR MdlBase[sizeof(MDL) + sizeof(PFN_NUMBER)];
    PMDL Mdl = (PMDL)MdlBase;
    PFILE_OBJECT FileObject = MemoryArea->SectionData.Segment->FileObject;
    LARGE_INTEGER FileOffset;

    FileOffset.QuadPart = MemoryArea->SectionData.Segment->Image.FileOffset + SegOffset;

    DPRINT("Reading file at offset %08x:%08x\n", FileOffset.HighPart, FileOffset.LowPart);

    Status = MmRequestPageMemoryConsumer(MC_USER, FALSE, Page);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(MdlBase, sizeof(MdlBase));
    MmInitializeMdl(Mdl, NULL, PAGE_SIZE);
    MmBuildMdlFromPages(Mdl, Page);
    Mdl->MdlFlags |= MDL_PAGES_LOCKED | MDL_IO_PAGE_READ;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Status = IoPageRead(FileObject, Mdl, &FileOffset, &Event, &IoStatus);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, WrPageIn, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA)
    {
        MmUnmapLockedPages (Mdl->MappedSystemVa, Mdl);
    }

    if (Status == STATUS_END_OF_FILE)
    {
        DPRINT1("Got STATUS_END_OF_FILE at offset %I64d for file %wZ.\n", SegOffset, &FileObject->FileName);
        Status = STATUS_SUCCESS;
    }

    if ((MemoryArea->VadNode.u.VadFlags.VadType == VadImageMap)
        && ((SegOffset + PAGE_SIZE) > MemoryArea->SectionData.Segment->RawLength.QuadPart))
    {
        KIRQL OldIrql;
        PUCHAR PageMap;

        DPRINT("Zeroing at offset %I64d for file %wZ.\n", SegOffset, &FileObject->FileName);

        /* Zero out the end of it */
        PageMap = MiMapPageInHyperSpace(PsGetCurrentProcess(), *Page, &OldIrql);
        RtlZeroMemory(PageMap + MemoryArea->SectionData.Segment->RawLength.QuadPart - SegOffset,
                      PAGE_SIZE - (MemoryArea->SectionData.Segment->RawLength.QuadPart - SegOffset));
        MiUnmapPageInHyperSpace(PsGetCurrentProcess(), PageMap, OldIrql);
    }

    return Status;
}

#else
NTSTATUS
NTAPI
MiReadPage(PMEMORY_AREA MemoryArea,
           LONGLONG SegOffset,
           PPFN_NUMBER Page)
/*
 * FUNCTION: Read a page for a section backed memory area.
 * PARAMETERS:
 *       MemoryArea - Memory area to read the page for.
 *       Offset - Offset of the page to read.
 *       Page - Variable that receives a page contains the read data.
 */
{
    MM_REQUIRED_RESOURCES Resources;
    NTSTATUS Status;

    RtlZeroMemory(&Resources, sizeof(MM_REQUIRED_RESOURCES));

    Resources.Context = MemoryArea->SectionData.Section->FileObject;
    Resources.FileOffset.QuadPart = SegOffset +
                                    MemoryArea->SectionData.Segment->Image.FileOffset;
    Resources.Consumer = MC_USER;
    Resources.Amount = PAGE_SIZE;

    DPRINT("%S, offset 0x%x, len 0x%x, page 0x%x\n", ((PFILE_OBJECT)Resources.Context)->FileName.Buffer, Resources.FileOffset.LowPart, Resources.Amount, Resources.Page[0]);

    Status = MiReadFilePage(MmGetKernelAddressSpace(), MemoryArea, &Resources);
    *Page = Resources.Page[0];
    return Status;
}
#endif

static VOID
MmAlterViewAttributes(PMMSUPPORT AddressSpace,
                      PVOID BaseAddress,
                      SIZE_T RegionSize,
                      ULONG OldType,
                      ULONG OldProtect,
                      ULONG NewType,
                      ULONG NewProtect)
{
    PMEMORY_AREA MemoryArea;
    PMM_SECTION_SEGMENT Segment;
    BOOLEAN DoCOW = FALSE;
    ULONG i;
    PEPROCESS Process = MmGetAddressSpaceOwner(AddressSpace);

    MemoryArea = MmLocateMemoryAreaByAddress(AddressSpace, BaseAddress);
    ASSERT(MemoryArea != NULL);
    Segment = MemoryArea->SectionData.Segment;
    MmLockSectionSegment(Segment);

    if ((Segment->WriteCopy) &&
            (NewProtect == PAGE_READWRITE || NewProtect == PAGE_EXECUTE_READWRITE))
    {
        DoCOW = TRUE;
    }

    if (OldProtect != NewProtect)
    {
        for (i = 0; i < PAGE_ROUND_UP(RegionSize) / PAGE_SIZE; i++)
        {
            SWAPENTRY SwapEntry;
            PVOID Address = (char*)BaseAddress + (i * PAGE_SIZE);
            ULONG Protect = NewProtect;

            /* Wait for a wait entry to disappear */
            do
            {
                MmGetPageFileMapping(Process, Address, &SwapEntry);
                if (SwapEntry != MM_WAIT_ENTRY)
                    break;
                MiWaitForPageEvent(Process, Address);
            }
            while (TRUE);

            /*
             * If we doing COW for this segment then check if the page is
             * already private.
             */
            if (DoCOW && MmIsPagePresent(Process, Address))
            {
                LARGE_INTEGER Offset;
                ULONG_PTR Entry;
                PFN_NUMBER Page;

                Offset.QuadPart = (ULONG_PTR)Address - MA_GetStartingAddress(MemoryArea)
                                  + MemoryArea->SectionData.ViewOffset.QuadPart;
                Entry = MmGetPageEntrySectionSegment(Segment, &Offset);
                /*
                 * An MM_WAIT_ENTRY is ok in this case...  It'll just count as
                 * IS_SWAP_FROM_SSE and we'll do the right thing.
                 */
                Page = MmGetPfnForProcess(Process, Address);

                Protect = PAGE_READONLY;
                if (IS_SWAP_FROM_SSE(Entry) || PFN_FROM_SSE(Entry) != Page)
                {
                    Protect = NewProtect;
                }
            }

            if (MmIsPagePresent(Process, Address) || MmIsDisabledPage(Process, Address))
            {
                MmSetPageProtect(Process, Address,
                                 Protect);
            }
        }
    }

    MmUnlockSectionSegment(Segment);
}

NTSTATUS
NTAPI
MmNotPresentFaultSectionView(PMMSUPPORT AddressSpace,
                             MEMORY_AREA* MemoryArea,
                             PVOID Address,
                             BOOLEAN Locked)
{
    LARGE_INTEGER Offset;
    PFN_NUMBER Page;
    NTSTATUS Status;
    PMM_SECTION_SEGMENT Segment;
    ULONG_PTR Entry;
    ULONG_PTR Entry1;
    ULONG Attributes;
    PMM_REGION Region;
    BOOLEAN HasSwapEntry;
    PVOID PAddress;
    PEPROCESS Process = MmGetAddressSpaceOwner(AddressSpace);
    SWAPENTRY SwapEntry;

    /*
     * There is a window between taking the page fault and locking the
     * address space when another thread could load the page so we check
     * that.
     */
    if (MmIsPagePresent(Process, Address))
    {
        return(STATUS_SUCCESS);
    }

    if (MmIsDisabledPage(Process, Address))
    {
        return(STATUS_ACCESS_VIOLATION);
    }

    /*
     * Check for the virtual memory area being deleted.
     */
    if (MemoryArea->DeleteInProgress)
    {
        return(STATUS_UNSUCCESSFUL);
    }

    PAddress = MM_ROUND_DOWN(Address, PAGE_SIZE);
    Offset.QuadPart = (ULONG_PTR)PAddress - MA_GetStartingAddress(MemoryArea)
                      + MemoryArea->SectionData.ViewOffset.QuadPart;

    Segment = MemoryArea->SectionData.Segment;
    Region = MmFindRegion((PVOID)MA_GetStartingAddress(MemoryArea),
                          &MemoryArea->SectionData.RegionListHead,
                          Address, NULL);
    ASSERT(Region != NULL);

    /* Check for a NOACCESS mapping */
    if (Region->Protect & PAGE_NOACCESS)
    {
        return STATUS_ACCESS_VIOLATION;
    }

    if (Region->Protect & PAGE_GUARD)
    {
        /* Remove it */
        Status = MmAlterRegion(AddressSpace, (PVOID)MA_GetStartingAddress(MemoryArea),
                &MemoryArea->SectionData.RegionListHead,
                Address, PAGE_SIZE, Region->Type, Region->Protect & ~PAGE_GUARD,
                MmAlterViewAttributes);

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Removing PAGE_GUARD protection failed : 0x%08x.\n", Status);
        }

        return STATUS_GUARD_PAGE_VIOLATION;
    }

    /*
     * Lock the segment
     */
    MmLockSectionSegment(Segment);
    Entry = MmGetPageEntrySectionSegment(Segment, &Offset);
    /*
     * Check if this page needs to be mapped COW
     */
    if ((Segment->WriteCopy) &&
            (Region->Protect == PAGE_READWRITE ||
             Region->Protect == PAGE_EXECUTE_READWRITE))
    {
        Attributes = Region->Protect == PAGE_READWRITE ? PAGE_READONLY : PAGE_EXECUTE_READ;
    }
    else
    {
        Attributes = Region->Protect;
    }

    /*
     * Check if someone else is already handling this fault, if so wait
     * for them
     */
    if (Entry && MM_IS_WAIT_PTE(Entry))
    {
        MmUnlockSectionSegment(Segment);
        MmUnlockAddressSpace(AddressSpace);
        MiWaitForPageEvent(NULL, NULL);
        MmLockAddressSpace(AddressSpace);
        DPRINT("Address 0x%p\n", Address);
        return(STATUS_MM_RESTART_OPERATION);
    }

    HasSwapEntry = MmIsPageSwapEntry(Process, Address);

    /* See if we should use a private page */
    if (HasSwapEntry)
    {
        SWAPENTRY DummyEntry;

        MmGetPageFileMapping(Process, Address, &SwapEntry);
        if (SwapEntry == MM_WAIT_ENTRY)
        {
            MmUnlockSectionSegment(Segment);
            MmUnlockAddressSpace(AddressSpace);
            MiWaitForPageEvent(NULL, NULL);
            MmLockAddressSpace(AddressSpace);
            return STATUS_MM_RESTART_OPERATION;
        }

        /*
            * Must be private page we have swapped out.
            */

        /*
        * Sanity check
        */
        MmDeletePageFileMapping(Process, Address, &SwapEntry);
        MmUnlockSectionSegment(Segment);

        /* Tell everyone else we are serving the fault. */
        MmCreatePageFileMapping(Process, Address, MM_WAIT_ENTRY);

        MmUnlockAddressSpace(AddressSpace);
        MI_SET_USAGE(MI_USAGE_SECTION);
        if (Process) MI_SET_PROCESS2(Process->ImageFileName);
        if (!Process) MI_SET_PROCESS2("Kernel Section");
        Status = MmRequestPageMemoryConsumer(MC_USER, TRUE, &Page);
        if (!NT_SUCCESS(Status))
        {
            KeBugCheck(MEMORY_MANAGEMENT);
        }

        if (HasSwapEntry)
        {
            Status = MmReadFromSwapPage(SwapEntry, Page);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("MmReadFromSwapPage failed, status = %x\n", Status);
                KeBugCheck(MEMORY_MANAGEMENT);
            }
        }

        MmLockAddressSpace(AddressSpace);
        MmDeletePageFileMapping(Process, PAddress, &DummyEntry);
        Status = MmCreateVirtualMapping(Process,
                                        PAddress,
                                        Region->Protect,
                                        &Page,
                                        1);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("MmCreateVirtualMapping failed, not out of memory\n");
            KeBugCheck(MEMORY_MANAGEMENT);
            return(Status);
        }

        /*
         * Store the swap entry for later use.
         */
        if (HasSwapEntry)
            MmSetSavedSwapEntryPage(Page, SwapEntry);

        /*
         * Add the page to the process's working set
         */
        if (Process) MmInsertRmap(Page, Process, Address);
        /*
         * Finish the operation
         */
        MiSetPageEvent(Process, Address);
        DPRINT("Address 0x%p\n", Address);
        return(STATUS_SUCCESS);
    }

    /*
     * Satisfying a page fault on a map of /Device/PhysicalMemory is easy
     */
    if ((*Segment->Flags) & MM_PHYSICALMEMORY_SEGMENT)
    {
        MmUnlockSectionSegment(Segment);
        /*
         * Just map the desired physical page
         */
        Page = (PFN_NUMBER)(Offset.QuadPart >> PAGE_SHIFT);
        Status = MmCreateVirtualMappingUnsafe(Process,
                                              PAddress,
                                              Region->Protect,
                                              &Page,
                                              1);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("MmCreateVirtualMappingUnsafe failed, not out of memory\n");
            KeBugCheck(MEMORY_MANAGEMENT);
            return(Status);
        }

        /*
         * Cleanup and release locks
         */
        MiSetPageEvent(Process, Address);
        DPRINT("Address 0x%p\n", Address);
        return(STATUS_SUCCESS);
    }

    /*
     * Get the entry corresponding to the offset within the section
     */
    Entry = MmGetPageEntrySectionSegment(Segment, &Offset);

    if (Entry == 0)
    {
        SWAPENTRY FakeSwapEntry;

        /*
         * If the entry is zero (and it can't change because we have
         * locked the segment) then we need to load the page.
         */

        /*
         * Release all our locks and read in the page from disk
         */
        MmSetPageEntrySectionSegment(Segment, &Offset, MAKE_SWAP_SSE(MM_WAIT_ENTRY));
        MmUnlockSectionSegment(Segment);
        MmCreatePageFileMapping(Process, PAddress, MM_WAIT_ENTRY);
        MmUnlockAddressSpace(AddressSpace);

        if ((Offset.QuadPart >= (LONGLONG)PAGE_ROUND_UP(Segment->RawLength.QuadPart)) && (MemoryArea->VadNode.u.VadFlags.VadType == VadImageMap))
        {
            MI_SET_USAGE(MI_USAGE_SECTION);
            if (Process) MI_SET_PROCESS2(Process->ImageFileName);
            if (!Process) MI_SET_PROCESS2("Kernel Section");
            Status = MmRequestPageMemoryConsumer(MC_USER, TRUE, &Page);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("MmRequestPageMemoryConsumer failed (Status %x)\n", Status);
            }

        }
        else
        {
            DPRINT("Getting fresh page for file %wZ at offset %I64d.\n", &Segment->FileObject->FileName, Offset.QuadPart);
            Status = MiReadPage(MemoryArea, Offset.QuadPart, &Page);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("MiReadPage failed (Status %x)\n", Status);
            }
        }
        if (!NT_SUCCESS(Status))
        {
            /*
             * FIXME: What do we know in this case?
             */
            /*
             * Cleanup and release locks
             */
            MmLockAddressSpace(AddressSpace);
            MiSetPageEvent(Process, Address);
            DPRINT("Address 0x%p\n", Address);
            return(Status);
        }

        /* Lock both segment and process address space while we proceed. */
        MmLockAddressSpace(AddressSpace);
        MmLockSectionSegment(Segment);

        MmDeletePageFileMapping(Process, PAddress, &FakeSwapEntry);
        DPRINT("CreateVirtualMapping Page %x Process %p PAddress %p Attributes %x\n",
               Page, Process, PAddress, Attributes);
        Status = MmCreateVirtualMapping(Process,
                                        PAddress,
                                        Attributes,
                                        &Page,
                                        1);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Unable to create virtual mapping\n");
            KeBugCheck(MEMORY_MANAGEMENT);
        }
        ASSERT(MmIsPagePresent(Process, PAddress));
        if (Process)
            MmInsertRmap(Page, Process, Address);

        /* Set this section offset has being backed by our new page. */
        Entry = MAKE_SSE(Page << PAGE_SHIFT, 1);
        MmSetPageEntrySectionSegment(Segment, &Offset, Entry);
        MmUnlockSectionSegment(Segment);

        MiSetPageEvent(Process, Address);
        DPRINT("Address 0x%p\n", Address);
        return(STATUS_SUCCESS);
    }
    else if (IS_SWAP_FROM_SSE(Entry))
    {
        SWAPENTRY SwapEntry;

        SwapEntry = SWAPENTRY_FROM_SSE(Entry);

        /* See if a page op is running on this segment. */
        if (SwapEntry == MM_WAIT_ENTRY)
        {
            MmUnlockSectionSegment(Segment);
            MmUnlockAddressSpace(AddressSpace);
            MiWaitForPageEvent(NULL, NULL);
            MmLockAddressSpace(AddressSpace);
            return STATUS_MM_RESTART_OPERATION;
        }

        /*
        * Release all our locks and read in the page from disk
        */
        MmUnlockSectionSegment(Segment);

        MmUnlockAddressSpace(AddressSpace);
        MI_SET_USAGE(MI_USAGE_SECTION);
        if (Process) MI_SET_PROCESS2(Process->ImageFileName);
        if (!Process) MI_SET_PROCESS2("Kernel Section");
        Status = MmRequestPageMemoryConsumer(MC_USER, TRUE, &Page);
        if (!NT_SUCCESS(Status))
        {
            KeBugCheck(MEMORY_MANAGEMENT);
        }

        Status = MmReadFromSwapPage(SwapEntry, Page);
        if (!NT_SUCCESS(Status))
        {
            KeBugCheck(MEMORY_MANAGEMENT);
        }

        /*
         * Relock the address space and segment
         */
        MmLockAddressSpace(AddressSpace);
        MmLockSectionSegment(Segment);

        /*
         * Check the entry. No one should change the status of a page
         * that has a pending page-in.
         */
        Entry1 = MmGetPageEntrySectionSegment(Segment, &Offset);
        if (Entry != Entry1)
        {
            DPRINT1("Someone changed ppte entry while we slept (%x vs %x)\n", Entry, Entry1);
            KeBugCheck(MEMORY_MANAGEMENT);
        }

        /*
         * Save the swap entry.
         */
        MmSetSavedSwapEntryPage(Page, SwapEntry);

        /* Map the page into the process address space */
        Status = MmCreateVirtualMapping(Process,
                                        PAddress,
                                        Attributes,
                                        &Page,
                                        1);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Unable to create virtual mapping\n");
            KeBugCheck(MEMORY_MANAGEMENT);
        }
        if (Process)
            MmInsertRmap(Page, Process, Address);

        /*
         * Mark the offset within the section as having valid, in-memory
         * data
         */
        Entry = MAKE_SSE(Page << PAGE_SHIFT, 1);
        MmSetPageEntrySectionSegment(Segment, &Offset, Entry);
        MmUnlockSectionSegment(Segment);

        MiSetPageEvent(Process, Address);
        DPRINT("Address 0x%p\n", Address);
        return(STATUS_SUCCESS);
    }
    else
    {
        /* We already have a page on this section offset. Map it into the process address space. */
        Page = PFN_FROM_SSE(Entry);

        Status = MmCreateVirtualMapping(Process,
                                        PAddress,
                                        Attributes,
                                        &Page,
                                        1);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Unable to create virtual mapping\n");
            KeBugCheck(MEMORY_MANAGEMENT);
        }

        if (Process)
            MmInsertRmap(Page, Process, Address);

        /* Take a reference on it */
        MmSharePageEntrySectionSegment(Segment, &Offset);
        MmUnlockSectionSegment(Segment);

        MiSetPageEvent(Process, Address);
        DPRINT("Address 0x%p\n", Address);
        return(STATUS_SUCCESS);
    }
}

NTSTATUS
NTAPI
MmAccessFaultSectionView(PMMSUPPORT AddressSpace,
                         MEMORY_AREA* MemoryArea,
                         PVOID Address)
{
    PMM_SECTION_SEGMENT Segment;
    PFN_NUMBER OldPage;
    PFN_NUMBER NewPage;
    NTSTATUS Status;
    PVOID PAddress;
    LARGE_INTEGER Offset;
    PMM_REGION Region;
    ULONG_PTR Entry;
    PEPROCESS Process = MmGetAddressSpaceOwner(AddressSpace);

    DPRINT("MmAccessFaultSectionView(%p, %p, %p)\n", AddressSpace, MemoryArea, Address);

    /* Make sure we have a page mapping for this address.  */
    Status = MmNotPresentFaultSectionView(AddressSpace, MemoryArea, Address, TRUE);
    if (!NT_SUCCESS(Status))
    {
        /* This is invalid access ! */
        return Status;
    }

    /*
     * Check if the page has already been set readwrite
     */
    if (MmGetPageProtect(Process, Address) & PAGE_READWRITE)
    {
        DPRINT("Address 0x%p\n", Address);
        return(STATUS_SUCCESS);
    }

    /*
     * Find the offset of the page
     */
    PAddress = MM_ROUND_DOWN(Address, PAGE_SIZE);
    Offset.QuadPart = (ULONG_PTR)PAddress - MA_GetStartingAddress(MemoryArea)
                      + MemoryArea->SectionData.ViewOffset.QuadPart;

    Segment = MemoryArea->SectionData.Segment;
    Region = MmFindRegion((PVOID)MA_GetStartingAddress(MemoryArea),
                          &MemoryArea->SectionData.RegionListHead,
                          Address, NULL);
    ASSERT(Region != NULL);

    /*
     * Check if we are doing COW
     */
    if (!((Segment->WriteCopy) &&
            (Region->Protect == PAGE_READWRITE ||
             Region->Protect == PAGE_EXECUTE_READWRITE)))
    {
        DPRINT("Address 0x%p\n", Address);
        return(STATUS_ACCESS_VIOLATION);
    }

    /* Get the page mapping this section offset. */
    MmLockSectionSegment(Segment);
    Entry = MmGetPageEntrySectionSegment(Segment, &Offset);

    /* Get the current page mapping for the process */
    ASSERT(MmIsPagePresent(Process, PAddress));
    OldPage = MmGetPfnForProcess(Process, PAddress);
    ASSERT(OldPage != 0);

    if (IS_SWAP_FROM_SSE(Entry) ||
            PFN_FROM_SSE(Entry) != OldPage)
    {
        MmUnlockSectionSegment(Segment);
        /* This is a private page. We must only change the page protection. */
        MmSetPageProtect(Process, PAddress, Region->Protect);
        return(STATUS_SUCCESS);
    }

    /*
     * Allocate a page
     */
    MI_SET_USAGE(MI_USAGE_SECTION);
    if (Process) MI_SET_PROCESS2(Process->ImageFileName);
    if (!Process) MI_SET_PROCESS2("Kernel Section");
    Status = MmRequestPageMemoryConsumer(MC_USER, TRUE, &NewPage);
    if (!NT_SUCCESS(Status))
    {
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    /*
     * Copy the old page
     */
    NT_VERIFY(NT_SUCCESS(MiCopyFromUserPage(NewPage, PAddress)));

    /*
     * Unshare the old page.
     */
    DPRINT("Swapping page (Old %x New %x)\n", OldPage, NewPage);
    MmDeleteVirtualMapping(Process, PAddress, NULL, NULL);
    if (Process)
        MmDeleteRmap(OldPage, Process, PAddress);
    MmUnsharePageEntrySectionSegment(MemoryArea, Segment, &Offset, FALSE, FALSE, NULL);
    MmUnlockSectionSegment(Segment);

    /*
     * Set the PTE to point to the new page
     */
    Status = MmCreateVirtualMapping(Process,
                                    PAddress,
                                    Region->Protect,
                                    &NewPage,
                                    1);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MmCreateVirtualMapping failed, unable to create virtual mapping, not out of memory\n");
        KeBugCheck(MEMORY_MANAGEMENT);
        return(Status);
    }

    if (Process)
        MmInsertRmap(NewPage, Process, PAddress);

    MiSetPageEvent(Process, Address);
    DPRINT("Address 0x%p\n", Address);
    return(STATUS_SUCCESS);
}

NTSTATUS
NTAPI
MmProtectSectionView(PMMSUPPORT AddressSpace,
                     PMEMORY_AREA MemoryArea,
                     PVOID BaseAddress,
                     SIZE_T Length,
                     ULONG Protect,
                     PULONG OldProtect)
{
    PMM_REGION Region;
    NTSTATUS Status;
    ULONG_PTR MaxLength;

    MaxLength = MA_GetEndingAddress(MemoryArea) - (ULONG_PTR)BaseAddress;
    if (Length > MaxLength)
        Length = (ULONG)MaxLength;

    Region = MmFindRegion((PVOID)MA_GetStartingAddress(MemoryArea),
                          &MemoryArea->SectionData.RegionListHead,
                          BaseAddress, NULL);
    ASSERT(Region != NULL);

    if ((MemoryArea->Flags & SEC_NO_CHANGE) &&
            Region->Protect != Protect)
    {
        return STATUS_INVALID_PAGE_PROTECTION;
    }

    *OldProtect = Region->Protect;
    Status = MmAlterRegion(AddressSpace, (PVOID)MA_GetStartingAddress(MemoryArea),
                           &MemoryArea->SectionData.RegionListHead,
                           BaseAddress, Length, Region->Type, Protect,
                           MmAlterViewAttributes);

    return(Status);
}

NTSTATUS NTAPI
MmQuerySectionView(PMEMORY_AREA MemoryArea,
                   PVOID Address,
                   PMEMORY_BASIC_INFORMATION Info,
                   PSIZE_T ResultLength)
{
    PMM_REGION Region;
    PVOID RegionBaseAddress;
    PMM_SECTION_SEGMENT Segment;

    Region = MmFindRegion((PVOID)MA_GetStartingAddress(MemoryArea),
                          &MemoryArea->SectionData.RegionListHead,
                          Address, &RegionBaseAddress);
    if (Region == NULL)
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (MemoryArea->VadNode.u.VadFlags.VadType == VadImageMap)
    {
        Segment = MemoryArea->SectionData.Segment;
        Info->AllocationBase = (PUCHAR)MA_GetStartingAddress(MemoryArea) - Segment->Image.VirtualAddress;
        Info->Type = MEM_IMAGE;
    }
    else
    {
        Info->AllocationBase = (PVOID)MA_GetStartingAddress(MemoryArea);
        Info->Type = MEM_MAPPED;
    }
    Info->BaseAddress = RegionBaseAddress;
    Info->AllocationProtect = MmProtectToValue[MemoryArea->VadNode.u.VadFlags.Protection];
    Info->RegionSize = Region->Length;
    Info->State = MEM_COMMIT;
    Info->Protect = Region->Protect;

    *ResultLength = sizeof(MEMORY_BASIC_INFORMATION);
    return(STATUS_SUCCESS);
}

VOID NTAPI
MmpDeleteSection(PVOID ObjectBody)
{
    PSECTION Section = ObjectBody;

    /* Check if it's an ARM3, or ReactOS section */
    if (!MiIsRosSectionObject(Section))
    {
        MiDeleteARM3Section(ObjectBody);
        return;
    }

    DPRINT("MmpDeleteSection(ObjectBody %p)\n", ObjectBody);
    if (Section->u.Flags.Image)
    {
        PMM_IMAGE_SECTION_OBJECT ImageSectionObject = (PMM_IMAGE_SECTION_OBJECT)Section->Segment;

        /*
         * NOTE: Section->ImageSection can be NULL for short time
         * during the section creating. If we fail for some reason
         * until the image section is properly initialized we shouldn't
         * process further here.
         */
        if (Section->Segment == NULL)
            return;

        /* We just dereference the first segment */
        ASSERT(ImageSectionObject->RefCount > 0);
        MmDereferenceSegment(ImageSectionObject->Segments);
    }
#ifdef NEWCC
    else if (Section->Segment && Section->Segment->Flags & MM_DATAFILE_SEGMENT)
    {
        ULONG RefCount = 0;
        PMM_SECTION_SEGMENT Segment = Section->Segment;

        if (Segment &&
                (RefCount = InterlockedDecrementUL(&Segment->ReferenceCount)) == 0)
        {
            DPRINT("Freeing section segment\n");
            Section->Segment = NULL;
            MmFinalizeSegment(Segment);
        }
        else
        {
            DPRINT("RefCount %d\n", RefCount);
        }
    }
#endif
    else
    {
        PMM_SECTION_SEGMENT Segment = (PMM_SECTION_SEGMENT)Section->Segment;

        /*
         * NOTE: Section->Segment can be NULL for short time
         * during the section creating.
         */
        if (Segment == NULL)
            return;

        Segment->SectionCount--;
        MmDereferenceSegment(Segment);
    }
}

VOID NTAPI
MmpCloseSection(IN PEPROCESS Process OPTIONAL,
                IN PVOID Object,
                IN ACCESS_MASK GrantedAccess,
                IN ULONG ProcessHandleCount,
                IN ULONG SystemHandleCount)
{
    DPRINT("MmpCloseSection(OB %p, HC %lu)\n", Object, ProcessHandleCount);
}

CODE_SEG("INIT")
NTSTATUS
NTAPI
MmCreatePhysicalMemorySection(VOID)
{
    PSECTION PhysSection;
    NTSTATUS Status;
    OBJECT_ATTRIBUTES Obj;
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\PhysicalMemory");
    LARGE_INTEGER SectionSize;
    HANDLE Handle;
    PMM_SECTION_SEGMENT Segment;

    /*
     * Create the section mapping physical memory
     */
    SectionSize.QuadPart = MmNumberOfPhysicalPages * PAGE_SIZE;
    InitializeObjectAttributes(&Obj,
                               &Name,
                               OBJ_PERMANENT | OBJ_KERNEL_EXCLUSIVE,
                               NULL,
                               NULL);
    /*
     * Create the Object
     */
    Status = ObCreateObject(KernelMode,
                            MmSectionObjectType,
                            &Obj,
                            ExGetPreviousMode(),
                            NULL,
                            sizeof(*PhysSection),
                            0,
                            0,
                            (PVOID*)&PhysSection);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MmCreatePhysicalMemorySection: failed to create object (0x%lx)\n", Status);
        return(Status);
    }

    /*
     * Initialize it
     */
    RtlZeroMemory(PhysSection, sizeof(*PhysSection));

    /* Mark this as a "ROS Section" */
    PhysSection->u.Flags.filler = 1;
    PhysSection->InitialPageProtection = PAGE_EXECUTE_READWRITE;
    PhysSection->u.Flags.PhysicalMemory = 1;
    PhysSection->SizeOfSection = SectionSize;
    Segment = ExAllocatePoolWithTag(NonPagedPool, sizeof(MM_SECTION_SEGMENT),
                                    TAG_MM_SECTION_SEGMENT);
    if (Segment == NULL)
    {
        ObDereferenceObject(PhysSection);
        return(STATUS_NO_MEMORY);
    }
    RtlZeroMemory(Segment, sizeof(MM_SECTION_SEGMENT));
    PhysSection->Segment = (PSEGMENT)Segment;
    Segment->RefCount = 1;

    Segment->ReferenceCount = &Segment->RefCount;
    Segment->Flags = &Segment->SegFlags;

    ExInitializeFastMutex(&Segment->Lock);
    Segment->Image.FileOffset = 0;
    Segment->Protection = PAGE_EXECUTE_READWRITE;
    Segment->RawLength = SectionSize;
    Segment->Length = SectionSize;
    Segment->SegFlags = MM_PHYSICALMEMORY_SEGMENT;
    Segment->WriteCopy = FALSE;
    Segment->Image.VirtualAddress = 0;
    Segment->Image.Characteristics = 0;
    MiInitializeSectionPageTable(Segment);

    Status = ObInsertObject(PhysSection,
                            NULL,
                            SECTION_ALL_ACCESS,
                            0,
                            NULL,
                            &Handle);
    if (!NT_SUCCESS(Status))
    {
        ObDereferenceObject(PhysSection);
    }
    ObCloseHandle(Handle, KernelMode);

    return(STATUS_SUCCESS);
}

CODE_SEG("INIT")
NTSTATUS
NTAPI
MmInitSectionImplementation(VOID)
{
    OBJECT_TYPE_INITIALIZER ObjectTypeInitializer;
    UNICODE_STRING Name;

    DPRINT("Creating Section Object Type\n");

    /* Initialize the section based root */
    ASSERT(MmSectionBasedRoot.NumberGenericTableElements == 0);
    MmSectionBasedRoot.BalancedRoot.u1.Parent = &MmSectionBasedRoot.BalancedRoot;

    /* Initialize the Section object type  */
    RtlZeroMemory(&ObjectTypeInitializer, sizeof(ObjectTypeInitializer));
    RtlInitUnicodeString(&Name, L"Section");
    ObjectTypeInitializer.Length = sizeof(ObjectTypeInitializer);
    ObjectTypeInitializer.DefaultPagedPoolCharge = sizeof(SECTION);
    ObjectTypeInitializer.PoolType = PagedPool;
    ObjectTypeInitializer.UseDefaultObject = TRUE;
    ObjectTypeInitializer.GenericMapping = MmpSectionMapping;
    ObjectTypeInitializer.DeleteProcedure = MmpDeleteSection;
    ObjectTypeInitializer.CloseProcedure = MmpCloseSection;
    ObjectTypeInitializer.ValidAccessMask = SECTION_ALL_ACCESS;
    ObjectTypeInitializer.InvalidAttributes = OBJ_OPENLINK;
    ObCreateObjectType(&Name, &ObjectTypeInitializer, NULL, &MmSectionObjectType);

    MmCreatePhysicalMemorySection();

    return(STATUS_SUCCESS);
}

static
NTSTATUS
NTAPI
MmCreateDataFileSection(PSECTION *SectionObject,
                        ACCESS_MASK DesiredAccess,
                        POBJECT_ATTRIBUTES ObjectAttributes,
                        PLARGE_INTEGER UMaximumSize,
                        ULONG SectionPageProtection,
                        ULONG AllocationAttributes,
                        PFILE_OBJECT FileObject,
                        BOOLEAN GotFileHandle)
/*
 * Create a section backed by a data file
 */
{
    PSECTION Section;
    NTSTATUS Status;
    LARGE_INTEGER MaximumSize;
    PMM_SECTION_SEGMENT Segment;
    KIRQL OldIrql;

    /*
     * Create the section
     */
    Status = ObCreateObject(ExGetPreviousMode(),
                            MmSectionObjectType,
                            ObjectAttributes,
                            ExGetPreviousMode(),
                            NULL,
                            sizeof(*Section),
                            0,
                            0,
                            (PVOID*)&Section);
    if (!NT_SUCCESS(Status))
    {
        return(Status);
    }
    /*
     * Initialize it
     */
    RtlZeroMemory(Section, sizeof(*Section));

    /* Mark this as a "ROS" section */
    Section->u.Flags.filler = 1;
    Section->InitialPageProtection = SectionPageProtection;
    Section->u.Flags.File = 1;

    if (AllocationAttributes & SEC_NO_CHANGE)
        Section->u.Flags.NoChange = 1;
    if (AllocationAttributes & SEC_RESERVE)
        Section->u.Flags.Reserve = 1;

    if (!GotFileHandle)
    {
        ASSERT(UMaximumSize != NULL);
        // ASSERT(UMaximumSize->QuadPart != 0);
        MaximumSize = *UMaximumSize;
    }
    else
    {
        LARGE_INTEGER FileSize;
        Status = FsRtlGetFileSize(FileObject, &FileSize);
        if (!NT_SUCCESS(Status))
        {
            ObDereferenceObject(Section);
            return Status;
        }

        /*
         * FIXME: Revise this once a locking order for file size changes is
         * decided
         */
        if ((UMaximumSize != NULL) && (UMaximumSize->QuadPart != 0))
        {
            MaximumSize = *UMaximumSize;
        }
        else
        {
            MaximumSize = FileSize;
            /* Mapping zero-sized files isn't allowed. */
            if (MaximumSize.QuadPart == 0)
            {
                ObDereferenceObject(Section);
                return STATUS_MAPPED_FILE_SIZE_ZERO;
            }
        }

        if (MaximumSize.QuadPart > FileSize.QuadPart)
        {
            Status = IoSetInformation(FileObject,
                                      FileEndOfFileInformation,
                                      sizeof(LARGE_INTEGER),
                                      &MaximumSize);
            if (!NT_SUCCESS(Status))
            {
                ObDereferenceObject(Section);
                return(STATUS_SECTION_NOT_EXTENDED);
            }
        }
    }

    if (FileObject->SectionObjectPointer == NULL)
    {
        ObDereferenceObject(Section);
        return STATUS_INVALID_FILE_FOR_SECTION;
    }

    /*
     * Lock the file
     */
    Status = MmspWaitForFileLock(FileObject);
    if (Status != STATUS_SUCCESS)
    {
        ObDereferenceObject(Section);
        return(Status);
    }

    /* Lock the PFN lock while messing with Section Object pointers */
    OldIrql = MiAcquirePfnLock();
    Segment = FileObject->SectionObjectPointer->DataSectionObject;

    while (Segment && (Segment->SegFlags & (MM_SEGMENT_INDELETE | MM_SEGMENT_INCREATE)))
    {
        LARGE_INTEGER ShortTime = {{-10 * 100 * 1000, -1}};

        MiReleasePfnLock(OldIrql);
        KeDelayExecutionThread(KernelMode, FALSE, &ShortTime);
        OldIrql = MiAcquirePfnLock();
        Segment = FileObject->SectionObjectPointer->DataSectionObject;
    }

    /*
     * If this file hasn't been mapped as a data file before then allocate a
     * section segment to describe the data file mapping
     */
    if (Segment == NULL)
    {
        Segment = ExAllocatePoolWithTag(NonPagedPool, sizeof(MM_SECTION_SEGMENT),
                                        TAG_MM_SECTION_SEGMENT);
        if (Segment == NULL)
        {
            //KeSetEvent((PVOID)&FileObject->Lock, IO_NO_INCREMENT, FALSE);
            MiReleasePfnLock(OldIrql);
            ObDereferenceObject(Section);
            return(STATUS_NO_MEMORY);
        }

        /* We are creating it */
        RtlZeroMemory(Segment, sizeof(*Segment));
        Segment->SegFlags = MM_DATAFILE_SEGMENT | MM_SEGMENT_INCREATE;
        Segment->RefCount = 1;

        FileObject->SectionObjectPointer->DataSectionObject = Segment;

        /* We're safe to release the lock now */
        MiReleasePfnLock(OldIrql);

        Section->Segment = (PSEGMENT)Segment;

        /* Self-referencing segment */
        Segment->Flags = &Segment->SegFlags;
        Segment->ReferenceCount = &Segment->RefCount;

        Segment->SectionCount = 1;

        ExInitializeFastMutex(&Segment->Lock);
        Segment->FileObject = FileObject;
        ObReferenceObject(FileObject);

        Segment->Image.FileOffset = 0;
        Segment->Protection = SectionPageProtection;

        Segment->Image.Characteristics = 0;
        Segment->WriteCopy = (SectionPageProtection & (PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
        if (AllocationAttributes & SEC_RESERVE)
        {
            Segment->Length.QuadPart = Segment->RawLength.QuadPart = 0;
        }
        else
        {
            Segment->RawLength.QuadPart = MaximumSize.QuadPart;
            Segment->Length.QuadPart = PAGE_ROUND_UP(Segment->RawLength.QuadPart);
        }
        Segment->Image.VirtualAddress = 0;
        MiInitializeSectionPageTable(Segment);

        /* We're good to use it now */
        OldIrql = MiAcquirePfnLock();
        Segment->SegFlags &= ~MM_SEGMENT_INCREATE;
        MiReleasePfnLock(OldIrql);
    }
    else
    {
        Section->Segment = (PSEGMENT)Segment;
        Segment->RefCount++;
        InterlockedIncrementUL(&Segment->SectionCount);

        MiReleasePfnLock(OldIrql);

        MmLockSectionSegment(Segment);

        if (MaximumSize.QuadPart > Segment->RawLength.QuadPart &&
                !(AllocationAttributes & SEC_RESERVE))
        {
            Segment->RawLength.QuadPart = MaximumSize.QuadPart;
            Segment->Length.QuadPart = PAGE_ROUND_UP(Segment->RawLength.QuadPart);
        }

        MmUnlockSectionSegment(Segment);
    }
    Section->SizeOfSection = MaximumSize;

    //KeSetEvent((PVOID)&FileObject->Lock, IO_NO_INCREMENT, FALSE);
    *SectionObject = Section;
    return(STATUS_SUCCESS);
}

/*
 TODO: not that great (declaring loaders statically, having to declare all of
 them, having to keep them extern, etc.), will fix in the future
*/
extern NTSTATUS NTAPI PeFmtCreateSection
(
    IN CONST VOID * FileHeader,
    IN SIZE_T FileHeaderSize,
    IN PVOID File,
    OUT PMM_IMAGE_SECTION_OBJECT ImageSectionObject,
    OUT PULONG Flags,
    IN PEXEFMT_CB_READ_FILE ReadFileCb,
    IN PEXEFMT_CB_ALLOCATE_SEGMENTS AllocateSegmentsCb
);

extern NTSTATUS NTAPI ElfFmtCreateSection
(
    IN CONST VOID * FileHeader,
    IN SIZE_T FileHeaderSize,
    IN PVOID File,
    OUT PMM_IMAGE_SECTION_OBJECT ImageSectionObject,
    OUT PULONG Flags,
    IN PEXEFMT_CB_READ_FILE ReadFileCb,
    IN PEXEFMT_CB_ALLOCATE_SEGMENTS AllocateSegmentsCb
);

static PEXEFMT_LOADER ExeFmtpLoaders[] =
{
    PeFmtCreateSection,
#ifdef __ELF
    ElfFmtCreateSection
#endif
};

static
PMM_SECTION_SEGMENT
NTAPI
ExeFmtpAllocateSegments(IN ULONG NrSegments)
{
    SIZE_T SizeOfSegments;
    PMM_SECTION_SEGMENT Segments;

    /* TODO: check for integer overflow */
    SizeOfSegments = sizeof(MM_SECTION_SEGMENT) * NrSegments;

    Segments = ExAllocatePoolWithTag(NonPagedPool,
                                     SizeOfSegments,
                                     TAG_MM_SECTION_SEGMENT);

    if(Segments)
        RtlZeroMemory(Segments, SizeOfSegments);

    return Segments;
}

static
NTSTATUS
NTAPI
ExeFmtpReadFile(IN PVOID File,
                IN PLARGE_INTEGER Offset,
                IN ULONG Length,
                OUT PVOID * Data,
                OUT PVOID * AllocBase,
                OUT PULONG ReadSize)
{
    NTSTATUS Status;
    LARGE_INTEGER FileOffset;
    ULONG AdjustOffset;
    ULONG OffsetAdjustment;
    ULONG BufferSize;
    ULONG UsedSize;
    PVOID Buffer;
    PFILE_OBJECT FileObject = File;
    IO_STATUS_BLOCK Iosb;

    ASSERT_IRQL_LESS(DISPATCH_LEVEL);

    if(Length == 0)
    {
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    FileOffset = *Offset;

    /* Negative/special offset: it cannot be used in this context */
    if(FileOffset.u.HighPart < 0)
    {
        KeBugCheck(MEMORY_MANAGEMENT);
    }

    AdjustOffset = PAGE_ROUND_DOWN(FileOffset.u.LowPart);
    OffsetAdjustment = FileOffset.u.LowPart - AdjustOffset;
    FileOffset.u.LowPart = AdjustOffset;

    BufferSize = Length + OffsetAdjustment;
    BufferSize = PAGE_ROUND_UP(BufferSize);

    /* Flush data since we're about to perform a non-cached read */
    CcFlushCache(FileObject->SectionObjectPointer,
                 &FileOffset,
                 BufferSize,
                 &Iosb);

    /*
     * It's ok to use paged pool, because this is a temporary buffer only used in
     * the loading of executables. The assumption is that MmCreateSection is
     * always called at low IRQLs and that these buffers don't survive a brief
     * initialization phase
     */
    Buffer = ExAllocatePoolWithTag(PagedPool,
                                   BufferSize,
                                   'rXmM');
    if (!Buffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    UsedSize = 0;

    Status = MiSimpleRead(FileObject, &FileOffset, Buffer, BufferSize, TRUE, &Iosb);

    UsedSize = (ULONG)Iosb.Information;

    if(NT_SUCCESS(Status) && UsedSize < OffsetAdjustment)
    {
        Status = STATUS_IN_PAGE_ERROR;
        ASSERT(!NT_SUCCESS(Status));
    }

    if(NT_SUCCESS(Status))
    {
        *Data = (PVOID)((ULONG_PTR)Buffer + OffsetAdjustment);
        *AllocBase = Buffer;
        *ReadSize = UsedSize - OffsetAdjustment;
    }
    else
    {
        ExFreePoolWithTag(Buffer, 'rXmM');
    }

    return Status;
}

#ifdef NASSERT
# define MmspAssertSegmentsSorted(OBJ_) ((void)0)
# define MmspAssertSegmentsNoOverlap(OBJ_) ((void)0)
# define MmspAssertSegmentsPageAligned(OBJ_) ((void)0)
#else
static
VOID
NTAPI
MmspAssertSegmentsSorted(IN PMM_IMAGE_SECTION_OBJECT ImageSectionObject)
{
    ULONG i;

    for( i = 1; i < ImageSectionObject->NrSegments; ++ i )
    {
        ASSERT(ImageSectionObject->Segments[i].Image.VirtualAddress >=
               ImageSectionObject->Segments[i - 1].Image.VirtualAddress);
    }
}

static
VOID
NTAPI
MmspAssertSegmentsNoOverlap(IN PMM_IMAGE_SECTION_OBJECT ImageSectionObject)
{
    ULONG i;

    MmspAssertSegmentsSorted(ImageSectionObject);

    for( i = 0; i < ImageSectionObject->NrSegments; ++ i )
    {
        ASSERT(ImageSectionObject->Segments[i].Length.QuadPart > 0);

        if(i > 0)
        {
            ASSERT(ImageSectionObject->Segments[i].Image.VirtualAddress >=
                   (ImageSectionObject->Segments[i - 1].Image.VirtualAddress +
                    ImageSectionObject->Segments[i - 1].Length.QuadPart));
        }
    }
}

static
VOID
NTAPI
MmspAssertSegmentsPageAligned(IN PMM_IMAGE_SECTION_OBJECT ImageSectionObject)
{
    ULONG i;

    for( i = 0; i < ImageSectionObject->NrSegments; ++ i )
    {
        ASSERT((ImageSectionObject->Segments[i].Image.VirtualAddress % PAGE_SIZE) == 0);
        ASSERT((ImageSectionObject->Segments[i].Length.QuadPart % PAGE_SIZE) == 0);
    }
}
#endif

static
int
__cdecl
MmspCompareSegments(const void * x,
                    const void * y)
{
    const MM_SECTION_SEGMENT *Segment1 = (const MM_SECTION_SEGMENT *)x;
    const MM_SECTION_SEGMENT *Segment2 = (const MM_SECTION_SEGMENT *)y;

    if (Segment1->Image.VirtualAddress > Segment2->Image.VirtualAddress)
        return 1;
    else if (Segment1->Image.VirtualAddress < Segment2->Image.VirtualAddress)
        return -1;
    else
        return 0;
}

/*
 * Ensures an image section's segments are sorted in memory
 */
static
VOID
NTAPI
MmspSortSegments(IN OUT PMM_IMAGE_SECTION_OBJECT ImageSectionObject,
                 IN ULONG Flags)
{
    if (Flags & EXEFMT_LOAD_ASSUME_SEGMENTS_SORTED)
    {
        MmspAssertSegmentsSorted(ImageSectionObject);
    }
    else
    {
        qsort(ImageSectionObject->Segments,
              ImageSectionObject->NrSegments,
              sizeof(ImageSectionObject->Segments[0]),
              MmspCompareSegments);
    }
}


/*
 * Ensures an image section's segments don't overlap in memory and don't have
 * gaps and don't have a null size. We let them map to overlapping file regions,
 * though - that's not necessarily an error
 */
static
BOOLEAN
NTAPI
MmspCheckSegmentBounds
(
    IN OUT PMM_IMAGE_SECTION_OBJECT ImageSectionObject,
    IN ULONG Flags
)
{
    ULONG i;

    if (Flags & EXEFMT_LOAD_ASSUME_SEGMENTS_NO_OVERLAP)
    {
        MmspAssertSegmentsNoOverlap(ImageSectionObject);
        return TRUE;
    }

    ASSERT(ImageSectionObject->NrSegments >= 1);

    for ( i = 0; i < ImageSectionObject->NrSegments; ++ i )
    {
        if(ImageSectionObject->Segments[i].Length.QuadPart == 0)
        {
            return FALSE;
        }

        if(i > 0)
        {
            /*
             * TODO: relax the limitation on gaps. For example, gaps smaller than a
             * page could be OK (Windows seems to be OK with them), and larger gaps
             * could lead to image sections spanning several discontiguous regions
             * (NtMapViewOfSection could then refuse to map them, and they could
             * e.g. only be allowed as parameters to NtCreateProcess, like on UNIX)
             */
            if ((ImageSectionObject->Segments[i - 1].Image.VirtualAddress +
                    ImageSectionObject->Segments[i - 1].Length.QuadPart) !=
                    ImageSectionObject->Segments[i].Image.VirtualAddress)
            {
                return FALSE;
            }
        }
    }

    return TRUE;
}

/*
 * Merges and pads an image section's segments until they all are page-aligned
 * and have a size that is a multiple of the page size
 */
static
BOOLEAN
NTAPI
MmspPageAlignSegments
(
    IN OUT PMM_IMAGE_SECTION_OBJECT ImageSectionObject,
    IN ULONG Flags
)
{
    ULONG i;
    ULONG LastSegment;
    PMM_SECTION_SEGMENT EffectiveSegment;

    if (Flags & EXEFMT_LOAD_ASSUME_SEGMENTS_PAGE_ALIGNED)
    {
        MmspAssertSegmentsPageAligned(ImageSectionObject);
        return TRUE;
    }

    LastSegment = 0;
    EffectiveSegment = &ImageSectionObject->Segments[LastSegment];

    for ( i = 0; i < ImageSectionObject->NrSegments; ++ i )
    {
        /*
         * The first segment requires special handling
         */
        if (i == 0)
        {
            ULONG_PTR VirtualAddress;
            ULONG_PTR VirtualOffset;

            VirtualAddress = EffectiveSegment->Image.VirtualAddress;

            /* Round down the virtual address to the nearest page */
            EffectiveSegment->Image.VirtualAddress = PAGE_ROUND_DOWN(VirtualAddress);

            /* Round up the virtual size to the nearest page */
            EffectiveSegment->Length.QuadPart = PAGE_ROUND_UP(VirtualAddress + EffectiveSegment->Length.QuadPart) -
                                                EffectiveSegment->Image.VirtualAddress;

            /* Adjust the raw address and size */
            VirtualOffset = VirtualAddress - EffectiveSegment->Image.VirtualAddress;

            if (EffectiveSegment->Image.FileOffset < VirtualOffset)
            {
                return FALSE;
            }

            /*
             * Garbage in, garbage out: unaligned base addresses make the file
             * offset point in curious and odd places, but that's what we were
             * asked for
             */
            EffectiveSegment->Image.FileOffset -= VirtualOffset;
            EffectiveSegment->RawLength.QuadPart += VirtualOffset;
        }
        else
        {
            PMM_SECTION_SEGMENT Segment = &ImageSectionObject->Segments[i];
            ULONG_PTR EndOfEffectiveSegment;

            EndOfEffectiveSegment = (ULONG_PTR)(EffectiveSegment->Image.VirtualAddress + EffectiveSegment->Length.QuadPart);
            ASSERT((EndOfEffectiveSegment % PAGE_SIZE) == 0);

            /*
             * The current segment begins exactly where the current effective
             * segment ended, therefore beginning a new effective segment
             */
            if (EndOfEffectiveSegment == Segment->Image.VirtualAddress)
            {
                LastSegment ++;
                ASSERT(LastSegment <= i);
                ASSERT(LastSegment < ImageSectionObject->NrSegments);

                EffectiveSegment = &ImageSectionObject->Segments[LastSegment];

                if (LastSegment != i)
                {
                    /*
                     * Copy the current segment. If necessary, the effective segment
                     * will be expanded later
                     */
                    *EffectiveSegment = *Segment;
                }

                /*
                 * Page-align the virtual size. We know for sure the virtual address
                 * already is
                 */
                ASSERT((EffectiveSegment->Image.VirtualAddress % PAGE_SIZE) == 0);
                EffectiveSegment->Length.QuadPart = PAGE_ROUND_UP(EffectiveSegment->Length.QuadPart);
            }
            /*
             * The current segment is still part of the current effective segment:
             * extend the effective segment to reflect this
             */
            else if (EndOfEffectiveSegment > Segment->Image.VirtualAddress)
            {
                static const ULONG FlagsToProtection[16] =
                {
                    PAGE_NOACCESS,
                    PAGE_READONLY,
                    PAGE_READWRITE,
                    PAGE_READWRITE,
                    PAGE_EXECUTE_READ,
                    PAGE_EXECUTE_READ,
                    PAGE_EXECUTE_READWRITE,
                    PAGE_EXECUTE_READWRITE,
                    PAGE_WRITECOPY,
                    PAGE_WRITECOPY,
                    PAGE_WRITECOPY,
                    PAGE_WRITECOPY,
                    PAGE_EXECUTE_WRITECOPY,
                    PAGE_EXECUTE_WRITECOPY,
                    PAGE_EXECUTE_WRITECOPY,
                    PAGE_EXECUTE_WRITECOPY
                };

                unsigned ProtectionFlags;

                /*
                 * Extend the file size
                 */

                /* Unaligned segments must be contiguous within the file */
                if (Segment->Image.FileOffset != (EffectiveSegment->Image.FileOffset +
                                                  EffectiveSegment->RawLength.QuadPart))
                {
                    return FALSE;
                }

                EffectiveSegment->RawLength.QuadPart += Segment->RawLength.QuadPart;

                /*
                 * Extend the virtual size
                 */
                ASSERT(PAGE_ROUND_UP(Segment->Image.VirtualAddress + Segment->Length.QuadPart) >= EndOfEffectiveSegment);

                EffectiveSegment->Length.QuadPart = PAGE_ROUND_UP(Segment->Image.VirtualAddress + Segment->Length.QuadPart) -
                                                    EffectiveSegment->Image.VirtualAddress;

                /*
                 * Merge the protection
                 */
                EffectiveSegment->Protection |= Segment->Protection;

                /* Clean up redundance */
                ProtectionFlags = 0;

                if(EffectiveSegment->Protection & PAGE_IS_READABLE)
                    ProtectionFlags |= 1 << 0;

                if(EffectiveSegment->Protection & PAGE_IS_WRITABLE)
                    ProtectionFlags |= 1 << 1;

                if(EffectiveSegment->Protection & PAGE_IS_EXECUTABLE)
                    ProtectionFlags |= 1 << 2;

                if(EffectiveSegment->Protection & PAGE_IS_WRITECOPY)
                    ProtectionFlags |= 1 << 3;

                ASSERT(ProtectionFlags < 16);
                EffectiveSegment->Protection = FlagsToProtection[ProtectionFlags];

                /* If a segment was required to be shared and cannot, fail */
                if(!(Segment->Protection & PAGE_IS_WRITECOPY) &&
                        EffectiveSegment->Protection & PAGE_IS_WRITECOPY)
                {
                    return FALSE;
                }
            }
            /*
             * We assume no holes between segments at this point
             */
            else
            {
                KeBugCheck(MEMORY_MANAGEMENT);
            }
        }
    }
    ImageSectionObject->NrSegments = LastSegment + 1;

    return TRUE;
}

NTSTATUS
ExeFmtpCreateImageSection(PFILE_OBJECT FileObject,
                          PMM_IMAGE_SECTION_OBJECT ImageSectionObject)
{
    LARGE_INTEGER Offset;
    PVOID FileHeader;
    PVOID FileHeaderBuffer;
    ULONG FileHeaderSize;
    ULONG Flags;
    ULONG OldNrSegments;
    NTSTATUS Status;
    ULONG i;

    /*
     * Read the beginning of the file (2 pages). Should be enough to contain
     * all (or most) of the headers
     */
    Offset.QuadPart = 0;

    Status = ExeFmtpReadFile (FileObject,
                              &Offset,
                              PAGE_SIZE * 2,
                              &FileHeader,
                              &FileHeaderBuffer,
                              &FileHeaderSize);

    if (!NT_SUCCESS(Status))
        return Status;

    if (FileHeaderSize == 0)
    {
        ExFreePool(FileHeaderBuffer);
        return STATUS_UNSUCCESSFUL;
    }

    /*
     * Look for a loader that can handle this executable
     */
    for (i = 0; i < RTL_NUMBER_OF(ExeFmtpLoaders); ++ i)
    {
        Flags = 0;

        Status = ExeFmtpLoaders[i](FileHeader,
                                   FileHeaderSize,
                                   FileObject,
                                   ImageSectionObject,
                                   &Flags,
                                   ExeFmtpReadFile,
                                   ExeFmtpAllocateSegments);

        if (!NT_SUCCESS(Status))
        {
            if (ImageSectionObject->Segments)
            {
                ExFreePool(ImageSectionObject->Segments);
                ImageSectionObject->Segments = NULL;
            }
        }

        if (Status != STATUS_ROS_EXEFMT_UNKNOWN_FORMAT)
            break;
    }

    ExFreePoolWithTag(FileHeaderBuffer, 'rXmM');

    /*
     * No loader handled the format
     */
    if (Status == STATUS_ROS_EXEFMT_UNKNOWN_FORMAT)
    {
        Status = STATUS_INVALID_IMAGE_NOT_MZ;
        ASSERT(!NT_SUCCESS(Status));
    }

    if (!NT_SUCCESS(Status))
        return Status;

    ASSERT(ImageSectionObject->Segments != NULL);
    ASSERT(ImageSectionObject->RefCount > 0);

    /*
     * Some defaults
     */
    /* FIXME? are these values platform-dependent? */
    if (ImageSectionObject->ImageInformation.MaximumStackSize == 0)
        ImageSectionObject->ImageInformation.MaximumStackSize = 0x40000;

    if(ImageSectionObject->ImageInformation.CommittedStackSize == 0)
        ImageSectionObject->ImageInformation.CommittedStackSize = 0x1000;

    if(ImageSectionObject->BasedAddress == NULL)
    {
        if(ImageSectionObject->ImageInformation.ImageCharacteristics & IMAGE_FILE_DLL)
            ImageSectionObject->BasedAddress = (PVOID)0x10000000;
        else
            ImageSectionObject->BasedAddress = (PVOID)0x00400000;
    }

    /*
     * And now the fun part: fixing the segments
     */

    /* Sort them by virtual address */
    MmspSortSegments(ImageSectionObject, Flags);

    /* Ensure they don't overlap in memory */
    if (!MmspCheckSegmentBounds(ImageSectionObject, Flags))
        return STATUS_INVALID_IMAGE_FORMAT;

    /* Ensure they are aligned */
    OldNrSegments = ImageSectionObject->NrSegments;

    if (!MmspPageAlignSegments(ImageSectionObject, Flags))
        return STATUS_INVALID_IMAGE_FORMAT;

    /* Trim them if the alignment phase merged some of them */
    if (ImageSectionObject->NrSegments < OldNrSegments)
    {
        PMM_SECTION_SEGMENT Segments;
        SIZE_T SizeOfSegments;

        SizeOfSegments = sizeof(MM_SECTION_SEGMENT) * ImageSectionObject->NrSegments;

        Segments = ExAllocatePoolWithTag(PagedPool,
                                         SizeOfSegments,
                                         TAG_MM_SECTION_SEGMENT);

        if (Segments == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        RtlCopyMemory(Segments, ImageSectionObject->Segments, SizeOfSegments);
        ExFreePool(ImageSectionObject->Segments);
        ImageSectionObject->Segments = Segments;
    }

    /* And finish their initialization */
    for ( i = 0; i < ImageSectionObject->NrSegments; ++ i )
    {
        ExInitializeFastMutex(&ImageSectionObject->Segments[i].Lock);
        ImageSectionObject->Segments[i].ReferenceCount = &ImageSectionObject->RefCount;
        ImageSectionObject->Segments[i].Flags = &ImageSectionObject->SegFlags;
        MiInitializeSectionPageTable(&ImageSectionObject->Segments[i]);
        ImageSectionObject->Segments[i].FileObject = FileObject;
    }

    ASSERT(ImageSectionObject->RefCount > 0);

    ImageSectionObject->FileObject = FileObject;

    ASSERT(NT_SUCCESS(Status));
    return Status;
}

NTSTATUS
MmCreateImageSection(PSECTION *SectionObject,
                     ACCESS_MASK DesiredAccess,
                     POBJECT_ATTRIBUTES ObjectAttributes,
                     PLARGE_INTEGER UMaximumSize,
                     ULONG SectionPageProtection,
                     ULONG AllocationAttributes,
                     PFILE_OBJECT FileObject)
{
    PSECTION Section;
    NTSTATUS Status;
    PMM_IMAGE_SECTION_OBJECT ImageSectionObject;
    KIRQL OldIrql;


    if (FileObject == NULL)
        return STATUS_INVALID_FILE_FOR_SECTION;

    if (FileObject->SectionObjectPointer == NULL)
    {
        DPRINT1("Denying section creation due to missing cache initialization\n");
        return STATUS_INVALID_FILE_FOR_SECTION;
    }

    /*
     * Create the section
     */
    Status = ObCreateObject (ExGetPreviousMode(),
                             MmSectionObjectType,
                             ObjectAttributes,
                             ExGetPreviousMode(),
                             NULL,
                             sizeof(*Section),
                             0,
                             0,
                             (PVOID*)(PVOID)&Section);
    if (!NT_SUCCESS(Status))
    {
        return(Status);
    }

    /*
     * Initialize it
     */
    RtlZeroMemory(Section, sizeof(*Section));

    /* Mark this as a "ROS" Section */
    Section->u.Flags.filler = 1;

    Section->InitialPageProtection = SectionPageProtection;
    Section->u.Flags.File = 1;
    Section->u.Flags.Image = 1;
    if (AllocationAttributes & SEC_NO_CHANGE)
        Section->u.Flags.NoChange = 1;

    OldIrql = MiAcquirePfnLock();

    /* Wait for it to be properly created or deleted */
    ImageSectionObject = FileObject->SectionObjectPointer->ImageSectionObject;
    while(ImageSectionObject && (ImageSectionObject->SegFlags & (MM_SEGMENT_INDELETE | MM_SEGMENT_INCREATE)))
    {
        LARGE_INTEGER ShortTime;

        MiReleasePfnLock(OldIrql);

        ShortTime.QuadPart = - 10 * 100 * 1000;
        KeDelayExecutionThread(KernelMode, FALSE, &ShortTime);

        OldIrql = MiAcquirePfnLock();
        ImageSectionObject = FileObject->SectionObjectPointer->ImageSectionObject;
    }

    if (ImageSectionObject == NULL)
    {
        NTSTATUS StatusExeFmt;
        PMM_SECTION_SEGMENT DataSectionObject;

        ImageSectionObject = ExAllocatePoolZero(NonPagedPool, sizeof(MM_IMAGE_SECTION_OBJECT), TAG_MM_SECTION_SEGMENT);
        if (ImageSectionObject == NULL)
        {
            MiReleasePfnLock(OldIrql);
            ObDereferenceObject(Section);
            return(STATUS_NO_MEMORY);
        }

        ImageSectionObject->SegFlags = MM_SEGMENT_INCREATE;
        ImageSectionObject->RefCount = 1;
        FileObject->SectionObjectPointer->ImageSectionObject = ImageSectionObject;

        /* Get a ref on the data section object */
        DataSectionObject = FileObject->SectionObjectPointer->DataSectionObject;
        while (DataSectionObject && (DataSectionObject->SegFlags & (MM_SEGMENT_INDELETE | MM_SEGMENT_INCREATE)))
        {
            LARGE_INTEGER ShortTime;

            MiReleasePfnLock(OldIrql);

            ShortTime.QuadPart = - 10 * 100 * 1000;
            KeDelayExecutionThread(KernelMode, FALSE, &ShortTime);

            OldIrql = MiAcquirePfnLock();
            DataSectionObject = FileObject->SectionObjectPointer->DataSectionObject;
            ASSERT(DataSectionObject->SegFlags & MM_DATAFILE_SEGMENT);
        }

        /* Get a ref on it. */
        if (DataSectionObject)
            InterlockedIncrementUL(&DataSectionObject->RefCount);

        MiReleasePfnLock(OldIrql);

        if (DataSectionObject)
        {
            if ((DataSectionObject->SectionCount - (FileObject->SectionObjectPointer->SharedCacheMap != NULL)) > 0)
            {
                /* Someone's got a section opened. Deny creation */
                DPRINT1("Denying image creation for %wZ: Sections opened: %lu.\n",
                        &FileObject->FileName, DataSectionObject->SectionCount);
                InterlockedExchangePointer(&FileObject->SectionObjectPointer->ImageSectionObject, NULL);
                ExFreePoolWithTag(ImageSectionObject, TAG_MM_SECTION_SEGMENT);
                MmDereferenceSegment(DataSectionObject);
                ObDereferenceObject(Section);
                return STATUS_ACCESS_DENIED;
            }

            /* Purge the cache. */
            CcPurgeCacheSection(FileObject->SectionObjectPointer, NULL, 0, FALSE);
        }

        StatusExeFmt = ExeFmtpCreateImageSection(FileObject, ImageSectionObject);

        if (!NT_SUCCESS(StatusExeFmt))
        {
            /* Unset */
            OldIrql = MiAcquirePfnLock();
            FileObject->SectionObjectPointer->ImageSectionObject = NULL;
            MiReleasePfnLock(OldIrql);

            if(ImageSectionObject->Segments != NULL)
                ExFreePool(ImageSectionObject->Segments);

            /*
             * If image file is empty, then return that the file is invalid for section
             */
            Status = StatusExeFmt;
            if (StatusExeFmt == STATUS_END_OF_FILE)
            {
                Status = STATUS_INVALID_FILE_FOR_SECTION;
            }

            ExFreePoolWithTag(ImageSectionObject, TAG_MM_SECTION_SEGMENT);
            ObDereferenceObject(Section);
            return(Status);
        }

        Section->Segment = (PSEGMENT)ImageSectionObject;
        ASSERT(ImageSectionObject->Segments);
        ASSERT(ImageSectionObject->RefCount > 0);

        /*
         * Lock the file
         */
        Status = MmspWaitForFileLock(FileObject);
        if (!NT_SUCCESS(Status))
        {
            /* Unset */
            OldIrql = MiAcquirePfnLock();
            FileObject->SectionObjectPointer->ImageSectionObject = NULL;
            MiReleasePfnLock(OldIrql);

            ExFreePool(ImageSectionObject->Segments);
            ExFreePool(ImageSectionObject);
            ObDereferenceObject(Section);
            return(Status);
        }

        OldIrql = MiAcquirePfnLock();
        ImageSectionObject->SegFlags &= ~MM_SEGMENT_INCREATE;

        /* Take a ref on the file on behalf of the newly created structure */
        ObReferenceObject(FileObject);

        MiReleasePfnLock(OldIrql);

        Status = StatusExeFmt;
    }
    else
    {
        /* Take one ref */
        ImageSectionObject->RefCount++;

        MiReleasePfnLock(OldIrql);

        Section->Segment = (PSEGMENT)ImageSectionObject;

        Status = STATUS_SUCCESS;
    }
    //KeSetEvent((PVOID)&FileObject->Lock, IO_NO_INCREMENT, FALSE);
    *SectionObject = Section;
    ASSERT(ImageSectionObject->RefCount > 0);

    return(Status);
}



static NTSTATUS
MmMapViewOfSegment(PMMSUPPORT AddressSpace,
                   PSECTION Section,
                   PMM_SECTION_SEGMENT Segment,
                   PVOID* BaseAddress,
                   SIZE_T ViewSize,
                   ULONG Protect,
                   LONGLONG ViewOffset,
                   ULONG AllocationType)
{
    PMEMORY_AREA MArea;
    NTSTATUS Status;
    ULONG Granularity;

    if (Segment->WriteCopy)
    {
        /* We have to do this because the not present fault
         * and access fault handlers depend on the protection
         * that should be granted AFTER the COW fault takes
         * place to be in Region->Protect. The not present fault
         * handler changes this to the correct protection for COW when
         * mapping the pages into the process's address space. If a COW
         * fault takes place, the access fault handler sets the page protection
         * to these values for the newly copied pages
         */
        if (Protect == PAGE_WRITECOPY)
            Protect = PAGE_READWRITE;
        else if (Protect == PAGE_EXECUTE_WRITECOPY)
            Protect = PAGE_EXECUTE_READWRITE;
    }

    if (*BaseAddress == NULL)
        Granularity = MM_ALLOCATION_GRANULARITY;
    else
        Granularity = PAGE_SIZE;

#ifdef NEWCC
    if (Segment->Flags & MM_DATAFILE_SEGMENT)
    {
        LARGE_INTEGER FileOffset;
        FileOffset.QuadPart = ViewOffset;
        ObReferenceObject(Section);
        return _MiMapViewOfSegment(AddressSpace, Segment, BaseAddress, ViewSize, Protect, &FileOffset, AllocationType, __FILE__, __LINE__);
    }
#endif
    Status = MmCreateMemoryArea(AddressSpace,
                                MEMORY_AREA_SECTION_VIEW,
                                BaseAddress,
                                ViewSize,
                                Protect,
                                &MArea,
                                AllocationType,
                                Granularity);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Mapping between 0x%p and 0x%p failed (%X).\n",
                (*BaseAddress), (char*)(*BaseAddress) + ViewSize, Status);
        return(Status);
    }

    InterlockedIncrementUL(Segment->ReferenceCount);

    MArea->SectionData.Segment = Segment;
    MArea->SectionData.ViewOffset.QuadPart = ViewOffset;
    if (Section->u.Flags.Image)
    {
        MArea->VadNode.u.VadFlags.VadType = VadImageMap;
    }

    MmInitializeRegion(&MArea->SectionData.RegionListHead,
                       ViewSize, 0, Protect);

    return(STATUS_SUCCESS);
}


static VOID
MmFreeSectionPage(PVOID Context, MEMORY_AREA* MemoryArea, PVOID Address,
                  PFN_NUMBER Page, SWAPENTRY SwapEntry, BOOLEAN Dirty)
{
    ULONG_PTR Entry;
    LARGE_INTEGER Offset;
    SWAPENTRY SavedSwapEntry;
    PMM_SECTION_SEGMENT Segment;
    PMMSUPPORT AddressSpace;
    PEPROCESS Process;

    AddressSpace = (PMMSUPPORT)Context;
    Process = MmGetAddressSpaceOwner(AddressSpace);

    Address = (PVOID)PAGE_ROUND_DOWN(Address);

    Offset.QuadPart = ((ULONG_PTR)Address - MA_GetStartingAddress(MemoryArea)) +
                      MemoryArea->SectionData.ViewOffset.QuadPart;

    Segment = MemoryArea->SectionData.Segment;

    Entry = MmGetPageEntrySectionSegment(Segment, &Offset);
    while (Entry && MM_IS_WAIT_PTE(Entry))
    {
        MmUnlockSectionSegment(Segment);
        MmUnlockAddressSpace(AddressSpace);

        MiWaitForPageEvent(NULL, NULL);

        MmLockAddressSpace(AddressSpace);
        MmLockSectionSegment(Segment);
        Entry = MmGetPageEntrySectionSegment(Segment, &Offset);
    }

    /*
     * For a dirty, datafile, non-private page, there shoulkd be no swap entry
     */
    if (*Segment->Flags & MM_DATAFILE_SEGMENT)
    {
        if (Page == PFN_FROM_SSE(Entry) && Dirty)
        {
            ASSERT(SwapEntry == 0);
        }
    }

    if (SwapEntry != 0)
    {
        /*
         * Sanity check
         */
        MmFreeSwapPage(SwapEntry);
    }
    else if (Page != 0)
    {
        if (IS_SWAP_FROM_SSE(Entry) ||
                Page != PFN_FROM_SSE(Entry))
        {
            ASSERT(Process != NULL);

            /*
             * Just dereference private pages
             */
            SavedSwapEntry = MmGetSavedSwapEntryPage(Page);
            if (SavedSwapEntry != 0)
            {
                MmFreeSwapPage(SavedSwapEntry);
                MmSetSavedSwapEntryPage(Page, 0);
            }
            MmDeleteRmap(Page, Process, Address);
            MmReleasePageMemoryConsumer(MC_USER, Page);
        }
        else
        {
            if (Process)
            {
                MmDeleteRmap(Page, Process, Address);
            }

            /* We don't dirtify for System Space Maps. We let Cc manage that */
            MmUnsharePageEntrySectionSegment(MemoryArea, Segment, &Offset, Process ? Dirty : FALSE, FALSE, NULL);
        }
    }
}

static NTSTATUS
MmUnmapViewOfSegment(PMMSUPPORT AddressSpace,
                     PVOID BaseAddress)
{
    NTSTATUS Status;
    PMEMORY_AREA MemoryArea;
    PMM_SECTION_SEGMENT Segment;
    PLIST_ENTRY CurrentEntry;
    PMM_REGION CurrentRegion;
    PLIST_ENTRY RegionListHead;

    MemoryArea = MmLocateMemoryAreaByAddress(AddressSpace,
                 BaseAddress);
    if (MemoryArea == NULL)
    {
        return(STATUS_UNSUCCESSFUL);
    }

    Segment = MemoryArea->SectionData.Segment;

#ifdef NEWCC
    if (Segment->Flags & MM_DATAFILE_SEGMENT)
    {
        MmUnlockAddressSpace(AddressSpace);
        Status = MmUnmapViewOfCacheSegment(AddressSpace, BaseAddress);
        MmLockAddressSpace(AddressSpace);

        return Status;
    }
#endif

    MemoryArea->DeleteInProgress = TRUE;

    MmLockSectionSegment(Segment);

    RegionListHead = &MemoryArea->SectionData.RegionListHead;
    while (!IsListEmpty(RegionListHead))
    {
        CurrentEntry = RemoveHeadList(RegionListHead);
        CurrentRegion = CONTAINING_RECORD(CurrentEntry, MM_REGION, RegionListEntry);
        ExFreePoolWithTag(CurrentRegion, TAG_MM_REGION);
    }

    if ((*Segment->Flags) & MM_PHYSICALMEMORY_SEGMENT)
    {
        Status = MmFreeMemoryArea(AddressSpace,
                                  MemoryArea,
                                  NULL,
                                  NULL);
    }
    else
    {
        Status = MmFreeMemoryArea(AddressSpace,
                                  MemoryArea,
                                  MmFreeSectionPage,
                                  AddressSpace);
    }
    MmUnlockSectionSegment(Segment);
    MmDereferenceSegment(Segment);
    return(Status);
}

NTSTATUS
NTAPI
MiRosUnmapViewOfSection(IN PEPROCESS Process,
                        IN PVOID BaseAddress,
                        IN BOOLEAN SkipDebuggerNotify)
{
    NTSTATUS Status;
    PMEMORY_AREA MemoryArea;
    PMMSUPPORT AddressSpace;
    PVOID ImageBaseAddress = 0;

    DPRINT("Opening memory area Process %p BaseAddress %p\n",
           Process, BaseAddress);

    ASSERT(Process);

    AddressSpace = Process ? &Process->Vm : MmGetKernelAddressSpace();

    MmLockAddressSpace(AddressSpace);
    MemoryArea = MmLocateMemoryAreaByAddress(AddressSpace,
                 BaseAddress);
    if (MemoryArea == NULL ||
#ifdef NEWCC
            ((MemoryArea->Type != MEMORY_AREA_SECTION_VIEW) && (MemoryArea->Type != MEMORY_AREA_CACHE)) ||
#else
            (MemoryArea->Type != MEMORY_AREA_SECTION_VIEW) ||
#endif
            MemoryArea->DeleteInProgress)

    {
        if (MemoryArea) ASSERT(MemoryArea->Type != MEMORY_AREA_OWNED_BY_ARM3);
        MmUnlockAddressSpace(AddressSpace);
        return STATUS_NOT_MAPPED_VIEW;
    }

    if (MemoryArea->VadNode.u.VadFlags.VadType == VadImageMap)
    {
        ULONG i;
        ULONG NrSegments;
        PMM_IMAGE_SECTION_OBJECT ImageSectionObject;
        PMM_SECTION_SEGMENT SectionSegments;
        PMM_SECTION_SEGMENT Segment;

        Segment = MemoryArea->SectionData.Segment;
        ImageSectionObject = ImageSectionObjectFromSegment(Segment);
        SectionSegments = ImageSectionObject->Segments;
        NrSegments = ImageSectionObject->NrSegments;

        MemoryArea->DeleteInProgress = TRUE;

        /* Search for the current segment within the section segments
         * and calculate the image base address */
        for (i = 0; i < NrSegments; i++)
        {
            if (Segment == &SectionSegments[i])
            {
                ImageBaseAddress = (char*)BaseAddress - (ULONG_PTR)SectionSegments[i].Image.VirtualAddress;
                break;
            }
        }
        if (i >= NrSegments)
        {
            KeBugCheck(MEMORY_MANAGEMENT);
        }

        for (i = 0; i < NrSegments; i++)
        {
            PVOID SBaseAddress = (PVOID)
                                 ((char*)ImageBaseAddress + (ULONG_PTR)SectionSegments[i].Image.VirtualAddress);

            Status = MmUnmapViewOfSegment(AddressSpace, SBaseAddress);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("MmUnmapViewOfSegment failed for %p (Process %p) with %lx\n",
                        SBaseAddress, Process, Status);
                ASSERT(NT_SUCCESS(Status));
            }
        }
    }
    else
    {
        Status = MmUnmapViewOfSegment(AddressSpace, BaseAddress);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("MmUnmapViewOfSegment failed for %p (Process %p) with %lx\n",
                    BaseAddress, Process, Status);
            ASSERT(NT_SUCCESS(Status));
        }
    }

    MmUnlockAddressSpace(AddressSpace);

    /* Notify debugger */
    if (ImageBaseAddress && !SkipDebuggerNotify) DbgkUnMapViewOfSection(ImageBaseAddress);

    return(STATUS_SUCCESS);
}




/**
 * Queries the information of a section object.
 *
 * @param SectionHandle
 *        Handle to the section object. It must be opened with SECTION_QUERY
 *        access.
 * @param SectionInformationClass
 *        Index to a certain information structure. Can be either
 *        SectionBasicInformation or SectionImageInformation. The latter
 *        is valid only for sections that were created with the SEC_IMAGE
 *        flag.
 * @param SectionInformation
 *        Caller supplies storage for resulting information.
 * @param Length
 *        Size of the supplied storage.
 * @param ResultLength
 *        Data written.
 *
 * @return Status.
 *
 * @implemented
 */
NTSTATUS
NTAPI
NtQuerySection(
    _In_ HANDLE SectionHandle,
    _In_ SECTION_INFORMATION_CLASS SectionInformationClass,
    _Out_ PVOID SectionInformation,
    _In_ SIZE_T SectionInformationLength,
    _Out_opt_ PSIZE_T ResultLength)
{
    PSECTION Section;
    KPROCESSOR_MODE PreviousMode;
    NTSTATUS Status;
    PAGED_CODE();

    PreviousMode = ExGetPreviousMode();
    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWrite(SectionInformation,
                          SectionInformationLength,
                          __alignof(ULONG));
            if (ResultLength != NULL)
            {
                ProbeForWrite(ResultLength,
                              sizeof(*ResultLength),
                              __alignof(SIZE_T));
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    if (SectionInformationClass == SectionBasicInformation)
    {
        if (SectionInformationLength < sizeof(SECTION_BASIC_INFORMATION))
        {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
    }
    else if (SectionInformationClass == SectionImageInformation)
    {
        if (SectionInformationLength < sizeof(SECTION_IMAGE_INFORMATION))
        {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
    }
    else
    {
        return STATUS_INVALID_INFO_CLASS;
    }

    Status = ObReferenceObjectByHandle(SectionHandle,
                                       SECTION_QUERY,
                                       MmSectionObjectType,
                                       PreviousMode,
                                       (PVOID*)(PVOID)&Section,
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to reference section: 0x%lx\n", Status);
        return Status;
    }

    switch(SectionInformationClass)
    {
        case SectionBasicInformation:
        {
            SECTION_BASIC_INFORMATION Sbi;

            Sbi.Size = Section->SizeOfSection;
            Sbi.BaseAddress = (PVOID)Section->Address.StartingVpn;

            Sbi.Attributes = 0;
            if (Section->u.Flags.Commit)
                Sbi.Attributes |= SEC_COMMIT;
            if (Section->u.Flags.Reserve)
                Sbi.Attributes |= SEC_RESERVE;
            if (Section->u.Flags.File)
                Sbi.Attributes |= SEC_FILE;
            if (Section->u.Flags.Image)
                Sbi.Attributes |= SEC_IMAGE;

            /* FIXME : Complete/test the list of flags passed back from NtCreateSection */

            if (Section->u.Flags.Image)
            {
                if (MiIsRosSectionObject(Section))
                {
                    PMM_IMAGE_SECTION_OBJECT ImageSectionObject = ((PMM_IMAGE_SECTION_OBJECT)Section->Segment);
                    Sbi.BaseAddress = 0;
                    Sbi.Size.QuadPart = ImageSectionObject->ImageInformation.ImageFileSize;
                }
                else
                {
                    /* Not supported yet */
                    ASSERT(FALSE);
                }
            }
            else if (MiIsRosSectionObject(Section))
            {
                Sbi.BaseAddress = (PVOID)((PMM_SECTION_SEGMENT)Section->Segment)->Image.VirtualAddress;
                Sbi.Size.QuadPart = ((PMM_SECTION_SEGMENT)Section->Segment)->RawLength.QuadPart;
            }
            else
            {
                DPRINT1("Unimplemented code path!");
            }

            _SEH2_TRY
            {
                *((SECTION_BASIC_INFORMATION*)SectionInformation) = Sbi;
                if (ResultLength != NULL)
                {
                    *ResultLength = sizeof(Sbi);
                }
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
            break;
        }
        case SectionImageInformation:
        {
            if (!Section->u.Flags.Image)
            {
                Status = STATUS_SECTION_NOT_IMAGE;
            }
            else if (MiIsRosSectionObject(Section))
            {
                PMM_IMAGE_SECTION_OBJECT ImageSectionObject = ((PMM_IMAGE_SECTION_OBJECT)Section->Segment);

                _SEH2_TRY
                {
                    PSECTION_IMAGE_INFORMATION Sii = (PSECTION_IMAGE_INFORMATION)SectionInformation;
                    *Sii = ImageSectionObject->ImageInformation;
                    if (ResultLength != NULL)
                    {
                        *ResultLength = sizeof(*Sii);
                    }
                }
                _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                {
                    Status = _SEH2_GetExceptionCode();
                }
                _SEH2_END;
            }
            else
            {
                _SEH2_TRY
                {
                    PSECTION_IMAGE_INFORMATION Sii = (PSECTION_IMAGE_INFORMATION)SectionInformation;
                    *Sii = *Section->Segment->u2.ImageInformation;
                    if (ResultLength != NULL)
                        *ResultLength = sizeof(*Sii);
                }
                _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                {
                    Status = _SEH2_GetExceptionCode();
                }
                _SEH2_END;
            }
            break;
        }
        default:
            DPRINT1("Unknown SectionInformationClass: %d\n", SectionInformationClass);
            Status = STATUS_NOT_SUPPORTED;
    }

    ObDereferenceObject(Section);

    return(Status);
}

/**********************************************************************
 * NAME       EXPORTED
 * MmMapViewOfSection
 *
 * DESCRIPTION
 * Maps a view of a section into the virtual address space of a
 * process.
 *
 * ARGUMENTS
 * Section
 *  Pointer to the section object.
 *
 * ProcessHandle
 *  Pointer to the process.
 *
 * BaseAddress
 *  Desired base address (or NULL) on entry;
 *  Actual base address of the view on exit.
 *
 * ZeroBits
 *  Number of high order address bits that must be zero.
 *
 * CommitSize
 *  Size in bytes of the initially committed section of
 *  the view.
 *
 * SectionOffset
 *  Offset in bytes from the beginning of the section
 *  to the beginning of the view.
 *
 * ViewSize
 *  Desired length of map (or zero to map all) on entry
 *  Actual length mapped on exit.
 *
 * InheritDisposition
 *  Specified how the view is to be shared with
 *  child processes.
 *
 * AllocationType
 *  Type of allocation for the pages.
 *
 * Protect
 *  Protection for the committed region of the view.
 *
 * RETURN VALUE
 * Status.
 *
 * @implemented
 */
NTSTATUS NTAPI
MmMapViewOfSection(IN PVOID SectionObject,
                   IN PEPROCESS Process,
                   IN OUT PVOID *BaseAddress,
                   IN ULONG_PTR ZeroBits,
                   IN SIZE_T CommitSize,
                   IN OUT PLARGE_INTEGER SectionOffset OPTIONAL,
                   IN OUT PSIZE_T ViewSize,
                   IN SECTION_INHERIT InheritDisposition,
                   IN ULONG AllocationType,
                   IN ULONG Protect)
{
    PSECTION Section;
    PMMSUPPORT AddressSpace;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN NotAtBase = FALSE;

    if (MiIsRosSectionObject(SectionObject) == FALSE)
    {
        DPRINT("Mapping ARM3 section into %s\n", Process->ImageFileName);
        return MmMapViewOfArm3Section(SectionObject,
                                      Process,
                                      BaseAddress,
                                      ZeroBits,
                                      CommitSize,
                                      SectionOffset,
                                      ViewSize,
                                      InheritDisposition,
                                      AllocationType,
                                      Protect);
    }

    ASSERT(Process);

    if (!Protect || Protect & ~PAGE_FLAGS_VALID_FOR_SECTION)
    {
        return STATUS_INVALID_PAGE_PROTECTION;
    }

    /* FIXME: We should keep this, but it would break code checking equality */
    Protect &= ~PAGE_NOCACHE;

    Section = SectionObject;
    AddressSpace = &Process->Vm;

    if (Section->u.Flags.NoChange)
        AllocationType |= SEC_NO_CHANGE;

    MmLockAddressSpace(AddressSpace);

    if (Section->u.Flags.Image)
    {
        ULONG i;
        ULONG NrSegments;
        ULONG_PTR ImageBase;
        SIZE_T ImageSize;
        PMM_IMAGE_SECTION_OBJECT ImageSectionObject;
        PMM_SECTION_SEGMENT SectionSegments;

        ImageSectionObject = ((PMM_IMAGE_SECTION_OBJECT)Section->Segment);
        SectionSegments = ImageSectionObject->Segments;
        NrSegments = ImageSectionObject->NrSegments;

        ASSERT(ImageSectionObject->RefCount > 0);

        ImageBase = (ULONG_PTR)*BaseAddress;
        if (ImageBase == 0)
        {
            ImageBase = (ULONG_PTR)ImageSectionObject->BasedAddress;
        }

        ImageSize = 0;
        for (i = 0; i < NrSegments; i++)
        {
            ULONG_PTR MaxExtent;
            MaxExtent = (ULONG_PTR)(SectionSegments[i].Image.VirtualAddress +
                                    SectionSegments[i].Length.QuadPart);
            ImageSize = max(ImageSize, MaxExtent);
        }

        ImageSectionObject->ImageInformation.ImageFileSize = (ULONG)ImageSize;

        /* Check for an illegal base address */
        if (((ImageBase + ImageSize) > (ULONG_PTR)MM_HIGHEST_VAD_ADDRESS) ||
                ((ImageBase + ImageSize) < ImageSize))
        {
            ASSERT(*BaseAddress == NULL);
            ImageBase = ALIGN_DOWN_BY((ULONG_PTR)MM_HIGHEST_VAD_ADDRESS - ImageSize,
                                      MM_VIRTMEM_GRANULARITY);
            NotAtBase = TRUE;
        }
        else if (ImageBase != ALIGN_DOWN_BY(ImageBase, MM_VIRTMEM_GRANULARITY))
        {
            ASSERT(*BaseAddress == NULL);
            ImageBase = ALIGN_DOWN_BY(ImageBase, MM_VIRTMEM_GRANULARITY);
            NotAtBase = TRUE;
        }

        /* Check there is enough space to map the section at that point. */
        if (MmLocateMemoryAreaByRegion(AddressSpace, (PVOID)ImageBase,
                                       PAGE_ROUND_UP(ImageSize)) != NULL)
        {
            /* Fail if the user requested a fixed base address. */
            if ((*BaseAddress) != NULL)
            {
                MmUnlockAddressSpace(AddressSpace);
                return(STATUS_CONFLICTING_ADDRESSES);
            }
            /* Otherwise find a gap to map the image. */
            ImageBase = (ULONG_PTR)MmFindGap(AddressSpace, PAGE_ROUND_UP(ImageSize), MM_VIRTMEM_GRANULARITY, FALSE);
            if (ImageBase == 0)
            {
                MmUnlockAddressSpace(AddressSpace);
                return(STATUS_CONFLICTING_ADDRESSES);
            }
            /* Remember that we loaded image at a different base address */
            NotAtBase = TRUE;
        }

        for (i = 0; i < NrSegments; i++)
        {
            PVOID SBaseAddress = (PVOID)
                                 ((char*)ImageBase + (ULONG_PTR)SectionSegments[i].Image.VirtualAddress);
            MmLockSectionSegment(&SectionSegments[i]);
            Status = MmMapViewOfSegment(AddressSpace,
                                        Section,
                                        &SectionSegments[i],
                                        &SBaseAddress,
                                        SectionSegments[i].Length.QuadPart,
                                        SectionSegments[i].Protection,
                                        0,
                                        0);
            MmUnlockSectionSegment(&SectionSegments[i]);
            if (!NT_SUCCESS(Status))
            {
                /* roll-back */
                while (i--)
                {
                    SBaseAddress =  ((char*)ImageBase + (ULONG_PTR)SectionSegments[i].Image.VirtualAddress);
                    MmLockSectionSegment(&SectionSegments[i]);
                    MmUnmapViewOfSegment(AddressSpace, SBaseAddress);
                    MmUnlockSectionSegment(&SectionSegments[i]);
                }

                MmUnlockAddressSpace(AddressSpace);
                return(Status);
            }
        }

        *BaseAddress = (PVOID)ImageBase;
        *ViewSize = ImageSize;
    }
    else
    {
        PMM_SECTION_SEGMENT Segment = (PMM_SECTION_SEGMENT)Section->Segment;
        LONGLONG ViewOffset;

        ASSERT(Segment->RefCount > 0);

        /* check for write access */
        if ((Protect & (PAGE_READWRITE|PAGE_EXECUTE_READWRITE)) &&
                !(Section->InitialPageProtection & (PAGE_READWRITE|PAGE_EXECUTE_READWRITE)))
        {
            MmUnlockAddressSpace(AddressSpace);
            return STATUS_SECTION_PROTECTION;
        }
        /* check for read access */
        if ((Protect & (PAGE_READONLY|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_WRITECOPY)) &&
                !(Section->InitialPageProtection & (PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
        {
            MmUnlockAddressSpace(AddressSpace);
            return STATUS_SECTION_PROTECTION;
        }
        /* check for execute access */
        if ((Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) &&
                !(Section->InitialPageProtection & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
        {
            MmUnlockAddressSpace(AddressSpace);
            return STATUS_SECTION_PROTECTION;
        }

        if (SectionOffset == NULL)
        {
            ViewOffset = 0;
        }
        else
        {
            ViewOffset = SectionOffset->QuadPart;
        }

        if ((ViewOffset % PAGE_SIZE) != 0)
        {
            MmUnlockAddressSpace(AddressSpace);
            return(STATUS_MAPPED_ALIGNMENT);
        }

        if ((*ViewSize) == 0)
        {
            (*ViewSize) = Section->SizeOfSection.QuadPart - ViewOffset;
        }
        else if (((*ViewSize)+ViewOffset) > Section->SizeOfSection.QuadPart)
        {
            (*ViewSize) = MIN(Section->SizeOfSection.QuadPart - ViewOffset, SIZE_T_MAX - PAGE_SIZE);
        }

        *ViewSize = PAGE_ROUND_UP(*ViewSize);

        MmLockSectionSegment(Segment);
        Status = MmMapViewOfSegment(AddressSpace,
                                    Section,
                                    Segment,
                                    BaseAddress,
                                    *ViewSize,
                                    Protect,
                                    ViewOffset,
                                    AllocationType & (MEM_TOP_DOWN|SEC_NO_CHANGE));
        MmUnlockSectionSegment(Segment);
        if (!NT_SUCCESS(Status))
        {
            MmUnlockAddressSpace(AddressSpace);
            return(Status);
        }
    }

    MmUnlockAddressSpace(AddressSpace);

    if (NotAtBase)
        Status = STATUS_IMAGE_NOT_AT_BASE;
    else
        Status = STATUS_SUCCESS;

    return Status;
}

/*
 * @unimplemented
 */
BOOLEAN NTAPI
MmCanFileBeTruncated (IN PSECTION_OBJECT_POINTERS SectionObjectPointer,
                      IN PLARGE_INTEGER   NewFileSize)
{
    KIRQL OldIrql = MiAcquirePfnLock();
    BOOLEAN Ret;
    PMM_SECTION_SEGMENT Segment;

CheckSectionPointer:
    /* Check whether an ImageSectionObject exists */
    if (SectionObjectPointer->ImageSectionObject != NULL)
    {
        DPRINT1("ERROR: File can't be truncated because it has an image section\n");
        MiReleasePfnLock(OldIrql);

        return FALSE;
    }

    Segment = (PMM_SECTION_SEGMENT)SectionObjectPointer->DataSectionObject;
    /* Wait for it to be created/deleted properly */
    while (Segment && (Segment->SegFlags & (MM_SEGMENT_INCREATE | MM_SEGMENT_INDELETE)))
    {
        LARGE_INTEGER ShortTime;

        ShortTime.QuadPart = -10 * 100 * 1000;

        /* Bad luck. Wait a bit for the operation to finish */
        MiReleasePfnLock(OldIrql);
        KeDelayExecutionThread(KernelMode, FALSE, &ShortTime);
        OldIrql = MiAcquirePfnLock();
        goto CheckSectionPointer;
    }

    if (Segment)
    {
        if ((Segment->SectionCount == 1) && (SectionObjectPointer->SharedCacheMap != NULL))
        {
            /* If the cache is the only one holding a reference to the segment, then it's fine to resize */
            Ret = TRUE;
        }
        else
        {
            /* We can't shrink, but we can extend */
            Ret = NewFileSize->QuadPart >= Segment->RawLength.QuadPart;
        }
    }
    else
    {
        Ret = TRUE;
    }

    MiReleasePfnLock(OldIrql);

    DPRINT("FIXME: didn't check for outstanding write probes\n");

    return Ret;
}




/*
 * @implemented
 */
BOOLEAN NTAPI
MmFlushImageSection (IN PSECTION_OBJECT_POINTERS SectionObjectPointer,
                     IN MMFLUSH_TYPE FlushType)
{
    switch(FlushType)
    {
        case MmFlushForDelete:
        case MmFlushForWrite:
        {
            BOOLEAN Ret = TRUE;
            KIRQL OldIrql = MiAcquirePfnLock();

            if (SectionObjectPointer->ImageSectionObject)
            {
                PMM_IMAGE_SECTION_OBJECT ImageSectionObject = SectionObjectPointer->ImageSectionObject;
                if (!(ImageSectionObject->SegFlags & MM_SEGMENT_INDELETE))
                    Ret = FALSE;
            }

            MiReleasePfnLock(OldIrql);
            return Ret;
        }
    }
    return FALSE;
}

/*
 * @implemented
 */
NTSTATUS NTAPI
MmMapViewInSystemSpace (IN PVOID SectionObject,
                        OUT PVOID * MappedBase,
                        IN OUT PSIZE_T ViewSize)
{
    LARGE_INTEGER SectionOffset;

    SectionOffset.QuadPart = 0;

    return MmMapViewInSystemSpaceEx(SectionObject, MappedBase, ViewSize, &SectionOffset);
}

NTSTATUS
NTAPI
MmMapViewInSystemSpaceEx (
    _In_ PVOID SectionObject,
    _Outptr_result_bytebuffer_ (*ViewSize) PVOID *MappedBase,
    _Inout_ PSIZE_T ViewSize,
    _Inout_ PLARGE_INTEGER SectionOffset
    )
{
    PSECTION Section = SectionObject;
    PMM_SECTION_SEGMENT Segment;
    PMMSUPPORT AddressSpace;
    NTSTATUS Status;
    PAGED_CODE();

    if (MiIsRosSectionObject(SectionObject) == FALSE)
    {
        return MiMapViewInSystemSpace(SectionObject,
                                      &MmSession,
                                      MappedBase,
                                      ViewSize,
                                      SectionOffset);
    }

    DPRINT("MmMapViewInSystemSpaceEx() called\n");

    Section = SectionObject;
    Segment = (PMM_SECTION_SEGMENT)Section->Segment;

    AddressSpace = MmGetKernelAddressSpace();

    MmLockAddressSpace(AddressSpace);

    if (*ViewSize == 0)
    {
        *ViewSize = MIN((Section->SizeOfSection.QuadPart - SectionOffset->QuadPart), SIZE_T_MAX);
    }

    MmLockSectionSegment(Segment);

    Status = MmMapViewOfSegment(AddressSpace,
                                Section,
                                Segment,
                                MappedBase,
                                *ViewSize,
                                PAGE_READWRITE,
                                SectionOffset->QuadPart,
                                SEC_RESERVE);

    MmUnlockSectionSegment(Segment);
    MmUnlockAddressSpace(AddressSpace);

    return Status;
}

NTSTATUS
NTAPI
MiRosUnmapViewInSystemSpace(IN PVOID MappedBase)
{
    PMMSUPPORT AddressSpace;
    NTSTATUS Status;

    DPRINT("MmUnmapViewInSystemSpace() called\n");

    AddressSpace = MmGetKernelAddressSpace();

    MmLockAddressSpace(AddressSpace);

    Status = MmUnmapViewOfSegment(AddressSpace, MappedBase);

    MmUnlockAddressSpace(AddressSpace);

    return Status;
}

/**********************************************************************
 * NAME       EXPORTED
 *  MmCreateSection@
 *
 * DESCRIPTION
 *  Creates a section object.
 *
 * ARGUMENTS
 * SectionObject (OUT)
 *  Caller supplied storage for the resulting pointer
 *  to a SECTION_OBJECT instance;
 *
 * DesiredAccess
 *  Specifies the desired access to the section can be a
 *  combination of:
 *   STANDARD_RIGHTS_REQUIRED |
 *   SECTION_QUERY   |
 *   SECTION_MAP_WRITE  |
 *   SECTION_MAP_READ  |
 *   SECTION_MAP_EXECUTE
 *
 * ObjectAttributes [OPTIONAL]
 *  Initialized attributes for the object can be used
 *  to create a named section;
 *
 * MaximumSize
 *  Maximizes the size of the memory section. Must be
 *  non-NULL for a page-file backed section.
 *  If value specified for a mapped file and the file is
 *  not large enough, file will be extended.
 *
 * SectionPageProtection
 *  Can be a combination of:
 *   PAGE_READONLY |
 *   PAGE_READWRITE |
 *   PAGE_WRITEONLY |
 *   PAGE_WRITECOPY
 *
 * AllocationAttributes
 *  Can be a combination of:
 *   SEC_IMAGE |
 *   SEC_RESERVE
 *
 * FileHandle
 *  Handle to a file to create a section mapped to a file
 *  instead of a memory backed section;
 *
 * File
 *  Unknown.
 *
 * RETURN VALUE
 *  Status.
 *
 * @implemented
 */
NTSTATUS NTAPI
MmCreateSection (OUT PVOID  * Section,
                 IN ACCESS_MASK  DesiredAccess,
                 IN POBJECT_ATTRIBUTES ObjectAttributes     OPTIONAL,
                 IN PLARGE_INTEGER  MaximumSize,
                 IN ULONG   SectionPageProtection,
                 IN ULONG   AllocationAttributes,
                 IN HANDLE   FileHandle   OPTIONAL,
                 IN PFILE_OBJECT  FileObject  OPTIONAL)
{
    NTSTATUS Status;
    ULONG Protection;
    PSECTION *SectionObject = (PSECTION *)Section;
    BOOLEAN FileLock = FALSE;

    /* Check if an ARM3 section is being created instead */
    if (!(AllocationAttributes & (SEC_IMAGE | SEC_PHYSICALMEMORY)))
    {
        if (!(FileObject) && !(FileHandle))
        {
            return MmCreateArm3Section(Section,
                                       DesiredAccess,
                                       ObjectAttributes,
                                       MaximumSize,
                                       SectionPageProtection,
                                       AllocationAttributes &~ 1,
                                       FileHandle,
                                       FileObject);
        }
    }

    /* Convert section flag to page flag */
    if (AllocationAttributes & SEC_NOCACHE) SectionPageProtection |= PAGE_NOCACHE;

    /* Check to make sure the protection is correct. Nt* does this already */
    Protection = MiMakeProtectionMask(SectionPageProtection);
    if (Protection == MM_INVALID_PROTECTION)
    {
        DPRINT1("Page protection is invalid\n");
        return STATUS_INVALID_PAGE_PROTECTION;
    }

    /* Check if this is going to be a data or image backed file section */
    if ((FileHandle) || (FileObject))
    {
        /* These cannot be mapped with large pages */
        if (AllocationAttributes & SEC_LARGE_PAGES)
        {
            DPRINT1("Large pages cannot be used with an image mapping\n");
            return STATUS_INVALID_PARAMETER_6;
        }

        /* Did the caller pass a handle? */
        if (FileHandle)
        {
            /* Reference the file handle to get the object */
            Status = ObReferenceObjectByHandle(FileHandle,
                                               MmMakeFileAccess[Protection],
                                               IoFileObjectType,
                                               ExGetPreviousMode(),
                                               (PVOID*)&FileObject,
                                               NULL);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Failed to get a handle to the FO: %lx\n", Status);
                return Status;
            }

            /* Lock the file */
            Status = FsRtlAcquireToCreateMappedSection(FileObject, SectionPageProtection);
            if (!NT_SUCCESS(Status))
            {
                ObDereferenceObject(FileObject);
                return Status;
            }

            FileLock = TRUE;

            /* Deny access if there are writes on the file */
            if ((AllocationAttributes & SEC_IMAGE) && (Status == STATUS_FILE_LOCKED_WITH_WRITERS))
            {
                DPRINT1("Cannot create image maps with writers open on the file!\n");
                Status = STATUS_ACCESS_DENIED;
                goto Quit;
            }
        }
        else
        {
            /* Reference the object directly */
            ObReferenceObject(FileObject);
        }
    }
    else
    {
        /* A handle must be supplied with SEC_IMAGE, as this is the no-handle path */
        if (AllocationAttributes & SEC_IMAGE) return STATUS_INVALID_FILE_FOR_SECTION;
    }

    if (AllocationAttributes & SEC_IMAGE)
    {
        Status = MmCreateImageSection(SectionObject,
                                      DesiredAccess,
                                      ObjectAttributes,
                                      MaximumSize,
                                      SectionPageProtection,
                                      AllocationAttributes,
                                      FileObject);
    }
#ifndef NEWCC
    else if (FileObject != NULL)
    {
        Status =  MmCreateDataFileSection(SectionObject,
                                          DesiredAccess,
                                          ObjectAttributes,
                                          MaximumSize,
                                          SectionPageProtection,
                                          AllocationAttributes,
                                          FileObject,
                                          FileHandle != NULL);
    }
#else
    else if (FileHandle != NULL || FileObject != NULL)
    {
        Status = MmCreateCacheSection(SectionObject,
                                      DesiredAccess,
                                      ObjectAttributes,
                                      MaximumSize,
                                      SectionPageProtection,
                                      AllocationAttributes,
                                      FileObject);
    }
#endif
    else
    {
        /* All cases should be handled above */
        Status = STATUS_INVALID_PARAMETER;
    }

Quit:
    if (FileLock)
        FsRtlReleaseFile(FileObject);
    if (FileObject)
        ObDereferenceObject(FileObject);

    return Status;
}

BOOLEAN
NTAPI
MmArePagesResident(
    _In_ PEPROCESS Process,
    _In_ PVOID Address,
    _In_ ULONG Length)
{
    PMEMORY_AREA MemoryArea;
    BOOLEAN Ret = TRUE;
    PMM_SECTION_SEGMENT Segment;
    LARGE_INTEGER SegmentOffset, RangeEnd;
    PMMSUPPORT AddressSpace = Process ? &Process->Vm : MmGetKernelAddressSpace();

    MmLockAddressSpace(AddressSpace);

    MemoryArea = MmLocateMemoryAreaByAddress(AddressSpace, Address);
    if (MemoryArea == NULL)
    {
        MmUnlockAddressSpace(AddressSpace);
        return FALSE;
    }

    /* Only supported in old Mm for now */
    ASSERT(MemoryArea->Type == MEMORY_AREA_SECTION_VIEW);
    /* For file mappings */
    ASSERT(MemoryArea->VadNode.u.VadFlags.VadType != VadImageMap);

    Segment = MemoryArea->SectionData.Segment;
    MmLockSectionSegment(Segment);

    SegmentOffset.QuadPart = PAGE_ROUND_DOWN(Address) - MA_GetStartingAddress(MemoryArea)
            + MemoryArea->SectionData.ViewOffset.QuadPart;
    RangeEnd.QuadPart = PAGE_ROUND_UP((ULONG_PTR)Address + Length) - MA_GetStartingAddress(MemoryArea)
            + MemoryArea->SectionData.ViewOffset.QuadPart;

    while (SegmentOffset.QuadPart < RangeEnd.QuadPart)
    {
        ULONG_PTR Entry = MmGetPageEntrySectionSegment(Segment, &SegmentOffset);
        if ((Entry == 0) || IS_SWAP_FROM_SSE(Entry))
        {
            Ret = FALSE;
            break;
        }
        SegmentOffset.QuadPart += PAGE_SIZE;
    }

    MmUnlockSectionSegment(Segment);

    MmUnlockAddressSpace(AddressSpace);
    return Ret;
}

NTSTATUS
NTAPI
MmMakePagesResident(
    _In_ PEPROCESS Process,
    _In_ PVOID Address,
    _In_ ULONG Length)
{
    PMEMORY_AREA MemoryArea;
    PMM_SECTION_SEGMENT Segment;
    LARGE_INTEGER SegmentOffset, RangeEnd;
    PMMSUPPORT AddressSpace = Process ? &Process->Vm : MmGetKernelAddressSpace();

    MmLockAddressSpace(AddressSpace);

    MemoryArea = MmLocateMemoryAreaByAddress(AddressSpace, Address);
    if (MemoryArea == NULL)
    {
        MmUnlockAddressSpace(AddressSpace);
        return STATUS_NOT_MAPPED_VIEW;
    }

    /* Only supported in old Mm for now */
    ASSERT(MemoryArea->Type == MEMORY_AREA_SECTION_VIEW);
    /* For file mappings */
    ASSERT(MemoryArea->VadNode.u.VadFlags.VadType != VadImageMap);

    Segment = MemoryArea->SectionData.Segment;
    MmLockSectionSegment(Segment);

    SegmentOffset.QuadPart = PAGE_ROUND_DOWN(Address) - MA_GetStartingAddress(MemoryArea)
            + MemoryArea->SectionData.ViewOffset.QuadPart;
    RangeEnd.QuadPart = PAGE_ROUND_UP((ULONG_PTR)Address + Length) - MA_GetStartingAddress(MemoryArea)
            + MemoryArea->SectionData.ViewOffset.QuadPart;

    DPRINT("MmMakePagesResident: Segment %p, 0x%I64x -> 0x%I64x\n", Segment, SegmentOffset.QuadPart, RangeEnd.QuadPart);

    while (SegmentOffset.QuadPart < RangeEnd.QuadPart)
    {
        ULONG_PTR Entry = MmGetPageEntrySectionSegment(Segment, &SegmentOffset);

        /* Let any pending read proceed */
        while (MM_IS_WAIT_PTE(Entry))
        {
            MmUnlockSectionSegment(Segment);
            MmUnlockAddressSpace(AddressSpace);
            MiWaitForPageEvent(NULL, NULL);
            MmLockAddressSpace(AddressSpace);
            MmLockSectionSegment(Segment);
            Entry = MmGetPageEntrySectionSegment(Segment, &SegmentOffset);
        }

        /* We are called from Cc, this can't be backed by the page files */
        ASSERT(!IS_SWAP_FROM_SSE(Entry));

        /* At this point, there may be a valid page there */
        if (Entry == 0)
        {
            PFN_NUMBER Page;
            NTSTATUS Status;

            /*
             * Release all our locks and read in the page from disk
             */
            MmSetPageEntrySectionSegment(Segment, &SegmentOffset, MAKE_SWAP_SSE(MM_WAIT_ENTRY));
            MmUnlockSectionSegment(Segment);
            MmUnlockAddressSpace(AddressSpace);

            /* FIXME: Read the whole range at once instead of one page at a time */
            /* Ignore file size, as Cc already checked on its side. */
            Status = MiReadPage(MemoryArea, SegmentOffset.QuadPart, &Page);
            if (!NT_SUCCESS(Status))
            {
                /* Reset the Segment entry and fail */
                MmLockSectionSegment(Segment);
                MmSetPageEntrySectionSegment(Segment, &SegmentOffset, 0);
                MmUnlockSectionSegment(Segment);
                MiSetPageEvent(Process, Address);
                return Status;
            }

            MmLockAddressSpace(AddressSpace);
            MmLockSectionSegment(Segment);

            /* We set it with 0 ref count, nobody maps this page yet. */
            MmSetPageEntrySectionSegment(Segment, &SegmentOffset, MAKE_SSE(Page << PAGE_SHIFT, 0));
            MiSetPageEvent(Process, Address);
        }
        SegmentOffset.QuadPart += PAGE_SIZE;
    }

    MmUnlockSectionSegment(Segment);

    MmUnlockAddressSpace(AddressSpace);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
MmRosFlushVirtualMemory(
    _In_ PEPROCESS Process,
    _Inout_ PVOID* Address,
    _Inout_ PSIZE_T Length,
    _Out_ PIO_STATUS_BLOCK Iosb)
{
    PMEMORY_AREA MemoryArea;
    PMM_SECTION_SEGMENT Segment;
    LARGE_INTEGER SegmentOffset, RangeEnd;
    PMMSUPPORT AddressSpace = Process ? &Process->Vm : MmGetKernelAddressSpace();
    PVOID CurrentAddress;

    PAGED_CODE();

    MmLockAddressSpace(AddressSpace);

    DPRINT("Flushing Process %p at %p --> 0x%x", Process, *Address, *Length);

    MemoryArea = MmLocateMemoryAreaByAddress(AddressSpace, *Address);
    if ((MemoryArea == NULL) || (MemoryArea->Type != MEMORY_AREA_SECTION_VIEW) ||
            (MemoryArea->VadNode.u.VadFlags.VadType == VadImageMap))
    {
        MmUnlockAddressSpace(AddressSpace);
        return STATUS_NOT_MAPPED_VIEW;
    }

    Segment = MemoryArea->SectionData.Segment;

    SegmentOffset.QuadPart = PAGE_ROUND_DOWN(*Address) - MA_GetStartingAddress(MemoryArea)
            + MemoryArea->SectionData.ViewOffset.QuadPart;
    RangeEnd.QuadPart = PAGE_ROUND_UP((ULONG_PTR)*Address + *Length) - MA_GetStartingAddress(MemoryArea)
            + MemoryArea->SectionData.ViewOffset.QuadPart;

    CurrentAddress = *Address;

    MmUnlockAddressSpace(AddressSpace);

    MmLockSectionSegment(Segment);

    Iosb->Information = 0;
    while (SegmentOffset.QuadPart < RangeEnd.QuadPart)
    {
        ULONG_PTR Entry = MmGetPageEntrySectionSegment(Segment, &SegmentOffset);

        /* Let any pending read proceed */
        while (MM_IS_WAIT_PTE(Entry))
        {
            MmUnlockSectionSegment(Segment);
            MiWaitForPageEvent(NULL, NULL);
            MmLockSectionSegment(Segment);
            Entry = MmGetPageEntrySectionSegment(Segment, &SegmentOffset);
        }

        /* We are called from Cc, this can't be backed by the page files */
        ASSERT(!IS_SWAP_FROM_SSE(Entry));

        /* At this point, there may be a valid page there */
        if (Entry != 0)
        {
            /* This will write the page to disk, if needed */
            MmCheckDirtySegment(Segment, &SegmentOffset, Process ? MmIsDirtyPage(Process, CurrentAddress) : FALSE, FALSE);
            Iosb->Information += PAGE_SIZE;
        }
        SegmentOffset.QuadPart += PAGE_SIZE;
        CurrentAddress = (PVOID)((ULONG_PTR)CurrentAddress + PAGE_SIZE);
    }

    MmUnlockSectionSegment(Segment);

    return STATUS_SUCCESS;
}

_Requires_exclusive_lock_held_(Segment->Lock)
BOOLEAN
NTAPI
MmCheckDirtySegment(
    PMM_SECTION_SEGMENT Segment,
    PLARGE_INTEGER Offset,
    BOOLEAN ForceDirty,
    BOOLEAN PageOut)
{
    ULONG_PTR Entry;
    NTSTATUS Status;
    PFN_NUMBER Page;

    ASSERT(Segment->Locked);

    DPRINT("Checking segment for file %wZ at offset 0x%I64X.\n", &Segment->FileObject->FileName, Offset->QuadPart);

    Entry = MmGetPageEntrySectionSegment(Segment, Offset);
    if (Entry == 0)
        return FALSE;

    Page = PFN_FROM_SSE(Entry);
    if ((IS_DIRTY_SSE(Entry)) || ForceDirty)
    {
        BOOLEAN DirtyAgain;

        /* We got a dirty entry. Is this segment copy on write */
        ASSERT(!Segment->WriteCopy);
        ASSERT(Segment->SegFlags & MM_DATAFILE_SEGMENT);

        /* Insert the cleaned entry back. Keep one ref to the page so nobody pages it out again behind us */
        MmSetPageEntrySectionSegment(Segment, Offset,
                MAKE_SSE(Page << PAGE_SHIFT, SHARE_COUNT_FROM_SSE(Entry) + 1));

        /* Tell the other users that we are clean again */
        MmSetCleanAllRmaps(Page);

        MmUnlockSectionSegment(Segment);

        /* Tell the FS driver who we are */
        if (PageOut)
            IoSetTopLevelIrp((PIRP)FSRTL_MOD_WRITE_TOP_LEVEL_IRP);

        /* Go ahead and write the page */
        DPRINT("Writing page at offset %I64d for file %wZ, Pageout: %s\n",
                Offset->QuadPart, &Segment->FileObject->FileName, PageOut ? "TRUE" : "FALSE");
        Status = MiWritePage(Segment, Offset->QuadPart, Page);

        if (PageOut)
            IoSetTopLevelIrp(NULL);

        MmLockSectionSegment(Segment);

        /* Get the entry again */
        Entry = MmGetPageEntrySectionSegment(Segment, Offset);
        ASSERT(PFN_FROM_SSE(Entry) == Page);

        if (!NT_SUCCESS(Status))
        {
            /* Damn, this failed. Consider this page as still dirty */
            DPRINT1("MiWritePage FAILED: Status 0x%08x!\n", Status);
            DirtyAgain = TRUE;
        }
        else
        {
            /* Check if someone dirtified this page while we were not looking */
            DirtyAgain = IS_DIRTY_SSE(Entry) || MmIsDirtyPageRmap(Page);
        }

        /* Drop the reference we got */
        Entry = MAKE_SSE(Page << PAGE_SHIFT, SHARE_COUNT_FROM_SSE(Entry) - 1);
        if (DirtyAgain)
        {
            Entry = DIRTY_SSE(Entry);
        }
        MmSetPageEntrySectionSegment(Segment, Offset, Entry);
    }

    /* Were this page hanging there just for the sake of being present ? */
    if (!IS_DIRTY_SSE(Entry) && (SHARE_COUNT_FROM_SSE(Entry) == 0) && PageOut)
    {
        /* Yes. Release it */
        MmSetPageEntrySectionSegment(Segment, Offset, 0);
        MmReleasePageMemoryConsumer(MC_USER, Page);
        /* Tell the caller we released the page */
        return TRUE;
    }

    return FALSE;
}

NTSTATUS
NTAPI
MmMakePagesDirty(
    _In_ PEPROCESS Process,
    _In_ PVOID Address,
    _In_ ULONG Length)
{
    PMEMORY_AREA MemoryArea;
    PMM_SECTION_SEGMENT Segment;
    LARGE_INTEGER SegmentOffset, RangeEnd;
    PMMSUPPORT AddressSpace = Process ? &Process->Vm : MmGetKernelAddressSpace();

    MmLockAddressSpace(AddressSpace);

    MemoryArea = MmLocateMemoryAreaByAddress(AddressSpace, Address);
    if (MemoryArea == NULL)
    {
        MmUnlockAddressSpace(AddressSpace);
        return STATUS_NOT_MAPPED_VIEW;
    }

    /* Only supported in old Mm for now */
    ASSERT(MemoryArea->Type == MEMORY_AREA_SECTION_VIEW);
    /* For file mappings */
    ASSERT(MemoryArea->VadNode.u.VadFlags.VadType != VadImageMap);

    Segment = MemoryArea->SectionData.Segment;
    MmLockSectionSegment(Segment);

    SegmentOffset.QuadPart = PAGE_ROUND_DOWN(Address) - MA_GetStartingAddress(MemoryArea)
            + MemoryArea->SectionData.ViewOffset.QuadPart;
    RangeEnd.QuadPart = PAGE_ROUND_UP((ULONG_PTR)Address + Length) - MA_GetStartingAddress(MemoryArea)
            + MemoryArea->SectionData.ViewOffset.QuadPart;

    DPRINT("MmMakePagesResident: Segment %p, 0x%I64x -> 0x%I64x\n", Segment, SegmentOffset.QuadPart, RangeEnd.QuadPart);

    while (SegmentOffset.QuadPart < RangeEnd.QuadPart)
    {
        ULONG_PTR Entry = MmGetPageEntrySectionSegment(Segment, &SegmentOffset);

        /* Let any pending read proceed */
        while (MM_IS_WAIT_PTE(Entry))
        {
            MmUnlockSectionSegment(Segment);
            MmUnlockAddressSpace(AddressSpace);
            MiWaitForPageEvent(NULL, NULL);
            MmLockAddressSpace(AddressSpace);
            MmLockSectionSegment(Segment);
            Entry = MmGetPageEntrySectionSegment(Segment, &SegmentOffset);
        }

        /* We are called from Cc, this can't be backed by the page files */
        ASSERT(!IS_SWAP_FROM_SSE(Entry));

        /* If there is no page there, there is nothing to make dirty */
        if (Entry != 0)
        {
            /* Dirtify the entry */
            MmSetPageEntrySectionSegment(Segment, &SegmentOffset, DIRTY_SSE(Entry));
        }

        SegmentOffset.QuadPart += PAGE_SIZE;
    }

    MmUnlockSectionSegment(Segment);

    MmUnlockAddressSpace(AddressSpace);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
MmExtendSection(
    _In_ PVOID _Section,
    _Inout_ PLARGE_INTEGER NewSize)
{
    PSECTION Section = _Section;

    /* It makes no sense to extend an image mapping */
    if (Section->u.Flags.Image)
        return STATUS_SECTION_NOT_EXTENDED;

    /* Nor is it possible to extend a page file mapping */
    if (!Section->u.Flags.File)
        return STATUS_SECTION_NOT_EXTENDED;

    if (!MiIsRosSectionObject(Section))
        return STATUS_NOT_IMPLEMENTED;

    /* We just extend the sizes. Shrinking is a no-op ? */
    if (NewSize->QuadPart > Section->SizeOfSection.QuadPart)
    {
        PMM_SECTION_SEGMENT Segment = (PMM_SECTION_SEGMENT)Section->Segment;
        Section->SizeOfSection = *NewSize;

        if (!Section->u.Flags.Reserve)
        {
            MmLockSectionSegment(Segment);
            if (Segment->RawLength.QuadPart < NewSize->QuadPart)
            {
                Segment->RawLength = *NewSize;
                Segment->Length.QuadPart = (NewSize->QuadPart + PAGE_SIZE - 1) & ~((LONGLONG)PAGE_SIZE);
            }
            MmUnlockSectionSegment(Segment);
        }
    }

    return STATUS_SUCCESS;
}

/* EOF */
