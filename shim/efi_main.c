/*
 * efi_main.c -- the EFI application entry point for the ios-sweet shim.
 *
 * WHAT THIS IS. The shim's job is to place the SWEETXNU payload and enter the
 * kernel with the MMU off. It used to run only as a `boot.img` payload handed
 * control by ABL. This makes the same code loadable by firmware as a normal EFI
 * application, which is what lets the `sweet-uefi` menu start it.
 *
 * WHAT IT DOES NOT DO YET. It does not read the payload from a file, and it does
 * not find the device tree. Both are environment questions that cannot be answered
 * from source alone -- see shim/efi/README.md. What it does is establish the frame:
 * receive the system table, prove the pieces link and run as a PE32+ application,
 * and hand off through the same reviewed path the boot.img build uses.
 *
 * For that reason `efi_main()` currently reports what it found and returns. That is
 * deliberate and it is the honest state of the port: a launcher that claims to boot
 * and then silently returns to firmware would be worse than one that names what it
 * is missing. `efi_handoff()` is called only when a payload address has actually
 * been supplied, which today means never from this entry point.
 */

#include <stdint.h>
#include "efi_app.h"


/* Provided by the shim proper (efi_handoff.c); declared here to keep efi.h free
 * of shim types. */
const char *efi_handoff(uintptr_t shim_pa, uintptr_t shim_len, uintptr_t dtb,
    uintptr_t payload_pa, uint64_t payload_len);

/* Where the linker put this image's first byte. */
extern char __image_base[];

static void
print(EFI_SYSTEM_TABLE *st, const char *s)
{
	CHAR16 buf[128];
	int i = 0;

	for (; s[i] != 0 && i < 127; i++) {
		buf[i] = (CHAR16)(uint8_t)s[i];
	}
	buf[i] = 0;
	st->ConOut->OutputString(st->ConOut, buf);
}

EFI_STATUS
efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	(void)image;

	st->ConOut->OutputString(st->ConOut,
	    L"\r\nios-sweet shim (EFI application)\r\n");

	print(st, "This image can place the SWEETXNU payload and enter the kernel, "
	    "but it does not know where the payload or the device tree are.\r\n");
	print(st, "Read shim/efi/README.md: the payload comes from the ESP and the "
	    "device tree has to be supplied, and neither is decided yet.\r\n");
	print(st, "The boot.img path (mkbootimg + fastboot boot) remains the "
	    "supported route.\r\n");

	/*
	 * A caller that knows the addresses can reach the real handoff:
	 *
	 *   efi_handoff((uintptr_t)__image_base, image_len, dtb, payload_pa, len);\n	 *\n	 * where image_len comes from the PE headers (SizeOfImage).
	 *
	 * It is not called here because guessing either address would turn a clear
	 * message into a silent hang inside a trampoline that has never run on
	 * hardware.
	 */
	print(st, "Returning to firmware.\r\n");
	return EFI_SUCCESS;
}
