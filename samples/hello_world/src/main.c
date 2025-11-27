/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

int main(void)
{
	printf("\n");
	printf("Flash Test Firmware v7\n");
	printf("\n");
	printf("=== NAND ===\n");
	printf(" == Read == \n");
	printf("* Read and display 32 bytes from address c0: flash read nand c0 20\n");
	printf("  Note: This commands reads a full page (2k) for every 16 bytes requested\n");
	printf("\n");
	printf("* Read 4096 bytes from address 0 once: flash read_test nand 0 4096 1\n");
	printf("  Note: This command does only **one** request per page (2k).\n");
	printf("\n");
	printf(" == Erase == \n");
	printf("* Erase bank 0 (128k): flash erase nand 0\n");
	printf("* Erase bank 1 (128k): flash erase nand 20000\n");
	printf("\n");
	printf(" == Write == \n");
	printf("* Write first page (2k) with increasing pattern 00 01 02 ...: flash write_test "
	       "nand 0 2048 1\n");
	printf("* Write second page (2k) with increasing pattern 00 01 02 ...: flash write_test "
	       "nand 800 2048 1\n");
	printf("\n");
	printf(" == Erase + Write == \n");
	printf("* Erase and write bank 0 once: flash erase_write_test nand 0 131072 1\n");
	printf("* Erase and write bank 0 twice: flash erase_write_test nand 0 131072 2\n");
	printf(" !! Caution: This may destroy your flash quickly!\n");

	return 0;
}
