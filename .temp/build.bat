@echo off
set IDF_TOOLS_PATH=C:\esp\v5.5.4
set IDF_PYTHON_ENV_PATH=C:\esp\v5.5.4\python_env\idf5.5_py3.11_env
call C:\esp\v5.5.4\frameworks\esp-idf-v5.5.4\export.bat
cd /D D:\ESP32-S3-RLCD-4.2
idf.py build
