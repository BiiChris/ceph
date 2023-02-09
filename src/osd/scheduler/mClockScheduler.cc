// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */


#include <memory>
#include <functional>

#include "osd/scheduler/mClockScheduler.h"
#include "common/dout.h"

namespace dmc = crimson::dmclock;
using namespace std::placeholders;

#define dout_context cct
#define dout_subsys ceph_subsys_mclock
#undef dout_prefix
#define dout_prefix *_dout << "mClockScheduler: "


namespace ceph::osd::scheduler {

mClockScheduler::mClockScheduler(CephContext *cct,
  int whoami,
  uint32_t num_shards,
  int shard_id,
  bool is_rotational,
  MonClient *monc)
  : cct(cct),
    whoami(whoami),
    num_shards(num_shards),
    shard_id(shard_id),
    is_rotational(is_rotational),
    monc(monc),
    scheduler(
      std::bind(&mClockScheduler::ClientRegistry::get_info,
                &client_registry,
                _1),
      dmc::AtLimit::Wait,
      cct->_conf.get_val<double>("osd_mclock_scheduler_anticipation_timeout"))
{
  cct->_conf.add_observer(this);
  ceph_assert(num_shards > 0);
  set_osd_capacity_params_from_config();
  set_mclock_profile();
  enable_mclock_profile_settings();
  client_registry.update_from_config(
    cct->_conf, osd_bandwidth_capacity_per_shard);
}

/* ClientRegistry holds the dmclock::ClientInfo configuration parameters
 * (reservation (bytes/second), weight (unitless), limit (bytes/second))
 * for each IO class in the OSD (client, background_recovery,
 * background_best_effort).
 *
 * mclock expects limit and reservation to have units of <cost>/second
 * (bytes/second), but osd_mclock_scheduler_client_(lim|res) are provided
 * as ratios of the OSD's capacity.  We convert from the one to the other
 * using the capacity_per_shard parameter.
 *
 * Note, mclock profile information will already have been set as a default
 * for the osd_mclock_scheduler_client_* parameters prior to calling
 * update_from_config -- see set_config_defaults_from_profile().
 */
void mClockScheduler::ClientRegistry::update_from_config(
  const ConfigProxy &conf,
  const double capacity_per_shard)
{
  auto get_res = [&](double res) {
    if (res) {
      return res * capacity_per_shard;
    } else {
      return default_min; // min reservation
    }
  };

  auto get_lim = [&](double lim) {
    if (lim) {
      return lim * capacity_per_shard;
    } else {
      return default_max; // high limit
    }
  };

  // Set external client infos
  double res = conf.get_val<double>(
    "osd_mclock_scheduler_client_res");
  double lim = conf.get_val<double>(
    "osd_mclock_scheduler_client_lim");
  uint64_t wgt = conf.get_val<uint64_t>(
    "osd_mclock_scheduler_client_wgt");
  default_external_client_info.update(
    get_res(res),
    wgt,
    get_lim(lim));

  // Set background recovery client infos
  res = conf.get_val<double>(
    "osd_mclock_scheduler_background_recovery_res");
  lim = conf.get_val<double>(
    "osd_mclock_scheduler_background_recovery_lim");
  wgt = conf.get_val<uint64_t>(
    "osd_mclock_scheduler_background_recovery_wgt");
  internal_client_infos[
    static_cast<size_t>(op_scheduler_class::background_recovery)].update(
      get_res(res),
      wgt,
      get_lim(lim));

  // Set background best effort client infos
  res = conf.get_val<double>(
    "osd_mclock_scheduler_background_best_effort_res");
  lim = conf.get_val<double>(
    "osd_mclock_scheduler_background_best_effort_lim");
  wgt = conf.get_val<uint64_t>(
    "osd_mclock_scheduler_background_best_effort_wgt");
  internal_client_infos[
    static_cast<size_t>(op_scheduler_class::background_best_effort)].update(
      get_res(res),
      wgt,
      get_lim(lim));
}

const dmc::ClientInfo *mClockScheduler::ClientRegistry::get_external_client(
  const client_profile_id_t &client) const
{
  auto ret = external_client_infos.find(client);
  if (ret == external_client_infos.end())
    return &default_external_client_info;
  else
    return &(ret->second);
}

const dmc::ClientInfo *mClockScheduler::ClientRegistry::get_info(
  const scheduler_id_t &id) const {
  switch (id.class_id) {
  case op_scheduler_class::immediate:
    ceph_assert(0 == "Cannot schedule immediate");
    return (dmc::ClientInfo*)nullptr;
  case op_scheduler_class::client:
    return get_external_client(id.client_profile_id);
  default:
    ceph_assert(static_cast<size_t>(id.class_id) < internal_client_infos.size());
    return &internal_client_infos[static_cast<size_t>(id.class_id)];
  }
}

void mClockScheduler::set_osd_capacity_params_from_config()
{
  uint64_t osd_bandwidth_capacity;
  double osd_iop_capacity;
  std::tie(osd_bandwidth_capacity, osd_iop_capacity) = [&, this] {
    if (is_rotational) {
      return std::make_tuple(
        cct->_conf.get_val<Option::size_t>(
          "osd_mclock_max_sequential_bandwidth_hdd"),
        cct->_conf.get_val<double>("osd_mclock_max_capacity_iops_hdd"));
    } else {
      return std::make_tuple(
        cct->_conf.get_val<Option::size_t>(
          "osd_mclock_max_sequential_bandwidth_ssd"),
        cct->_conf.get_val<double>("osd_mclock_max_capacity_iops_ssd"));
    }
  }();

  osd_bandwidth_capacity = std::max<uint64_t>(1, osd_bandwidth_capacity);
  osd_iop_capacity = std::max<double>(1.0, osd_iop_capacity);

  osd_bandwidth_cost_per_io =
    static_cast<double>(osd_bandwidth_capacity) / osd_iop_capacity;
  osd_bandwidth_capacity_per_shard = static_cast<double>(osd_bandwidth_capacity)
    / static_cast<double>(num_shards);

  dout(1) << __func__ << ": osd_bandwidth_cost_per_io: "
          << std::fixed << std::setprecision(2)
          << osd_bandwidth_cost_per_io << " bytes/io"
          << ", osd_bandwidth_capacity_per_shard "
          << osd_bandwidth_capacity_per_shard << " bytes/second"
          << dendl;
}

void mClockScheduler::set_mclock_profile()
{
  mclock_profile = cct->_conf.get_val<std::string>("osd_mclock_profile");
  dout(1) << __func__ << " mclock profile: " << mclock_profile << dendl;
}

std::string mClockScheduler::get_mclock_profile()
{
  return mclock_profile;
}

// Sets allocations for 'balanced' mClock profile
//
// min and max specification:
//   0 (min): specifies no minimum reservation
//   0 (max): specifies no upper limit
//
//  Client Allocation:
//    reservation: 40% | weight: 1 | limit: 100% |
//  Background Recovery Allocation:
//    reservation: 40% | weight: 1 | limit: 70% |
//  Background Best Effort Allocation:
//    reservation: 20% | weight: 1 | limit: 0 (max) |
void mClockScheduler::set_balanced_profile_allocations()
{
  // Set [res, wgt, lim] in that order for each mClock client class.
  client_allocs[
    static_cast<size_t>(op_scheduler_class::client)].update(
      0.4, 1.0, 1.0);
  client_allocs[
    static_cast<size_t>(op_scheduler_class::background_recovery)].update(
      0.4, 1.0, 0.7);
  client_allocs[
    static_cast<size_t>(op_scheduler_class::background_best_effort)].update(
      0.2, 1.0, 0.0);
}

// Sets allocations for 'high_recovery_ops' mClock profile
//
// min and max specification:
//   0 (min): specifies no minimum reservation
//   0 (max): specifies no upper limit
//
// Client Allocation:
//   reservation: 30% | weight: 1 | limit: 80% |
// Background Recovery Allocation:
//   reservation: 60% | weight: 2 | limit: 0 (max) |
// Background Best Effort Allocation:
//   reservation: 0 (min) | weight: 1 | limit: 0 (max) |
void mClockScheduler::set_high_recovery_ops_profile_allocations()
{
  // Set [res, wgt, lim] in that order for each mClock client class.
  client_allocs[
    static_cast<size_t>(op_scheduler_class::client)].update(
      0.3, 1.0, 0.8);
  client_allocs[
    static_cast<size_t>(op_scheduler_class::background_recovery)].update(
      0.6, 2.0, 0.0);
  client_allocs[
    static_cast<size_t>(op_scheduler_class::background_best_effort)].update(
      0.0, 1.0, 0.0);
}

// Sets allocations for 'high_client_ops' mClock profile
//
// min and max specification:
//   0 (min): specifies no minimum reservation
//   0 (max): specifies no upper limit
//
// Client Allocation:
//   reservation: 60% | weight: 5 | limit: 0 (max) |
// Background Recovery Allocation:
//   reservation: 20% | weight: 1 | limit: 50% |
// Background Best Effort Allocation:
//   reservation: 20% | weight: 1 | limit: 0 (max) |
void mClockScheduler::set_high_client_ops_profile_allocations()
{
  // Set [res, wgt, lim] in that order for each mClock client class.
  client_allocs[
    static_cast<size_t>(op_scheduler_class::client)].update(
      0.6, 5.0, 0.0);
  client_allocs[
    static_cast<size_t>(op_scheduler_class::background_recovery)].update(
      0.2, 1.0, 0.5);
  client_allocs[
    static_cast<size_t>(op_scheduler_class::background_best_effort)].update(
      0.2, 1.0, 0.0);
}

void mClockScheduler::enable_mclock_profile_settings()
{
  // Nothing to do for "custom" profile
  if (mclock_profile == "custom") {
    return;
  }

  // Set mclock and ceph config options for the chosen profile
  if (mclock_profile == "balanced") {
    set_balanced_profile_allocations();
  } else if (mclock_profile == "high_recovery_ops") {
    set_high_recovery_ops_profile_allocations();
  } else if (mclock_profile == "high_client_ops") {
    set_high_client_ops_profile_allocations();
  } else {
    ceph_assert("Invalid choice of mclock profile" == 0);
    return;
  }

  // Set the mclock config parameters
  set_profile_config();
}

void mClockScheduler::set_profile_config()
{
  // Let only a single osd shard (id:0) set the profile configs
  if (shard_id > 0) {
    return;
  }

  ClientAllocs client = client_allocs[
    static_cast<size_t>(op_scheduler_class::client)];
  ClientAllocs rec = client_allocs[
    static_cast<size_t>(op_scheduler_class::background_recovery)];
  ClientAllocs best_effort = client_allocs[
    static_cast<size_t>(op_scheduler_class::background_best_effort)];

  // Set external client params
  cct->_conf.set_val_default("osd_mclock_scheduler_client_res",
    std::to_string(client.res));
  cct->_conf.set_val_default("osd_mclock_scheduler_client_wgt",
    std::to_string(uint64_t(client.wgt)));
  cct->_conf.set_val_default("osd_mclock_scheduler_client_lim",
    std::to_string(client.lim));
  dout(10) << __func__ << " client QoS params: " << "["
           << client.res << "," << client.wgt << "," << client.lim
           << "]" << dendl;

  // Set background recovery client params
  cct->_conf.set_val_default("osd_mclock_scheduler_background_recovery_res",
    std::to_string(rec.res));
  cct->_conf.set_val_default("osd_mclock_scheduler_background_recovery_wgt",
    std::to_string(uint64_t(rec.wgt)));
  cct->_conf.set_val_default("osd_mclock_scheduler_background_recovery_lim",
    std::to_string(rec.lim));
  dout(10) << __func__ << " Recovery QoS params: " << "["
           << rec.res << "," << rec.wgt << "," << rec.lim
           << "]" << dendl;

  // Set background best effort client params
  cct->_conf.set_val_default("osd_mclock_scheduler_background_best_effort_res",
    std::to_string(best_effort.res));
  cct->_conf.set_val_default("osd_mclock_scheduler_background_best_effort_wgt",
    std::to_string(uint64_t(best_effort.wgt)));
  cct->_conf.set_val_default("osd_mclock_scheduler_background_best_effort_lim",
    std::to_string(best_effort.lim));
  dout(10) << __func__ << " Best effort QoS params: " << "["
    << best_effort.res << "," << best_effort.wgt << "," << best_effort.lim
    << "]" << dendl;

  // Apply the configuration changes
  update_configuration();
}

uint32_t mClockScheduler::calc_scaled_cost(int item_cost)
{
  auto cost = static_cast<uint32_t>(
    std::max<int>(
      1, // ensure cost is non-zero and positive
      item_cost));
  auto cost_per_io = static_cast<uint32_t>(osd_bandwidth_cost_per_io);

  // Calculate total scaled cost in bytes
  return cost_per_io + cost;
}

void mClockScheduler::update_configuration()
{
  // Apply configuration change. The expectation is that
  // at least one of the tracked mclock config option keys
  // is modified before calling this method.
  cct->_conf.apply_changes(nullptr);
}

void mClockScheduler::dump(ceph::Formatter &f) const
{
  // Display queue sizes
  f.open_object_section("queue_sizes");
  f.dump_int("immediate", immediate.size());
  f.dump_int("scheduler", scheduler.request_count());
  f.close_section();

  // client map and queue tops (res, wgt, lim)
  std::ostringstream out;
  f.open_object_section("mClockClients");
  f.dump_int("client_count", scheduler.client_count());
  out << scheduler;
  f.dump_string("clients", out.str());
  f.close_section();

  // Display sorted queues (res, wgt, lim)
  f.open_object_section("mClockQueues");
  f.dump_string("queues", display_queues());
  f.close_section();
}

void mClockScheduler::enqueue(OpSchedulerItem&& item)
{
  auto id = get_scheduler_id(item);

  // TODO: move this check into OpSchedulerItem, handle backwards compat
  if (op_scheduler_class::immediate == id.class_id) {
    immediate.push_front(std::move(item));
  } else {
    auto cost = calc_scaled_cost(item.get_cost());
    item.set_qos_cost(cost);
    dout(20) << __func__ << " " << id
             << " item_cost: " << item.get_cost()
             << " scaled_cost: " << cost
             << dendl;

    // Add item to scheduler queue
    scheduler.add_request(
      std::move(item),
      id,
      cost);
  }

 dout(20) << __func__ << " client_count: " << scheduler.client_count()
          << " queue_sizes: [ imm: " << immediate.size()
          << " sched: " << scheduler.request_count() << " ]"
          << dendl;
 dout(30) << __func__ << " mClockClients: "
          << scheduler
          << dendl;
 dout(30) << __func__ << " mClockQueues: { "
          << display_queues() << " }"
          << dendl;
}

void mClockScheduler::enqueue_front(OpSchedulerItem&& item)
{
  immediate.push_back(std::move(item));
  // TODO: item may not be immediate, update mclock machinery to permit
  // putting the item back in the queue
}

WorkItem mClockScheduler::dequeue()
{
  if (!immediate.empty()) {
    WorkItem work_item{std::move(immediate.back())};
    immediate.pop_back();
    return work_item;
  } else {
    mclock_queue_t::PullReq result = scheduler.pull_request();
    if (result.is_future()) {
      return result.getTime();
    } else if (result.is_none()) {
      ceph_assert(
	0 == "Impossible, must have checked empty() first");
      return {};
    } else {
      ceph_assert(result.is_retn());

      auto &retn = result.get_retn();
      return std::move(*retn.request);
    }
  }
}

std::string mClockScheduler::display_queues() const
{
  std::ostringstream out;
  scheduler.display_queues(out);
  return out.str();
}

const char** mClockScheduler::get_tracked_conf_keys() const
{
  static const char* KEYS[] = {
    "osd_mclock_scheduler_client_res",
    "osd_mclock_scheduler_client_wgt",
    "osd_mclock_scheduler_client_lim",
    "osd_mclock_scheduler_background_recovery_res",
    "osd_mclock_scheduler_background_recovery_wgt",
    "osd_mclock_scheduler_background_recovery_lim",
    "osd_mclock_scheduler_background_best_effort_res",
    "osd_mclock_scheduler_background_best_effort_wgt",
    "osd_mclock_scheduler_background_best_effort_lim",
    "osd_mclock_max_capacity_iops_hdd",
    "osd_mclock_max_capacity_iops_ssd",
    "osd_mclock_max_sequential_bandwidth_hdd",
    "osd_mclock_max_sequential_bandwidth_ssd",
    "osd_mclock_profile",
    NULL
  };
  return KEYS;
}

void mClockScheduler::handle_conf_change(
  const ConfigProxy& conf,
  const std::set<std::string> &changed)
{
  if (changed.count("osd_mclock_max_capacity_iops_hdd") ||
      changed.count("osd_mclock_max_capacity_iops_ssd")) {
    set_osd_capacity_params_from_config();
    if (mclock_profile != "custom") {
      enable_mclock_profile_settings();
    }
    client_registry.update_from_config(
      conf, osd_bandwidth_capacity_per_shard);
  }
  if (changed.count("osd_mclock_max_sequential_bandwidth_hdd") ||
      changed.count("osd_mclock_max_sequential_bandwidth_ssd")) {
    set_osd_capacity_params_from_config();
    client_registry.update_from_config(
      conf, osd_bandwidth_capacity_per_shard);
  }
  if (changed.count("osd_mclock_profile")) {
    set_mclock_profile();
    if (mclock_profile != "custom") {
      enable_mclock_profile_settings();
      client_registry.update_from_config(
        conf, osd_bandwidth_capacity_per_shard);
    }
  }

  auto get_changed_key = [&changed]() -> std::optional<std::string> {
    static const std::vector<std::string> qos_params = {
      "osd_mclock_scheduler_client_res",
      "osd_mclock_scheduler_client_wgt",
      "osd_mclock_scheduler_client_lim",
      "osd_mclock_scheduler_background_recovery_res",
      "osd_mclock_scheduler_background_recovery_wgt",
      "osd_mclock_scheduler_background_recovery_lim",
      "osd_mclock_scheduler_background_best_effort_res",
      "osd_mclock_scheduler_background_best_effort_wgt",
      "osd_mclock_scheduler_background_best_effort_lim"
    };

    for (auto &qp : qos_params) {
      if (changed.count(qp)) {
        return qp;
      }
    }
    return std::nullopt;
  };

  if (auto key = get_changed_key(); key.has_value()) {
    if (mclock_profile == "custom") {
      client_registry.update_from_config(
        conf, osd_bandwidth_capacity_per_shard);
    } else {
      // Attempt to change QoS parameter for a built-in profile. Restore the
      // profile defaults by making one of the OSD shards remove the key from
      // config monitor store. Note: monc is included in the check since the
      // mock unit test currently doesn't initialize it.
      if (shard_id == 0 && monc) {
        static const std::vector<std::string> osds = {
          "osd",
          "osd." + std::to_string(whoami)
        };

        for (auto osd : osds) {
          std::string cmd =
            "{"
              "\"prefix\": \"config rm\", "
              "\"who\": \"" + osd + "\", "
              "\"name\": \"" + *key + "\""
            "}";
          std::vector<std::string> vcmd{cmd};

          dout(10) << __func__ << " Removing Key: " << *key
                   << " for " << osd << " from Mon db" << dendl;
          monc->start_mon_command(vcmd, {}, nullptr, nullptr, nullptr);
        }
      }
    }
  }
}

mClockScheduler::~mClockScheduler()
{
  cct->_conf.remove_observer(this);
}

}
