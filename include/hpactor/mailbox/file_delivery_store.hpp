// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hpactor/msg/durable_delivery_store.hpp>
#include <mutex>
#include <string>

namespace hpactor::mailbox {

/// \brief File-backed delivery store that survives process restart.
///
/// Persists outbox and inbox records as flat text files in a
/// configurable root directory. Writes are made atomic via
/// temporary-file rename so partial writes are never visible.
class FileDeliveryStore : public msg::DurableDeliveryStore {
  public:
    explicit FileDeliveryStore(std::string root_dir);

    result<void> put_outbox(const msg::PendingSend& record) override;
    result<void> mark_outbox_complete(MessageId id) override;
    result<std::vector<msg::PendingSend>> load_pending_outbox() override;
    result<void> put_inbox(MessageId id, uint64_t ttl_ns) override;
    result<bool> seen_inbox(MessageId id) override;

  private:
    std::string outbox_path() const;
    std::string inbox_path() const;
    std::string root_dir_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::mailbox
