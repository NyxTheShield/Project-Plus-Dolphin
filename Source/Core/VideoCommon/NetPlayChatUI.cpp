// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/NetPlayChatUI.h"
#include "Core/Config/GraphicsSettings.h"

#include <algorithm>
#include <cstdint>

#include <imgui.h>

constexpr size_t MAX_BACKLOG_SIZE = 100;
constexpr size_t VISIBLE_MESSAGE_COUNT = 3;
constexpr float CHAT_FONT_SCALE = 1.25f;
constexpr std::chrono::seconds MESSAGE_LIFETIME{10};

constexpr std::array<NetPlayChatUI::Color, 6> USER_COLORS{{
    {0.38f, 0.86f, 1.0f},
    {1.0f, 0.68f, 0.32f},
    {0.58f, 1.0f, 0.52f},
    {1.0f, 0.52f, 0.78f},
    {0.78f, 0.65f, 1.0f},
    {1.0f, 0.88f, 0.38f},
}};

NetPlayChatUI::Color GetUserColor(std::string_view name)
{
  std::uint32_t hash = 2166136261u;
  for (const unsigned char character : name)
  {
    hash ^= character;
    hash *= 16777619u;
  }
  return USER_COLORS[hash % USER_COLORS.size()];
}

void DrawTextShadow(ImVec2 position, std::string_view text, float wrap_width)
{
  ImGui::GetWindowDrawList()->AddText(
      ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(position.x + 1.0f, position.y + 1.0f),
      IM_COL32(0, 0, 0, 220), text.data(), text.data() + text.size(), wrap_width);
}

std::unique_ptr<NetPlayChatUI> g_netplay_chat_ui;

NetPlayChatUI::NetPlayChatUI(std::string local_name,
                             std::function<void(const std::string&)> callback)
    : m_local_name{std::move(local_name)}, m_message_callback{std::move(callback)}
{
}

NetPlayChatUI::~NetPlayChatUI() = default;

void NetPlayChatUI::Display()
{
  if (m_close_input.exchange(false, std::memory_order_acq_rel))
  {
    m_input_open = false;
    m_focus_input = false;
    m_input_active.store(false, std::memory_order_release);
  }

  if (m_open_input.exchange(false, std::memory_order_acq_rel))
  {
    m_input_open = true;
    m_focus_input = true;
  }

  if (!Config::Get(Config::GFX_SHOW_NETPLAY_MESSAGES))
    return;

  const float scale = ImGui::GetIO().DisplayFramebufferScale.x;
  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const float overlay_width = std::max(200.0f * scale,
                                       std::min(620.0f * scale, display_size.x - 24.0f * scale));

  ImGui::SetNextWindowPos(ImVec2(12.0f * scale, display_size.y - 12.0f * scale),
                          ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2(overlay_width, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground;
  if (!m_input_open)
    flags |= ImGuiWindowFlags_NoInputs;

  if (ImGui::Begin("##NetplayChatOverlay", nullptr, flags))
  {
    ImGui::SetWindowFontScale(CHAT_FONT_SCALE);

    std::deque<Message> visible_messages;
    {
      std::lock_guard guard(m_messages_mutex);
      const auto now = std::chrono::steady_clock::now();
      while (!m_messages.empty() && m_messages.front().expires_at <= now)
        m_messages.pop_front();

      const size_t first_message =
          m_messages.size() > VISIBLE_MESSAGE_COUNT ? m_messages.size() - VISIBLE_MESSAGE_COUNT : 0;
      visible_messages.insert(visible_messages.end(), m_messages.begin() + first_message,
                              m_messages.end());
    }

    for (const Message& message : visible_messages)
    {
      const float wrap_width = overlay_width - 4.0f * scale;
      const size_t separator = message.text.find(": ");
      if (separator != std::string::npos)
      {
        const std::string_view sender{message.text.data(), separator + 1};
        const std::string_view body{message.text.data() + separator + 2,
                                    message.text.size() - separator - 2};
        const Color sender_color = GetUserColor(sender);
        DrawTextShadow(ImGui::GetCursorScreenPos(), sender, 0.0f);
        ImGui::TextColored(ImVec4(sender_color[0], sender_color[1], sender_color[2], 1.0f), "%.*s",
                           static_cast<int>(sender.size()), sender.data());
        ImGui::SameLine(0.0f, 5.0f * scale);
        const float body_wrap_width =
            std::max(1.0f, wrap_width - (ImGui::GetCursorScreenPos().x - ImGui::GetWindowPos().x));
        DrawTextShadow(ImGui::GetCursorScreenPos(), body, body_wrap_width);
        ImGui::PushTextWrapPos(ImGui::GetWindowPos().x + wrap_width);
        ImGui::TextColored(
            ImVec4(message.color[0], message.color[1], message.color[2], 1.0f), "%.*s",
            static_cast<int>(body.size()), body.data());
        ImGui::PopTextWrapPos();
      }
      else
      {
        DrawTextShadow(ImGui::GetCursorScreenPos(), message.text, wrap_width);
        ImGui::PushTextWrapPos(ImGui::GetWindowPos().x + wrap_width);
        ImGui::TextColored(
            ImVec4(message.color[0], message.color[1], message.color[2], 1.0f), "%s",
            message.text.c_str());
        ImGui::PopTextWrapPos();
      }
    }

    if (m_input_open)
    {
      if (!visible_messages.empty())
        ImGui::Spacing();

      const std::string prompt = (m_local_name.empty() ? "Player" : m_local_name) + ":";
      ImGui::TextUnformatted(prompt.c_str());
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.58f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.66f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.72f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
      ImGui::PushItemWidth(-1.0f);
      if (m_focus_input)
      {
        ImGui::SetKeyboardFocusHere();
        m_focus_input = false;
      }

      const bool submitted =
          ImGui::InputText("##NetplayMessageBuffer", m_message_buf, IM_ARRAYSIZE(m_message_buf),
                           ImGuiInputTextFlags_EnterReturnsTrue);
      m_input_active.store(ImGui::IsItemActive(), std::memory_order_release);
      if (submitted)
        SendChatMessage();
      else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
      {
        m_input_open = false;
        m_input_active.store(false, std::memory_order_release);
      }

      ImGui::PopItemWidth();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(3);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void NetPlayChatUI::AppendChat(std::string message, Color color)
{
  std::lock_guard guard(m_messages_mutex);
  if (m_messages.size() >= MAX_BACKLOG_SIZE)
    m_messages.pop_front();

  m_messages.push_back(
      Message{std::move(message), color, std::chrono::steady_clock::now() + MESSAGE_LIFETIME});
}

void NetPlayChatUI::SendChatMessage()
{
  // Check whether the input field is empty
  if (m_message_buf[0] != '\0')
  {
    if (m_message_callback)
      m_message_callback(m_message_buf);

    m_message_buf[0] = '\0';
  }

  m_input_open = false;
  m_input_active.store(false, std::memory_order_release);
}

void NetPlayChatUI::Activate()
{
  // Let T be typed normally while the chat input owns the keyboard.
  if (m_input_active.load(std::memory_order_acquire))
    return;

  Config::SetCurrent(Config::GFX_SHOW_NETPLAY_MESSAGES, true);
  m_open_input.store(true, std::memory_order_release);
}

void NetPlayChatUI::Collapse()
{
  Config::SetCurrent(Config::GFX_SHOW_NETPLAY_MESSAGES, false);
  m_close_input.store(true, std::memory_order_release);
}

void NetPlayChatUI::Expand()
{
  Config::SetCurrent(Config::GFX_SHOW_NETPLAY_MESSAGES, true);
}
