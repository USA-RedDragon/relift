// SPDX-License-Identifier: MIT
// The ares platform of the Master System and Game Gear drivers: paks from
// memory, video to the trace writer, audio drained, input from the script.
#pragma once
#include <ares/ares.hpp>
#include "oracle.hpp"

namespace {

struct OraclePlatform : ares::Platform {
  std::shared_ptr<vfs::directory> cartPak;
  /** The console's pak: empty, or a BIOS image with --bios. */
  std::shared_ptr<vfs::directory> sysPak = std::make_shared<vfs::directory>();
  /** The Master System's Pause button takes the script's Start. */
  bool pauseIsStart = false;
  auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
    auto n = node->name();
    if(n == "Master System" || n == "Game Gear") return sysPak;
    return cartPak;
  }
  auto video(ares::Node::Video::Screen, const u32* data, u32 pitch, u32 width, u32 height) -> void override {
    orc::onVideo(data, pitch, width, height);
  }
  auto audio(ares::Node::Audio::Stream stream) -> void override {
    f64 samples[2];
    while(stream->pending()) stream->read(samples);
  }
  auto input(ares::Node::Input::Input node) -> void override {
    if(auto button = std::dynamic_pointer_cast<ares::Core::Input::Button>(node)) {
      std::string n = (const char*)node->name();
      bool v = orc::buttonHeld(n);
      if(n == "1") v = v || orc::buttonHeld("a");
      if(n == "2") v = v || orc::buttonHeld("b");
      if(n == "Pause") v = v || (pauseIsStart && orc::buttonHeld("start"));
      button->setValue(v);
    }
  }
};

}  // namespace
