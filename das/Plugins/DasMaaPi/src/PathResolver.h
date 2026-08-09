#pragma once

#include <filesystem>
#include <string>

namespace Das::Plugins::DasMaaPi
{
    // 判断 child_exec 是否“裸命令名”——仅单个普通组件、无路径分隔符
    // （如 python / node / git）。带分隔符（./x、subdir/x、/usr/bin/x）
    // 返回 false。裸命令走系统 PATH 解析，与 CWD 无关；相对路径需基于
    // interface 目录拼成绝对路径后再交给子进程启动（Windows CreateProcess
    // 用父进程 CWD 解析相对可执行路径，process_start_dir 只决定子进程
    // 启动后的 CWD——相对 exec 不预解析会 spawn 失败，os error 2/3）。
    bool IsBareCommand(const std::string& value);

    // 解析 agent / pretask 的可执行入口路径：
    //   - 裸命令名（python / node）→ 原样返回，由系统 PATH 解析
    //   - 绝对路径 / 带盘符前缀 → lexically_normal 规范化
    //   - 相对路径 → 基于 interface_directory 拼成绝对路径再规范化
    // 供 AgentRuntimeService（agent）与 MaaRuntime::RunPretasks（pretask）
    // 共用，保证相对入口的解析行为一致（MXU maa_agent.rs:254
    // resolve_child_exec_path 同语义）。
    std::filesystem::path ResolveChildExec(
        const std::string& child_exec,
        const std::string& interface_directory);
} // namespace Das::Plugins::DasMaaPi
