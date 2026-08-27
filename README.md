ccache – a fast compiler cache
==============================

[![Build Status](https://travis-ci.org/ccache/ccache.svg?branch=master)](https://travis-ci.org/ccache/ccache)
[![Code Quality: Cpp](https://img.shields.io/lgtm/grade/cpp/g/ccache/ccache.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/ccache/ccache/context:cpp)
[![Total Alerts](https://img.shields.io/lgtm/alerts/g/ccache/ccache.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/ccache/ccache/alerts)

ccache is a compiler cache. It speeds up recompilation by caching the result of
previous compilations and detecting when the same compilation is being done
again. Supported languages are C, C++, Objective-C and Objective-C++.


关于本分支（3.7-maint-redis）
-----------------------------

本分支是 ccache 3.7 维护分支（基于 v3.7.12）的特殊定制版本：在保持 3.7
纯 C 实现的前提下，将 ccache 4.x 的 **Redis 远程存储**特性移植到 3.7，
供无法构建 4.x（需要 C++11 编译器）的老平台使用。已在 AIX 5.1 /
GCC 4.5.4 与 Solaris 10 x86 / GCC 4.9 实机完成全功能验证。

### 新增功能

* **remote_storage**（配置项 / 环境变量 `CCACHE_REMOTE_STORAGE`）：指定
  Redis 服务器作为远程（二级）存储。行为与 4.x 一致：本地缓存优先，
  未命中时查询远端并回填本地；编译结果先写本地再写远端；远端不可达时
  编译不受影响（仅计入 `remote storage errors` 统计）；`ccache -C` 只清
  本地缓存，不触碰远端。
* **remote_only**（`CCACHE_REMOTE_ONLY`）：只使用远端存储，本地缓存目录
  仅作为瞬态工作区（与 4.x 语义一致：不读、不删本地既有条目）。
* 新增统计计数器：`remote_storage_hit` / `remote_storage_miss` /
  `remote_storage_error`（`ccache -s` / `--print-stats` 可见）。

URL 语法与 4.x 相同：

    redis://[[USERNAME:]PASSWORD@]HOST[:PORT][/DBNUMBER]
    redis+unix:SOCKET_PATH[?db=DBNUMBER]

详细说明见 doc/MANUAL.adoc 中的 remote_storage / remote_only 条目。

### 与 ccache 4.x 的兼容性

* **兼容**：配置语法与用法、`ccache --version` 的 `Features:` 输出、
  Redis key 布局风格（`ccache:<hash>`，每条目 2 个 key：结果 + manifest）、
  存储交互语义（本地优先/远端回填/故障降级）与统计计数行为。
* **不兼容**：缓存条目本身——3.x 与 4.x 的哈希算法（MD4 vs BLAKE3）和
  条目序列化格式不同，条目互不命中。两个版本可以共用同一个 Redis
  数据库而互不干扰。

### 实现说明

* 内置自包含的 RESP2 协议客户端（`src/redis.c`），零外部依赖（不使用
  hiredis），仅使用 POSIX socket API，便于在老平台直接构建。
* 缓存条目以自定义 bundle 格式（magic `cCE1`）打包 3.7 的多文件条目
  （.o/.stderr/.d/.gcno/.su/.dia/.dwo）为单个 Redis key；manifest 单独
  一个 key。
* 兼容 GCC 4.5.4（C99，无 C11 特性），并包含 AIX 5.1 老 libc 的移植性
  修复：`stdint.h` fallback、`x_strndup` 自带实现（AIX 的 `strndup` 可能
  不写 NUL 终止）、手动十进制解析（GCC 4.5 的 builtin `strtoul` 不写
  endptr）、`round()` fallback。
* 远端清理依赖 Redis 侧配置 LRU 驱逐（与 4.x 相同，ccache 的本地缓存
  清理绝不触碰远端条目）。

### 主要变更

| 文件 | 说明 |
|---|---|
| `src/redis.c`、`src/redis.h` | 新增：RESP2 客户端、URL 解析、bundle 编解码 |
| `src/ccache.c` | 读写路径集成（from_cache/to_cache/manifest）、remote_only 逻辑 |
| `src/conf.c`、`src/conf.h`、`src/confitems.gperf`、`src/envtoconfitems.gperf` | remote_storage / remote_only 配置项、凭据脱敏 |
| `src/ccache.h`、`src/stats.c` | remote_storage_hit/miss/error 统计计数器 |
| `src/stdint.h`、`src/util.c`、`src/cleanup.c`、`m4/feature_macros.m4`、`configure.ac` | AIX 5.1 / Solaris 10 移植性修复 |
| `unittest/test_redis.c` | 新增：单元测试（URL 解析、bundle 编解码、脱敏） |
| `test/suites/redis.bash` | 新增：集成测试（需本机 redis-server，无则自动跳过） |
| `doc/MANUAL.adoc`、`doc/NEWS.adoc` | 文档 |

### 测试情况

* 单元测试（`make unittest`）与集成测试全部通过。
* 使用 ccache 自举编译（以本 ccache 包装编译器编译 ccache 自身源码）在
  真实 Redis 环境完成双轮验证：首轮全量 miss 并上传远端，清空本地缓存后
  重编达到 100% 远端命中，产物与直连编译 bit 一致。
* 客户端经过针对畸形/恶意服务器响应的模糊测试（10 种协议违规场景），
  全部场景下编译正常完成、无崩溃。
* **AIX 5.1 实机验证**：编译、本地/远端命中、remote_only、配置读写
  （-p/-k/--set-config）全部正常；与 ccache 3.1.4 既有缓存目录安全共存。
* **AIX 性能实测**（双核 POWER4，自举编译，-j2）：无 ccache 6s；
  本地命中 2s（3×）；远端命中 3s（2×）；remote_only 4s（1.5×）。

### 在 AIX 5.1 上构建

AIX 上只需 GCC 与 GNU make（`gmake`），无需 autoconf/gperf。使用预生成
好 `configure`、gperf lookup 与 `version.c` 的源码包（含 `dev_mode_disabled`
标记），解包后执行：

    CC=gcc ./configure --prefix=/opt/ccache --with-bundled-zlib
    gmake && make install

说明：

* `--with-bundled-zlib` 静态链入捆绑 zlib，产物只依赖基系统
  `libc.a/shr.o`，可拷贝到未安装 Toolbox zlib 的其他 AIX 5.1 直接运行；
  若目标机确定有 `/opt/freeware` 的 zlib 可省略此选项。
* 使用捆绑 zlib 时，AIX 的 `ar` 打包 32 位对象需 `OBJECT_MODE=32`
  （gmake 报 `ar: 0707-126 ... object file mode` 时，执行
  `cd src/zlib && OBJECT_MODE=32 ar cr libz.a *.o` 后继续 gmake）。
* 编译产物与安装位置无关，`--prefix` 仅决定 `make install` 的目的地。


### 在 Solaris 10 x86 上构建

Solaris 上需要 GCC（如 OpenCSW 的 `/opt/csw/bin/gcc`）与 GNU make
（`gmake`），`ar`/`ranlib` 在 `/usr/ccs/bin`（需加入 PATH）：

    PATH=/usr/ccs/bin:/opt/csw/bin:$PATH \
    CC=/opt/csw/bin/gcc ../configure --with-bundled-zlib
    gmake

说明：

* 同样推荐 `--with-bundled-zlib`（Solaris 10 系统自带 zlib 低于 1.2.3）；
  产物只依赖 `/lib` 基系统库（libsocket/libnsl/libm/libc），可直接拷贝
  到未安装 OpenCSW 的 Solaris 10 运行。
* Solaris 10 不支持 `SO_RCVTIMEO`/`SO_SNDTIMEO`、`MSG_NOSIGNAL` 与
  `SO_NOSIGPIPE`，redis 客户端的读写超时与 SIGPIPE 防御分别以 poll
  超时与专用信号处理实现（对其他平台无行为影响）。


General information
-------------------

* [Main web site](https://ccache.dev)
* [Documentation](https://ccache.dev/documentation.html)
  * [Latest manual](https://ccache.dev/manual/latest.html)
  * [Installation from Git source repository](https://github.com/ccache/ccache/blob/master/doc/INSTALL.md)
  * [Installation from release archive](https://github.com/ccache/ccache/blob/master/doc/INSTALL-from-release-archive.md)
* [Release notes](https://ccache.dev/releasenotes.html)
* [Credits and history](https://ccache.dev/credits.html)
* [License and copyright](https://ccache.dev/license.html)


Contributing to ccache
----------------------

* [Source repository](https://github.com/ccache/ccache)
* [Notes on how to contribute](https://github.com/ccache/ccache/blob/master/CONTRIBUTING.md)
* [Mailing list](https://lists.samba.org/mailman/listinfo/ccache/)
* [Bug report info](https://ccache.dev/bugs.html)
* [Issue tracker](https://github.com/ccache/ccache/issues)
  * [Help wanted!](https://github.com/ccache/ccache/labels/help%20wanted)
  * [Good first issues!](https://github.com/ccache/ccache/labels/good%20first%20issue)
