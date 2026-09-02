#include "scanengine/hybrid/canonical_generation.hpp"

namespace scanengine {
namespace hybrid {

QVector<int> canonicalNoRepeatNgramBannedTokens(const QVector<int>& history,
                                                int ngramSize) {
    QVector<int> banned;
    if (ngramSize <= 0 || history.size() + 1 < ngramSize)
        return banned;

    const int prefixSize = ngramSize - 1;
    const int currentPrefixStart = history.size() - prefixSize;
    const int lastNgramStart = history.size() - ngramSize;
    for (int start = 0; start <= lastNgramStart; ++start) {
        bool matches = true;
        for (int offset = 0; offset < prefixSize; ++offset) {
            if (history.at(start + offset)
                != history.at(currentPrefixStart + offset)) {
                matches = false;
                break;
            }
        }
        if (matches)
            banned.push_back(history.at(start + prefixSize));
    }
    return banned;
}

namespace {

bool cancellationRequested(const CanonicalGenerationRequest& request) {
    return request.isCancelled && request.isCancelled();
}

CanonicalGenerationResult initialResult(
    const CanonicalGenerationRequest& request) {
    CanonicalGenerationResult result;
    result.promptIds = request.promptIds;
    result.history = request.promptIds;
    return result;
}

CanonicalGenerationResult invalidRequest(
    const CanonicalGenerationRequest& request,
    const QString& error) {
    CanonicalGenerationResult result = initialResult(request);
    result.status = CanonicalGenerationStatus::Failed;
    result.stopReason = CanonicalGenerationStopReason::InvalidArguments;
    result.error = error;
    return result;
}

}  // namespace

CanonicalGenerationResult runCanonicalGeneration(
    CanonicalGenerationExecutor& executor,
    const CanonicalGenerationRequest& request) {
    if (request.promptIds.isEmpty()) {
        return invalidRequest(request, QStringLiteral("promptIds must not be empty"));
    }
    if (request.maxNewTokens <= 0) {
        return invalidRequest(
            request, QStringLiteral("maxNewTokens must be greater than zero"));
    }
    if (request.eosTokenId < 0) {
        return invalidRequest(request, QStringLiteral("eosTokenId must be non-negative"));
    }
    if (request.padTokenId < 0) {
        return invalidRequest(request, QStringLiteral("padTokenId must be non-negative"));
    }

    CanonicalGenerationResult result = initialResult(request);
    QVector<int> workingHistory = request.promptIds;
    QVector<int> workingNewIds;
    CanonicalTokenSelection workingLastSelection;
    for (int step = 0; step < request.maxNewTokens; ++step) {
        if (cancellationRequested(request)) {
            result.status = CanonicalGenerationStatus::Cancelled;
            result.stopReason = CanonicalGenerationStopReason::CancelRequested;
            result.committedStepsBeforeTerminal = workingNewIds.size();
            result.error.clear();
            return result;
        }

        const QVector<int> banned = canonicalNoRepeatNgramBannedTokens(
            workingHistory, request.noRepeatNgramSize);
        CanonicalTokenSelection selection;
        QString executorError;
        if (!executor.selectAfterMask(banned, &selection, &executorError)) {
            // Once an executor operation starts, its failure wins over a
            // cancellation request that arrives during that failing call.
            result.status = CanonicalGenerationStatus::Failed;
            result.stopReason = CanonicalGenerationStopReason::SelectFailed;
            result.committedStepsBeforeTerminal = workingNewIds.size();
            result.error = executorError;
            return result;
        }
        if (selection.tokenId < 0) {
            // Treat a successful call that violates the selection contract as
            // an executor error before taking the post-select cancel sample.
            result.status = CanonicalGenerationStatus::Failed;
            result.stopReason = CanonicalGenerationStopReason::InvalidSelection;
            result.committedStepsBeforeTerminal = workingNewIds.size();
            result.error = QStringLiteral("executor selected a negative token id");
            return result;
        }

        // Cancellation wins the race with a selected token. Until this check
        // passes, selection is provisional: history stays unchanged and the
        // executor must not advance.
        if (cancellationRequested(request)) {
            result.status = CanonicalGenerationStatus::Cancelled;
            result.stopReason = CanonicalGenerationStopReason::CancelRequested;
            result.committedStepsBeforeTerminal = workingNewIds.size();
            result.error.clear();
            return result;
        }

        workingLastSelection = selection;
        workingNewIds.push_back(selection.tokenId);
        workingHistory.push_back(selection.tokenId);

        if (selection.tokenId == request.eosTokenId) {
            result.status = CanonicalGenerationStatus::Completed;
            result.stopReason = CanonicalGenerationStopReason::EosToken;
            result.newIds = workingNewIds;
            result.history = workingHistory;
            result.lastSelection = workingLastSelection;
            result.committedStepsBeforeTerminal = workingNewIds.size();
            result.error.clear();
            return result;
        }
        if (selection.tokenId == request.padTokenId) {
            result.status = CanonicalGenerationStatus::Completed;
            result.stopReason = CanonicalGenerationStopReason::PadToken;
            result.newIds = workingNewIds;
            result.history = workingHistory;
            result.lastSelection = workingLastSelection;
            result.committedStepsBeforeTerminal = workingNewIds.size();
            result.error.clear();
            return result;
        }
        if (step + 1 == request.maxNewTokens) {
            result.status = CanonicalGenerationStatus::Completed;
            result.stopReason =
                CanonicalGenerationStopReason::TokenBudgetExhausted;
            result.newIds = workingNewIds;
            result.history = workingHistory;
            result.lastSelection = workingLastSelection;
            result.committedStepsBeforeTerminal = workingNewIds.size();
            result.error.clear();
            return result;
        }

        executorError.clear();
        if (!executor.advance(selection.tokenId, &executorError)) {
            // As with select, preserve a failing executor's exact error even
            // when cancellation races with the failing advance.
            result.status = CanonicalGenerationStatus::Failed;
            result.stopReason = CanonicalGenerationStopReason::AdvanceFailed;
            result.committedStepsBeforeTerminal = workingNewIds.size();
            result.error = executorError;
            return result;
        }
    }

    // A positive budget always returns from inside the loop. Retain one
    // explicit terminal fallback so every control path has one result state.
    result.status = CanonicalGenerationStatus::Completed;
    result.stopReason = CanonicalGenerationStopReason::TokenBudgetExhausted;
    result.newIds = workingNewIds;
    result.history = workingHistory;
    result.lastSelection = workingLastSelection;
    result.committedStepsBeforeTerminal = workingNewIds.size();
    result.error.clear();
    return result;
}

}  // namespace hybrid
}  // namespace scanengine
