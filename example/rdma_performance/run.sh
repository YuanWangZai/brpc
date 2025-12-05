#!/bin/bash

# ================= 配置区域 =================
# Server 的地址 (如果是本机就是 0.0.0.0，如果是别的机器填 IP)
SERVER="0.0.0.0:8002"

# 每次测多久 (秒)
DURATION=10

# 客户端二进制文件名字
CLIENT="./performance_client"

# 是否使用 RDMA (如果你没配 RDMA 就填 false)
USE_RDMA="false"

# 结果保存文件
RESULT_FILE="experiment_result.txt"
# ===========================================

# 检查客户端是否存在
if [ ! -f "$CLIENT" ]; then
    echo "错误: 找不到 $CLIENT，请先编译 client！"
    exit 1
fi

echo "开始实验... 结果将追加到 $RESULT_FILE"
echo "========================================" >> $RESULT_FILE
echo "实验时间: $(date)" >> $RESULT_FILE
echo "========================================" >> $RESULT_FILE

# --- 函数: 运行单次测试 ---
run_test() {
    local threads=$1
    local size=$2
    local desc=$3

    echo "正在运行: [${desc}] 线程数=${threads}, 数据包大小=${size}B, 持续=${DURATION}s ..."
    
    # 运行 client，并将输出同时打印到屏幕和文件
    # -test_seconds 控制运行时间，跑完自动停
    # -dummy_port 防止多次运行端口冲突
    $CLIENT \
        -server="${SERVER}" \
        -use_rdma="${USE_RDMA}" \
        -test_seconds="${DURATION}" \
        -thread_num="${threads}" \
        -attachment_size="${size}" \
        -dummy_port=$((8000 + threads + size/1024)) \
        >> temp_output.log 2>&1

    # 提取吞吐量结果 (根据你的 client 输出格式调整 grep)
    # 假设输出里有 "Throughput: xxxMB/s"
    throughput=$(grep "Throughput:" temp_output.log | tail -n 1)
    
    echo "  -> 结果: ${throughput}"
    echo "  -> [${desc}] Threads: ${threads}, Size: ${size} => ${throughput}" >> $RESULT_FILE
    
    # 清理临时文件
    rm temp_output.log
    
    # 休息 2 秒，让 Server 喘口气
    sleep 2
}

# ================= 实验 A: 固定大小 4KB，变并发 =================
echo ">>> 开始实验 A: 测并发 (Latency vs IOPS) <<<"
echo "--- 实验 A ---" >> $RESULT_FILE

# 循环测试不同的线程数: 1, 4, 8, 16, 32, 64
for t in 1 4 8 16 32 64; do
    run_test $t 4096 "Exp A"
done

# ================= 实验 B: 固定并发 32，变大小 =================
echo ">>> 开始实验 B: 测带宽 (Bandwidth) <<<"
echo "--- 实验 B ---" >> $RESULT_FILE

# 设定一个基准并发数，通常选实验 A 中效果最好的，比如 32
FIXED_THREAD=32

# 循环测试不同的大小: 4K, 16K, 64K, 256K, 1M
# 注意：确保你的 Server 端 g_max_offset 足够大，且 Server 支持这几个大小的对齐
for s in 4096 16384 65536 262144 1048576; do
    run_test $FIXED_THREAD $s "Exp B"
done

echo "所有实验结束！请查看 $RESULT_FILE 获取汇总数据。"