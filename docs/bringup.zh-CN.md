# FT8719 Bring-up 与真机验证

[English](bringup.md) | [简体中文](bringup.zh-CN.md)

本文是 `README.md` 的操作配套文档，说明在动真机之前要检查什么、如何分阶段运行
探针、如何阅读诊断页，以及还有哪些内容必须在硬件上验证。

先说结论：协议层、触摸核心和 HID 快照有主机测试；XNU 探针只做过语法检查；
**本仓库目前还没有在任何一台真实 merlinx 上运行过**。第一次启动请按实验对待。

## 已经验证的内容

| 项目 | 验证方式 | 结果 |
| --- | --- | --- |
| 包格式、CRC、寄存器访问 | `Test/fts-protocol.c` + 假 FT8719 总线 | 通过 |
| Boot 识别、应用识别 | 假总线 | 通过 |
| 固件长度检查、PRAM 写入、ECC、启动 | 假总线 | 通过 |
| 报告解码、恢复标记 | 假总线 | 通过 |
| 触点生命周期、掩码、释放、映射 | `Test/touch-core.c` | 通过 |
| seqlock 快照 | `Test/fts-hid.c` | 通过 |
| `FtsProbe.cpp` 的 XNU API 使用 | `clang++ -fsyntax-only` + macOS `Kernel.framework` 头文件 | 无错误、无警告 |
| 真实 SPI 引擎、GPIO、IRQ、面板 | **未运行** | 待验证 |

运行主机测试：

```sh
./Test/run.sh
```

预期输出：

```
fts-protocol: ok
touch-core: ok
fts-hid: ok
```

## 第一次启动之前

1. **确认板子上真的是 FT8719。** merlinx 设备树复用了 Novatek 命名的 SPI
   节点，并在同一节点里列出了 `focaltech,fts`。不要因为节点名就假设它选择
   了哪颗控制器。
2. **先确认板级 SPI 引擎自己能工作。** 先跑板级的 loopback 或只读事务测试。
   本仓库不包含 MediaTek SPI 驱动。
3. **确认复位极性和 IRQ 线。** `FtsBoard.h` 写的是复位 GPIO 92 低有效、IRQ
   GPIO 1、flags `0x2001`。请按具体板子版本的设备树再核对一次。
4. **决定是否允许写固件。** 仓库不包含镜像。如果板级没有提供
   `fts_board_firmware`，stage 2 无法恢复卡在 bootloader 里的控制器。
5. **保持探针默认关闭。** 在 stage 1 通过之前，不要把 `fts-touch=` 加进默认
   启动参数。

## 启动参数

| 参数 | 作用 |
| --- | --- |
| `fts-touch=1` | 复位、识别控制器，然后停止。只读。 |
| `fts-touch=2` | 型号不是 `0x8719` 时下载固件，然后读取版本。 |
| `fts-touch=3` | 同时启动帧循环并发布触摸。 |
| `fts-touch-poll=1` | 不使用 `fts_board_irq_wait`，改为轮询。 |
| `fts-touch-swap=1` | 覆盖板级坐标轴交换。 |
| `fts-touch-invert-x=1` | 覆盖 X 轴反转。 |
| `fts-touch-invert-y=1` | 覆盖 Y 轴反转。 |

板级在总线、复位线和中断都可用之后调用 `fts_touch_probe(board)` 启动探针。
该调用会创建内核线程并立即返回。

## Stage 1：只识别

先使用：

```
fts-touch=1
```

预期诊断页：

```
FTS stage 1/1 step 1 err 0 id8719 ver00
FTS fw1 tx0 crc0 poll0 last0 rc0
FTS frames0 reads0 idle0 rec0 bad0 c0
FTS last -1,-1 rel0 dup0 range0 drop0
```

字段含义：

- `stage <done>/<requested>`：阶段进度。`stage 1/1` 表示 stage 1 完成。
- `step <n>`：正在执行或失败的步骤。`step 1` 是总线/chip 初始化和识别。
- `err <n>`：第一个错误，成功时为 `0`。负值采用 errno 风格。
- `id<hhhh>`：识别出的型号。`8719` 表示应用固件正在运行。`87a9` 和 `87b9`
  是 ROM/PB 别名，表示“当前在 bootloader”。
- `ver<hh>`：固件版本，在 stage 2 读取。
- `fw<n>`：framing。`1` 表示命令带两字节长度字段。
- `tx/crc/poll/last/rc`：传输错误数、CRC 错误数、最后一次空闲轮询次数、
  最后一次协议错误、最后一次报告分类。

如果 stage 1 返回 `id87a9` 或 `id87b9`，说明控制器活着，但应用固件没有运行。
不要继续 stage 3；只有在明确要使用板级镜像时才进入 stage 2。

### Stage 1 失败提示

| 现象 | 可能原因 |
| --- | --- |
| `step 1` 处 `err -5`，`tx` 增长 | SPI 传输失败：时钟、pinmux、片选，或板级回调返回错误 |
| `err -110` | 状态轮询始终不到空闲，或识别始终无响应：SPI 模式错误、复位极性错误，或控制器一直被按在复位 |
| `err -19`（`FTS_ERR_CHIP`） | 总线通了，但 id 不是 FT8719 别名：面板节点错误、片选错误，或换了控制器 |
| `err -74` 且 `crc` 增长 | 能读到数据但 CRC 不匹配：读长度字段错误、首字节延迟错误，或信号完整性有问题 |
| `err -22` | 缓冲区或参数错误；检查 `kBusBytes` 和板级传输长度 |

## Stage 2：固件与版本

只有在板级提供镜像、并且镜像是该面板的已知良好版本时才运行：

```
fts-touch=2
```

只有识别出的型号不是 `0x8719` 时才会下载。流程采用 vendor 针对该型号的 RAM
下载路径：进入 bootloader，用 `0xAE` 地址/长度命令按 `4K-4` 分块写 PRAM，
让 bootloader 计算 ECC，与主机 ECC 比较，然后用 `0x08` 启动应用。

下载成功后的预期结果：

```
FTS stage 2/2 step 2 err 0 id8719 ver42
```

注意事项：

- 下载失败可能让控制器停在 bootloader。请确保固件镜像和板级恢复路径都可用。
- 不要使用其他面板或其他 FT8719 供应商的镜像。
- 本仓库刻意不内嵌、不下载、不生成固件。
- 驱动使用保守的慢速包大小，不执行 vendor 的 fast-download 协商。

## Stage 3：帧循环

只有在 stage 1 和 stage 2 都通过后才使用：

```
fts-touch=3
```

预期行为：

- 触摸面板时 `FTS frames<n>` 持续增长。
- `reads` 是触摸报告数，`idle` 是空轮询数，`rec` 是恢复次数，`bad` 是协议
  错误数。
- `c<n>` 是最后一次发布报告里的条目数，包含抬起条目。
- `last x,y` 跟随第一个上报触点。
- `rel/dup/range/drop` 是触摸核心的诊断计数。正常使用时应保持为 0。`rel`
  表示某个活动触点在完整报告里消失；`dup` 表示同一硬件 id 在一帧里重复；
  `range` 表示 id 大于 9；`drop` 表示无效事件或对非活动 id 的抬起。

如果怀疑中断路径有问题，用 `fts-touch-poll=1` 对比。轮询更慢、更耗 CPU，但
可以把 IRQ 接线问题从 SPI 路径里隔离出来。

## 真机验证清单

逐项记录结果。只有全部在真实 merlinx 上通过，才能把驱动标记为已验证。

- [ ] Stage 1 识别出 `id8719`。
- [ ] Stage 1 运行数分钟后 `tx`、`crc`、`bad` 仍为 0。
- [ ] Stage 2 读到合理的固件版本（`ver` 不是 `0x00`/`0xFF`）。
- [ ] Stage 3 单指上报位置符合预期。
- [ ] 第二根手指产生第二个触点，且硬件 id 不同。
- [ ] 十指（或面板真实上限）都能上报，没有 id 丢失。
- [ ] 抬起报告正常到达；松手后没有触点卡住。
- [ ] 拖动时坐标连续，没有轴交换或反转。
- [ ] 左上角和右下角上报的坐标与面板方向一致；只能根据照片或实际角点测试
      调整 `FtsBoard.h`。
- [ ] 正常使用时 `rel`、`dup`、`range`、`drop` 保持为 0。
- [ ] `suspend` 释放所有触点，`resume` 后无需重启即可恢复触摸。
- [ ] 长时间空闲（30 分钟以上）不会出现恢复风暴。
- [ ] 人为制造总线错误或复位后，帧循环能恢复。
- [ ] 诊断页始终可读，且不会拖住触摸线程。

## 失败信息阅读

`fts_touch_error` 保留第一个错误，`fts_touch_step` 保留失败的步骤。步骤编号：

| 步骤 | 含义 |
| --- | --- |
| 0 | Session 分配 |
| 1 | 缓冲区分配、总线初始化、识别 |
| 2 | 固件下载、版本读取、触摸核心配置 |
| 3 | 帧循环启动 |

错误值：

| 值 | 名称 | 含义 |
| --- | --- | --- |
| `0` | `FTS_OK` | 成功 |
| `-5` | `FTS_ERR_BUS` | 板级传输回调失败 |
| `-19` | `FTS_ERR_CHIP` | id 不是 FT8719 别名 |
| `-22` | `FTS_ERR_PARAM` | 参数或缓冲区大小错误 |
| `-71` | `FTS_ERR_STATE` | 报告或状态不一致 |
| `-74` | `FTS_ERR_CRC` | 读 CRC 不匹配 |
| `-110` | `FTS_ERR_TIMEOUT` | 状态轮询或识别超时 |
| `-2` | no firmware | stage 2 需要镜像，但板级没有提供 |

## Suspend 与 resume

`fts_touch_suspend()` 和 `fts_touch_resume()` 只在 stage 3 启动后有效；之前调用
是 no-op。suspend 会先置标志，等帧线程释放所有触点并回 ack，然后写
`0xA5=0x03`。resume 先等控制器重新响应识别读；只有失败时才写 `0xA5=0x00`，
再失败才复位。这里遵循 vendor 的恢复顺序，而不是猜一个顺序。

不要在中断上下文调用。固件下载进行中不要调用 suspend。

## 已知限制

- 探针是 bring-up 与验证驱动，不是完整的 HID 驱动。板级 HID 源读取 seqlock
  快照，并自己拥有 report descriptor。
- 只支持 FT8719。ROM/PB 别名只用于识别，不能替代应用固件。
- 驱动始终使用保守的固件包大小，不协商 fast download。
- 没有 ESD workqueue，也没有手势、手套、皮套、充电器或接近感应处理；这些
  vendor 功能不在本仓库范围内。
- `FtsProbe.cpp` 尚未链接进真实 XNU 构建。语法检查使用的是 macOS
  `Kernel.framework` 头文件，具体板级 XNU 树仍可能暴露不同的 API。

## 回滚

删除 `fts-touch=` 启动参数即可。没有该参数时，`fts_touch_probe()` 会直接返回，
不创建线程，也完全不碰硬件。即使驱动已经编进内核，默认也是惰性的。

## 如何记录验证结果

记录真机运行时，请包含：

- 板子版本和面板型号；
- XNU commit 和板级 SPI 驱动 commit；
- 完整的启动参数；
- stage 1、stage 2，以及 stage 3 运行十分钟后的四行诊断信息；
- 本次运行使用 IRQ 还是轮询；
- 用于确认方向的照片或角点测试坐标；
- 板级传输相关的内核日志。

不要只凭主机测试就报告“可用”。
