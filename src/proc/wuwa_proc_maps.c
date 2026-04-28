#include "wuwa_proc_maps.h"

#include "wuwa_ioctl.h"
#include "wuwa_utils.h"

#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/pid.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include(<linux/pgsize_migration_inline.h>)
#include <linux/pgsize_migration_inline.h>
#endif

#if __has_include(<linux/page_size_compat.h>)
#include <linux/page_size_compat.h>
#endif

#ifndef VM_PAD_MASK
#define VM_PAD_MASK 0
#endif

#ifndef VM_PAD_SHIFT
#define VM_PAD_SHIFT 0
#endif

#ifndef __VM_NO_COMPAT
#define __VM_NO_COMPAT 0
#endif

#ifdef CONFIG_CFI_CLANG
#define WUWA_NO_CFI __nocfi
#else
#define WUWA_NO_CFI
#endif

#define WUWA_PROC_MAPS_MIN_CHUNK (16 * 1024)
#define WUWA_PROC_MAPS_MAX_CHUNK (1024 * 1024)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define WUWA_HAS_VMA_ITERATOR 1
#else
#define WUWA_HAS_VMA_ITERATOR 0
#endif

typedef unsigned long (*wuwa_vma_pages_fn_t)(struct vm_area_struct* vma);
#if WUWA_HAS_VMA_ITERATOR
typedef void (*wuwa_fold_fixup_fn_t)(struct vma_iterator* iter, unsigned long* end);
#endif
typedef void (*wuwa_show_pad_vma_fn_t)(struct vm_area_struct* vma, struct seq_file* m, void* func, bool smaps);

struct wuwa_maps_ctx {
#if WUWA_HAS_VMA_ITERATOR
    struct vma_iterator* iter;
#endif
};

static wuwa_vma_pages_fn_t vma_data_pages_fn;
static wuwa_vma_pages_fn_t vma_pad_pages_fn;
#if WUWA_HAS_VMA_ITERATOR
static wuwa_fold_fixup_fn_t fold_fixup_fn;
#endif
static wuwa_show_pad_vma_fn_t show_pad_vma_fn;
static bool maps_symbols_resolved;

static const char* wuwa_pad_vma_name(struct vm_area_struct* vma) {
    (void)vma;
    return "[page size compat]";
}

static const struct vm_operations_struct wuwa_pad_vma_ops = {
    .name = wuwa_pad_vma_name,
};

static void wuwa_resolve_maps_symbols(void) {
    if (maps_symbols_resolved)
        return;

    vma_data_pages_fn = (wuwa_vma_pages_fn_t)kallsyms_lookup_name_ex("vma_data_pages");
    vma_pad_pages_fn = (wuwa_vma_pages_fn_t)kallsyms_lookup_name_ex("vma_pad_pages");
#if WUWA_HAS_VMA_ITERATOR
    fold_fixup_fn = (wuwa_fold_fixup_fn_t)kallsyms_lookup_name_ex("__fold_filemap_fixup_entry");
#endif
    show_pad_vma_fn = (wuwa_show_pad_vma_fn_t)kallsyms_lookup_name_ex("show_map_pad_vma");
    maps_symbols_resolved = true;
}

static unsigned long WUWA_NO_CFI wuwa_call_vma_pages(wuwa_vma_pages_fn_t fn, struct vm_area_struct* vma) {
    return fn(vma);
}

#if WUWA_HAS_VMA_ITERATOR
static void WUWA_NO_CFI wuwa_call_fold_fixup(wuwa_fold_fixup_fn_t fn,
                                             struct vma_iterator* iter,
                                             unsigned long* end) {
    fn(iter, end);
}
#endif

static void WUWA_NO_CFI wuwa_call_show_pad_vma(wuwa_show_pad_vma_fn_t fn,
                                               struct vm_area_struct* vma,
                                               struct seq_file* m,
                                               void* show_fn) {
    fn(vma, m, show_fn, false);
}

static unsigned long wuwa_vma_pad_pages(struct vm_area_struct* vma) {
    unsigned long pad_pages;

    if (vma_pad_pages_fn)
        return wuwa_call_vma_pages(vma_pad_pages_fn, vma);

#if VM_PAD_MASK != 0
    pad_pages = (vma->vm_flags & VM_PAD_MASK) >> VM_PAD_SHIFT;
    if (pad_pages > vma_pages(vma))
        return 0;
    return pad_pages;
#else
    pad_pages = 0;
    return pad_pages;
#endif
}

static unsigned long wuwa_vma_data_pages(struct vm_area_struct* vma) {
    unsigned long pad_pages;
    unsigned long pages;

    if (vma_data_pages_fn)
        return wuwa_call_vma_pages(vma_data_pages_fn, vma);

    pages = vma_pages(vma);
    pad_pages = wuwa_vma_pad_pages(vma);
    return pad_pages > pages ? 0 : pages - pad_pages;
}

static unsigned long wuwa_vma_map_end(struct vm_area_struct* vma) {
    unsigned long pad_pages = wuwa_vma_pad_pages(vma);

    if (!pad_pages)
        return vma->vm_end;
    if ((pad_pages << PAGE_SHIFT) > vma->vm_end - vma->vm_start)
        return vma->vm_end;
    return vma->vm_end - (pad_pages << PAGE_SHIFT);
}

#if WUWA_HAS_VMA_ITERATOR
static void wuwa_fold_filemap_fixup_entry(struct vma_iterator* iter, unsigned long* end) {
    struct vm_area_struct* next_vma;

    if (!iter)
        return;
    if (fold_fixup_fn) {
        wuwa_call_fold_fixup(fold_fixup_fn, iter, end);
        return;
    }
    if (!__VM_NO_COMPAT)
        return;

    next_vma = vma_next(iter);
    if (next_vma && (next_vma->vm_flags & __VM_NO_COMPAT)) {
        *end = next_vma->vm_end;
        return;
    }
    vma_prev(iter);
}
#endif

static void wuwa_show_vma_header_prefix(struct seq_file* m,
                                        unsigned long start,
                                        unsigned long end,
                                        vm_flags_t flags,
                                        unsigned long long pgoff,
                                        dev_t dev,
                                        unsigned long ino) {
    seq_setwidth(m, 25 + sizeof(void*) * 6 - 1);
    seq_printf(m, "%08lx-%08lx %c%c%c%c %08llx %02x:%02x %lu ",
               start,
               end,
               flags & VM_READ ? 'r' : '-',
               flags & VM_WRITE ? 'w' : '-',
               flags & VM_EXEC ? 'x' : '-',
               flags & VM_MAYSHARE ? 's' : 'p',
               pgoff,
               MAJOR(dev),
               MINOR(dev),
               ino);
}

static bool wuwa_vma_anon_name_is_user_ptr(void) {
#ifdef CONFIG_ANON_VMA_NAME
    return __builtin_types_compatible_p(typeof(((struct vm_area_struct*)0)->anon_name), const char __user*);
#else
    return false;
#endif
}

static void* wuwa_vma_anon_name_raw(struct vm_area_struct* vma) {
#ifdef CONFIG_ANON_VMA_NAME
    if (wuwa_vma_anon_name_is_user_ptr() && vma->vm_file)
        return NULL;
    return (void*)vma->anon_name;
#else
    (void)vma;
    return NULL;
#endif
}

static const char* wuwa_vma_anon_name_kernel(struct vm_area_struct* vma) {
    void* anon_name = wuwa_vma_anon_name_raw(vma);

    if (!anon_name || wuwa_vma_anon_name_is_user_ptr())
        return NULL;
    return (const char*)anon_name + sizeof(struct kref);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
static void wuwa_seq_print_user_anon_name(struct seq_file* m, struct vm_area_struct* vma) {
    const char __user* name = (const char __user*)wuwa_vma_anon_name_raw(vma);
    struct mm_struct* mm = vma->vm_mm;
    unsigned long page_start_vaddr;
    unsigned long page_offset;
    unsigned long num_pages;
    unsigned long max_len = NAME_MAX;
    int i;

    if (!name || !mm)
        return;

    page_start_vaddr = (unsigned long)name & PAGE_MASK;
    page_offset = (unsigned long)name - page_start_vaddr;
    num_pages = DIV_ROUND_UP(page_offset + max_len, PAGE_SIZE);

    seq_puts(m, "[anon:");

    for (i = 0; i < num_pages; i++) {
        int len;
        int write_len;
        const char* kaddr;
        long pages_pinned;
        struct page* page;

        pages_pinned = get_user_pages_remote(mm, page_start_vaddr, 1, 0, &page, NULL, NULL);
        if (pages_pinned < 1) {
            seq_puts(m, "<fault>]");
            return;
        }

        kaddr = (const char*)kmap(page);
        len = min(max_len, PAGE_SIZE - page_offset);
        write_len = strnlen(kaddr + page_offset, len);
        seq_write(m, kaddr + page_offset, write_len);
        kunmap(page);
        put_user_page(page);

        if (write_len != len)
            break;

        max_len -= len;
        page_offset = 0;
        page_start_vaddr += PAGE_SIZE;
    }

    seq_putc(m, ']');
}
#else
static void wuwa_seq_print_user_anon_name(struct seq_file* m, struct vm_area_struct* vma) {
    (void)m;
    (void)vma;
}
#endif

static void wuwa_show_map_vma(struct seq_file* m, struct vm_area_struct* vma) {
    struct mm_struct* mm = vma->vm_mm;
    struct file* file = vma->vm_file;
    vm_flags_t flags = vma->vm_flags;
    unsigned long ino = 0;
    unsigned long long pgoff = 0;
    unsigned long start = vma->vm_start;
    unsigned long end = wuwa_vma_map_end(vma);
    dev_t dev = 0;
    const char* name = NULL;
    const char* anon_name = NULL;

    if (file) {
        struct inode* inode = file_inode(file);
        dev = inode->i_sb->s_dev;
        ino = inode->i_ino;
        pgoff = ((loff_t)vma->vm_pgoff) << PAGE_SHIFT;
    }
    if (__VM_NO_COMPAT && (flags & __VM_NO_COMPAT))
        return;

#if WUWA_HAS_VMA_ITERATOR
    struct wuwa_maps_ctx* ctx = m->private;

    wuwa_fold_filemap_fixup_entry(ctx ? ctx->iter : NULL, &end);
#endif
    wuwa_show_vma_header_prefix(m, start, end, flags, pgoff, dev, ino);
    if (mm)
        anon_name = wuwa_vma_anon_name_kernel(vma);

    if (file) {
        seq_pad(m, ' ');
        if (anon_name)
            seq_printf(m, "[anon_shmem:%s]", anon_name);
        else
            seq_file_path(m, file, "\n");
        goto done;
    }
    if (vma->vm_ops && vma->vm_ops->name) {
        name = vma->vm_ops->name(vma);
        if (name)
            goto done;
    }
    name = arch_vma_name(vma);
    if (!name) {
        if (!mm)
            name = "[vdso]";
        else if (vma_is_initial_heap(vma))
            name = "[heap]";
        else if (vma_is_initial_stack(vma))
            name = "[stack]";
        else if (anon_name) {
            seq_pad(m, ' ');
            seq_printf(m, "[anon:%s]", anon_name);
        } else if (wuwa_vma_anon_name_is_user_ptr() && wuwa_vma_anon_name_raw(vma)) {
            seq_pad(m, ' ');
            wuwa_seq_print_user_anon_name(m, vma);
        }
    }

done:
    if (name) {
        seq_pad(m, ' ');
        seq_puts(m, name);
    }
    seq_putc(m, '\n');
}

static void wuwa_show_local_pad_vma(struct vm_area_struct* vma, struct seq_file* m) {
    struct vm_area_struct pad;

    if (!wuwa_vma_pad_pages(vma))
        return;

    memcpy(&pad, vma, sizeof(pad));
    pad.vm_file = NULL;
    pad.vm_ops = &wuwa_pad_vma_ops;
    pad.vm_start = wuwa_vma_map_end(vma);
    __vm_flags_mod(&pad, 0, VM_READ | VM_WRITE | VM_EXEC | VM_PAD_MASK);
    wuwa_show_map_vma(m, &pad);
}

static void wuwa_show_pad_vma(struct vm_area_struct* vma, struct seq_file* m) {
    if (show_pad_vma_fn) {
        wuwa_call_show_pad_vma(show_pad_vma_fn, vma, m, (void*)wuwa_show_map_vma);
        return;
    }
    wuwa_show_local_pad_vma(vma, m);
}

static void wuwa_show_map(struct seq_file* m, struct vm_area_struct* vma) {
    if (wuwa_vma_data_pages(vma))
        wuwa_show_map_vma(m, vma);

    wuwa_show_pad_vma(vma, m);
}

static void wuwa_seq_file_init(struct seq_file* m, char* buf, size_t size, void* private) {
    memset(m, 0, sizeof(*m));
    m->buf = buf;
    m->size = size;
    m->private = private;
}

static int wuwa_emit_proc_maps(struct mm_struct* mm,
                               struct wuwa_get_proc_maps_cmd* cmd,
                               struct seq_file* m,
                               struct wuwa_maps_ctx* ctx) {
    struct vm_area_struct* vma;
#if WUWA_HAS_VMA_ITERATOR
    struct vma_iterator iter;

    vma_iter_init(&iter, mm, cmd->start_addr);
    ctx->iter = &iter;
#else
    (void)ctx;
#endif
    cmd->next_addr = cmd->start_addr;
    cmd->eof = 1;

#if WUWA_HAS_VMA_ITERATOR
    while ((vma = vma_next(&iter)) != NULL) {
#else
    for (vma = find_vma(mm, cmd->start_addr); vma != NULL; vma = vma->vm_next) {
#endif
        size_t before = m->count;
        size_t before_pad = m->pad_until;

        wuwa_show_map(m, vma);
        if (seq_has_overflowed(m)) {
            m->count = before;
            m->pad_until = before_pad;
            if (before == 0)
                return -ENOSPC;
            cmd->next_addr = vma->vm_start;
            cmd->eof = 0;
            return 0;
        }
        cmd->next_addr = vma->vm_end;
    }

    return 0;
}

static int wuwa_get_proc_maps(struct wuwa_get_proc_maps_cmd* cmd, char* kbuf, size_t kbuf_size) {
    struct task_struct* task;
    struct mm_struct* mm;
    struct seq_file seq;
    struct wuwa_maps_ctx ctx;
    int ret;

    task = get_target_task(cmd->pid);
    if (!task)
        return -ESRCH;

    mm = get_task_mm(task);
    put_task_struct(task);
    if (!mm)
        return -ESRCH;

    wuwa_resolve_maps_symbols();
    wuwa_seq_file_init(&seq, kbuf, kbuf_size, &ctx);
    MM_READ_LOCK(mm);
    ret = wuwa_emit_proc_maps(mm, cmd, &seq, &ctx);
    MM_READ_UNLOCK(mm);
    mmput(mm);

    cmd->bytes_written = ret ? 0 : seq.count;
    return ret;
}

static int wuwa_validate_proc_maps_cmd(struct wuwa_get_proc_maps_cmd* cmd) {
    if (!cmd->buf || cmd->buf_size == 0)
        return -EINVAL;
    if (cmd->buf_size < WUWA_PROC_MAPS_MIN_CHUNK)
        return -ENOSPC;

    cmd->bytes_written = 0;
    cmd->next_addr = cmd->start_addr;
    cmd->eof = 0;
    return 0;
}

int do_get_proc_maps(struct socket* sock, void __user* arg) {
    struct wuwa_get_proc_maps_cmd cmd;
    size_t kbuf_size;
    char* kbuf;
    int ret;

    (void)sock;

    if (copy_from_user(&cmd, arg, sizeof(cmd)))
        return -EFAULT;

    ret = wuwa_validate_proc_maps_cmd(&cmd);
    if (ret)
        return ret;

    kbuf_size = min_t(size_t, cmd.buf_size, WUWA_PROC_MAPS_MAX_CHUNK);
    kbuf = kvzalloc(kbuf_size, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    ret = wuwa_get_proc_maps(&cmd, kbuf, kbuf_size);
    if (!ret && cmd.bytes_written && copy_to_user(cmd.buf, kbuf, cmd.bytes_written))
        ret = -EFAULT;
    if (!ret && copy_to_user(arg, &cmd, sizeof(cmd)))
        ret = -EFAULT;

    kvfree(kbuf);
    return ret;
}
