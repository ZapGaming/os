/* Final link + ELF32 serialization stage -- see include/cc/elf_writer.h.
 * Everything up to here (cc/codegen.c) has been building m->text/
 * rodata/data plus a list of pending relocations; this file is where
 * "pending" ends: it lays out .text/.rodata/.data (in that order,
 * 16-byte aligned against each other purely for tidiness -- x86 has no
 * alignment requirement this ABI actually needs), patches every
 * relocation now that each section's absolute base address in the
 * fixed 0x04000000-0x0F000000 window is known, and writes out a
 * complete ELF32 ET_EXEC/EM_386 header + one PT_LOAD program header +
 * the three sections' bytes. */
#include <cc/elf_writer.h>
#include <kernel/kheap.h>
#include <string.h>

#define EI_NIDENT 16
#define ET_EXEC 2
#define EM_386 3
#define PT_LOAD 1

struct elf32_header {
    uint8_t e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed));

static uint32_t align16(uint32_t x) { return (x + 15u) & ~15u; }

int cc_elf_link_and_write(struct cc_module *m, int entry_func_index, uint8_t **out_buf, uint32_t *out_len) {
    uint32_t headers_size = (uint32_t)(sizeof(struct elf32_header) + sizeof(struct elf32_phdr));
    uint32_t text_file_off = align16(headers_size);
    uint32_t rodata_file_off = align16(text_file_off + m->text.len);
    uint32_t data_file_off = align16(rodata_file_off + m->rodata.len);
    uint32_t filesz = data_file_off + m->data.len;
    uint32_t memsz = filesz + m->bss_size;

    if (entry_func_index < 0 || (uint32_t)entry_func_index >= (uint32_t)m->func_count) {
        cc_module_errorf(m, 0, "internal error: no entry function to link");
        return 0;
    }
    if (memsz > CC_USER_LOAD_MAX - CC_USER_LOAD_MIN) {
        cc_module_errorf(m, 0, "compiled program is too large to fit in the user load window");
        return 0;
    }

    uint32_t text_base = CC_USER_LOAD_MIN + text_file_off;
    uint32_t rodata_base = CC_USER_LOAD_MIN + rodata_file_off;
    uint32_t data_base = CC_USER_LOAD_MIN + data_file_off;
    uint32_t bss_base = CC_USER_LOAD_MIN + filesz;

    for (int i = 0; i < m->reloc_count; i++) {
        struct cc_reloc *r = &m->relocs[i];
        uint32_t addr;
        switch (r->section) {
            case CC_SEC_TEXT: addr = text_base + r->offset_in_section; break;
            case CC_SEC_RODATA: addr = rodata_base + r->offset_in_section; break;
            case CC_SEC_DATA: addr = data_base + r->offset_in_section; break;
            case CC_SEC_BSS: addr = bss_base + r->offset_in_section; break;
            case CC_SEC_FUNC: addr = text_base + m->funcs[r->offset_in_section].text_offset; break;
            default: addr = 0; break;
        }
        cc_buf_patch_u32(&m->text, r->text_offset, addr);
    }

    uint32_t entry = text_base + m->funcs[entry_func_index].text_offset;

    uint8_t *buf = (uint8_t *)kmalloc(filesz);
    if (!buf) { cc_module_errorf(m, 0, "out of memory writing the output ELF file"); return 0; }
    memset(buf, 0, filesz);

    struct elf32_header *eh = (struct elf32_header *)buf;
    eh->e_ident[0] = 0x7F; eh->e_ident[1] = 'E'; eh->e_ident[2] = 'L'; eh->e_ident[3] = 'F';
    eh->e_ident[4] = 1; /* ELFCLASS32 */
    eh->e_ident[5] = 1; /* ELFDATA2LSB */
    eh->e_ident[6] = 1; /* EV_CURRENT */
    eh->e_type = ET_EXEC;
    eh->e_machine = EM_386;
    eh->e_version = 1;
    eh->e_entry = entry;
    eh->e_phoff = (uint32_t)sizeof(struct elf32_header);
    eh->e_shoff = 0;
    eh->e_flags = 0;
    eh->e_ehsize = (uint16_t)sizeof(struct elf32_header);
    eh->e_phentsize = (uint16_t)sizeof(struct elf32_phdr);
    eh->e_phnum = 1;
    eh->e_shentsize = 0;
    eh->e_shnum = 0;
    eh->e_shstrndx = 0;

    struct elf32_phdr *ph = (struct elf32_phdr *)(buf + eh->e_phoff);
    ph->p_type = PT_LOAD;
    ph->p_offset = 0;
    ph->p_vaddr = CC_USER_LOAD_MIN;
    ph->p_paddr = CC_USER_LOAD_MIN;
    ph->p_filesz = filesz;
    ph->p_memsz = memsz;
    ph->p_flags = 7; /* R+W+X -- kernel/elf.c doesn't check flags, but keep it a valid, sensible value */
    ph->p_align = 0x1000;

    if (m->text.len) memcpy(buf + text_file_off, m->text.data, m->text.len);
    if (m->rodata.len) memcpy(buf + rodata_file_off, m->rodata.data, m->rodata.len);
    if (m->data.len) memcpy(buf + data_file_off, m->data.data, m->data.len);

    *out_buf = buf;
    *out_len = filesz;
    return 1;
}
