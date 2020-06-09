/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2020 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "caf/detail/core_export.hpp"
#include "caf/fwd.hpp"

namespace caf::telemetry {

/// Manages a collection (family) of metrics. All children share the same
/// prefix, name, and label dimensions.
class CAF_CORE_EXPORT metric_family {
public:
  // -- constructors, destructors, and assignment operators --------------------

  metric_family(std::string prefix, std::string name,
                std::vector<std::string> label_names, std::string helptext)
    : prefix_(std::move(prefix)),
      name_(std::move(name)),
      label_names_(std::move(label_names)),
      helptext_(std::move(helptext)) {
    // nop
  }

  virtual ~metric_family();

  // -- properties -------------------------------------------------------------

  const auto& prefix() const noexcept {
    return prefix_;
  }

  const auto& name() const noexcept {
    return name_;
  }

  const auto& label_names() const noexcept {
    return label_names_;
  }

  const auto& helptext() const noexcept {
    return helptext_;
  }

  virtual void scrape() = 0;

private:
  std::string prefix_;
  std::string name_;
  std::vector<std::string> label_names_;
  std::string helptext_;
};

} // namespace caf::telemetry
