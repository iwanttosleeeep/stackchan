# 刷机手册(Arduino,CoreS3 + StickC-Plus)

## 0. 铁律
第一次刷机前,全量备份出厂固件并存哈希:
    esptool.py --port /dev/cu.usbmodemXXXX --baud 460800 read_flash 0x0 0x1000000 factory_$(date +%Y%m%d).bin
    shasum -a 256 factory_*.bin > factory.sha256
备份+哈希双份存(本地+云盘)。回滚咒语:
    esptool.py --port /dev/cu.usbmodemXXXX --baud 460800 write_flash 0x0 factory_XXXXXXXX.bin

## 1. 工具链(arduino-cli,比IDE的下载器抗烂网)
    brew install arduino-cli
    arduino-cli config init --overwrite
    arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
    arduino-cli core update-index
    arduino-cli core install m5stack:esp32   # 断了就重跑;个别包卡住时按报错URL浏览器手动下载,
                                             # 放进 ~/Library/Arduino15/staging/packages/ 再重跑

## 2. 库
    arduino-cli lib install M5Unified M5GFX ArduinoJson IRremoteESP8266
    arduino-cli lib search avatar   # 按实际注册名安装 M5Stack-Avatar
    arduino-cli config set library.enable_unsafe_install true
    arduino-cli lib install --git-url https://github.com/m5stack/StackChan-BSP.git
    arduino-cli lib install --git-url https://github.com/m5stack/M5Unit-NFC.git

## 3. 机器人(CoreS3)
1. firmware/StackChanSenn/StackChanSenn.ino 填三个配置(SSID/密码/token)+ BASE_URL域名。
2. USB连接,查端口: ls /dev/cu.usbmodem*
3.  arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 StackChanSenn
    arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn m5stack:esp32:m5stack_cores3 StackChanSenn
4. 开机流程:困倦脸 → Connecting Wi-Fi(仅2.4GHz!)→ Online → "remote channel: N"(记下N!)

## 4. 遥控器(StickC-Plus + JoyC)
1. controller/StickSenn/StickSenn.ino 把 ESPNOW_CHANNEL 改成上面的N。
2. 板卡名先查: arduino-cli board listall | grep -i stick
3. compile/upload同上,fqbn换成查到的StickC-Plus名。

## 5. 已知联调点
- 音量:setVolume(180),0-255自调。
- 摇杆方向反:doMove映射处翻正负号(仓库版已含实测修正)。
- 拍照init failed:摄像头与触摸共享I2C,必须在setup()内、BSP循环启动前init(仓库版已如此)。
- speak后轮询卡死:pollTask栈不足,20480起步(仓库版已如此)。
