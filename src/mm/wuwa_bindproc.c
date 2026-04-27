#include "wuwa_bindproc.h"
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/version.h>
#include "asm-generic/errno-base.h"
#include "karray_list.h"
#include "linux/anon_inodes.h"
#include "linux/compiler_types.h"
#include "linux/sched/task.h"
#include "linux/types.h"
#include "wuwa_common.h"
#include "wuwa_ioctl.h"
#include "wuwa_page_walk.h"
#include "wuwa_utils.h"

struct wuwa_mapped_page_info {
    uintptr_t phys_addr; /* Physical page address (page-aligned) */
    void* mapped_ptr; /* Kernel virtual address from ioremap */
};

struct wuwa_bindproc_private {
    pid_t pid;
    pgprot_t prot; /* Memory protection type (use WMT_*) */
    struct karray_list* mapped_pages; /* of struct wuwa_mapped_page_info * */
    struct mutex lock; /* Protects mapped_pages access */
};

/* Helper: Find cached mapping for physical page */
static void* find_cached_mapping(struct wuwa_bindproc_private* priv, uintptr_t phys_page) {
    size_t i;
    for (i = 0; i < priv->mapped_pages->size; i++) {
        struct wuwa_mapped_page_info* info = arraylist_get(priv->mapped_pages, i);
        if (info && info->phys_addr == phys_page) {
            return info->mapped_ptr;
        }
    }
    return NULL;
}

/* Helper: Add new mapping to cache */
static int add_cached_mapping(struct wuwa_bindproc_private* priv, uintptr_t phys_page, void* mapped) {
    struct wuwa_mapped_page_info* info = kmalloc(sizeof(*info), GFP_KERNEL);
    if (!info) {
        return -ENOMEM;
    }
    info->phys_addr = phys_page;
    info->mapped_ptr = mapped;

    if (arraylist_add(priv->mapped_pages, info) < 0) {
        kfree(info);
        return -ENOMEM;
    }
    return 0;
}

static ssize_t bindproc_read(struct file* f, char __user* dest_va, size_t size, loff_t* src_va) { return -EINVAL; }

static ssize_t bindproc_write(struct file* f, const char __user* data, size_t size, loff_t* offset) { return -EINVAL; }

struct bp_read_memory_cmd {
    uintptr_t src_va; /* Input: Virtual address to read from */
    uintptr_t dst_va; /* Input: Virtual address to write to */
    size_t size; /* Input: Size of memory to read */
};

struct bp_write_memory_cmd {
    uintptr_t src_va; /* Input: Virtual address to read from */
    uintptr_t dst_va; /* Input: Virtual address to write to */
    size_t size; /* Input: Size of memory to write */
};

struct bp_memory_iov_cmd {
    struct iovec __user* local_iov;
    unsigned long local_iovcnt;
    struct iovec __user* remote_iov;
    unsigned long remote_iovcnt;
    size_t bytes_done;
};

#define WUWA_BP_IOCTL_SET_MEMORY_PROT _IOWR('B', 1, int) /* arg: int (WMT_*) */
#define WUWA_BP_IOCTL_READ_MEMORY _IOWR('B', 2, struct bp_read_memory_cmd)
#define WUWA_BP_IOCTL_WRITE_MEMORY _IOWR('B', 3, struct bp_write_memory_cmd)
#define WUWA_BP_IOCTL_READV_MEMORY _IOWR('B', 4, struct bp_memory_iov_cmd)
#define WUWA_BP_IOCTL_WRITEV_MEMORY _IOWR('B', 5, struct bp_memory_iov_cmd)

#define WUWA_BP_IOV_MAX 1024UL
#define WUWA_BP_MAX_RW_SIZE (PAGE_SIZE * 16)

static int bindproc_transfer_memory(struct wuwa_bindproc_private* private_data, uintptr_t local_va,
                                    uintptr_t remote_va, size_t size, bool write) {
    uintptr_t offset, page_start;
    size_t bytes_to_copy, copied = 0;
    int ret = 0;

    if (size == 0 || size > WUWA_BP_MAX_RW_SIZE || !local_va || !remote_va) {
        return -EINVAL;
    }

    while (copied < size) {
        uintptr_t pa;
        void* mapped;

        ret = translate_process_vaddr(private_data->pid, remote_va + copied, &pa);
        if (ret < 0) {
            wuwa_err("failed to translate VA 0x%lx: %d\n", remote_va + copied, ret);
            return ret;
        }

        offset = pa & ~PAGE_MASK;
        page_start = pa & PAGE_MASK;
        bytes_to_copy = min_t(size_t, size - copied, PAGE_SIZE - offset);

        mapped = find_cached_mapping(private_data, page_start);
        if (!mapped) {
            mapped = wuwa_ioremap_prot(page_start, PAGE_SIZE, private_data->prot);
            if (!mapped) {
                wuwa_err("failed to ioremap physical address 0x%lx\n", page_start);
                return -ENOMEM;
            }

            ret = add_cached_mapping(private_data, page_start, mapped);
            if (ret < 0) {
                wuwa_err("failed to cache mapping for PA 0x%lx\n", page_start);
                iounmap(mapped);
                return ret;
            }
        }

        if (write) {
            ret = copy_from_user(mapped + offset, (void __user*)(local_va + copied), bytes_to_copy);
        } else {
            ret = copy_to_user((void __user*)(local_va + copied), mapped + offset, bytes_to_copy);
        }

        if (ret != 0) {
            return -EFAULT;
        }

        copied += bytes_to_copy;
    }

    return copied;
}

static int bindproc_copy_iovec(struct iovec __user* iovecs, unsigned long index, struct iovec* iov) {
    if (copy_from_user(iov, &iovecs[index], sizeof(*iov))) {
        return -EFAULT;
    }

    if (iov->iov_len > 0 && !iov->iov_base) {
        return -EINVAL;
    }

    return 0;
}

static int bindproc_transfer_iov(struct wuwa_bindproc_private* private_data, struct bp_memory_iov_cmd* cmd,
                                 bool write) {
    unsigned long local_index = 0, remote_index = 0;
    size_t local_offset = 0, remote_offset = 0;

    if (!cmd->local_iov || !cmd->remote_iov || cmd->local_iovcnt == 0 || cmd->remote_iovcnt == 0) {
        return -EINVAL;
    }

    if (cmd->local_iovcnt > WUWA_BP_IOV_MAX || cmd->remote_iovcnt > WUWA_BP_IOV_MAX) {
        return -EINVAL;
    }

    while (local_index < cmd->local_iovcnt && remote_index < cmd->remote_iovcnt) {
        struct iovec local_iov, remote_iov;
        uintptr_t local_base, remote_base;
        size_t chunk;
        int ret;

        ret = bindproc_copy_iovec(cmd->local_iov, local_index, &local_iov);
        if (ret < 0) {
            return cmd->bytes_done ? 0 : ret;
        }

        ret = bindproc_copy_iovec(cmd->remote_iov, remote_index, &remote_iov);
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
        chunk = min_t(size_t, chunk, WUWA_BP_MAX_RW_SIZE);

        ret = bindproc_transfer_memory(private_data, local_base, remote_base, chunk, write);
        if (ret < 0) {
            return cmd->bytes_done ? 0 : ret;
        }

        cmd->bytes_done += chunk;
        local_offset += chunk;
        remote_offset += chunk;
    }

    return 0;
}

static long bindproc_ioctl(struct file* f, unsigned int cmd, unsigned long arg) {
    struct wuwa_bindproc_private* private_data = f->private_data;
    int ret = 0;
    if (!private_data) {
        return -EINVAL;
    }

    switch (cmd) {
    case WUWA_BP_IOCTL_SET_MEMORY_PROT:
        {
            int prot;
            pgprot_t new_prot;

            if (copy_from_user(&prot, (int __user*)arg, sizeof(prot))) {
                return -EFAULT;
            }

            if (prot < WMT_NORMAL || prot > WMT_NORMAL_iNC_oWB) {
                return -EINVAL;
            }

            if (convert_wmt_to_pgprot(prot, &new_prot) < 0) {
                return -EINVAL;
            }

            if(pgprot_val(new_prot) != pgprot_val(private_data->prot)) {
                /* Clear existing mappings if protection changes */
                size_t i;

                mutex_lock(&private_data->lock);

                for (i = 0; i < private_data->mapped_pages->size; i++) {
                    struct wuwa_mapped_page_info* info = arraylist_get(private_data->mapped_pages, i);
                    if (info) {
                        if (info->mapped_ptr) {
                            iounmap(info->mapped_ptr);
                        }
                        kfree(info);
                    }
                }
                arraylist_clear(private_data->mapped_pages);

                mutex_unlock(&private_data->lock);
            }

            private_data->prot = new_prot;
            wuwa_info("set memory prot to %d for pid %d\n", prot, private_data->pid);
            return ret;
        }
    case WUWA_BP_IOCTL_READ_MEMORY:
        {
            struct bp_read_memory_cmd cmd;
            if (copy_from_user(&cmd, (struct bp_read_memory_cmd __user*)arg, sizeof(cmd))) {
                return -EFAULT;
            }
            return bindproc_transfer_memory(private_data, cmd.dst_va, cmd.src_va, cmd.size, false);
        }
    case WUWA_BP_IOCTL_WRITE_MEMORY:
        {
            struct bp_write_memory_cmd cmd;
            if (copy_from_user(&cmd, (struct bp_write_memory_cmd __user*)arg, sizeof(cmd))) {
                return -EFAULT;
            }
            return bindproc_transfer_memory(private_data, cmd.src_va, cmd.dst_va, cmd.size, true);
        }
    case WUWA_BP_IOCTL_READV_MEMORY:
    case WUWA_BP_IOCTL_WRITEV_MEMORY:
        {
            struct bp_memory_iov_cmd iov_cmd;
            if (copy_from_user(&iov_cmd, (struct bp_memory_iov_cmd __user*)arg, sizeof(iov_cmd))) {
                return -EFAULT;
            }

            iov_cmd.bytes_done = 0;
            ret = bindproc_transfer_iov(private_data, &iov_cmd, cmd == WUWA_BP_IOCTL_WRITEV_MEMORY);
            if (copy_to_user((void __user*)arg, &iov_cmd, sizeof(iov_cmd))) {
                return -EFAULT;
            }

            return ret;
        }
    default:
        wuwa_err("unknown ioctl cmd: 0x%x\n", cmd);
        return -ENOTTY;
    }

    return -EINVAL;
}

static int bindproc_mmap(struct file* f, struct vm_area_struct* vma) { return -EINVAL; }

static int bindproc_release(struct inode* inode, struct file* f) {
    struct wuwa_bindproc_private* private_data = f->private_data;
    if (private_data) {
        size_t i;

        mutex_lock(&private_data->lock);

        /* Unmap all cached pages */
        for (i = 0; i < private_data->mapped_pages->size; i++) {
            struct wuwa_mapped_page_info* info = arraylist_get(private_data->mapped_pages, i);
            if (info) {
                if (info->mapped_ptr) {
                    iounmap(info->mapped_ptr);
                }
                kfree(info);
            }
        }

        arraylist_destroy(private_data->mapped_pages);

        mutex_unlock(&private_data->lock);
        mutex_destroy(&private_data->lock);

        kfree(private_data);
        f->private_data = NULL;
    }
    return 0;
}

loff_t bindproc_llseek(struct file* file, loff_t offset, int whence) {
    switch (whence) {
    case SEEK_SET:
        if (offset < 0)
            return -EINVAL;
        file->f_pos = offset;
        break;
    case SEEK_CUR:
        if (file->f_pos + offset < 0)
            return -EINVAL;
        file->f_pos += offset;
        break;
    case SEEK_END:
        return -EINVAL; /* Not supported */
    default:
        return -EINVAL;
    }
    return file->f_pos;
}

static const struct file_operations bindproc_fops = {
    .owner = THIS_MODULE,
    .release = bindproc_release,
    .read = bindproc_read,
    .write = bindproc_write,
    .unlocked_ioctl = bindproc_ioctl,
    .mmap = bindproc_mmap,
    .llseek = bindproc_llseek,
};


int do_bind_proc(struct socket* sock, void __user* arg) {
    struct wuwa_bind_proc_cmd cmd;
    struct wuwa_bindproc_private* private_data = NULL;
    struct file* filp = NULL;
    int fd = -1;
    int ret = 0;

    /* Copy command from userspace */
    if (copy_from_user(&cmd, arg, sizeof(cmd))) {
        return -EFAULT;
    }

    /* Validate PID */
    if (cmd.pid <= 0) {
        wuwa_err("invalid pid: %d\n", cmd.pid);
        return -EINVAL;
    }

    /* Verify target process exists */
    struct task_struct* task = get_target_task(cmd.pid);
    if (!task) {
        wuwa_err("failed to find task for pid: %d\n", cmd.pid);
        return -ESRCH;
    }
    put_task_struct(task);

    /* Allocate private data structure */
    private_data = kmalloc(sizeof(*private_data), GFP_KERNEL);
    if (!private_data) {
        wuwa_err("failed to allocate memory for private_data\n");
        return -ENOMEM;
    }

    private_data->pid = cmd.pid;
    mutex_init(&private_data->lock);
    private_data->prot = __pgprot(PROT_NORMAL); /* Default to normal memory */

    private_data->mapped_pages = arraylist_create(16);
    if (!private_data->mapped_pages) {
        wuwa_err("failed to create mapped_pages arraylist\n");
        ret = -ENOMEM;
        goto err_free_private;
    }

    /* Allocate file descriptor */
    fd = get_unused_fd_flags(O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        wuwa_err("failed to get unused fd: %d\n", fd);
        ret = fd;
        goto err_free_private;
    }

    /* Create anonymous inode file */
    filp = anon_inode_getfile("[wuwa_bindproc]", &bindproc_fops, private_data, O_RDWR | O_CLOEXEC);
    if (IS_ERR(filp)) {
        wuwa_err("failed to create anon inode file: %ld\n", PTR_ERR(filp));
        ret = PTR_ERR(filp);
        goto err_put_fd;
    }

    /* Copy result back to userspace before installing fd */
    cmd.fd = fd;
    if (copy_to_user(arg, &cmd, sizeof(cmd))) {
        wuwa_err("failed to copy cmd back to user\n");
        ret = -EFAULT;
        goto err_fput;
    }

    /* Install fd only after successful copy_to_user */
    fd_install(fd, filp);

    return 0;

err_fput:
    /* File not yet installed, safe to fput (releases private_data via bindproc_release) */
    fput(filp);
    /* Fall through to put_unused_fd */
err_put_fd:
    put_unused_fd(fd);
    return ret;

err_free_private:
    /* Only reached if file creation failed */
    mutex_destroy(&private_data->lock);
    kfree(private_data);
    return ret;
}
