#include "takedown/guest/dmac_source_chain.hpp"

#include <stdexcept>

namespace takedown::guest {
namespace {

// Adapts the optional std::function hooks to the walker's consumer interface.
struct FunctionConsumer {
    const DmacSourceCallbacks& callbacks;

    [[nodiscard]] std::optional<DmacQuadword> read_quadword(const DmacSourceAddress source) const {
        return callbacks.read_quadword(source);
    }

    void on_tag(const DmacSourceTag& tag) const {
        if (callbacks.on_tag) {
            callbacks.on_tag(tag);
        }
    }

    void on_tag_word(const std::uint32_t word) const {
        if (callbacks.on_tag_word) {
            callbacks.on_tag_word(word);
        }
    }

    void on_payload_quadword(const DmacSourceAddress address, const DmacQuadword& quadword) const {
        if (callbacks.on_payload_quadword) {
            callbacks.on_payload_quadword(address, quadword);
        }
    }

    [[nodiscard]] bool on_payload_run(const DmacSourceAddress first, const std::uint32_t count) const {
        return callbacks.on_payload_run && callbacks.on_payload_run(first, count);
    }

    void on_tag_complete(const DmacSourceTag& tag, const DmacSourceCursor& cursor) const {
        if (callbacks.on_tag_complete) {
            callbacks.on_tag_complete(tag, cursor);
        }
    }
};

} // namespace

DmacSourceResult walk_dmac_source_chain(DmacSourceCursor cursor, const DmacSourceOptions& options,
                                        const DmacSourceCallbacks& callbacks) {
    if (!callbacks.read_quadword) {
        throw std::invalid_argument("DMAC source-chain traversal requires a reader");
    }
    return walk_dmac_source_chain_with(cursor, options, FunctionConsumer{callbacks});
}

} // namespace takedown::guest
