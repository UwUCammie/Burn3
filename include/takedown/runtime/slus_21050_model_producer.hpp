#pragma once
#include "takedown/runtime/slus_21050_model_publication.hpp"
#include <functional>
#include <memory>
#include <exception>

namespace takedown::runtime::slus_21050 {
// Keys belong to the game's resource owner (or a mod's resource namespace).
// Addresses and source-content scans have no meaning to this owner.
struct ModelProducerTexture {
    std::uint32_t texture{};
    std::uint64_t resource{};
    ModelTextureSampler sampler;
    TakedownNative::TextureFunction function{};
    bool alpha{};
};
struct ModelProducerLimits {
    std::size_t queued_models{8};
    std::size_t queued_payload_bytes{64U*1024U*1024U};
    std::size_t texture_bytes{64U*1024U*1024U};
    std::size_t texture_keys{4096};
};
struct ModelProducerStatistics {
    std::uint64_t prepared{}, published{}, cancelled{}, declined{}, texture_replacements{};
    std::uint64_t triangles{}, texture_definitions{}, sink_retries{}, sink_failures{};
    std::size_t queued_models{}, queued_payload_bytes{}, retained_texture_bytes{};
    bool failed{};
};
struct ModelProducerTicket {
    std::uint64_t value{};
    bool operator==(const ModelProducerTicket&) const = default;
};
struct ModelProducerPrepared {
    ModelProducerTicket ticket;
    ModelMaterialFrame completed_frame;
    ModelMaterialCache completed_cache;
};
// Borrowed only during prepare_transaction; accepted updates are copied into
// private immutable storage before that call returns.
struct ModelProducerTextureUpdate {
    std::uint64_t resource{};
    const ModelTextureAsset* asset{};
};
enum class ModelProducerPublish { Empty, WrongOrder, Retry, Published, Failed };

// Serialized owner of complete native model operations. All source views are
// consumed during enqueue; accepted work owns its geometry, state and immutable
// texture versions. No source, renderer or guest state is changed by enqueue.
// The caller must explicitly consume the front ticket at its real execution
// boundary. Preparing an operation never draws it or fabricates completion.
//
// A sink returning false guarantees that it has published NOTHING. Any exception
// poisons this connection: partial output is not retryable or replayable through
// a compatibility path. Sink acceptance means ordered publication, not GPU
// completion; the renderer retains its own resources until GPU retirement.
class ModelProducer {
public:
    class Transaction {
    public:
        ~Transaction();
        Transaction(Transaction&&) noexcept;
        Transaction& operator=(Transaction&&) noexcept;
        Transaction(const Transaction&)=delete;
        Transaction& operator=(const Transaction&)=delete;
        // Requires a token which has not been moved from or committed.
        [[nodiscard]] const ModelProducerPrepared& completion() const noexcept;
    private:
        friend class ModelProducer;
        struct State;
        explicit Transaction(std::unique_ptr<State>) noexcept;
        std::unique_ptr<State> state_;
    };
    explicit ModelProducer(ModelProducerLimits = {});
    ~ModelProducer();
    ModelProducer(const ModelProducer&)=delete;
    ModelProducer& operator=(const ModelProducer&)=delete;
    // Always copy into private storage, even from a shared_ptr<const> caller:
    // const access alone does not prove that no mutable aliases exist.
    [[nodiscard]] std::optional<TakedownNative::TextureVersion> replace_texture(
        std::uint64_t resource,const ModelTextureAsset&);
    // New work cannot select an unloaded key; already queued work retains it.
    // Generations never rewind, including unload/reload of the same key.
    [[nodiscard]] bool unload_texture(std::uint64_t resource);
    [[nodiscard]] std::optional<ModelProducerPrepared> enqueue(
        const ModelRenderInput&,std::span<const ModelRasterPass>,
        std::span<const ModelProducerTexture>);
    // A previously prepared native operation may enter the same queue without
    // redoing visibility/lighting. It is taken by value and rebound to this
    // owner's current texture versions before admission.
    [[nodiscard]] std::optional<ModelProducerPrepared> enqueue_prepared(
        PreparedModelRender,std::span<const ModelRasterPass>,
        std::span<const ModelProducerTexture>,bool inherit_scene_ambient=false);
    // Stage updates, immutable bindings, complete compilation and all budget
    // checks without changing this owner. The token is bound to this owner's
    // revision; any intervening successful catalog/queue mutation invalidates it.
    // Staging storage belongs to the caller until commit/destruction; the owner
    // budgets bound committed work, not arbitrarily many outstanding tokens.
    [[nodiscard]] std::optional<Transaction> prepare_transaction(
        PreparedModelRender,std::span<const ModelRasterPass>,
        std::span<const ModelProducerTexture>,std::span<const ModelProducerTextureUpdate>,
        bool inherit_scene_ambient=false) const;
    [[nodiscard]] bool transaction_current(const Transaction&) const noexcept;
    // Stale/foreign/consumed tokens reject before mutation. Success transfers
    // preallocated storage only: no compilation, publication or allocation.
    [[nodiscard]] std::optional<ModelProducerPrepared> commit_transaction(Transaction&&) noexcept;
    [[nodiscard]] ModelProducerPublish publish(ModelProducerTicket,
        const std::function<bool(const TakedownNative::ModelPublication&)>& sink);
    [[nodiscard]] std::optional<ModelProducerTicket> front() const noexcept;
    [[nodiscard]] std::exception_ptr failure() const noexcept;
    // Only the complete unpublished suffix can be abandoned. Definitions
    // selected for later work are then rebuilt from accepted publications.
    // This is a native queue cancellation, not a guest queue release operation.
    [[nodiscard]] bool cancel_pending() noexcept;
    [[nodiscard]] ModelProducerStatistics statistics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
