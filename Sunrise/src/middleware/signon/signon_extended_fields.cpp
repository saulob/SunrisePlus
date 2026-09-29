#include "signon_extended_fields.h"

#include <array>
#include <atomic>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../steam/interfaces/internal.h"

namespace sunrise::middleware::signon::extended {
namespace {

/** Success field 12 carries the optional extended sub-message. */
constexpr unsigned kExtendedField = 12;
/** SignOn success field 15 carries the country text. */
constexpr unsigned kCountryField = 15;
constexpr std::string_view kCountry = "KR";
/** The server fills field 1 of the extended sub-message. Fields 2 and 3 are request-side only. */
constexpr unsigned kNetworkIdField = 1;
/**
 * Relayed into the peer key exchange as 64 bits and never validated. Kept in the shape a real
 * server uses so a reader that range-checks the class nibble still accepts it.
 */
constexpr std::uint64_t kNetworkId = 0x4000000000000001ULL;
/** The extended sub-message holds one varint and its field key. */
constexpr std::size_t kExtendedBufferSize = 16;

/**
 * Encodes the extended sub-message into fixed storage.
 * Field 4, the server's 128-byte public key, is omitted. The applier zeroes its block for any
 * other length, and a synthetic key would ship invented material to peers.
 * @param size Cleared, then receives the encoded byte count.
 * @return True when the network id fits.
 */
[[nodiscard]] bool encode_extended(std::span<std::byte> output, std::size_t& size) noexcept {
    size = 0;
    Writer writer(output);
    if (!writer.varint(kNetworkIdField, kNetworkId)) {
        return false;
    }
    size = writer.size();
    return true;
}

} // namespace

/** Appends the extended sub-message and Korean country text to SignOn success. */
bool append(Writer& success) noexcept {
    std::array<std::byte, kExtendedBufferSize> extended{};
    std::size_t extendedSize = 0;
    const bool encoded = encode_extended(extended, extendedSize)
                         && success.bytes(kExtendedField, std::span(extended).first(extendedSize))
                         && success.bytes(kCountryField, text_bytes(kCountry));
    if (encoded) {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set()) {
            core::log::writef(core::log::Channel::server,
                              core::log::Level::warn,
                              "DEBUG_SAULO korea_test steam_country=%s signon_country=%.*s",
                              steam::interfaces::methods::country(nullptr),
                              static_cast<int>(kCountry.size()),
                              kCountry.data());
        }
    }
    return encoded;
}

} // namespace sunrise::middleware::signon::extended
