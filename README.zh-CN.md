# UniXNU FT8719 FocalTech 触摸驱动

[English](README.md) | [简体中文](README.zh-CN.md)

一个 MIT 许可、与具体板级实现解耦的 FocalTech FT8719 触摸控制器主机驱动。
它面向 XNU，并已按 merlinx（Redmi Note 9 4G，MediaTek MT6768）接入。

仓库分为三个可独立测试的层：

| 层 | 文件 | 依赖 |
| --- | --- | --- |
| 线协议 | `FtsProtocol.h`、`FtsProtocol.c` | 仅 C99，不依赖 XNU |
| 触摸核心 | `FtsTouchCore.h`、`FtsTouchCore.c` | 仅 C99，不依赖 XNU |
| HID 快照 | `FtsHid.h`、`FtsHid.c` | 仅 C99，不依赖 XNU |
| 板级事实 | `FtsBoard.h` | merlinx 设备树取值 |
| XNU 探针 | `FtsProbe.cpp` | XNU 内核头文件 |

协议层和触摸核心层通过假总线在主机上测试。XNU 探针刻意保持很薄：它不包含
MediaTek SPI 引擎、GPIO 驱动或时钟驱动，这些由板级仓库通过弱符号提供。

主机需要下发的面板固件放在 `firmware/`，该文件是专有二进制，不受 MIT 许可覆盖。
来源与边界见 `NOTICE`。

## 状态

- 协议层、触摸核心、HID 快照：已有主机测试覆盖。
- XNU 探针：已用 macOS `Kernel.framework` 头文件做语法检查，但尚未链接进真实
  XNU 构建。它需要在 XNU 源码树或板级 kext 构建中编译。
- 硬件：本仓库尚未在真机上验证。Bring-up 分阶段进行，默认关闭。

## 文档

- [`docs/bringup.zh-CN.md`](docs/bringup.zh-CN.md) - 安全规则、分阶段探针、
  诊断页、故障排查和真机验证清单。
- [`docs/bringup.md`](docs/bringup.md) - English version.

## 协议概要

FT8719 通过 SPI 使用一种包协议。一个片选周期只承载一个包：

| 包 | 首字节 | 负载 |
| --- | --- | --- |
| 状态 | `0x05` | 一个状态字节 |
| 命令 | `0xC0` | 控制字节，随后是命令字节 |
| 数据 | `0x3F` | 负载；读操作末尾还有两字节 CRC |

控制字节写为 `0x00`，带数据 CRC 的读为 `0xA0`。其低四位是命令长度。应用固件
运行后，读写命令都会追加两字节大端数据长度。每次传输的首字节之后需要 30 µs
间隔；每个包发送前都必须通过 `0x05` 状态轮询确认空闲。

读数据使用 CRC-16/Kermit，反射多项式 `0x8408`，初值 `0xFFFF`（`123456789`
的校验值为 `0x6F91`）。

一次触摸报告由状态字节、触点数量和最多十条六字节记录组成。驱动一次读取 90
字节，因为 vendor 在同一次传输里还会读取 28 字节手势尾部；CRC 覆盖全部 90
字节。只有前 62 字节是触摸数据。

## 总线契约

`FtsProtocol` 不关心 SPI 如何实现，只调用：

```c
struct fts_bus {
    void *ctx;
    int  (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx,
                 size_t len, unsigned first_delay_us);
    void (*delay_us)(void *ctx, unsigned us);
    void (*delay_ms)(void *ctx, unsigned ms);
    void (*reset)(void *ctx, int asserted);
};
```

`xfer` 表示一个片选周期：先发送 `tx[0]`，等待 `first_delay_us`，再发送
`tx[1..len)`。写操作时 `rx` 可以为 `NULL`。缓冲区由板级层拥有，并且板级层
负责串行化对 chip 对象的访问。协议层同步调用 `xfer`，绝不会在中断里调用。

Vendor 驱动里的 `CD_PACKAGE_BUFLEN` 前缀只是主机缓冲区布局的产物；真正上线
的只有 `tx[0]` 和前缀之后的负载，因此本契约与线上行为等价。

## 板级接入

板级仓库需要提供以下弱符号：

```c
int  fts_board_spi_transfer(void *context, const uint8_t *tx, uint8_t *rx,
                            size_t len, unsigned first_delay_us);
void fts_board_delay_us(void *context, unsigned us);
void fts_board_delay_ms(void *context, unsigned ms);
void fts_board_reset(void *context, int asserted);   // 1 = 复位拉低
int  fts_board_irq_wait(void *context, unsigned timeout_ms);
```

`fts_board_irq_wait` 在有一帧就绪时返回 `0`，超时返回 `FTS_ERR_TIMEOUT`，板级
错误返回其他负值。它是可选的；不提供时探针会轮询。

板级还需要提供固件镜像：

```c
extern const uint8_t  fts_board_firmware[];
extern const uint32_t fts_board_firmware_size;
```

`firmware/focaltech_ts_fw_xinli.bin` 就是 merlinx 的镜像。它不会被自动链接：
板级在构建时把它转成 C 数组并定义这两个符号。没有镜像时仍可识别控制器，但这颗
芯片从程序 RAM 运行，冷启动不下载应用就会停住。详见
[`firmware/README.zh-CN.md`](firmware/README.zh-CN.md)。

入口函数：

```c
int fts_touch_probe(void *board);   // 总线可用后调用一次
int fts_touch_suspend(void);        // 从线程调用，仅 stage 3
int fts_touch_resume(void);
```

`fts_touch_probe` 启动一个内核线程后立即返回。只有存在 `fts-touch` 启动参数时
它才会真正工作，因此加入驱动不会改变默认启动行为。

### 诊断页

板级启动日志可以读取：

```c
char fts_touch_rows[4][120];
volatile uint32_t fts_touch_stage_req, fts_touch_stage_done;
volatile uint32_t fts_touch_step, fts_touch_frames, fts_touch_reads;
volatile uint32_t fts_touch_idle, fts_touch_recover, fts_touch_bad;
volatile uint32_t fts_touch_contacts, fts_touch_model, fts_touch_version;
volatile int fts_touch_error;
volatile int fts_touch_last_x, fts_touch_last_y;
```

## 启动参数

| 参数 | 含义 |
| --- | --- |
| `fts-touch=1` | 复位并识别控制器，然后停止 |
| `fts-touch=2` | 把 `firmware/` 写入控制器，并读取固件版本 |
| `fts-touch=3` | 同时启动帧循环并发布触摸 |
| `fts-touch-poll=1` | 不使用 `fts_board_irq_wait`，改为轮询 |
| `fts-touch-swap=1` | 覆盖板级坐标轴交换 |
| `fts-touch-invert-x=1` | 覆盖 X 轴反转 |
| `fts-touch-invert-y=1` | 覆盖 Y 轴反转 |

Stage 1 是安全的首次启动，只做只读识别。Stage 2 把应用镜像写进控制器的程序
RAM。在这颗芯片上这不是“升级”：控制器不从片上 flash 启动，所以复位之后要靠
stage 2 才能让面板真正可用。

## HID 输出

`FtsHid.c` 通过 seqlock 发布最新触点集合：

```c
extern volatile uint32_t fts_touch_seq;
extern volatile uint32_t fts_touch_count;
extern volatile uint32_t fts_touch_x_max;
extern volatile uint32_t fts_touch_y_max;
extern volatile int32_t  fts_touch_xs[FTS_HID_MAX_POINTS];
extern volatile int32_t  fts_touch_ys[FTS_HID_MAX_POINTS];
extern volatile uint32_t fts_touch_ids[FTS_HID_MAX_POINTS];
extern volatile uint32_t fts_touch_tips[FTS_HID_MAX_POINTS];
```

坐标单位是面板像素。最大值随快照一起发布，因此 HID report descriptor 可以
自行归一化，不需要硬编码面板常量。读取方可以使用 `fts_hid_read()`，也可以
自己实现 seqlock 循环。可以通过 `fts_hid_set_sink()` 注册回调。

## merlinx 板级事实

来自 merlinx 设备树：

- SPI0，片选 0，8 MHz（`spi-max-frequency = <8000000>`）。
- 复位 GPIO 92，低有效。
- 中断 GPIO 1，flags `0x2001`。
- 面板 `1080 x 2340`，10 点触控。
- 默认不做坐标轴交换或反转。

设备树节点名是 Novatek，因为 merlin 的两块面板共用同一个总线节点；同一节点
同时列出了 `focaltech,fts`。具体来源注释见 `FtsBoard.h`。

## 固件

| 项目 | 值 |
| --- | --- |
| 文件 | `firmware/focaltech_ts_fw_xinli.bin` |
| 大小 | 131072 字节，两个 64 KiB 应用槽 |
| SHA-256 | `05296053fd79eb961da2ecfc3b0ff88b93a7fc431f71065d8584b449bcb7a30a` |
| 来源 | 设备上的 `/vendor/firmware/focaltech_ts_fw_xinli.bin` |

驱动只使用槽 A：校验 `0x100` 处的长度对，然后把 `0xF124` 字节写进 PRAM，与
vendor 驱动的行为完全一致。槽 B 属于设备发布的原始镜像，保留它是为了逐字节比对。

这个二进制是专有的，不受 `LICENSE` 覆盖。格式、校验和以及把它变成
`fts_board_firmware[]` 的构建接法见
[`firmware/README.zh-CN.md`](firmware/README.zh-CN.md)。

## 构建与测试

纯逻辑层只需要 C99 编译器：

```sh
./Test/run.sh
```

脚本会编译 `FtsProtocol.c`、`FtsTouchCore.c`、`FtsHid.c` 和 `Test/` 下的测试，
然后输出：

```
fts-protocol: ok
touch-core: ok
fts-hid: ok
```

`FtsProbe.cpp` 由板级 XNU 构建编译，需要把仓库根目录加入 include path。它期望
`FtsBoard.h` 描述当前板子；换板时复制或覆盖该头文件。

## 许可证

MIT，见 `LICENSE`。每个源文件都带有 `SPDX-License-Identifier: MIT`。Vendor
驱动仅用于核对线上行为；GPL 边界和固件政策见 `NOTICE`。
