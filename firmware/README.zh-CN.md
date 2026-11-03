# 固件

[English](README.md) | [简体中文](README.zh-CN.md)

`focaltech_ts_fw_xinli.bin` 是 merlinx 面板对应的 FT8719 应用镜像。

| 项目 | 值 |
| --- | --- |
| 大小 | 131072 字节（128 KiB） |
| SHA-256 | `05296053fd79eb961da2ecfc3b0ff88b93a7fc431f71065d8584b449bcb7a30a` |
| 布局 | 两个 64 KiB 应用槽 |
| 槽 A | 头部在 `0x100`，代码长度 `0xF124` |
| 槽 B | 头部在 `0x10100`，代码长度 `0xE504` |

该文件取自设备本身的 `/vendor/firmware/focaltech_ts_fw_xinli.bin`，也就是
vendor 内核驱动为该面板请求的路径。

## 为什么需要它

merlinx 上的 FT8719 不从片上 flash 运行应用。每次复位后都必须由主机把应用写进
控制器的程序 RAM，用 ROM 的 ECC 命令校验，然后再启动。这是 vendor 驱动对该型号
唯一的升级路径，而且每次开机都会执行。

没有这个镜像时控制器会停在 ROM 里。注意这颗芯片在 ROM 模式和应用模式下都上报
`0x8719`，所以单靠识别读并不能证明应用已经在运行。

## 格式

每个 64 KiB 槽在偏移 `0x100` 处是标准的 FocalTech 应用头：

| 偏移 | 含义 |
| --- | --- |
| `0x100` | 代码长度，大端 |
| `0x102` | 代码长度的按位取反 |
| `0x108` | 常量块长度 |
| `0x10A` | 常量块长度的按位取反 |

两个槽的长度对相加都是 `0xFFFF`。驱动只读取槽 A，校验长度对，从偏移 `0` 开始
写入正好 `code_len` 个字节到 PRAM，忽略槽 B。这与 vendor 驱动一致：它同样从文件
开头写 `code_len` 字节，从不碰第二个槽。

保留槽 B 是因为它本来就属于设备发布的镜像；删掉反而会让文件和已知良好副本更难
比对。

## 板级如何接入

本仓库不会把 blob 链接进任何东西。探针通过板级仓库提供的两个弱符号取得它：

```c
extern const uint8_t  fts_board_firmware[];
extern const uint32_t fts_board_firmware_size;
```

在板级构建时从这个文件生成，例如：

```sh
xxd -i -n fts_board_firmware firmware/focaltech_ts_fw_xinli.bin \
    > build/fts_firmware_blob.c
```

并在同一个编译单元里把 `fts_board_firmware_size` 定义为
`sizeof(fts_board_firmware)`。生成的 `.c` 放在板级构建目录，不要放进本仓库。

## 许可

该镜像是 FocalTech/ODM 的专有固件，**不**受 `LICENSE` 中 MIT 许可覆盖，本项目
也不授予任何再分发权利。它在这里只是为了方便 merlinx 机主拿到与面板和驱动匹配
的确切镜像。如果要在没有授权的情况下公开再分发源码，请删除它；只要板级提供等价
镜像，驱动本身并不依赖这个文件。
