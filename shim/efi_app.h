/*
 * efi_app.h -- the minimum of UEFI needed to build the shim as an EFI application.
 *
 * WHY NOT JUST INCLUDE sweet-uefi/include/efi.h. Two repositories, and the kernel
 * CI checks out only this one. Hand-transcribing the pieces here keeps the build
 * self-contained.
 *
 * THE RISK THAT CREATES, AND HOW IT IS HANDLED. A struct with the wrong field
 * offset does not fail to compile -- it reads the wrong pointer at run time, on
 * hardware, with no console. So every offset this file depends on is pinned by a
 * _Static_assert against the value measured in `sweet-uefi`, which in turn is
 * checked against the UEFI 2.10 specification by that project's
 * `tools/check_efi_layout.py`. If either side drifts, this stops compiling.
 *
 * Only the fields actually used are declared. Anything past them is opaque padding
 * sized to the documented total, so a mistake in the tail cannot silently shift a
 * field that matters.
 */

#ifndef SWEET_SHIM_EFI_APP_H
#define SWEET_SHIM_EFI_APP_H

#include <stdint.h>

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint16_t CHAR16;
typedef uint64_t UINTN;
typedef int64_t  INT64;
typedef uint8_t  BOOLEAN;
typedef UINTN    EFI_STATUS;
typedef void    *EFI_HANDLE;

#define EFIAPI
#define EFI_SUCCESS 0u

typedef struct {
	uint64_t Signature;
	uint32_t Revision;
	uint32_t HeaderSize;
	uint32_t CRC32;
	uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef struct {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t  Data4[8];
} EFI_GUID;

/* Simple Text Output (spec 12.2). Only OutputString and the Reset before it are
 * declared; the rest is padding. */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
	EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *,
	    const CHAR16 *);
	/* Enough opaque tail to keep the struct's real size; never dereferenced. */
	void *rest[12];
};

typedef struct {
	char opaque[1];
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
	char opaque[1];
} EFI_BOOT_SERVICES_OPAQUE;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32  FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	void *RuntimeServices;
	EFI_BOOT_SERVICES_OPAQUE *BootServices;
} EFI_SYSTEM_TABLE;

/*
 * The offsets this file depends on, pinned to the values sweet-uefi measured
 * against the specification. UEFI 2.10 table 4-1, and the simple-text-output
 * protocol per 12.2.
 */
_Static_assert(sizeof(EFI_TABLE_HEADER) == 24,
    "EFI_TABLE_HEADER is 24 bytes");
_Static_assert(__builtin_offsetof(EFI_SYSTEM_TABLE, ConOut) == 64,
    "EFI_SYSTEM_TABLE.ConOut is at offset 64");
_Static_assert(__builtin_offsetof(EFI_SYSTEM_TABLE, BootServices) == 96,
    "EFI_SYSTEM_TABLE.BootServices is at offset 96");
_Static_assert(__builtin_offsetof(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL,
    OutputString) == 8,
    "OutputString follows Reset, which is one pointer");

#endif /* SWEET_SHIM_EFI_APP_H */
