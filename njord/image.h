// Copyright 2026 Adam DePrince. Apache-2.0.
#pragma once
#include <string>
struct ImageOptions {
    std::string ca;
    int width = 1024;
    float quality = 65;
    long timeout = 60;
    bool allow_private = false;
};
// Runs in an isolated child, with one blocking socket connected to the relay.
void njord_image_worker(int fd, const ImageOptions& options) noexcept;
