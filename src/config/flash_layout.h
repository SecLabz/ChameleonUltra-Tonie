#pragma once
/* Application and library boundaries preserve the original bootloader. */
#define APP_FLASH_START 0x27000u
#define APP_FLASH_END 0xc3000u
#define LIBRARY_FLASH_START APP_FLASH_END
#define LIBRARY_FLASH_END 0xf3000u
