# 13.56 MHz Reader/Card Emulator Usage

This repository includes a Python-based virtual reader+card emulator (`tools/rfid_14443_emulator.py`) that mirrors the UART framing the Qt 示例程序 uses (commands 0x46–0x4C). It lets you test tag registration/充值/进出场计费逻辑 without physical hardware by pairing two virtual serial ports (e.g., com0com on Windows) between the emulator and the Qt 应用。

## 环境准备
1. **Windows 主机安装 com0com**（或任意可创建虚拟串口对的工具）。假设得到一对端口 `COM9 <-> COM10`。
2. **Python 3 与 pyserial**：`pip install pyserial`。
3. 将 `tools/rfid_14443_emulator.py` 保留默认值即可；如需定制卡号或密钥，可用命令行参数覆盖。

## 启动步骤
1. 在 Windows 主机上打开命令行，运行（以 com0com 生成的端口为例）：
   ```bash
   python tools/rfid_14443_emulator.py --port COM9 --baud 19200
   ```
   * `--port` 设为虚拟端口对的一端。
   * `--baud` 默认 115200，Qt 工程默认 19200，可改为 19200 保持一致。
   * 可选：`--uid <8hex>`、`--keya <12hex>` 调整卡 UID 或 KeyA（默认 UID `B7D2FF79`，KeyA `FFFFFFFFFFFF`）。
2. 在 **虚拟机/客体系统**（运行 Qt 程序的一侧）将串口指向虚拟端口对的另一端（例：`COM10`），波特率设为 **19200，8N1**。
3. 启动 Qt 停车系统。界面将自动寻卡/选卡/认证，读取块 1、块 2 的用户信息；注册、充值、入场、出场动作都会通过虚拟读写器与脚本交互。

## 是否需要修改 Python 脚本？
* **无需修改**：默认脚本已模拟 S50 卡、KeyA 验证、读写块 1/2 逻辑，能直接配合当前 Qt 程序。
* **可选调整**：
  * 想更换默认卡号/密钥，可使用 `--uid`、`--keya` 参数；无需改代码。
  * 若要预置不同数据到块 1/2，可在脚本中 `self.blocks[1]`、`self.blocks[2]` 初始值处修改，或运行期间在 Qt 界面完成写入后保持脚本运行。

## 交互说明
* Qt 应用发送的 `寻卡-防冲突-选卡-认证-读/写` 命令序列会在脚本终端打印 `RX/TX`，便于排查帧格式。
* 若看到 `RX invalid frame`，请确认波特率、起止符/转义是否与工程一致（脚本使用与 `IEEE1443Package` 相同的累加校验与转义规则）。
