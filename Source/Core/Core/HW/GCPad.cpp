// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/GCPad.h"

#include "Common/Common.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/System.h"
#include "Core/HW/SystemTimers.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/GCPadStatus.h"
#include "InputCommon/InputConfig.h"

namespace Pad
{
static InputConfig s_config("GCPadNew", _trans("Pad"), "GCPad", "Pad");
InputConfig* GetConfig()
{
  return &s_config;
}

void Shutdown()
{
  s_config.UnregisterHotplugCallback();

  s_config.ClearControllers();
}

void Initialize()
{
  if (s_config.ControllersNeedToBeCreated())
  {
    for (unsigned int i = 0; i < 4; ++i)
      s_config.CreateController<GCPad>(i);
  }

  s_config.RegisterHotplugCallback();

  // Load the saved controller config
  s_config.LoadConfig();
}

void LoadConfig()
{
  s_config.LoadConfig();
}

void GenerateDynamicInputTextures()
{
  s_config.GenerateControllerTextures();
}

bool IsInitialized()
{
  return !s_config.ControllersNeedToBeCreated();
}

GCPadStatus GetStatus(int pad_num)
{
  GCPadStatus status = static_cast<GCPad*>(s_config.GetController(pad_num))->GetInput();
  ApplyReplayBootInput(pad_num, &status);
  return status;
}

void ApplyReplayBootInput(int pad_num, GCPadStatus* status)
{
  if (pad_num == 0 && Config::Get(Config::MAIN_REPLAY_PLAYBACK_PROJECT_PLUS))
  {
    auto& system = Core::System::GetInstance();
    const u64 hold_ticks = static_cast<u64>(system.GetSystemTimers().GetTicksPerSecond()) * 10;
    if (system.GetCoreTiming().GetTicks() < hold_ticks)
      status->button |= PAD_TRIGGER_Z;
  }
}

ControllerEmu::ControlGroup* GetGroup(int pad_num, PadGroup group)
{
  return static_cast<GCPad*>(s_config.GetController(pad_num))->GetGroup(group);
}

void Rumble(const int pad_num, const ControlState strength)
{
  static_cast<GCPad*>(s_config.GetController(pad_num))->SetOutput(strength);
}

void ResetRumble(const int pad_num)
{
  static_cast<GCPad*>(s_config.GetController(pad_num))->SetOutput(0.0);
}

bool GetMicButton(const int pad_num)
{
  return static_cast<GCPad*>(s_config.GetController(pad_num))->GetMicButton();
}
}  // namespace Pad
