#include <fstream>
#include <glog/logging.h>
#include <poll.h>
#include "pika_slaveping_thread.h"
#include "pika_trysync_thread.h"
#include "pika_server.h"
#include "pika_conf.h"
#include "env.h"

extern PikaServer* g_pika_server;
extern PikaConf* g_pika_conf;

PikaTrysyncThread::~PikaTrysyncThread() {
  should_exit_ = true;
  pthread_join(thread_id(), NULL);
  slash::StopRsync(g_pika_conf->db_sync_path());
  delete cli_;
  DLOG(INFO) << " Trysync thread " << pthread_self() << " exit!!!";
}

bool PikaTrysyncThread::Send() {
  pink::RedisCmdArgsType argv;
  std::string wbuf_str;
  std::string requirepass = g_pika_conf->requirepass();
  if (requirepass != "") {
    argv.push_back("auth");
    argv.push_back(requirepass);
    pink::RedisCli::SerializeCommand(argv, &wbuf_str);
  }

  argv.clear();
  std::string tbuf_str;
  argv.push_back("trysync");
  argv.push_back(g_pika_server->host());
  argv.push_back(std::to_string(g_pika_server->port()));
  uint32_t filenum;
  uint64_t pro_offset;
  g_pika_server->logger_->GetProducerStatus(&filenum, &pro_offset);
  
  argv.push_back(std::to_string(filenum));
  argv.push_back(std::to_string(pro_offset));
  pink::RedisCli::SerializeCommand(argv, &tbuf_str);

  wbuf_str.append(tbuf_str);
  DLOG(INFO) << wbuf_str;

  pink::Status s;
  s = cli_->Send(&wbuf_str);
  if (!s.ok()) {
    LOG(WARNING) << "Connect master, Send, error: " <<strerror(errno);
    return false;
  }
  return true;
}

bool PikaTrysyncThread::RecvProc() {
  bool should_auth = g_pika_conf->requirepass() == "" ? false : true;
  bool is_authed = false;
  pink::Status s;
  std::string reply;

  while (1) {
    s = cli_->Recv(NULL);
    if (!s.ok()) {
      LOG(WARNING) << "Connect master, Recv, error: " <<strerror(errno);
      return false;
    }

    reply = cli_->argv_[0];
    DLOG(INFO) << "Reply from master after trysync: " << reply;
    if (!is_authed && should_auth) {
      if (kInnerReplOk != slash::StringToLower(reply)) {
        g_pika_server->RemoveMaster();
        return false;
      }
      is_authed = true;
    } else {
      if (cli_->argv_.size() == 1 &&
          slash::string2l(reply.data(), reply.size(), &sid_)) {
        // Luckly, I got your point, the sync is comming
        DLOG(INFO) << "Recv sid from master: " << sid_;
        break;
      }
      // Failed

      if (kInnerReplWait == reply) {
        // You can't sync this time, but may be different next time,
        // This may happened when 
        // 1, Master do bgsave first.
        // 2, Master waiting for an existing bgsaving process
        // 3, Master do dbsyncing
        DLOG(INFO) << "Need wait to sync";
        g_pika_server->NeedWaitDBSync();
      } else {
        g_pika_server->RemoveMaster();
      }
      return false;
    }
  }
  return true;
}

// Try to update master offset
// This may happend when dbsync from master finished
// Here we do:
// 1, Check dbsync finished, got the new binlog offset
// 2, Replace the old db
// 3, Update master offset, and the PikaTrysyncThread cron will connect and do slaveof task with master
bool PikaTrysyncThread::TryUpdateMasterOffset() {
  // Check dbsync finished
  std::string info_path = g_pika_conf->db_sync_path() + kBgsaveInfoFile;
  if (!slash::FileExists(info_path)) {
    return false;
  }

  // Got new binlog offset
  std::ifstream is(info_path);
  if (!is) {
    LOG(ERROR) << "Failed to open info file after db sync";
    return false;
  }
  std::string line, master_ip;
  int lineno = 0;
  int64_t filenum = 0, offset = 0, tmp = 0, master_port = 0;
  while (std::getline(is, line)) {
    lineno++;
    if (lineno == 2) {
      master_ip = line;
    } else if (lineno > 2 && lineno < 6) {
      if (!slash::string2l(line.data(), line.size(), &tmp) || tmp < 0) {
        LOG(ERROR) << "Format of info file after db sync error, line : " << line;
        is.close();
        return false;
      }
      if (lineno == 3) { master_port = tmp; }
      else if (lineno == 4) { filenum = tmp; }
      else { offset = tmp; }

    } else if (lineno > 5) {
      LOG(ERROR) << "Format of info file after db sync error, line : " << line;
      is.close();
      return false;
    }
  }
  is.close();
  LOG(INFO) << "Information from dbsync info. master_ip: " << master_ip
    << ", master_port: " << master_port
    << ", filenum: " << filenum
    << ", offset: " << offset;

  // Sanity check
  if (master_ip != g_pika_server->master_ip() ||
      master_port != g_pika_server->master_port()) {
    LOG(ERROR) << "Error master ip port: " << master_ip << ":" << master_port;
    return false;
  }

  // Replace the old db
  slash::StopRsync(g_pika_conf->db_sync_path());
  slash::DeleteFile(info_path);
  if (!g_pika_server->ChangeDb(g_pika_conf->db_sync_path())) {
    LOG(ERROR) << "Failed to change db";
    return false;
  }

  // Update master offset
  g_pika_server->logger_->SetProducerStatus(filenum, offset);
  g_pika_server->WaitDBSyncFinish();
  return true;
}

void PikaTrysyncThread::PrepareRsync() {
  std::string db_sync_path = g_pika_conf->db_sync_path();
  slash::StopRsync(db_sync_path);
  slash::CreatePath(db_sync_path + "kv");
  slash::CreatePath(db_sync_path + "hash");
  slash::CreatePath(db_sync_path + "list");
  slash::CreatePath(db_sync_path + "set");
  slash::CreatePath(db_sync_path + "zset");
}

// TODO maybe use RedisCli
void* PikaTrysyncThread::ThreadMain() {
  while (!should_exit_) {
    sleep(1);
    if (g_pika_server->WaitingDBSync()) {
      //Try to update offset by db sync
      if (TryUpdateMasterOffset()) {
        DLOG(INFO) << "Success Update Master Offset";
      }
    }

    if (!g_pika_server->ShouldConnectMaster()) {
      continue;
    }
    sleep(2);
    DLOG(INFO) << "Should connect master";
    
    std::string master_ip = g_pika_server->master_ip();
    int master_port = g_pika_server->master_port();
    
    // Start rsync
    std::string dbsync_path = g_pika_conf->db_sync_path();
    PrepareRsync();
    std::string ip_port = slash::IpPortString(master_ip, master_port);
    // We append the master ip port after module name
    // To make sure only data from current master is received
    int ret = slash::StartRsync(dbsync_path, kDBSyncModule + "_" + ip_port, g_pika_conf->port() + 300);
    if (0 != ret) {
      LOG(ERROR) << "Failed to start rsync, path:" << dbsync_path << " error : " << ret;
    }
    DLOG(INFO) << "Finish to start rsync, path:" << dbsync_path;


    if ((cli_->Connect(master_ip, master_port)).ok()) {
      cli_->set_send_timeout(5000);
      cli_->set_recv_timeout(5000);
      if (Send() && RecvProc()) {
        g_pika_server->ConnectMasterDone();
        // Stop rsync, binlog sync with master is begin
        slash::StopRsync(dbsync_path);
        delete g_pika_server->ping_thread_;
        g_pika_server->ping_thread_ = new PikaSlavepingThread(sid_);
        g_pika_server->ping_thread_->StartThread();
        DLOG(INFO) << "Trysync success";
      }
      cli_->Close();
    } else {
      LOG(WARNING) << "Failed to connect to master, " << master_ip << ":" << master_port;
    }
  }
  return NULL;
}
