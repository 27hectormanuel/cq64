// SPDX-License-Identifier: CC0-1.0
// Windows build: the ELF definitions the loader uses (values from the ELF and ARM ELF specs).
#pragma once
#include <stdint.h>
typedef uint32_t Elf32_Addr, Elf32_Off, Elf32_Word;
typedef int32_t Elf32_Sword;
typedef uint16_t Elf32_Half;
#define EI_NIDENT 16
#define EI_CLASS 4
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define ELFCLASS32 1
#define EM_ARM 40
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type, e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff, e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;
typedef struct {
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr, p_paddr;
    Elf32_Word p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;
typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info, st_other;
    Elf32_Half st_shndx;
} Elf32_Sym;
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_ARM_EXIDX 0x70000001
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_INIT 12
#define DT_FINI 13
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_GNU_HASH 0x6ffffef5
#define SHN_UNDEF 0
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_OBJECT 1
#define STT_FUNC 2
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((unsigned char)(i))
#define R_ARM_NONE 0
#define R_ARM_REL32 3
#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
