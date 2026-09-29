#pragma once

#include "internal.h"

namespace sunrise::middleware::signon::extended {

/**
 * Appends the optional SignOn success extended sub-message and Korean country text in field 15.
 * City and ISP fields remain absent.
 * @param success Writer bound to the success sub-message storage.
 * @return True when the sub-message fits.
 */
[[nodiscard]] bool append(Writer& success) noexcept;

} // namespace sunrise::middleware::signon::extended
