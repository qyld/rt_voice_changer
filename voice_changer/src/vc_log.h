// 运行日志: 界面程序写到 %APPDATA%\AIVoiceChanger\log.txt, 命令行同时打印到控制台
#pragma once
#include <string>

void vc_log_set_file(const std::wstring& path);
void vc_log_set_console(bool on);
void vc_log(const std::string& msg);
