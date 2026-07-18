#pragma once

#include <cstddef>
#include <string_view>

namespace gitcube {

// Cube-with-a-planet favicon, in three forms: a hand-authored SVG (crisp at any size,
// used by modern browsers), a multi-resolution ICO (16/32/48/64px, the legacy
// /favicon.ico browsers still probe for), and a 180x180 PNG for apple-touch-icon /
// bookmark contexts. All three are generated from the same design; see
// docs/favicon/ for the source SVG and the script that rendered the raster copies.
inline constexpr std::string_view kFaviconSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <defs>
    <radialGradient id="planet" cx="35%" cy="30%" r="75%">
      <stop offset="0%" stop-color="#6cafc7"/>
      <stop offset="55%" stop-color="#3f7e96"/>
      <stop offset="100%" stop-color="#20455a"/>
    </radialGradient>
  </defs>

  <!-- cube: left, right, top faces -->
  <polygon points="6.4,19.2 32,32 32,53.76 6.4,40.96" fill="#8a5a2b" stroke="#3a2614" stroke-width="1.6" stroke-linejoin="round"/>
  <polygon points="57.6,19.2 32,32 32,53.76 57.6,40.96" fill="#b4791f" stroke="#3a2614" stroke-width="1.6" stroke-linejoin="round"/>
  <polygon points="32,6.4 6.4,19.2 32,32 57.6,19.2" fill="#e8c98a" stroke="#3a2614" stroke-width="1.6" stroke-linejoin="round"/>

  <!-- ring: back half (behind the sphere) -->
  <path d="M 15.76,26.24 A 16.24,5.75 0 0 1 48.24,26.24" fill="none" stroke="#f5ebd6" stroke-width="1.5" stroke-linecap="round"/>

  <!-- planet sphere -->
  <circle cx="32" cy="26.24" r="9.28" fill="url(#planet)"/>

  <!-- ring: front half (in front of the sphere) -->
  <path d="M 15.76,26.24 A 16.24,5.75 0 0 0 48.24,26.24" fill="none" stroke="#f5ebd6" stroke-width="1.9" stroke-linecap="round"/>
</svg>
)SVG";

extern const unsigned char kFaviconIcoData[];
extern const std::size_t kFaviconIcoData_size;

extern const unsigned char kAppleTouchIconPngData[];
extern const std::size_t kAppleTouchIconPngData_size;

} // namespace gitcube
