// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

class NetPlayChatUI
{
public:
  NetPlayChatUI(std::string local_name, std::function<void(const std::string&)> callback);
  ~NetPlayChatUI();

  using Color = std::array<float, 3>;

  void Display();
  void AppendChat(std::string message, Color color);
  void SendChatMessage();
  void Activate();
  void Collapse();
  void Expand();

private:
  struct Message
  {
    std::string text;
    Color color;
    std::chrono::steady_clock::time_point expires_at;
  };

  char m_message_buf[256] = {};
  std::string m_local_name;
  std::atomic_bool m_open_input = false;
  std::atomic_bool m_close_input = false;
  std::atomic_bool m_input_active = false;
  bool m_input_open = false;
  bool m_focus_input = false;

  std::mutex m_messages_mutex;
  std::deque<Message> m_messages;
  std::function<void(const std::string&)> m_message_callback;
};

extern std::unique_ptr<NetPlayChatUI> g_netplay_chat_ui;
