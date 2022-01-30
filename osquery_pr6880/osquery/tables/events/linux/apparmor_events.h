/**
 * Copyright (c) 2014-present, The osquery authors
 *
 * This source code is licensed as defined by the LICENSE file found in the
 * root directory of this source tree.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR GPL-2.0-only)
 */

#pragma once

#include <linux/audit.h>

#include <osquery/events/eventsubscriber.h>
#include <osquery/events/linux/apparmor_events.h>
#include <osquery/events/linux/auditeventpublisher.h>

namespace osquery {
class AppArmorEventSubscriber final
    : public EventSubscriber<AuditEventPublisher> {
 public:
  /// The process event subscriber declares an audit event type subscription.
  Status init();

  /// Kernel events matching the event type will fire.
  Status callback(const ECRef& ec, const SCRef& sc);

  /// Processes the updates received from the callback
  static Status processEvents(
      QueryData& emitted_row_list,
      const std::vector<AuditEvent>& event_list) noexcept;

  /// Returns the set of events that this subscriber can handle
  static const std::set<int>& getEventSet() noexcept;
};
} // namespace osquery
