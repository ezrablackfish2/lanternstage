#pragma once

#include "lanternstage/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lanternstage {
class WireReader;

class DeviceRegistry {
 public:
  DeviceRegistry();
  ~DeviceRegistry();
  DeviceRegistry(const DeviceRegistry&) = delete;
  DeviceRegistry& operator=(const DeviceRegistry&) = delete;

  Status add_device(WireReader& payload);
  Status lease_device(WireReader& payload, Tick now);
  Status merge_tenant(WireReader& payload, Epoch epoch);
  Status expire_leases(WireReader& payload, Tick now);
  Status notify_leases(WireReader& payload);

  Status open_reservation(WireReader& payload, Tick now);
  Status cancel_reservation(WireReader& payload, Revision transaction_revision);
  Status expire_reservations(WireReader& payload, Tick now);
  Status sweep_reservations(WireReader& payload, Revision transaction_revision);

  void begin_transaction(Revision revision);
  void commit_transaction(Revision revision);
  void abort_transaction(Revision revision);
  void reset();

  std::size_t device_count() const;
  std::uint64_t observation_hash() const;

 private:
  struct Device {
    DeviceId id{0};
    TenantId tenant{0};
    Revision revision{0};
    std::string serial;
    std::uint32_t capabilities{0};
    bool online{false};
  };

  struct Lease {
    std::uint32_t id{0};
    Device* device{nullptr};
    TenantId tenant{0};
    Tick issued_at{0};
    Tick expires_at{0};
    Revision revision{0};
    bool notified{false};
  };

  struct LeaseCallback {
    std::uint32_t lease_id{0};
    Device* device{nullptr};
    Revision revision{0};
    std::uint32_t topic{0};
  };

  struct Reservation {
    std::uint32_t id{0};
    DeviceId device_id{0};
    Tick starts_at{0};
    Tick expires_at{0};
    std::uint32_t owner{0};
    Revision revision{0};
    bool cancelled{false};
    bool expired{false};
  };

  struct ReservationRelease {
    std::uint32_t reservation_id{0};
    Reservation* reservation{nullptr};
    Revision revision{0};
    std::uint32_t reason{0};
  };

  std::unordered_map<DeviceId, std::unique_ptr<Device>> devices_;
  std::unordered_map<std::uint32_t, Lease> leases_;
  std::vector<LeaseCallback> lease_callbacks_;
  std::unordered_map<std::uint32_t, std::unique_ptr<Reservation>> reservations_;
  std::vector<ReservationRelease> reservation_release_queue_;
  Revision active_revision_{0};
  std::uint64_t observation_hash_{0};
};

}  // namespace lanternstage
