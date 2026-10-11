#pragma once
namespace nvof {
constexpr unsigned normalize_height_limit(unsigned value) noexcept {return value==1080 || value==1440?value:0;}
// Deliberately use source height, not width/long edge or display dimensions.
constexpr bool input_height_selected(int height,unsigned limit) noexcept {
    return height>0 && (!normalize_height_limit(limit) || static_cast<unsigned>(height)<=limit);
}
}
