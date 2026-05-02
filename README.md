# WPS WPP FPS Unlock Experiment

WPS Linux 的PPT播放没有实现硬件加速，同时会锁24帧，为了解决这个问题，可以：

1. 用 `LD_PRELOAD` 拦截 WPS 自带 Qt 的 `kso_qt::QElapsedTimer::elapsed()` 和 `restart()`，把返回时间按比例缩小。
2. 再用 `libfaketime` 加速进程时间，让原本 40ms 左右的帧节奏更快触发。

WPS 的 QtCore 内部也会直接调用这些函数，不一定经过动态符号表。所以这个实现还会在进程内对 WPS QtCore 的 `elapsed()`、`restart()`、`nsecsElapsed()` 做 inline patch，确保 QtCore 内部直接调用也会被缩放。

默认参数是 `WPS_FAKE_SPEED=2.0`，并自动设置 `WPS_QELAPSED_SCALE=0.5`。也就是让 libfaketime 把时间加速 2 倍，再把 `QElapsedTimer` 的返回值减半，目标是提高实际刷新频率，同时尽量保持动画速度接近原始速度。

## 构建

```bash
make
make verify
make verify-runtime
make verify-startup-patch
```

## 运行

先退出已有 WPS/WPP 进程，否则 WPS 可能复用已经启动的单实例进程，新的 `LD_PRELOAD` 不会生效。

```bash
bin/wpp-fps-unlock /path/to/demo.pptx
```

## 调参

接近 48fps：

```bash
WPS_FAKE_SPEED=2.0 bin/wpp-fps-unlock demo.pptx
```

接近 60fps：

```bash
WPS_FAKE_SPEED=2.5 bin/wpp-fps-unlock demo.pptx
```

如果动画速度不对，可以手动覆盖缩放比例：

```bash
WPS_FAKE_SPEED=2.5 WPS_QELAPSED_SCALE=0.4 bin/wpp-fps-unlock demo.pptx
```

开启调试输出，确认 hook 被调用：

```bash
WPS_QELAPSED_DEBUG=1 bin/wpp-fps-unlock demo.pptx
```

如果某次 WPS 更新后 inline patch 的函数前缀变了，可以先禁用 inline patch 做排查：

```bash
WPS_QELAPSED_INLINE=0 bin/wpp-fps-unlock demo.pptx
```

## 说明

当前机器上的 WPS 使用的是 `/usr/lib/office6/libQt5CoreKso.so.5.12.12`，符号不是普通的 `QElapsedTimer`，而是 `kso_qt::QElapsedTimer`，并带 `Qt_5` 符号版本。所以这个 preload 库专门导出了 WPS 需要的 `kso_qt` 符号，同时也兼容普通 Qt5 符号。

启动脚本默认直接执行 `/usr/lib/office6/wpp`，避免 `/usr/bin/wpp` 包装脚本吞掉调试输出。WPS 如果安装在别的位置，可以设置 `WPS_WPP_BIN`。

如果 WPS 启动异常，先用较保守的参数测试：

```bash
WPS_FAKE_SPEED=1.5 bin/wpp-fps-unlock demo.pptx
```
