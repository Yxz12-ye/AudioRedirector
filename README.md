## 📢 致谢
本项目基于微软 [Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) 中的 SysVAD 示例开发，遵循 MIT 许可证。

## 介绍

这是一个把本地音频重定向虚拟麦克风的小工具, 包含`内核驱动`和`用户软件`两块

## 使用方式

驱动方面, 目前没有签名, 也**非常不推荐**在物理机上使用

如果需要使用, 则要执行

```bash
bcdedit /set TESTSIGNING ON
```

重启后执行

```
hdwwiz
```

然后选择`ComponentizedAudioSample.inf`安装

重启后, 启动软件

```
vm_mic_sender --stats <时间:ms> [--wav] [<wav地址>] [get-help]
```

之后音频就能转发到虚拟麦克风了
