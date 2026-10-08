#pragma once

/// The one place the build label lives. Written into the F12 / "Save frame (JSON)" frame dump
/// (meta.build) and anywhere else a build label is recorded. Bump it with each package.
inline constexpr const char* kBuildLabel = "40c";
