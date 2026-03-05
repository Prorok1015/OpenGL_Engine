#pragma once
#include <image.h>
#include <memory>
#include <string_view>

std::unique_ptr<stb_image::Image> load_image(const std::string_view file);

