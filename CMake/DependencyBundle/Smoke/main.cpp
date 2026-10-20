#include <cstdio>
#include <SDL3/SDL_version.h>
#include <SFML/Network/Packet.hpp>
#include <fmt/format.h>
#include <glslang/Public/ShaderLang.h>
#include <libusb.h>
#include <mbedtls/sha256.h>

int main()
{
  sf::Packet packet;
  packet << 42;
  int value = 0;
  packet >> value;
  unsigned char digest[32];
  const unsigned char input[] = {'a', 'b', 'c'};
  if (!packet || value != 42 || SDL_GetVersion() < 3002000 || !libusb_get_version() ||
      mbedtls_sha256_ret(input, sizeof(input), digest, 0) != 0 ||
      digest[0] != 0xba || digest[31] != 0xad || !glslang::InitializeProcess())
    return 1;
  glslang::FinalizeProcess();
  std::puts(fmt::format("Dependency bundle linked successfully (SDL {})", SDL_GetVersion()).c_str());
  return 0;
}
