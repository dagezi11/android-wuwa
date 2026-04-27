#include "wuwa_ioctl.h"

#include <asm-generic/errno-base.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include "wuwa_utils.h"

#define WUWA_IOV_MAX 1024UL

#if !defined(ARCH_HAS_VALID_PHYS_ADDR_RANGE) || defined(MODULE)
static inline int wuwa_valid_phys_addr_range(uintptr_t addr, size_t size) {
    return addr + size <= __pa(high_memory);
}
#define WUWA_VALID_PHYS_ADDR_RANGE(x, y) wuwa_valid_phys_addr_range(x, y)
#else
#define WUWA_VALID_PHYS_ADDR_RANGE(x, y) valid_phys_addr_range(x, y)
#endif

static int copy_user_iovec(struct iovec __user* iovecs, unsigned long index, struct iovec* iov) {
    if (copy_from_user(iov, &iovecs[index], sizeof(*iov))) {
        return -EFAULT;
    }

    if (iov->iov_len > 0 && !iov->iov_base) {
        return -EINVAL;
    }

    return 0;
}

static int validate_iov_cmd(struct wuwa_memory_iov_cmd* cmd) {
    if (cmd->pid <= 0 || !cmd->local_iov || !cmd->remote_iov) {
        return -EINVAL;
    }

    if (cmd->local_iovcnt == 0 || cmd->remote_iovcnt == 0) {
        return -EINVAL;
    }

    if (cmd->local_iovcnt > WUWA_IOV_MAX || cmd->remote_iovcnt > WUWA_IOV_MAX) {
        return -EINVAL;
    }

    if (cmd->mode != WUWA_MEMORY_IOV_PHYS_DIRECT && cmd->mode != WUWA_MEMORY_IOV_IOREMAP) {
        return -EINVAL;
    }

    if (cmd->mode == WUWA_MEMORY_IOV_IOREMAP && (cmd->prot < WMT_NORMAL || cmd->prot > WMT_NORMAL_iNC_oWB)) {
        return -EINVAL;
    }

    return 0;
}

static int direct_memory_transfer(pid_t pid, uintptr_t local_va, uintptr_t remote_va, size_t size, bool write) {
    size_t done = 0;

    while (done < size) {
        uintptr_t pa;
        uintptr_t remote_cur = remote_va + done;
        uintptr_t local_cur = local_va + done;
        size_t chunk = min_t(size_t, size - done, PAGE_SIZE - (remote_cur & (PAGE_SIZE - 1)));
        void* mapped;
        int ret;

        ret = translate_process_vaddr(pid, remote_cur, &pa);
        if (ret < 0) {
            return ret;
        }

        if (!pa || !pfn_valid(__phys_to_pfn(pa)) || !WUWA_VALID_PHYS_ADDR_RANGE(pa, chunk)) {
            return -EFAULT;
        }

        mapped = phys_to_virt(pa);
        if (!mapped) {
            return -ENOMEM;
        }

        if (write) {
            if (copy_from_user(mapped, (void __user*)local_cur, chunk)) {
                return -EACCES;
            }
        } else {
            if (copy_to_user((void __user*)local_cur, mapped, chunk)) {
                return -EACCES;
            }
        }

        done += chunk;
    }

    return 0;
}

static int ioremap_memory_transfer(pid_t pid, uintptr_t local_va, uintptr_t remote_va, size_t size, int prot_type,
                                   bool write) {
    pgprot_t prot;
    size_t done = 0;
    int ret = convert_wmt_to_pgprot(prot_type, &prot);

    if (ret < 0) {
        return ret;
    }

    while (done < size) {
        uintptr_t pa;
        uintptr_t remote_cur = remote_va + done;
        uintptr_t local_cur = local_va + done;
        size_t chunk = min_t(size_t, size - done, PAGE_SIZE - (remote_cur & (PAGE_SIZE - 1)));
        void* mapped;

        ret = translate_process_vaddr(pid, remote_cur, &pa);
        if (ret < 0) {
            return ret;
        }

        if (!pa || !pfn_valid(__phys_to_pfn(pa))) {
            return -EFAULT;
        }

        mapped = wuwa_ioremap_prot(pa, chunk, prot);
        if (!mapped) {
            return -ENOMEM;
        }

        if (write) {
            ret = copy_from_user(mapped, (void __user*)local_cur, chunk);
        } else {
            ret = copy_to_user((void __user*)local_cur, mapped, chunk);
        }
        iounmap(mapped);

        if (ret) {
            return -EACCES;
        }

        done += chunk;
    }

    return 0;
}

static int memory_iov_transfer(struct wuwa_memory_iov_cmd* cmd, bool write) {
    unsigned long local_index = 0, remote_index = 0;
    size_t local_offset = 0, remote_offset = 0;

    while (local_index < cmd->local_iovcnt && remote_index < cmd->remote_iovcnt) {
        struct iovec local_iov, remote_iov;
        uintptr_t local_base, remote_base;
        size_t chunk;
        int ret;

        ret = copy_user_iovec(cmd->local_iov, local_index, &local_iov);
        if (ret < 0) {
            return cmd->bytes_done ? 0 : ret;
        }

        ret = copy_user_iovec(cmd->remote_iov, remote_index, &remote_iov);
        if (ret < 0) {
            return cmd->bytes_done ? 0 : ret;
        }

        if (local_offset >= local_iov.iov_len) {
            local_index++;
            local_offset = 0;
            continue;
        }

        if (remote_offset >= remote_iov.iov_len) {
            remote_index++;
            remote_offset = 0;
            continue;
        }

        local_base = (uintptr_t)local_iov.iov_base + local_offset;
        remote_base = (uintptr_t)remote_iov.iov_base + remote_offset;
        chunk = min_t(size_t, local_iov.iov_len - local_offset, remote_iov.iov_len - remote_offset);

        if (cmd->mode == WUWA_MEMORY_IOV_PHYS_DIRECT) {
            ret = direct_memory_transfer(cmd->pid, local_base, remote_base, chunk, write);
        } else {
            ret = ioremap_memory_transfer(cmd->pid, local_base, remote_base, chunk, cmd->prot, write);
        }

        if (ret < 0) {
            return cmd->bytes_done ? 0 : ret;
        }

        cmd->bytes_done += chunk;
        local_offset += chunk;
        remote_offset += chunk;
    }

    return 0;
}

static int do_memory_iov(void __user* arg, bool write) {
    struct wuwa_memory_iov_cmd cmd;
    int ret;

    if (copy_from_user(&cmd, arg, sizeof(cmd))) {
        return -EFAULT;
    }

    cmd.bytes_done = 0;
    ret = validate_iov_cmd(&cmd);
    if (ret < 0) {
        return ret;
    }

    ret = memory_iov_transfer(&cmd, write);
    if (copy_to_user(arg, &cmd, sizeof(cmd))) {
        return -EFAULT;
    }

    return ret;
}

int do_readv_memory(struct socket* sock, void __user* arg) {
    return do_memory_iov(arg, false);
}

int do_writev_memory(struct socket* sock, void __user* arg) {
    return do_memory_iov(arg, true);
}
