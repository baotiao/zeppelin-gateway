#include "src/zgw_server.h"

#include <atomic>

#include <glog/logging.h>
#include "slash/include/slash_mutex.h"
#include "slash/include/env.h"
#include "src/s3_cmds/zgw_s3_command.h"
#include "src/zgw_monitor.h"

extern ZgwConfig* g_zgw_conf;
extern ZgwMonitor* g_zgw_monitor;

static std::atomic<int> zgw_thread_id(0);

int ZgwThreadEnvHandle::SetEnv(void** env) const {
  zgwstore::ZgwStore* store;
  std::string lock_name = hostname() + std::to_string(g_zgw_conf->server_port)
    + std::to_string(zgw_thread_id++);
  Status s = zgwstore::ZgwStore::Open(g_zgw_conf->zp_meta_ip_ports,
                                      g_zgw_conf->redis_ip_port,
                                      g_zgw_conf->zp_table_name,
                                      lock_name, kZgwRedisLockTTL,
                                      g_zgw_conf->redis_passwd,
                                      &store);
  if (!s.ok()) {
    LOG(FATAL) << "Can not open ZgwStore: " << s.ToString();
    return -1;
  }
  *env = store;
  stores_.push_back(store);

  return 0;
}

ZgwThreadEnvHandle::~ZgwThreadEnvHandle() {
  for (auto s : stores_) {
    delete s;
  }
}

static ZgwThreadEnvHandle env_handle;

ZgwServer::ZgwServer()
    : should_exit_(false),
      worker_num_(g_zgw_conf->worker_num) {
  if (worker_num_ > kMaxWorkerThread) {
    LOG(WARNING) << "Exceed max worker thread num: " << kMaxWorkerThread;
    worker_num_ = kMaxWorkerThread;
  }

  conn_factory_ = new ZgwConnFactory();
  std::set<std::string> ips;
  ips.insert(g_zgw_conf->server_ip);
  zgw_dispatch_thread_ = pink::NewDispatchThread(ips, g_zgw_conf->server_port,
                                                 worker_num_, conn_factory_,
                                                 0, nullptr, &env_handle);
  zgw_dispatch_thread_->set_thread_name("DispatchThread");

  admin_conn_factory_ = new ZgwAdminConnFactory();
  zgw_admin_thread_ = pink::NewHolyThread(g_zgw_conf->admin_port,
                                          admin_conn_factory_,
                                          0, nullptr, &env_handle);
  zgw_admin_thread_->set_thread_name("AdminThread");
}

ZgwServer::~ZgwServer() {
  delete zgw_dispatch_thread_;
  delete zgw_admin_thread_;
  delete conn_factory_;
  delete admin_conn_factory_;

  LOG(INFO) << "ZgwServerThread exit!!!";
}

void ZgwServer::Exit() {
  zgw_dispatch_thread_->StopThread();
  zgw_admin_thread_->StopThread();
  should_exit_.store(true);
}

Status ZgwServer::Start() {
  Status s;
  LOG(INFO) << "Waiting for ZgwServerThread Init...";

  if (zgw_dispatch_thread_->StartThread() != 0) {
    return Status::Corruption("Launch DispatchThread failed");
  }
  if (zgw_admin_thread_->StartThread() != 0) {
    return Status::Corruption("Launch AdminThread failed");
  }

  LOG(INFO) << "ZgwServerThread Init Success!";

  while (running()) {
    // DoTimingTask
    slash::SleepForMicroseconds(kZgwCronInterval);
    g_zgw_monitor->UpdateAndGetQPS();
  }

  return Status::OK();
}
