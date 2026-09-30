# nabu 功耗测量

`nabu-power-measure.sh` 是小米平板 5（nabu，SM8150）的功耗测量脚本，直接读
PM8150B 燃料计（`qcom-battery`）的 `current_now` / `voltage_now`。

它取代了本目录原来的两个 Python 脚本：

- `compare_iris_power.py`（比较 `qcom_iris` 加载/卸载的功耗）
- `measure_power.py`（固定时长测功耗）

## 为什么换掉旧的

- 旧脚本需要电池提供 `energy_now`，或同时提供 `charge_now` + `voltage_now`；
  **nabu 这几种都没有**，只能靠 `current_now`/`voltage_now`，旧脚本在这台机器上
  取不到能量字段。
- 新脚本只依赖 `current_now`/`voltage_now`，并额外支持：

  | 能力 | 说明 |
  |---|---|
  | 采样模式 | 按 `电流 × 电压` 给出平均/最小/最大功率，可存 CSV |
  | 挂起模式 `--suspend` | 挂起前后读电量，按**电量差**算能量（挂起期间电流寄存器读不到） |
  | `--screen-off` | 走 GNOME screensaver 熄屏（best-effort） |

## 用法

```sh
# 熄屏待机，采 120 s
sudo ./nabu-power-measure.sh --screen-off --duration 120 -l standby

# 挂起 10 分钟，按电量差算待机功率（脚本自己用 RTC 闹钟唤醒）
sudo ./nabu-power-measure.sh --suspend 600 -l "suspend 10min"

# 纯采样 + 存 CSV（不用 root）
./nabu-power-measure.sh -d 60 -o /tmp/nabu-power.csv
```

全部选项见 `./nabu-power-measure.sh --help`。

## 输出

采样模式：

```
nabu power measurement  [standby]
  gauge      : /sys/class/power_supply/qcom-battery
  mode       : sample every 1s for 120s
  screen-off : yes
  samples    : 120
  current    : avg -295.8 mA
  power      : avg -1.256 W   min -1.748 W   max -1.157 W
  capacity   : 81% -> 81%
```

挂起模式：

```
  before     : 80%  4.247 V
  suspending : ~600s ...
  after      : 78%
  drop       : 2%  (~0.740 Wh over 600s)
  power      : 4.440 W
  resolution : 1% step = 2.22 W at 600s - use >=1800s for standby
```

## 注意

- **必须处于放电状态**：插着充电器时脚本直接拒绝（电流读数无意义）。
- `current_now` 符号沿用标准：**负 = 放电**。
- **表盘分辨率很粗**：1% = 0.37 Wh。短测没有意义——

  | 挂起时长 | 1% 对应 | 适用 |
  |---|---|---|
  | 30 s | ~44 W | 无意义 |
  | 30 min | ~0.74 W | 勉强 |
  | 1 h | ~0.37 W | 可用 |

  想要更准只能上**外部 USB-C 功率计**（挂起时电流寄存器读不到）。
- **挂起期间进程被冻结、电流读不到**，所以 `--suspend` 用挂起前后的**电量差**而不是
  实时采样。挂起必须**由脚本触发**（它负责上 RTC 闹钟、醒来后接着算）；手动按电源键
  挂起时脚本只会被冻住、算不出那一段。
- `--screen-off` 是 best-effort：跑完可能要点一下屏幕才亮。

## iris 的 A/B 功耗对比

原来的 `compare_iris_power.py` 用来比较 `qcom_iris` 加载/卸载的功耗，现在手动两步即可：

```sh
# A：卸载 iris（无视频硬解）
sudo modprobe -r qcom_iris
sudo ./nabu-power-measure.sh --screen-off -d 120 -l "iris unloaded"

# B：加载 iris
sudo modprobe qcom_iris
sudo ./nabu-power-measure.sh --screen-off -d 120 -l "iris loaded"
```

对比两次的 `power avg`。测试期间保持固定的屏幕亮度、音量、网络状态和播放内容；
短于几十秒的测量容易受表盘分辨率和后台任务影响。
