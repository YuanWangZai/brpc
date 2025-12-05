// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "butil/logging.h"
#include "butil/time.h"
#include "brpc/server.h"
#include "bvar/variable.h"
#include "test.pb.h"

#include <fcntl.h>     
#include <unistd.h>    
#include <sys/ioctl.h>  
#include <linux/fs.h> 
#include <stdlib.h>

#ifdef BRPC_WITH_RDMA

DEFINE_int32(port, 8002, "TCP Port of this server");
DEFINE_bool(use_rdma, true, "Use RDMA or not");

butil::atomic<uint64_t> g_last_time(0);

const
const double K_TEST_RATIO = 0.1;
const uint64_t K_MAX_BYTES_LIMIT = 0;
int g_disk_fd = -1;
uint64_t g_max_offset = 0;

namespace test {
class PerfTestServiceImpl : public PerfTestService {
public:
    PerfTestServiceImpl() {}
    ~PerfTestServiceImpl() {}

    void Test(google::protobuf::RpcController* cntl_base,
              const PerfTestRequest* request,
              PerfTestResponse* response,
              google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        // 统计CPU使用率，每100ms更新一次
        uint64_t last = g_last_time.load(butil::memory_order_relaxed);
        uint64_t now = butil::monotonic_time_us();
        if (now > last && now - last > 100000) {
            if (g_last_time.exchange(now, butil::memory_order_relaxed) == last) {
                response->set_cpu_usage(bvar::Variable::describe_exposed("process_cpu_usage"));
            } else {
                response->set_cpu_usage("");
            }
        } else {
            response->set_cpu_usage("");
        }

        if (g_disk_fd < 0) {
            LOG(ERROR) << "Disk not initialized!";
            return;
        }

        static thread_local void* tls_buf = nullptr;
        static thread_local size_t tls_buf_cap = 0;
        
        brpc::Controller* cntl = static_cast<brpc::Controller*>(cntl_base);
        size_t io_size = cntl->request_attachment().size();
        if (tls_buf == nullptr || tls_buf_cap < io_size) {
            if (tls_buf) free(tls_buf); // 释放旧的（如果太小）
            
            // 申请新的对齐内存
            if (posix_memalign(&tls_buf, 4096, io_size) != 0) {
                LOG(ERROR) << "Memory allocation failed";
                return;
            }
            tls_buf_cap = io_size; // 记录当前容量
        }
        cntl->request_attachment().copy_to(tls_buf, io_size);

        uint64_t offset = rand() % (g_max_offset - io_size);
        offset = (offset / 4096) * 4096;

        ssize_t ret = pwrite(g_disk_fd, tls_buf, io_size, offset);

        if (ret < 0) {
            LOG(ERROR) << "Disk write failed, ret=" << ret;
            return;
        }
    }
};
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    // --- 【NVMe 初始化逻辑】 ---
    // O_RDWR: 读写模式
    // O_DIRECT: 绕过 Page Cache，直接测硬件
    g_disk_fd = open(K_DISK_PATH, O_RDWR | O_DIRECT);
    if (g_disk_fd < 0) {
        LOG(ERROR) << "Fatal: Failed to open " << K_DISK_PATH 
                   << ". Please check permission (sudo?) or device name.";
        return -1;
    }

    uint64_t disk_physical_size = 0;
    if (ioctl(g_disk_fd, BLKGETSIZE64, &disk_physical_size) < 0) {
        LOG(ERROR) << "Failed to get disk size";
        close(g_disk_fd);
        return -1;
    }

    // 计算测试范围
    if (K_MAX_BYTES_LIMIT > 0) {
        // 如果设置了固定大小，优先使用
        g_max_offset = K_MAX_BYTES_LIMIT;
        LOG(INFO) << "Test Mode: Fixed Size (" << g_max_offset / 1024 / 1024 / 1024 << " GB)";
    } else {
        // 否则使用比例
        g_max_offset = disk_physical_size * K_TEST_RATIO;
        LOG(INFO) << "Test Mode: Ratio (" << K_TEST_RATIO * 100 << "%)";
    }

    // 不要超过物理磁盘大小
    if (g_max_offset > disk_physical_size) {
        g_max_offset = disk_physical_size;
    }

    LOG(INFO) << "Disk Init Success. Target: " << K_DISK_PATH;
    LOG(INFO) << "Max LBA Offset: " << g_max_offset << " bytes";
    // -------------------------

    brpc::Server server;
    test::PerfTestServiceImpl perf_test_service_impl;

    if (server.AddService(&perf_test_service_impl, 
                          brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Fail to add service";
        return -1;
    }
    g_last_time.store(0, butil::memory_order_relaxed);

    brpc::ServerOptions options;
    options.use_rdma = FLAGS_use_rdma;
    if (server.Start(FLAGS_port, &options) != 0) {
        LOG(ERROR) << "Fail to start EchoServer";
        return -1;
    }

    server.RunUntilAskedToQuit();
    return 0;
}

#else


int main(int argc, char* argv[]) {
    LOG(ERROR) << " brpc is not compiled with rdma. To enable it, please refer to https://github.com/apache/brpc/blob/master/docs/en/rdma.md";
    return 0;
}

#endif
