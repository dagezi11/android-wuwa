[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/fuqiuluo/android-wuwa)

> 加入 OICQ 群：943577597
>
>
>
> 内核驱动开发工具包：[Ylarod/ddk](https://github.com/Ylarod/ddk)

# 功能特性

## 内核保护绕过
- [x] **CFI 绕过** - 自动修补内核 CFI 检查函数，以禁用控制流完整性保护
- [x] **禁用 Kprobe 黑名单** - 清空 kprobe 黑名单，允许 hook 受保护的内核函数（内核 6.1+）

## 地址转换
- [x] **虚拟地址转换** - 通过软件页表遍历，将虚拟地址转换为物理地址
- [x] **硬件地址转换** - 使用 ARM64 AT 指令进行更快、更准确的地址转换
- [x] **PTE 直接映射** - 绕过 VMA，直接在页表中创建映射，支持隐身模式
- [x] **页表遍历** - 遍历完整的进程页表，并输出到 dmesg

## 内存访问
- [x] **物理内存读写** - 通过 phys_to_virt 直接访问，单次操作最高 50MB
- [x] **ioremap 读写** - 支持多种内存类型（Normal/Device/Write-Through 等）

## 进程管理
- [x] **查找进程** - 根据名称定位进程 PID
- [x] **存活检查** - 检查进程是否仍在运行
- [x] **权限提升** - 将当前进程提权为 root
- [x] **获取模块基址** - 查询目标进程中的模块加载地址
- [ ] **隐藏进程** - 设置进程不可见标志
- [x] **隐藏模块** - 从系统中隐藏内核模块

## 高级功能
- [x] **DMA 缓冲区导出** - 将进程内存导出为 dma-buf fd，用于零拷贝共享
- [x] **页面信息查询** - 获取页面 flags/refcount/mapcount 信息
- [x] **调试信息** - 获取 TTBR0/task_struct/mm_struct/pgd 等内核结构信息
- [x] **自定义协议族** - 基于 socket 的用户态通信接口
- [ ] **远程线程创建** - 在目标进程中创建新线程（尚未实现）

# 编译说明

## 方式一：使用 DDK（推荐）

[DDK（内核驱动开发工具包）](https://github.com/Ylarod/ddk) 提供容器化编译环境，并预先配置好内核源码。

### 前置条件

- 已安装并启动 Docker
- 已安装 DDK 工具

### 安装 DDK

```bash
sudo curl -fsSL https://raw.githubusercontent.com/Ylarod/ddk/main/scripts/ddk -o /usr/local/bin/ddk
sudo chmod +x /usr/local/bin/ddk
```

### 使用 DDK 编译

编译脚本支持多个命令和选项（会根据系统语言环境支持中文/英文）：

**命令：**
```bash
./scripts/build-ddk.sh build [target]    # 编译内核模块
./scripts/build-ddk.sh clean [target]    # 清理编译产物
./scripts/build-ddk.sh compdb [target]   # 为 IDE 生成 compile_commands.json
./scripts/build-ddk.sh list              # 列出已安装的 DDK 镜像
```

**编译示例：**
```bash
# 使用默认目标编译（android12-5.10）
./scripts/build-ddk.sh build

# 编译指定目标
./scripts/build-ddk.sh build android14-6.1

# 编译并裁剪调试符号（文件体积更小）
./scripts/build-ddk.sh build -t android14-6.1 --strip

# 清理编译产物
./scripts/build-ddk.sh clean android12-5.10

# 生成 compile_commands.json，方便 IDE 支持
./scripts/build-ddk.sh compdb
```

可用目标：查看 [DDK 容器版本](https://github.com/Ylarod/ddk/pkgs/container/ddk/versions)

**注意**：在部分系统中，Docker 需要 root 权限。如果遇到权限错误，请使用 `sudo` 运行脚本。

## 方式二：从 CI 下载

可以从 GitHub Actions CI 构建中下载预编译的内核模块：

1. 打开 [Actions 页面](../../actions)
2. 选择最新的成功工作流运行记录
3. 下载与你的内核版本匹配的构建产物

## 方式三：手动编译

如果你有自己的 Android 内核源码树：

```bash
# 设置内核源码路径
export KERNEL_SRC=/path/to/android/kernel/source

# 编译模块
make

# 清理编译产物
make clean
```

**注意**：手动编译仅在内核 6.1 上测试过，不保证其他版本可用。

# 如何连接 WuWa 驱动

请阅读 [连接指南](docs/FindDriver.md)。

# 鸣谢

- [Diamorphine](https://github.com/m0nad/Diamorphine)
- [kernel-inline-hook-framework](https://github.com/WeiJiLab/kernel-inline-hook-framework)
