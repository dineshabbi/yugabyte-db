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
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/rpc/messenger.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <list>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <boost/scope_exit.hpp>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/rpc/acceptor.h"
#include "yb/rpc/connection.h"
#include "yb/rpc/constants.h"
#include "yb/rpc/rpc_header.pb.h"
#include "yb/rpc/rpc_service.h"
#include "yb/rpc/yb_rpc.h"

#include "yb/util/errno.h"
#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/net/socket.h"
#include "yb/util/status.h"
#include "yb/util/threadpool.h"
#include "yb/util/trace.h"

using namespace std::literals;
using std::string;
using std::shared_ptr;
using strings::Substitute;

DECLARE_int32(num_connections_to_server);
DEFINE_int32(rpc_default_keepalive_time_ms, 65000,
             "If an RPC connection from a client is idle for this amount of time, the server "
             "will disconnect the client. Setting flag to 0 disables this clean up.");
TAG_FLAG(rpc_default_keepalive_time_ms, advanced);
DEFINE_uint64(io_thread_pool_size, 4, "Size of allocated IO Thread Pool.");

namespace yb {
namespace rpc {

class Messenger;
class ServerBuilder;

MessengerBuilder::MessengerBuilder(std::string name)
    : name_(std::move(name)),
      connection_keepalive_time_(FLAGS_rpc_default_keepalive_time_ms * 1ms),
      num_reactors_(4),
      coarse_timer_granularity_(100ms),
      connection_context_factory_(&std::make_unique<YBConnectionContext>) {
}

MessengerBuilder& MessengerBuilder::set_connection_keepalive_time(
    CoarseMonoClock::Duration keepalive) {
  connection_keepalive_time_ = keepalive;
  return *this;
}

MessengerBuilder& MessengerBuilder::set_num_reactors(int num_reactors) {
  num_reactors_ = num_reactors;
  return *this;
}

MessengerBuilder& MessengerBuilder::set_coarse_timer_granularity(
    CoarseMonoClock::Duration granularity) {
  coarse_timer_granularity_ = granularity;
  return *this;
}

MessengerBuilder &MessengerBuilder::set_metric_entity(
    const scoped_refptr<MetricEntity>& metric_entity) {
  metric_entity_ = metric_entity;
  return *this;
}

Result<std::shared_ptr<Messenger>> MessengerBuilder::Build() {
  std::unique_ptr<Messenger> messenger(new Messenger(*this));
  RETURN_NOT_OK(messenger->Init());

  // See docs on Messenger::retain_self_ for info about this odd hack.
  return shared_ptr<Messenger>(
    messenger.release(), std::mem_fun(&Messenger::AllExternalReferencesDropped));
}

// See comment on Messenger::retain_self_ member.
void Messenger::AllExternalReferencesDropped() {
  Shutdown();
  CHECK(retain_self_.get());
  // If we have no more external references, then we no longer
  // need to retain ourself. We'll destruct as soon as all our
  // internal-facing references are dropped (ie those from reactor
  // threads).
  retain_self_.reset();
}

void Messenger::Shutdown() {
  // Since we're shutting down, it's OK to block.
  ThreadRestrictions::ScopedAllowWait allow_wait;

  decltype(reactors_) reactors;
  std::unique_ptr<Acceptor> acceptor;
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    if (closing_) {
      return;
    }
    VLOG(1) << "shutting down messenger " << name_;
    closing_ = true;

    DCHECK(rpc_services_.empty()) << "Unregister RPC services before shutting down Messenger";
    rpc_services_.clear();

    acceptor.swap(acceptor_);

    reactors = reactors_;
  }

  if (acceptor) {
    acceptor->Shutdown();
  }

  for (auto* reactor : reactors) {
    reactor->Shutdown();
  }

  scheduler_.Shutdown();
  io_thread_pool_.Shutdown();

  for (auto* reactor : reactors) {
    reactor->Join();
  }

  io_thread_pool_.Join();
}

Status Messenger::ListenAddress(const Endpoint& accept_endpoint, Endpoint* bound_endpoint) {
  Acceptor* acceptor;
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    if (!acceptor_) {
      acceptor_.reset(new Acceptor(this));
    }
    auto accept_host = accept_endpoint.address();
    auto& outbound_address = accept_host.is_v6() ? outbound_address_v6_
                                                 : outbound_address_v4_;
    if (outbound_address.is_unspecified() && !accept_host.is_unspecified()) {
      outbound_address = accept_host;
    }
    acceptor = acceptor_.get();
  }
  return acceptor->Listen(accept_endpoint, bound_endpoint);
}

Status Messenger::StartAcceptor() {
  std::lock_guard<percpu_rwlock> guard(lock_);
  if (acceptor_) {
    return acceptor_->Start();
  } else {
    return STATUS(IllegalState, "Trying to start acceptor w/o active addresses");
  }
}

void Messenger::BreakConnectivityWith(const IpAddress& address) {
  LOG(INFO) << "TEST: Break connectivity with: " << address;

  std::unique_ptr<CountDownLatch> latch;
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    if (broken_connectivity_.empty()) {
      has_broken_connectivity_.store(true, std::memory_order_release);
    }
    if (broken_connectivity_.insert(address).second) {
      latch.reset(new CountDownLatch(reactors_.size()));
      for (auto* reactor : reactors_) {
        reactor->ScheduleReactorTask(MakeFunctorReactorTask(
            [&latch, address](Reactor* reactor) {
              reactor->DropWithRemoteAddress(address);
              latch->CountDown();
            }));
      }
    }
  }

  if (latch) {
    latch->Wait();
  }
}

void Messenger::RestoreConnectivityWith(const IpAddress& address) {
  LOG(INFO) << "TEST: Restore connectivity with: " << address;

  std::lock_guard<percpu_rwlock> guard(lock_);
  broken_connectivity_.erase(address);
  if (broken_connectivity_.empty()) {
    has_broken_connectivity_.store(false, std::memory_order_release);
  }
}

bool Messenger::IsArtificiallyDisconnectedFrom(const IpAddress& remote) {
  if (has_broken_connectivity_.load(std::memory_order_acquire)) {
    shared_lock<rw_spinlock> guard(lock_.get_lock());
    return broken_connectivity_.count(remote) != 0;
  }
  return false;
}

void Messenger::ShutdownAcceptor() {
  std::unique_ptr<Acceptor> acceptor;
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    acceptor.swap(acceptor_);
  }
  if (acceptor) {
    acceptor->Shutdown();
  }
}

// Register a new RpcService to handle inbound requests.
Status Messenger::RegisterService(const string& service_name,
                                  const scoped_refptr<RpcService>& service) {
  DCHECK(service);
  std::lock_guard<percpu_rwlock> guard(lock_);
  if (InsertIfNotPresent(&rpc_services_, service_name, service)) {
    UpdateServicesCache(&guard);
    return Status::OK();
  } else {
    return STATUS_SUBSTITUTE(AlreadyPresent, "Service $0 is already present", service_name);
  }
}

Status Messenger::UnregisterAllServices() {
  decltype(rpc_services_) rpc_services_copy; // Drain rpc services here,
                                             // to avoid deleting them in locked state.
  {
    std::lock_guard<percpu_rwlock> guard(lock_);
    rpc_services_.swap(rpc_services_copy);
    UpdateServicesCache(&guard);
  }
  rpc_services_copy.clear();
  return Status::OK();
}

// Unregister an RpcService.
Status Messenger::UnregisterService(const string& service_name) {
  std::lock_guard<percpu_rwlock> guard(lock_);
  if (rpc_services_.erase(service_name)) {
    UpdateServicesCache(&guard);
    return Status::OK();
  } else {
    return STATUS(ServiceUnavailable, Substitute("service $0 not registered on $1",
                 service_name, name_));
  }
}

void Messenger::QueueOutboundCall(OutboundCallPtr call) {
  const auto& remote = call->conn_id().remote();
  Reactor *reactor = RemoteToReactor(remote, call->conn_id().idx());

  if (IsArtificiallyDisconnectedFrom(remote.address())) {
    LOG(INFO) << "TEST: Rejected connection to " << remote;
    reactor->ScheduleReactorTask(MakeFunctorReactorTask([call](Reactor*) {
      call->Transferred(STATUS(NetworkError, "TEST: Connectivity is broken"), nullptr);
    }));
    return;
  }

  reactor->QueueOutboundCall(std::move(call));
}

void Messenger::QueueInboundCall(InboundCallPtr call) {
  auto service = rpc_service(call->service_name());
  if (PREDICT_FALSE(!service)) {
    Status s = STATUS_FORMAT(ServiceUnavailable,
                             "Service $0 not registered on $1",
                             call->service_name(),
                             name_);
    LOG(WARNING) << s;
    call->RespondFailure(ErrorStatusPB::ERROR_NO_SUCH_SERVICE, s);
    return;
  }

  // The RpcService will respond to the client on success or failure.
  service->QueueInboundCall(std::move(call));
}

void Messenger::Handle(InboundCallPtr call) {
  auto service = rpc_service(call->service_name());
  if (PREDICT_FALSE(!service)) {
    Status s = STATUS_FORMAT(ServiceUnavailable,
                             "Service $0 not registered on $1",
                             call->service_name(),
                             name_);
    LOG(WARNING) << s;
    call->RespondFailure(ErrorStatusPB::ERROR_NO_SUCH_SERVICE, s);
    return;
  }

  service->Handle(std::move(call));
}

void Messenger::RegisterInboundSocket(Socket *new_socket, const Endpoint& remote) {
  if (IsArtificiallyDisconnectedFrom(remote.address())) {
    auto status = new_socket->Close();
    LOG(INFO) << "TEST: Rejected connection from " << remote
              << ", close status: " << status.ToString();
    return;
  }

  int idx = num_connections_accepted_.fetch_add(1) % FLAGS_num_connections_to_server;
  Reactor *reactor = RemoteToReactor(remote, idx);
  reactor->RegisterInboundSocket(new_socket, remote);
}

Messenger::Messenger(const MessengerBuilder &bld)
  : name_(bld.name_),
    connection_context_factory_(bld.connection_context_factory_),
    metric_entity_(bld.metric_entity_),
    retain_self_(this),
    io_thread_pool_(FLAGS_io_thread_pool_size),
    scheduler_(&io_thread_pool_.io_service()) {
  for (int i = 0; i < bld.num_reactors_; i++) {
    reactors_.push_back(new Reactor(retain_self_, i, bld));
  }
}

Messenger::~Messenger() {
  std::lock_guard<percpu_rwlock> guard(lock_);
  CHECK(closing_) << "Should have already shut down";
  STLDeleteElements(&reactors_);
}

size_t Messenger::max_concurrent_requests() const {
  return FLAGS_num_connections_to_server;
}

Reactor* Messenger::RemoteToReactor(const Endpoint& remote, uint32_t idx) {
  uint32_t hashCode = hash_value(remote);
  int reactor_idx = (hashCode + idx) % reactors_.size();
  // This is just a static partitioning; where each connection
  // to a remote is assigned to a particular reactor. We could
  // get a lot fancier with assigning Sockaddrs to Reactors,
  // but this should be good enough.
  return reactors_[reactor_idx];
}

Status Messenger::Init() {
  Status status;
  for (Reactor* r : reactors_) {
    RETURN_NOT_OK(r->Init());
  }

  return Status::OK();
}

Status Messenger::DumpRunningRpcs(const DumpRunningRpcsRequestPB& req,
                                  DumpRunningRpcsResponsePB* resp) {
  shared_lock<rw_spinlock> guard(lock_.get_lock());
  for (Reactor* reactor : reactors_) {
    RETURN_NOT_OK(reactor->DumpRunningRpcs(req, resp));
  }
  return Status::OK();
}

Status Messenger::QueueEventOnAllReactors(ServerEventListPtr server_event) {
  shared_lock<rw_spinlock> guard(lock_.get_lock());
  for (Reactor* reactor : reactors_) {
    reactor->QueueEventOnAllConnections(server_event);
  }
  return Status::OK();
}

void Messenger::RemoveScheduledTask(int64_t id) {
  CHECK_NE(id, -1);
  std::lock_guard<std::mutex> guard(mutex_scheduled_tasks_);
  scheduled_tasks_.erase(id);
}

void Messenger::AbortOnReactor(int64_t task_id) {
  DCHECK(!reactors_.empty());
  CHECK_NE(task_id, -1);

  std::lock_guard<std::mutex> guard(mutex_scheduled_tasks_);
  auto iter = scheduled_tasks_.find(task_id);
  if (iter != scheduled_tasks_.end()) {
    const auto& task = iter->second;
    task->AbortTask(STATUS(Aborted, "Task aborted by messenger"));
    scheduled_tasks_.erase(iter);
  }
}

int64_t Messenger::ScheduleOnReactor(const std::function<void(const Status&)>& func,
                                     MonoDelta when,
                                     const shared_ptr<Messenger>& msgr) {
  DCHECK(!reactors_.empty());

  // If we're already running on a reactor thread, reuse it.
  Reactor* chosen = nullptr;
  for (Reactor* r : reactors_) {
    if (r->IsCurrentThread()) {
      chosen = r;
    }
  }
  if (chosen == nullptr) {
    // Not running on a reactor thread, pick one at random.
    chosen = reactors_[rand() % reactors_.size()];
  }

  int64_t task_id = 0;
  if (msgr != nullptr) {
    task_id = next_task_id_.fetch_add(1);
  }
  auto task = std::make_shared<DelayedTask>(func, when, task_id, msgr);
  if (msgr != nullptr) {
    std::lock_guard<std::mutex> guard(mutex_scheduled_tasks_);
    scheduled_tasks_[task_id] = task;
  }
  chosen->ScheduleReactorTask(task);
  return task_id;
}

void Messenger::UpdateServicesCache(std::lock_guard<percpu_rwlock>* guard) {
  DCHECK_ONLY_NOTNULL(guard);

  rpc_services_cache_.Set(rpc_services_);
}

scoped_refptr<RpcService> Messenger::rpc_service(const string& service_name) const {
  auto cache = rpc_services_cache_.get();
  auto it = cache->find(service_name);
  if (it != cache->end()) {
    // Since our cache is a cache of whole rpc_services_ map, we could check only it.
    return it->second;
  }
  return scoped_refptr<RpcService>();
}

} // namespace rpc
} // namespace yb
