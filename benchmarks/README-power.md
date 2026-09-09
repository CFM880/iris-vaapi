# Iris 功耗对比

`compare_iris_power.py` 默认在不播放视频、不运行额外负载的待机状态下，分别测试
`qcom_iris` 加载和卸载两种状态。运行期间从 Linux power-supply sysfs 采样电压、
电流，并记录：

- `battery_consumption_uwh`：电池能量下降量，单位 µWh；
- `battery_consumption_percent`：电量百分比下降量；
- `average_power_w`：采样到的电池电压 × 电流的平均值，单位 W。

设备需要提供 `energy_now`，或同时提供 `charge_now` 和 `voltage_now`。默认会自动
选择 `type` 为 `Battery` 的电源设备；nabu 的电池也可以显式指定为
`--power-supply qcom-battery`。

示例：

```sh
sudo ./compare_iris_power.py \
  --power-supply qcom-battery \
  --duration 60 \
  --interval 1 \
  --output-dir ./power-results
```

如需以后加入自定义负载，可以在命令末尾使用 `-- command ...`；默认不启动任何
视频或其它测试程序。

脚本会在结束时恢复启动前的模块状态。结果写入 `power-results/results.csv`，
每次负载的标准输出和错误输出分别写入同目录下的日志文件。测试命令应保持固定
的屏幕亮度、音量、网络状态和播放内容；短于几十秒的测试容易受到电池计量器
分辨率和系统后台任务影响。
