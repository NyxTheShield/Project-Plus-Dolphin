// Copyright 2026 Brawlback Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <optional>
#include <utility>

#include <picojson.h>

namespace Core::ManagedLaunch
{
struct ValidationResult
{
  bool valid = false;
  std::string error;

  explicit operator bool() const { return valid; }
};

struct ParseResult;

class Ticket final
{
public:
  const picojson::object& GetFields() const { return m_fields; }

private:
  friend struct ParseResult;
  friend ParseResult ParseTicket(const picojson::value& root);
  explicit Ticket(picojson::object fields) : m_fields(std::move(fields)) {}

  picojson::object m_fields;
};

struct ParseResult
{
  std::optional<Ticket> ticket;
  std::string error;

  explicit operator bool() const { return ticket.has_value(); }
};

ValidationResult ValidateTicket(const picojson::value& root);
ParseResult ParseTicket(const picojson::value& root);
std::string NormalizedSummary(const picojson::value& root);
std::string FormatTicketForLog(const picojson::value& root);
}  // namespace Core::ManagedLaunch
