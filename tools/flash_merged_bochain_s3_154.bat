@echo off
chcp 65001 >nul
setlocal

if "%~1"=="" (
  echo 用法：flash_merged_bochain_s3_154.bat COM端口 固件bin文件
  echo 示例：flash_merged_bochain_s3_154.bat COM7 bochain-xiaozhi-s3-st7789-154-merged.bin
  exit /b 1
)

if "%~2"=="" (
  echo 用法：flash_merged_bochain_s3_154.bat COM端口 固件bin文件
  echo 示例：flash_merged_bochain_s3_154.bat COM7 bochain-xiaozhi-s3-st7789-154-merged.bin
  exit /b 1
)

set PORT=%~1
set BIN=%~2

echo 正在刷入：%BIN%
echo 端口：%PORT%
python -m esptool --chip esp32s3 --port %PORT% --baud 921600 write_flash 0x0 "%BIN%"

pause
