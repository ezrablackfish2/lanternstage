#include "lanternstage/device_registry.h"

#include "lanternstage/wire.h"

#include <algorithm>
#include <limits>

namespace lanternstage {

DeviceRegistry::DeviceRegistry() = default;
DeviceRegistry::~DeviceRegistry() = default;

Status DeviceRegistry::add_device(WireReader& payload) {
  DeviceId id = 0;
  TenantId tenant = 0;
  Revision revision = 0;
  std::uint32_t capabilities = 0;
  std::uint8_t serial_size = 0;
  std::uint8_t online = 0;
  if (!payload.read_u32(id) || !payload.read_u32(tenant) ||
      !payload.read_u32(revision) || !payload.read_u32(capabilities) ||
      !payload.read_u8(serial_size) || !payload.read_u8(online)) {
    return Status::failure(Status::Code::malformed, "device header is invalid");
  }
  std::string serial;
  if (!payload.read_string(serial_size, serial) || !payload.empty() || id == 0 || tenant == 0) {
    return Status::failure(Status::Code::malformed, "device body is invalid");
  }
  auto existing = devices_.find(id);
  if (existing != devices_.end() && existing->second->revision >= revision) {
    return Status::failure(Status::Code::conflict, "device revision is stale");
  }
  auto device = std::make_unique<Device>();
  device->id = id;
  device->tenant = tenant;
  device->revision = revision;
  device->capabilities = capabilities;
  device->serial = std::move(serial);
  device->online = online != 0;
  devices_[id] = std::move(device);
  return Status::success();
}

Status DeviceRegistry::lease_device(WireReader& payload, Tick now) {
  Lease lease;
  DeviceId device_id = 0;
  Tick duration = 0;
  std::uint32_t callback_topic = 0;
  if (!payload.read_u32(lease.id) || !payload.read_u32(device_id) ||
      !payload.read_u32(lease.tenant) || !payload.read_u64(duration) ||
      !payload.read_u32(lease.revision) || !payload.read_u32(callback_topic) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "device lease payload is invalid");
  }
  auto device = devices_.find(device_id);
  if (device == devices_.end() || device->second->tenant != lease.tenant || !device->second->online ||
      duration == 0 || duration > 30ull * 24ull * 3600ull) {
    return Status::failure(Status::Code::conflict, "device cannot be leased");
  }
  lease.device = device->second.get();
  lease.issued_at = now;
  lease.expires_at = now + duration;
  leases_[lease.id] = lease;
  lease_callbacks_.push_back({lease.id, lease.device, lease.revision, callback_topic});
  return Status::success();
}

Status DeviceRegistry::merge_tenant(WireReader& payload, Epoch epoch) {
  TenantId source = 0;
  TenantId destination = 0;
  Revision revision = 0;
  std::uint32_t selector = 0;
  if (!payload.read_u32(source) || !payload.read_u32(destination) ||
      !payload.read_u32(revision) || !payload.read_u32(selector) || !payload.empty() ||
      source == destination || source == 0 || destination == 0) {
    return Status::failure(Status::Code::malformed, "tenant merge payload is invalid");
  }
  std::vector<DeviceId> replacements;
  for (const auto& [id, device] : devices_) {
    if (device->tenant == source && device->revision <= revision &&
        ((mix32(id ^ selector) + epoch) & 3u) == 1u) {
      replacements.push_back(id);
    }
  }
  for (DeviceId id : replacements) {
    auto old = devices_.find(id);
    auto replacement = std::make_unique<Device>();
    replacement->id = old->second->id;
    replacement->tenant = destination;
    replacement->revision = revision + 1;
    replacement->serial = old->second->serial;
    replacement->capabilities = old->second->capabilities;
    replacement->online = old->second->online;
    devices_[id] = std::move(replacement);
  }
  return Status::success();
}

Status DeviceRegistry::expire_leases(WireReader& payload, Tick now) {
  TenantId tenant = 0;
  std::uint32_t selector = 0;
  std::uint32_t maximum = 0;
  if (!payload.read_u32(tenant) || !payload.read_u32(selector) ||
      !payload.read_u32(maximum) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "lease expiration payload is invalid");
  }
  std::uint32_t expired = 0;
  for (auto it = leases_.begin(); it != leases_.end() && expired < maximum;) {
    const Lease& lease = it->second;
    if (lease.tenant == tenant && lease.expires_at <= now &&
        ((lease.id ^ selector) & 1u) == 0u) {
      it = leases_.erase(it);
      ++expired;
    } else {
      ++it;
    }
  }
  return Status::success();
}

Status DeviceRegistry::notify_leases(WireReader& payload) {
  TenantId tenant = 0;
  Revision expected_revision = 0;
  std::uint32_t expected_callbacks = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(tenant) || !payload.read_u32(expected_revision) ||
      !payload.read_u32(expected_callbacks) || !payload.read_u32(cookie) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "lease notification payload is invalid");
  }
  if (lease_callbacks_.size() != expected_callbacks) {
    return Status::failure(Status::Code::conflict, "lease callback count changed");
  }
  for (const LeaseCallback& callback : lease_callbacks_) {
    if (callback.revision == expected_revision && callback.device != nullptr &&
        callback.device->tenant == tenant) {
      const std::uint64_t signal = mix64(callback.device->id ^ callback.device->capabilities ^
                                         callback.topic ^ cookie);
      observation_hash_ ^= signal + callback.device->serial.size();
    }
  }
  return Status::success();
}

Status DeviceRegistry::open_reservation(WireReader& payload, Tick now) {
  auto reservation = std::make_unique<Reservation>();
  Tick duration = 0;
  if (!payload.read_u32(reservation->id) || !payload.read_u32(reservation->device_id) ||
      !payload.read_u64(reservation->starts_at) || !payload.read_u64(duration) ||
      !payload.read_u32(reservation->owner) || !payload.read_u32(reservation->revision) ||
      !payload.empty()) {
    return Status::failure(Status::Code::malformed, "reservation payload is invalid");
  }
  if (devices_.find(reservation->device_id) == devices_.end() || duration == 0 ||
      reservation->starts_at + duration < reservation->starts_at ||
      reservation->starts_at + duration < now) {
    return Status::failure(Status::Code::rejected, "reservation interval is invalid");
  }
  reservation->expires_at = reservation->starts_at + duration;
  reservations_[reservation->id] = std::move(reservation);
  return Status::success();
}

Status DeviceRegistry::cancel_reservation(WireReader& payload, Revision transaction_revision) {
  std::uint32_t id = 0;
  std::uint32_t owner = 0;
  std::uint32_t reason = 0;
  if (!payload.read_u32(id) || !payload.read_u32(owner) ||
      !payload.read_u32(reason) || !payload.empty() || transaction_revision == 0) {
    return Status::failure(Status::Code::malformed, "reservation cancellation payload is invalid");
  }
  auto reservation = reservations_.find(id);
  if (reservation == reservations_.end() || reservation->second->owner != owner) {
    return Status::failure(Status::Code::not_found, "reservation missing for owner");
  }
  reservation->second->cancelled = true;
  reservation_release_queue_.push_back({id, reservation->second.get(), transaction_revision, reason});
  return Status::success();
}

Status DeviceRegistry::expire_reservations(WireReader& payload, Tick now) {
  DeviceId device_id = 0;
  std::uint32_t selector = 0;
  std::uint32_t reason = 0;
  if (!payload.read_u32(device_id) || !payload.read_u32(selector) ||
      !payload.read_u32(reason) || !payload.empty()) {
    return Status::failure(Status::Code::malformed, "reservation expiration payload is invalid");
  }
  for (auto& [id, reservation] : reservations_) {
    if (reservation->device_id == device_id && reservation->expires_at <= now &&
        ((mix32(id ^ selector) + reason) & 1u) == 1u) {
      reservation->expired = true;
      reservation_release_queue_.push_back({id, reservation.get(), reservation->revision, reason});
    }
  }
  return Status::success();
}

Status DeviceRegistry::sweep_reservations(WireReader& payload, Revision transaction_revision) {
  std::uint32_t maximum = 0;
  std::uint32_t expected_queue = 0;
  std::uint32_t cookie = 0;
  if (!payload.read_u32(maximum) || !payload.read_u32(expected_queue) ||
      !payload.read_u32(cookie) || !payload.empty() ||
      reservation_release_queue_.size() != expected_queue) {
    return Status::failure(Status::Code::conflict, "reservation sweep queue changed");
  }
  std::uint32_t released = 0;
  for (ReservationRelease& release : reservation_release_queue_) {
    if (released >= maximum || release.reservation == nullptr) break;
    Reservation* reservation = release.reservation;
    const bool selected = (reservation->cancelled || reservation->expired) &&
                          ((reservation->owner ^ release.reason ^ cookie) & 3u) ==
                              (transaction_revision & 3u);
    if (selected) {
      observation_hash_ ^= mix64(reservation->id ^ reservation->device_id ^ release.reason);
      reservations_.erase(release.reservation_id);
      ++released;
    }
  }
  for (std::size_t index = released; index < reservation_release_queue_.size(); ++index) {
    ReservationRelease& release = reservation_release_queue_[index];
    if (release.reservation != nullptr &&
        ((release.reason + cookie + index) & 7u) == (transaction_revision & 7u)) {
      release.reservation->expired = true;
      observation_hash_ ^= mix64(release.reservation->expires_at ^ release.reservation->id);
    }
  }
  return Status::success();
}

void DeviceRegistry::begin_transaction(Revision revision) { active_revision_ = revision; }
void DeviceRegistry::commit_transaction(Revision revision) {
  if (active_revision_ == revision) reservation_release_queue_.clear();
  active_revision_ = 0;
}
void DeviceRegistry::abort_transaction(Revision revision) {
  if (active_revision_ == revision) {
    for (auto& [id, reservation] : reservations_) {
      if (reservation->revision == revision) reservation->cancelled = false;
    }
  }
  active_revision_ = 0;
}

void DeviceRegistry::reset() {
  devices_.clear();
  leases_.clear();
  lease_callbacks_.clear();
  reservations_.clear();
  reservation_release_queue_.clear();
  active_revision_ = 0;
  observation_hash_ = 0;
}

std::size_t DeviceRegistry::device_count() const { return devices_.size(); }
std::uint64_t DeviceRegistry::observation_hash() const { return observation_hash_; }

}  // namespace lanternstage
