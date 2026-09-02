#pragma once

#include <QString>
#include <QVector>

#include <functional>

namespace scanengine {
namespace hybrid {

enum class CanonicalGenerationStatus {
    Completed,
    Cancelled,
    Failed,
};

enum class CanonicalGenerationStopReason {
    EosToken,
    PadToken,
    TokenBudgetExhausted,
    CancelRequested,
    InvalidArguments,
    InvalidSelection,
    SelectFailed,
    AdvanceFailed,
};

struct CanonicalTokenSelection {
    int tokenId = -1;
    float score = 0.0f;
};

// transformers 4.57.6 NoRepeatNGramLogitsProcessor for one decoder-only
// hypothesis. History is the complete input_ids row, including the expanded
// prompt. Completion ids preserve their left-to-right order and duplicates.
// A non-positive size disables the processor.
QVector<int> canonicalNoRepeatNgramBannedTokens(
    const QVector<int>& history,
    int ngramSize);

// A prefill-ready device executor. The implementation owns logits and
// applies bannedTokenIds as -infinity before selecting. Greedy ties must select
// the lowest token id, matching torch.argmax. Full logits never cross this
// interface.
//
// One run consumes the executor. After runCanonicalGeneration returns -- for
// Completed, Cancelled, or Failed alike -- the caller must reset or destroy the
// executor before reuse. Result rollback never promises to roll back device or
// KV-cache state inside the executor.
class CanonicalGenerationExecutor {
public:
    virtual ~CanonicalGenerationExecutor() = default;

    virtual bool selectAfterMask(const QVector<int>& bannedTokenIds,
                                 CanonicalTokenSelection* selection,
                                 QString* error) = 0;

    // Consume a committed, non-terminal token and prepare the next selection.
    // A false return may leave executor state partially consumed.
    virtual bool advance(int selectedTokenId, QString* error) = 0;
};

struct CanonicalGenerationRequest {
    // Complete expanded decoder prompt after prefill. It is authoritative
    // no-repeat history and is retained in the result.
    QVector<int> promptIds;
    int maxNewTokens = 0;
    int noRepeatNgramSize = 0;
    int eosTokenId = -1;
    int padTokenId = -1;

    // Called immediately before every select and again after a successful,
    // valid select but before committing the token or calling advance. An
    // empty callback means false.
    //
    // Once selectAfterMask or advance has been invoked, an executor false
    // return (or an invalid selection) takes priority over cancellation that
    // arrives during that failing operation, preserving the exact executor
    // error. Cancellation observed after a successful, valid select wins
    // before token commit. Cancellation arriving during a successful advance
    // is observed at the next pre-select check.
    std::function<bool()> isCancelled;
};

struct CanonicalGenerationResult {
    CanonicalGenerationStatus status = CanonicalGenerationStatus::Failed;
    CanonicalGenerationStopReason stopReason =
        CanonicalGenerationStopReason::InvalidArguments;
    QVector<int> promptIds;
    QVector<int> newIds;
    QVector<int> history;
    CanonicalTokenSelection lastSelection;
    // Number of tokens accepted into the private working history before the
    // terminal outcome. Cancelled/Failed results expose only this count, never
    // partial token values.
    int committedStepsBeforeTerminal = 0;
    QString error;
};

// Run a single-hypothesis greedy decode from a prefill-ready executor. The
// common layer owns complete history, no-repeat policy, stopping, and
// cancellation. Results are transactional: only Completed publishes newIds,
// full history, and lastSelection. Cancelled/Failed retains only promptIds in
// history. This value-level transaction does not roll back executor state. A
// terminal token and the last budgeted token are published but are not advanced
// because no subsequent selection is needed. Every return consumes executor;
// reset or destroy it before reuse.
CanonicalGenerationResult runCanonicalGeneration(
    CanonicalGenerationExecutor& executor,
    const CanonicalGenerationRequest& request);

}  // namespace hybrid
}  // namespace scanengine
